#include "Renderer.hpp"

#include <codecvt>
#include <dxcapi.h> // Include DXC API header
#include <locale>

#include "DX12Device.hpp"     // Need full definitions now
#include "CommandQueue.hpp"
#include "SwapChain.hpp"
#include "CommandListManager.hpp"
#include "RootSignature.hpp"
#include "PipelineStateObject.hpp"
#include "DescriptorHeap.hpp"
#include "Buffer.hpp"
#include "Camera.hpp"
#include "Mesh.hpp"
#include "Texture.hpp"
#include "d3dx12.h"         // For helpers
#include <stdexcept>
#include <glm/gtc/type_ptr.hpp> // For memcpy with glm

#include "Shader.hpp"
#pragma comment(lib, "dxcompiler.lib") // Link against DXC library

inline size_t AlignUp(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}


Renderer::Renderer() : m_device(nullptr),
                       m_commandQueue(nullptr),
                       m_swapChain(nullptr),
                       m_numFramesInFlight(0),
                       m_currentFrameIndex(0),
                       m_totalTime(0.0f) {
    for (UINT i = 0; i < SwapChain::kBackBufferCount; ++i) { // Use constant from SwapChain
        m_frameFenceValues[i] = 0;
    }
    m_dsvHandleCPU = {0};
}

Renderer::~Renderer() {
    shutdown();
}

bool Renderer::init(DX12Device* pDevice, CommandQueue* pCommandQueue, SwapChain* pSwapChain, UINT numFrames) {
    m_device = pDevice;
    m_commandQueue = pCommandQueue;
    m_swapChain = pSwapChain;
    m_numFramesInFlight = numFrames; // Should match swap chain buffer count

    if (!checkRayTracingSupport()) {
        OutputDebugStringW(L"Warning: DirectX Raytracing Tier 1.1 not supported.\n");
        // Allow continuing without DXR, but flag it
        m_rayTracingSupported = false;
    } else {
        m_rayTracingSupported = true;
        OutputDebugStringW(L"DirectX Raytracing Tier 1.1 Supported.\n");
    }

    // Create command manager first
    m_commandManager = std::make_unique<CommandListManager>();
    if (!m_commandManager->create(m_device->getDevice(), D3D12_COMMAND_LIST_TYPE_DIRECT, m_numFramesInFlight)) {
        return false;
    }

    // Create other resources
    if (!createDescriptorHeaps()) {
        return false;
    }
    if (!createDepthStencilResources()) {
        return false;
    }
    if (!createConstantBuffersAndViews()) {
        return false;
    }
    if (!createRootSignatureAndPSO()) {
        return false;
    } // PSO depends on Root Sig

    // Create DXR specific resources (if supported)
    if (m_rayTracingSupported) {
        if (!CreateDXRResources()) {
            return false;
        }
        if (!CreateDXRRootSignature()) {
            return false;
        }
        if (!CreateDXRStateObject(L"Raytracing.hlsl")) {
            return false;
        }
        if (!BuildShaderBindingTable()) {
            return false;
        }
        ID3D12Device5* dxrDevice = nullptr;
        m_device->getDevice()->QueryInterface(IID_PPV_ARGS(&dxrDevice));
        if (dxrDevice) {
            auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            auto instanceDescBufferSize = sizeof(D3D12_RAYTRACING_INSTANCE_DESC); // Only one instance
            auto instanceDescBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(instanceDescBufferSize);
            HRESULT hr = dxrDevice->CreateCommittedResource(
                &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &instanceDescBufferDesc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_tlasBuffers.instanceDesc));
            if (FAILED(hr)) {
                OutputDebugStringW(L"Failed to create TLAS Instance Desc Buffer.\n");
                dxrDevice->Release();
                return false;
            }
            m_tlasBuffers.instanceDesc->SetName(L"TLAS Instance Descriptors");
            dxrDevice->Release();
        } else {
            return false;
        }
    }

    // Get initial state
    m_currentFrameIndex = m_swapChain->getCurrentBackBufferIndex();
    setupViewportAndScissor(m_swapChain->getWidth(), m_swapChain->getHeight()); // Use SwapChain dimensions

    return true;
}

void Renderer::shutdown() {
    // Ensure GPU is idle before releasing resources
    if (m_commandQueue) {
        m_commandQueue->join();
    }

    // Release owned resources (unique_ptrs handle most)
    m_rasterPipelineState.reset();
    m_rasterRootSignature.reset();
    m_depthStencilBuffer.Reset();
    m_dsvHeap.reset();
    m_srvHeap.reset();
    for (auto& cb: m_perFrameObjectCBs) {
        if (cb) cb->unmap();
        cb.reset();
    }
    for (auto& cb: m_perFrameLightCBs) {
        if (cb) cb->unmap();
        cb.reset();
    }
    if (m_materialCB) m_materialCB->unmap();
    m_materialCB.reset();
    m_perFrameObjectCBs.clear();
    m_perFrameLightCBs.clear();
    m_commandManager.reset();

    // Pointers to external objects are not released here
    m_device = nullptr;
    m_commandQueue = nullptr;
    m_swapChain = nullptr;
}

void Renderer::render(float deltaTime, Camera* camera, Mesh* mesh, Texture* texture, bool useRaytracing) {
    if (!camera || !mesh || !texture || !m_rasterPipelineState) {
        return; // Need essential objects
    }

    // --- Wait & Update CB Data ---
    waitForGpu();
    updateConstantBuffers(deltaTime, camera, mesh); // Update CBs for this frame

    // --- Record Rendering Commands ---
    if (!m_commandManager->resetAllocator(m_currentFrameIndex)) return;
    if (!m_commandManager->resetCommandList(m_currentFrameIndex, m_rasterPipelineState->getPipeline())) return;

    ID3D12GraphicsCommandList5* commandList = nullptr;
    if (m_rayTracingSupported && useRaytracing) {
        m_commandManager->getCommandList()->QueryInterface(IID_PPV_ARGS(&commandList));
        if (!commandList) {
            useRaytracing = false;
        } // Fallback if query fails
    }
    ID3D12GraphicsCommandList* baseCmdList = m_commandManager->getCommandList();


    ID3D12Resource* currentBackBuffer = m_swapChain->getCurrentBackBufferResource();
    if (!currentBackBuffer) {
        if (commandList) {
            commandList->Release();
        }
        return;
    }
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(currentBackBuffer, D3D12_RESOURCE_STATE_PRESENT,
                                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
    baseCmdList->ResourceBarrier(1, &barrier);

    // Set common descriptor heap (SRV/CBV heap)
    ID3D12DescriptorHeap* ppHeaps[] = {m_srvHeap->getHeapPointer()};
    baseCmdList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
    if (useRaytracing) {
        renderRaytraced(camera, mesh, texture, static_cast<ID3D12GraphicsCommandList5*>(commandList));
    } else {
        renderRaster(baseCmdList, mesh, texture);
    }
    // Barrier: Render Target -> Present
    barrier = CD3DX12_RESOURCE_BARRIER::Transition(currentBackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   D3D12_RESOURCE_STATE_PRESENT);
    baseCmdList->ResourceBarrier(1, &barrier);

    // Finish Recording & Execute
    HRESULT hr = baseCmdList->Close();
    if (FAILED(hr)) { /* Handle error */
    } else {
        ID3D12CommandList* ppCommandLists[] = {baseCmdList};
        m_commandQueue->executeCommandLists(1, ppCommandLists);
        if (!m_swapChain->present(0)) {
        }
        moveToNextFrame();
    }

    // Release queried interface
    if (commandList) commandList->Release();
}

void Renderer::signalAssetUploadComplete() {
}

bool Renderer::checkRayTracingSupport() {
    D3D12_FEATURE_DATA_D3D12_OPTIONS5 options5 = {};
    HRESULT hr = m_device->getDevice()->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS5, &options5, sizeof(options5));
    if (SUCCEEDED(hr) && options5.RaytracingTier >= D3D12_RAYTRACING_TIER_1_1) {
        return true;
    }
    return false;
}

bool Renderer::createDescriptorHeaps() {
    ID3D12Device* device = m_device->getDevice();

    m_dsvHeap = std::make_unique<DescriptorHeap>();
    if (!m_dsvHeap->create(device, D3D12_DESCRIPTOR_HEAP_TYPE_DSV, 1, D3D12_DESCRIPTOR_HEAP_FLAG_NONE)) {
        return false;
    }
    m_dsvHeap->getHeapPointer()->SetName(L"DSV Heap"); // Set name on underlying heap

    // Create SRV Heap (e.g., 1 for texture + spare)
    m_srvHeap = std::make_unique<DescriptorHeap>();
    /*const UINT numFrameLightCBVs = SwapChain::kBackBufferCount;
    const UINT numMaterialCBVs = 1;
    const UINT numTextureSRVs = 1;
    const UINT totalDescriptors = numFrameLightCBVs + numMaterialCBVs + numTextureSRVs + 4; // +4 spare*/
    // Create CBV/SRV Heap
    // Need space for:
    // Raster: k Light CBVs + 1 Material CBV + 1 Texture SRV = k+2
    // DXR: 1 Camera CBV + 1 VB SRV + 1 IB SRV + 1 Output UAV = 4
    // Total = k + 2 + 4 + spare = k + 6 + spare
    const UINT numFrameLightCBVs = m_numFramesInFlight;
    const UINT numMaterialCBVs = 1;
    const UINT numTextureSRVs = 1;
    const UINT numDxrCameraCBVs = 1;
    const UINT numDxrObjectCBVs = 1;
    const UINT numDxrLightCBVs = 1;
    const UINT numDxrMaterialCBVs = 1;
    const UINT numDxrBufferSRVs = 2; // VB + IB
    const UINT numDxrOutputUAVs = 1;
    const UINT totalDescriptors = numFrameLightCBVs + numMaterialCBVs + numTextureSRVs + numDxrObjectCBVs
                                  + numDxrCameraCBVs + numDxrBufferSRVs + numDxrOutputUAVs + numDxrLightCBVs +
                                  numDxrMaterialCBVs + 4; // +4 spare
    if (!m_srvHeap->create(device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, totalDescriptors, true)) { // Shader visible
        OutputDebugStringW(L"Error: Failed to create SRV Heap.\n");
        return false;
    }
    m_srvHeap->getHeapPointer()->SetName(L"SRV Heap");
    return true;
}

bool Renderer::createDepthStencilResources() {
    ID3D12Device* device = m_device->getDevice();
    if (!m_dsvHeap) return false; // Ensure heap exists

    // 1. Create the Depth Stencil Buffer Resource (Texture)
    DXGI_FORMAT dsvFormat = DXGI_FORMAT_D32_FLOAT;
    D3D12_RESOURCE_DESC depthStencilDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        dsvFormat, m_swapChain->getWidth(), m_swapChain->getHeight(), 1, 0, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    D3D12_CLEAR_VALUE depthOptimizedClearValue = {};
    depthOptimizedClearValue.Format = dsvFormat;
    depthOptimizedClearValue.DepthStencil.Depth = 1.0f;
    depthOptimizedClearValue.DepthStencil.Stencil = 0;
    auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = device->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &depthStencilDesc,
        D3D12_RESOURCE_STATE_DEPTH_WRITE, &depthOptimizedClearValue,
        IID_PPV_ARGS(&m_depthStencilBuffer));
    if (FAILED(hr)) {
        OutputDebugStringW(L"Error: Failed to create Depth Buffer Resource.\n");
        return false;
    }
    m_depthStencilBuffer->SetName(L"Depth Stencil Buffer");

    // 2. Allocate descriptor handle from the DSV heap
    D3D12_GPU_DESCRIPTOR_HANDLE ignoredGpuHandle; // DSV heap is not shader visible
    if (!m_dsvHeap->allocateDescriptor(m_dsvHandleCPU, ignoredGpuHandle)) {
        OutputDebugStringW(L"Error: Failed to allocate DSV descriptor.\n");
        return false;
    }

    // 3. Create the Depth Stencil View (DSV) descriptor
    D3D12_DEPTH_STENCIL_VIEW_DESC dsvDesc = {};
    dsvDesc.Format = dsvFormat;
    dsvDesc.ViewDimension = D3D12_DSV_DIMENSION_TEXTURE2D;
    dsvDesc.Flags = D3D12_DSV_FLAG_NONE;
    device->CreateDepthStencilView(m_depthStencilBuffer.Get(), &dsvDesc, m_dsvHandleCPU); // Use allocated handle

    return true;
}

bool Renderer::createConstantBuffersAndViews() {
    ID3D12Device* device = m_device->getDevice();

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {0};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {0};

    m_perFrameLightCBs.resize(m_numFramesInFlight);
    for (UINT i = 0; i < m_numFramesInFlight; ++i) {
        m_perFrameLightCBs[i] = std::make_unique<Buffer>();
        if (!m_perFrameLightCBs[i]->create(device, sizeof(LightConstant),
                                           D3D12_HEAP_TYPE_UPLOAD,
                                           D3D12_RESOURCE_STATE_GENERIC_READ, true)) {
            OutputDebugStringW(L"Error: Failed to create per-frame light constant buffer.\n");
            return false;
        }
        wchar_t name[50];
        swprintf_s(name, L"Per-Frame Light Constant Buffer %u", i);
        m_perFrameLightCBs[i]->getResource()->SetName(name);
        // Persistently map the buffers
        m_perFrameLightCBs[i]->map();

        if (!m_srvHeap->allocateDescriptor(cpuHandle, gpuHandle)) {
            return false;
        }

        D3D12_CONSTANT_BUFFER_VIEW_DESC cbvDesc = {};
        cbvDesc.BufferLocation = m_perFrameLightCBs[i]->getGPUVirtualAddress();
        cbvDesc.SizeInBytes = static_cast<UINT>(m_perFrameLightCBs[i]->getAlignedSize());
        device->CreateConstantBufferView(&cbvDesc, cpuHandle);
    }

    m_materialCB = std::make_unique<Buffer>();
    // Create in UPLOAD heap for potential updates, though likely static for now
    if (!m_materialCB->create(device, sizeof(MaterialConstant), D3D12_HEAP_TYPE_UPLOAD,
                              D3D12_RESOURCE_STATE_GENERIC_READ, true)) {
        return false;
    }
    m_materialCB->getResource()->SetName(L"Material Constant Buffer");
    // Update with initial material data
    MaterialConstant materialConsts = {};
    materialConsts.specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f); // White specular
    materialConsts.specularPower = 32.0f; // Medium shininess
    void* matMapped = m_materialCB->map();
    if (matMapped) {
        memcpy(matMapped, &materialConsts, sizeof(materialConsts));
        m_materialCB->unmap(sizeof(materialConsts));
    }

    if (!m_srvHeap->allocateDescriptor(cpuHandle, gpuHandle)) {
        return false;
    }
    D3D12_CONSTANT_BUFFER_VIEW_DESC matCbvDesc = {};
    matCbvDesc.BufferLocation = m_materialCB->getGPUVirtualAddress();
    matCbvDesc.SizeInBytes = static_cast<UINT>(m_materialCB->getAlignedSize());
    device->CreateConstantBufferView(&matCbvDesc, cpuHandle);

    m_dxrCameraCB = std::make_unique<Buffer>();
    if (!m_dxrCameraCB->create(device, sizeof(DXRCameraConstants), D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_STATE_GENERIC_READ, true)) {
        return false;
    }

    m_dxrCameraCB->getResource()->SetName(L"DXR Camera CB");
    m_dxrCameraCB->map(); // Persistently map
    if (!m_srvHeap->allocateDescriptor(cpuHandle, gpuHandle)) return false; // Allocate slot k+1
    m_dxrCameraCbvHandleGPU = gpuHandle; // Store GPU handle for binding
    D3D12_CONSTANT_BUFFER_VIEW_DESC dxrCamCbvDesc = {};
    dxrCamCbvDesc.BufferLocation = m_dxrCameraCB->getGPUVirtualAddress();
    dxrCamCbvDesc.SizeInBytes = static_cast<UINT>(m_dxrCameraCB->getAlignedSize());
    device->CreateConstantBufferView(&dxrCamCbvDesc, cpuHandle);

    m_dxrObjectCB = std::make_unique<Buffer>();
    if (!m_dxrObjectCB->create(device, sizeof(DXRObjectConstants), D3D12_HEAP_TYPE_UPLOAD,
                               D3D12_RESOURCE_STATE_GENERIC_READ, true))
        return false;
    m_dxrObjectCB->getResource()->SetName(L"DXR Object CB");
    m_dxrObjectCB->map(); // Persistently map for updates
    if (!m_srvHeap->allocateDescriptor(cpuHandle, m_dxrObjectCbvHandleGPU)) return false; // Store GPU handle
    D3D12_CONSTANT_BUFFER_VIEW_DESC dxrObjCbvDesc = {};
    dxrObjCbvDesc.BufferLocation = m_dxrObjectCB->getGPUVirtualAddress();
    dxrObjCbvDesc.SizeInBytes = static_cast<UINT>(m_dxrObjectCB->getAlignedSize());
    device->CreateConstantBufferView(&dxrObjCbvDesc, cpuHandle);

    m_dxrLightCB = std::make_unique<Buffer>();
    if (!m_dxrLightCB->create(device, sizeof(LightConstant), D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ,
                              true))
        return false;
    m_dxrLightCB->getResource()->SetName(L"DXR Light CB");
    m_dxrLightCB->map(); // Persistently map
    if (!m_srvHeap->allocateDescriptor(cpuHandle, m_dxrLightCbvHandleGPU)) return false;
    D3D12_CONSTANT_BUFFER_VIEW_DESC dxrLightCbvDesc = {};
    dxrLightCbvDesc.BufferLocation = m_dxrLightCB->getGPUVirtualAddress();
    dxrLightCbvDesc.SizeInBytes = static_cast<UINT>(m_dxrLightCB->getAlignedSize());
    device->CreateConstantBufferView(&dxrLightCbvDesc, cpuHandle);

    // 6. ***** NEW ***** DXR Material Constant Buffer & CBV
    m_dxrMaterialCB = std::make_unique<Buffer>();
    if (!m_dxrMaterialCB->create(device, sizeof(MaterialConstant), D3D12_HEAP_TYPE_UPLOAD,
                                 D3D12_RESOURCE_STATE_GENERIC_READ, true))
        return false;
    m_dxrMaterialCB->getResource()->SetName(L"DXR Material CB");
    // Update with initial DXR material data (can be same as raster)
    MaterialConstant dxrMaterialConsts = {};
    dxrMaterialConsts.specularColor = vec4(1.0f, 1.0f, 1.0f, 1.0f);
    dxrMaterialConsts.specularPower = 32.0f;
    void* dxrMatMapped = m_dxrMaterialCB->map();
    if (dxrMatMapped) {
        memcpy(dxrMatMapped, &dxrMaterialConsts, sizeof(dxrMaterialConsts));
        m_dxrMaterialCB->unmap(sizeof(dxrMaterialConsts));
    } // Map, copy, unmap once
    else {
        m_dxrMaterialCB->map();
    } // Re-map if initial map failed or was unmapped

    if (!m_srvHeap->allocateDescriptor(cpuHandle, m_dxrMaterialCbvHandleGPU)) return false;
    D3D12_CONSTANT_BUFFER_VIEW_DESC dxrMatCbvDesc = {};
    dxrMatCbvDesc.BufferLocation = m_dxrMaterialCB->getGPUVirtualAddress();
    dxrMatCbvDesc.SizeInBytes = static_cast<UINT>(m_dxrMaterialCB->getAlignedSize());
    device->CreateConstantBufferView(&dxrMatCbvDesc, cpuHandle);

    m_perFrameObjectCBs.resize(m_numFramesInFlight);
    size_t bufferSize = (sizeof(ObjectConstant) + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) & ~(
                            D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1);

    for (UINT i = 0; i < m_numFramesInFlight; ++i) {
        m_perFrameObjectCBs[i] = std::make_unique<Buffer>();
        if (!m_perFrameObjectCBs[i]->create(device, bufferSize, D3D12_HEAP_TYPE_UPLOAD,
                                            D3D12_RESOURCE_STATE_GENERIC_READ, true)) {
            OutputDebugStringW(L"Error: Failed to create per-frame object constant buffer.\n");
            return false;
        }
        wchar_t name[50];
        swprintf_s(name, L"Per-Frame Object Constant Buffer %u", i);
        m_perFrameObjectCBs[i]->getResource()->SetName(name);
        // Persistently map the buffers
        m_perFrameObjectCBs[i]->map();
    }
    return true;
}

bool Renderer::createRootSignatureAndPSO() {
    // Move Root Signature and PSO creation logic from Application::CreateRootSignatureAndPSO here
    // Use m_pDevice->GetDevice()
    // Store results in m_rootSignature and m_pipelineState
    ID3D12Device* device = m_device->getDevice();

    // Create Root Signature (Root CBV@p0, SRV Table@t0, Light Table@b2, Material Table@b3)
    m_rasterRootSignature = std::make_unique<RootSignature>();
    CD3DX12_DESCRIPTOR_RANGE1 ranges[3];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0); // Tex@t0
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 2); // Light@b2
    ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 3); // Mat@b3
    CD3DX12_ROOT_PARAMETER1 rootParameters[4];
    rootParameters[0].InitAsConstantBufferView(0, 0); // Object@b0
    rootParameters[1].InitAsDescriptorTable(1, &ranges[0], D3D12_SHADER_VISIBILITY_PIXEL);
    rootParameters[2].InitAsDescriptorTable(1, &ranges[1], D3D12_SHADER_VISIBILITY_ALL);
    rootParameters[3].InitAsDescriptorTable(1, &ranges[2], D3D12_SHADER_VISIBILITY_PIXEL);
    CD3DX12_STATIC_SAMPLER_DESC sampler(0, D3D12_FILTER_MIN_MAG_MIP_LINEAR, D3D12_TEXTURE_ADDRESS_MODE_WRAP,
                                        D3D12_TEXTURE_ADDRESS_MODE_WRAP, D3D12_TEXTURE_ADDRESS_MODE_WRAP);
    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    rootSignatureDesc.Init_1_1(_countof(rootParameters), rootParameters, 1, &sampler,
                               D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    // Serialize and Create...
    ComPtr<ID3DBlob> signatureBlob, errorBlob;
    HRESULT hr;
    hr = D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &signatureBlob,
                                               &errorBlob);
    if (FAILED(hr)) {
        return false;
    }
    hr = device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
                                     IID_PPV_ARGS(m_rasterRootSignature->GetSignatureAddressOf()));
    if (FAILED(hr)) {
        return false;
    }
    m_rasterRootSignature->getSignature()->SetName(L"Main Root Sig (Lighting)");

    auto vertexShader = std::make_unique<Shader>();
    if (!vertexShader->loadAndCompile(L"SimpleShaders.hlsl", "VSMain", "vs_5_1")) return false;
    auto pixelShader = std::make_unique<Shader>();
    if (!pixelShader->loadAndCompile(L"SimpleShaders.hlsl", "PSMain", "ps_5_1")) return false;


    // Define Input Layout
    D3D12_INPUT_ELEMENT_DESC inputElementDescs[] = {
        {
            "POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, position),
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        },
        {
            "COLOR", 0, DXGI_FORMAT_R32G32B32A32_FLOAT, 0, offsetof(Vertex, color),
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        },
        {
            "TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, offsetof(Vertex, texCoord),
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        },
        {
            "NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, offsetof(Vertex, normal),
            D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0
        }
    };
    D3D12_INPUT_LAYOUT_DESC inputLayoutDesc = {inputElementDescs, _countof(inputElementDescs)};

    // Create PSO
    m_rasterPipelineState = std::make_unique<PipelineStateObject>();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = inputLayoutDesc;
    psoDesc.pRootSignature = m_rasterRootSignature->getSignature(); // Use the NEW root signature
    psoDesc.VS = vertexShader->getBytecode();
    psoDesc.PS = pixelShader->getBytecode();
    psoDesc.RasterizerState = CD3DX12_RASTERIZER_DESC(D3D12_DEFAULT);
    psoDesc.RasterizerState.CullMode = D3D12_CULL_MODE_BACK;
    psoDesc.RasterizerState.FrontCounterClockwise = TRUE;
    psoDesc.BlendState = CD3DX12_BLEND_DESC(D3D12_DEFAULT);
    psoDesc.DepthStencilState.DepthEnable = TRUE;
    psoDesc.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    psoDesc.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_LESS;
    psoDesc.DepthStencilState.StencilEnable = FALSE;
    psoDesc.SampleMask = UINT_MAX;
    psoDesc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    psoDesc.NumRenderTargets = 1;
    psoDesc.RTVFormats[0] = m_swapChain->getFormat();
    psoDesc.DSVFormat = DXGI_FORMAT_D32_FLOAT;
    psoDesc.SampleDesc.Count = 1;


    if (!m_rasterPipelineState->create(device, psoDesc)) {
        return false;
    }
    m_rasterPipelineState->getPipeline()->SetName(L"Main PSO (Lighting)");

    return true;
}

void Renderer::setupViewportAndScissor(UINT width, UINT height) {
    m_viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    m_scissorRect = CD3DX12_RECT(0, 0, width, height);
}

void Renderer::updateConstantBuffers(float deltaTime, Camera* camera, Mesh* mesh) {
    m_totalTime += deltaTime; // Approximate time update - better to pass deltaTime

    // --- Update Per-Frame Light Constant Buffer ---
    Buffer* currentLightCB = m_perFrameLightCBs[m_currentFrameIndex].get();
    LightConstant lightConsts = {};
    lightConsts.ambientColor = vec4(0.2f, 0.2f, 0.2f, 1.0f);
    float lightX = sin(m_totalTime * 0.5f) * 3.0f;
    float lightZ = cos(m_totalTime * 0.5f) * 3.0f;
    lightConsts.lightPosition = vec3(lightX, 2.0f, lightZ);
    lightConsts.lightColor = vec4(1.0f, 1.0f, 1.0f, 1.0f);
    lightConsts.cameraPosition = camera->getPosition();
    void* lightMappedData = currentLightCB->map();
    if (lightMappedData) {
        memcpy(lightMappedData, &lightConsts, sizeof(lightConsts));
        currentLightCB->unmap(sizeof(lightConsts));
    }

    // --- Update Per-Object Constant Buffer ---
    Buffer* currentObjectCB = m_perFrameObjectCBs[m_currentFrameIndex].get();
    glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
    //glm::mat4 worldMatrix = glm::rotate(glm::mat4(1.0f), m_totalTime * 0.1f, glm::vec3(0.0f, 1.0f, 0.0f));
    glm::mat4 worldMatrix = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f));
    ObjectConstant objectConsts;
    objectConsts.worldMatrix = worldMatrix;
    objectConsts.mvpMatrix = viewProj * worldMatrix;
    // Map, copy, unmap
    void* objectMappedData = currentObjectCB->map();
    if (objectMappedData) {
        memcpy(objectMappedData, &objectConsts, sizeof(ObjectConstant));
        // Unmap after writing (specify range for potential optimization)
        currentObjectCB->unmap(sizeof(ObjectConstant));
    }

    if (m_rayTracingSupported && m_dxrCameraCB) {
        DXRCameraConstants dxrCamConsts = {};
        mat4 view = camera->getViewMatrix();
        mat4 proj = camera->getProjectionMatrix();
        dxrCamConsts.inverseViewProjectMatrix = glm::inverse(proj * view);
        dxrCamConsts.position = camera->getPosition();
        void* dxrCamMapped = m_dxrCameraCB->map();
        if (dxrCamMapped) {
            memcpy(dxrCamMapped, &dxrCamConsts, sizeof(dxrCamConsts));
            m_dxrCameraCB->unmap(sizeof(DXRCameraConstants));
        }
    }
    if (m_rayTracingSupported && m_dxrObjectCB && mesh) { // Check pMesh too
        DXRObjectConstants dxrObjConsts = {};
        // Use the same world matrix as raster for consistency for now
        dxrObjConsts.worldMatrix = worldMatrix;
        dxrObjConsts.invTransposeWorldMatrix = glm::transpose(glm::inverse(glm::mat3(dxrObjConsts.worldMatrix)));
        // For normals

        void* dxrObjMapped = m_dxrObjectCB->map();
        if (dxrObjMapped) {
            memcpy(dxrObjMapped, &dxrObjConsts, sizeof(dxrObjConsts));
            m_dxrObjectCB->unmap(sizeof(DXRObjectConstants));
        }
    }
    if (m_rayTracingSupported && m_tlasBuffers.instanceDesc && m_blasBuffers.result) {
        D3D12_RAYTRACING_INSTANCE_DESC instanceDesc = {};
        glm::mat4 transposedWorld = glm::transpose(worldMatrix); // DXR instance transform is row-major
        memcpy(instanceDesc.Transform, glm::value_ptr(transposedWorld), sizeof(instanceDesc.Transform));
        instanceDesc.InstanceMask = 1;
        instanceDesc.InstanceID = 0;
        instanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_TRIANGLE_CULL_DISABLE;
        instanceDesc.AccelerationStructure = m_blasBuffers.result->GetGPUVirtualAddress();

        void* pMappedData = nullptr;
        HRESULT hr = m_tlasBuffers.instanceDesc->Map(0, nullptr, &pMappedData);
        if (SUCCEEDED(hr) && pMappedData) {
            memcpy(pMappedData, &instanceDesc, sizeof(instanceDesc));
            m_tlasBuffers.instanceDesc->Unmap(0, nullptr);
        } else {
            OutputDebugStringW(L"Error: Failed to map TLAS instance descriptor buffer for update.\n");
        }
    }
    if (m_rayTracingSupported && m_dxrLightCB) {
        void* dxrLightMapped = m_dxrLightCB->map();
        if (dxrLightMapped) {
            memcpy(dxrLightMapped, &lightConsts, sizeof(lightConsts));
            m_dxrLightCB->unmap(sizeof(LightConstant));
        }
    }
}

void Renderer::waitForGpu() {
    UINT64 fenceValue = m_frameFenceValues[m_currentFrameIndex];
    if (fenceValue > 0) {
        m_commandQueue->waitForFence(fenceValue);
    }
}

void Renderer::moveToNextFrame() {
    const UINT64 currentFenceValue = m_commandQueue->signal();
    m_frameFenceValues[m_currentFrameIndex] = currentFenceValue;
    m_currentFrameIndex = m_swapChain->getCurrentBackBufferIndex();
}

D3D12_GPU_DESCRIPTOR_HANDLE Renderer::GetGpuHandleFromHeap(DescriptorHeap* heap, UINT index) {
    if (!heap || index >= heap->getCapacity()) { // Check against capacity
        return {0};
    }
    return CD3DX12_GPU_DESCRIPTOR_HANDLE(heap->getGPUHeapStart(), index, heap->getDescriptorSize());
}

bool Renderer::CreateDXRResources() {
    ID3D12Device* device = m_device->getDevice();
    if (!device || !m_srvHeap) {
        return false;
    }

    if (!m_srvHeap->allocateDescriptor(m_dxrOutputUavCpuHandle, m_dxrOutputUavGpuHandle)) {
        return false;
    }

    DXGI_FORMAT format = m_swapChain->getFormat();
    UINT width = m_swapChain->getWidth();
    UINT height = m_swapChain->getHeight();

    D3D12_RESOURCE_DESC texDesc = CD3DX12_RESOURCE_DESC::Tex2D(
        format, width, height, 1, 1, 1, 0,
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS // IMPORTANT: Need UAV flag
    );

    auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = device->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &texDesc,
        D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr,
        IID_PPV_ARGS(&m_dxrOutputTexture));

    if (FAILED(hr)) {
        OutputDebugStringW(L"Error: Failed to create DXR Output Texture.\n");
        return false;
    }
    m_device->getDevice()->SetName(L"DXR Output Texture");

    // 2. Allocate descriptor and create UAV
    // Find the next available slot in the heap (after Light CBVs, Mat CBV, Tex SRV)
    UINT uavDescriptorIndex = m_numFramesInFlight + 1 + 1; // CBVs + MatCBV + TexSRV
    if (!m_srvHeap->allocateDescriptor(m_dxrOutputUavCpuHandle, m_dxrOutputUavGpuHandle)) { // Store handles
        OutputDebugStringW(L"Error: Failed to allocate UAV descriptor for DXR Output.\n");
        return false;
    }
    // Ensure calculation is correct if AllocateDescriptor doesn't return handles reliably
    // m_dxrOutputUavCpuHandle = CD3DX12_CPU_DESCRIPTOR_HANDLE(m_srvCbvHeap->GetHeapPointer()->GetCPUDescriptorHandleForHeapStart(), uavDescriptorIndex, m_srvCbvHeap->GetDescriptorSize());
    // m_dxrOutputUavGpuHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(m_srvCbvHeap->GetHeapPointer()->GetGPUDescriptorHandleForHeapStart(), uavDescriptorIndex, m_srvCbvHeap->GetDescriptorSize());

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = format;
    uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    uavDesc.Texture2D.MipSlice = 0;
    uavDesc.Texture2D.PlaneSlice = 0;

    device->CreateUnorderedAccessView(m_dxrOutputTexture.Get(), nullptr, &uavDesc, m_dxrOutputUavCpuHandle);

    return true;
}

bool Renderer::CreateDXRRootSignature() {
    ID3D12Device* device = m_device->getDevice();
    // Param 0: Output UAV Table (u0)
    // Param 1: TLAS SRV (t0) - Root Descriptor
    // Param 2: Camera CBV Table (b1)
    // Param 3: Texture SRV Table (t1)
    // Param 4: VB SRV Table (t2)
    // Param 5: IB SRV Table (t3)
    // Param 6: DXR Object CBV Table (b2)  <--- NEW
    CD3DX12_DESCRIPTOR_RANGE1 ranges[7];
    ranges[0].Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0); // Output UAV @ u0
    ranges[1].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1); // Camera CBV @ b1
    ranges[2].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // Texture SRV @ t1
    ranges[3].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2); // VB SRV @ t2
    ranges[4].Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3); // IB SRV @ t3
    ranges[5].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 2); // DXR Object CBV @ b2
    ranges[6].Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 3);

    CD3DX12_DESCRIPTOR_RANGE1 uavRange;
    uavRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0); // u0
    CD3DX12_DESCRIPTOR_RANGE1 camCbRange;
    camCbRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 1); // b1
    CD3DX12_DESCRIPTOR_RANGE1 texSrRange;
    texSrRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 1); // t1
    CD3DX12_DESCRIPTOR_RANGE1 vbSrRange;
    vbSrRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 2); // t2
    CD3DX12_DESCRIPTOR_RANGE1 ibSrRange;
    ibSrRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 3); // t3
    CD3DX12_DESCRIPTOR_RANGE1 objCbRange;
    objCbRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 2); // b2
    CD3DX12_DESCRIPTOR_RANGE1 lightCbRange;
    lightCbRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 3); // b3
    CD3DX12_DESCRIPTOR_RANGE1 matCbRange;
    matCbRange.Init(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, 1, 4); // b4


    CD3DX12_ROOT_PARAMETER1 rootParameters[9]; // 9 Parameters total
    rootParameters[0].InitAsDescriptorTable(1, &uavRange); // Output UAV
    rootParameters[1].InitAsShaderResourceView(0); // TLAS @ t0
    rootParameters[2].InitAsDescriptorTable(1, &camCbRange); // Camera CBV
    rootParameters[3].InitAsDescriptorTable(1, &texSrRange); // Texture SRV
    rootParameters[4].InitAsDescriptorTable(1, &vbSrRange); // VB SRV
    rootParameters[5].InitAsDescriptorTable(1, &ibSrRange); // IB SRV
    rootParameters[6].InitAsDescriptorTable(1, &objCbRange); // DXR Object CBV
    rootParameters[7].InitAsDescriptorTable(1, &lightCbRange); // DXR Light CBV
    rootParameters[8].InitAsDescriptorTable(1, &matCbRange); // DXR Material CBV

    CD3DX12_STATIC_SAMPLER_DESC staticSampler(
        0, // shaderRegister (s0)
        D3D12_FILTER_MIN_MAG_MIP_LINEAR,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP,
        D3D12_TEXTURE_ADDRESS_MODE_WRAP);

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSignatureDesc;
    // ***** MODIFIED ***** Initialize with 1 static sampler
    rootSignatureDesc.Init_1_1(
        _countof(rootParameters), rootParameters,
        1, &staticSampler, // Provide the static sampler
        D3D12_ROOT_SIGNATURE_FLAG_NONE); // No other flags needed


    // Serialize and Create
    ComPtr<ID3DBlob> signatureBlob, errorBlob;
    HRESULT hr = D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_1,
                                                       &signatureBlob, &errorBlob);
    if (FAILED(hr)) {
        return false;
    }
    hr = device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_dxrRootSignature));
    if (FAILED(hr)) {
        return false;
    }
    m_dxrRootSignature->SetName(L"DXR Global Root Signature");

    return true;
}

bool Renderer::CreateDXRStateObject(const std::wstring& shaderPath) {
    ID3D12Device5* dxrDevice = nullptr;
    m_device->getDevice()->QueryInterface(IID_PPV_ARGS(&dxrDevice));
    if (!dxrDevice) return false;

    // --- Use DXC Compiler ---
    ComPtr<IDxcUtils> dxcUtils;
    ComPtr<IDxcCompiler3> dxcCompiler;
    ComPtr<IDxcLibrary> dxcLibrary;
    HRESULT hr = DxcCreateInstance(CLSID_DxcUtils, IID_PPV_ARGS(&dxcUtils));
    if FAILED(hr) {
        OutputDebugStringW(L"Failed to create DXC Utils.\n");
        dxrDevice->Release();
        return false;
    }
    hr = DxcCreateInstance(CLSID_DxcCompiler, IID_PPV_ARGS(&dxcCompiler));
    if FAILED(hr) {
        OutputDebugStringW(L"Failed to create DXC Compiler.\n");
        dxrDevice->Release();
        return false;
    }
    hr = DxcCreateInstance(CLSID_DxcLibrary, IID_PPV_ARGS(&dxcLibrary));
    if FAILED(hr) {
        OutputDebugStringW(L"Failed to create DXC Library.\n");
        dxrDevice->Release();
        return false;
    }


    // Create default include handler
    ComPtr<IDxcIncludeHandler> dxcIncludeHandler;
    hr = dxcUtils->CreateDefaultIncludeHandler(&dxcIncludeHandler);
    if FAILED(hr) {
        OutputDebugStringW(L"Failed to create DXC Include Handler.\n");
        dxrDevice->Release();
        return false;
    }


    // Load the shader source file
    ComPtr<IDxcBlobEncoding> sourceBlob;
    hr = dxcUtils->LoadFile(shaderPath.c_str(), nullptr, &sourceBlob);
    if (FAILED(hr)) {
        OutputDebugStringW((L"Error: Failed to load DXR shader file: " + shaderPath + L"\n").c_str());
        dxrDevice->Release();
        return false;
    }

    DxcBuffer sourceBuffer;
    sourceBuffer.Ptr = sourceBlob->GetBufferPointer();
    sourceBuffer.Size = sourceBlob->GetBufferSize();
    sourceBuffer.Encoding = DXC_CP_ACP;

    // Compile the shader library
    LPCWSTR args[] = {L"-E", L"", L"-T", L"lib_6_3", DXC_ARG_DEBUG, DXC_ARG_SKIP_OPTIMIZATIONS};
    ComPtr<IDxcResult> compileResult;
    hr = dxcCompiler->Compile(&sourceBuffer, args, _countof(args), dxcIncludeHandler.Get(),
                              IID_PPV_ARGS(&compileResult));

    // --- Enhanced Error Handling ---
    bool compilationFailed = FAILED(hr); // Check initial Compile call result
    HRESULT compileStatus = E_FAIL; // Assume failure initially
    std::string dxcErrors = "";

    if (SUCCEEDED(hr) && compileResult) { // Check if we got a result object
        compileResult->GetStatus(&compileStatus); // Get the compilation status
        compilationFailed = FAILED(compileStatus); // Update failure status

        ComPtr<IDxcBlobUtf8> errorsBlob;
        if (SUCCEEDED(compileResult->GetOutput(DXC_OUT_ERRORS, IID_PPV_ARGS(&errorsBlob), nullptr))) {
            if (errorsBlob && errorsBlob->GetStringLength() > 0) {
                dxcErrors = errorsBlob->GetStringPointer();
                OutputDebugStringA("DXC Compilation Errors/Warnings:\n");
                OutputDebugStringA(dxcErrors.c_str());
                OutputDebugStringA("\n");
            }
        } else {
            OutputDebugStringW(L"Warning: Failed to get DXC compilation errors blob.\n");
        }
    } else if (FAILED(hr)) {
        OutputDebugStringW(L"Error: dxcCompiler->Compile() call failed directly.\n");
    }

    if (compilationFailed) {
        OutputDebugStringW(L"Error: DXC Compilation Failed.\n");
        // Display error message box
        std::wstring errorMsg = L"DXC Shader Compilation Failed for: " + shaderPath +
                                L"\n\nCheck Debug Output for details.";
        if (!dxcErrors.empty()) {
            // Convert narrow string error to wide string for MessageBoxW
            std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>> converter;
            errorMsg += L"\n\nErrors:\n" + converter.from_bytes(dxcErrors);
        }
        MessageBoxW(nullptr, errorMsg.c_str(), L"Shader Error", MB_OK | MB_ICONERROR);
        dxrDevice->Release();
        return false;
    }
    // --- End Enhanced Error Handling ---

    // Get the compiled DXIL blob
    ComPtr<IDxcBlob> dxilBlob;
    hr = compileResult->GetOutput(DXC_OUT_OBJECT, IID_PPV_ARGS(&dxilBlob), nullptr);
    if (FAILED(hr) || !dxilBlob) { /* ... error ... */
        dxrDevice->Release();
        return false;
    }

    // --- Build the State Object ---
    CD3DX12_STATE_OBJECT_DESC rtPipeline{D3D12_STATE_OBJECT_TYPE_RAYTRACING_PIPELINE};

    // 1. DXIL Library Subobject
    auto dxilLib = rtPipeline.CreateSubobject<CD3DX12_DXIL_LIBRARY_SUBOBJECT>();
    D3D12_SHADER_BYTECODE dxilLibBytecode = CD3DX12_SHADER_BYTECODE();
    dxilLibBytecode.BytecodeLength = dxilBlob->GetBufferSize();
    dxilLibBytecode.pShaderBytecode = dxilBlob->GetBufferPointer();
    // Set the DXIL bytecode
    dxilLib->SetDXILLibrary(&dxilLibBytecode);
    // Define exports - ENSURE THESE MATCH HLSL EXACTLY (CASE-SENSITIVE)
    dxilLib->DefineExport(L"RayGen");
    dxilLib->DefineExport(L"Miss");
    dxilLib->DefineExport(L"ClosestHit");

    // 2. Hit Group Subobject
    auto hitGroup = rtPipeline.CreateSubobject<CD3DX12_HIT_GROUP_SUBOBJECT>();
    hitGroup->SetHitGroupExport(L"HitGroup"); // Name used in SBT
    hitGroup->SetHitGroupType(D3D12_HIT_GROUP_TYPE_TRIANGLES);
    hitGroup->SetClosestHitShaderImport(L"ClosestHit"); // Link to CHS export

    // 3. Shader Config Subobject
    auto shaderConfig = rtPipeline.CreateSubobject<CD3DX12_RAYTRACING_SHADER_CONFIG_SUBOBJECT>();
    UINT payloadSize = sizeof(float) * 4;
    UINT attributeSize = sizeof(float) * 2;
    shaderConfig->Config(payloadSize, attributeSize);

    // 4. Global Root Signature Subobject
    auto globalRootSig = rtPipeline.CreateSubobject<CD3DX12_GLOBAL_ROOT_SIGNATURE_SUBOBJECT>();
    globalRootSig->SetRootSignature(m_dxrRootSignature.Get());

    // 5. Pipeline Config Subobject
    auto pipelineConfig = rtPipeline.CreateSubobject<CD3DX12_RAYTRACING_PIPELINE_CONFIG_SUBOBJECT>();
    UINT maxRecursionDepth = 1;
    pipelineConfig->Config(maxRecursionDepth);

    // 6. Create the State Object
    hr = dxrDevice->CreateStateObject(rtPipeline, IID_PPV_ARGS(&m_dxrStateObject));
    if (FAILED(hr)) {
        OutputDebugStringW(L"Error: Failed to create DXR State Object (RTPSO). HRESULT: ");
        OutputDebugStringW(std::to_wstring(hr).c_str());
        OutputDebugStringW(L"\n");
        // Check debug layer output for more specific reasons!
        dxrDevice->Release();
        return false;
    }
    m_dxrStateObject->SetName(L"DXR State Object");

    dxrDevice->Release();
    return true;
}

bool Renderer::BuildShaderBindingTable() {
    ID3D12Device* device = m_device->getDevice();
    if (!m_dxrStateObject) {
        return false;
    }

    ComPtr<ID3D12StateObjectProperties> stateObjectProps;
    HRESULT hr = m_dxrStateObject.As(&stateObjectProps);
    if (FAILED(hr)) {
        return false;
    }

    void* rayGenId = stateObjectProps->GetShaderIdentifier(L"RayGen");
    void* missId = stateObjectProps->GetShaderIdentifier(L"Miss");
    void* hitGroupId = stateObjectProps->GetShaderIdentifier(L"HitGroup");
    if (!rayGenId || !missId || !hitGroupId) {
        OutputDebugStringW(L"Error: Failed to get shader identifiers from RTPSO.\n");
        return false;
    }

    UINT shaderIdSize = D3D12_SHADER_IDENTIFIER_SIZE_IN_BYTES;

    m_sbtEntrySize = AlignUp(shaderIdSize, D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT);
    if (m_sbtEntrySize == 0) {
        return false;
    }

    UINT tableAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT; // 64 bytes
    UINT64 rayGenTableStart = 0; // Assumes SBT buffer starts aligned
    UINT64 missTableStart = AlignUp(rayGenTableStart + m_sbtEntrySize, tableAlignment);
    UINT64 hitGroupTableStart = AlignUp(missTableStart + m_sbtEntrySize, tableAlignment);
    // Total size needed is the start of the last table + size of its entry
    UINT sbtSize = static_cast<UINT>(hitGroupTableStart + m_sbtEntrySize);
    sbtSize = AlignUp(sbtSize, tableAlignment);


    if (sbtSize == 0) {
        OutputDebugStringW(L"Error: Calculated total SBT size is zero.\n");
        return false;
    }
    OutputDebugStringW((L"Calculated SBT Size: " + std::to_wstring(sbtSize) + L"\n").c_str());
    OutputDebugStringW(
        (L"  RayGen Start: 0, Miss Start: " + std::to_wstring(missTableStart) + L", HitGroup Start: " +
         std::to_wstring(hitGroupTableStart) + L"\n").c_str());

    m_shaderBindingTable.Reset();
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto sbtBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(sbtSize);
    hr = device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &sbtBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
        IID_PPV_ARGS(&m_shaderBindingTable));
    if (FAILED(hr)) {
        OutputDebugStringW(L"Error: Failed to create Shader Binding Table buffer.\n");
        return false;
    }
    m_shaderBindingTable->SetName(L"Shader Binding Table");
    // Map the buffer and copy shader identifiers
    UINT8* pSBT = nullptr;
    hr = m_shaderBindingTable->Map(0, nullptr, reinterpret_cast<void**>(&pSBT));
    if (FAILED(hr)) return false;

    // Copy RayGen ID (Offset 0)
    memcpy(pSBT + rayGenTableStart, rayGenId, shaderIdSize);

    // Copy Miss ID (Offset by 1 entry size)
    memcpy(pSBT + missTableStart, missId, shaderIdSize);

    // Copy HitGroup ID (Offset by 2 entry sizes)
    memcpy(pSBT + hitGroupTableStart, hitGroupId, shaderIdSize);

    // Unmap the buffer
    m_shaderBindingTable->Unmap(0, nullptr);

    return true;
}

bool Renderer::buildAccelerationStructures(Mesh* mesh) {
    if (!m_rayTracingSupported || !mesh) {
        return false;
    }
    ID3D12Device5* device = nullptr;
    HRESULT hr = m_device->getDevice()->QueryInterface(IID_PPV_ARGS(&device));
    if (FAILED(hr)) {
        return false;
    }

    waitForGpu();
    if (!m_commandManager->resetAllocator(m_currentFrameIndex)) {
        OutputDebugStringW(L"Failed to reset allocator for AS build.\n");
        device->Release();
        return false;
    }
    if (!m_commandManager->resetCommandList(m_currentFrameIndex)) { // Reset without PSO
        OutputDebugStringW(L"Failed to reset command list for AS build.\n");
        device->Release();
        return false;
    }

    ID3D12GraphicsCommandList5* commandList = nullptr;
    m_commandManager->getCommandList()->QueryInterface(IID_PPV_ARGS(&commandList));
    if (!commandList) {
        device->Release();
        return false;
    }

    bool success = true;
    if (!buildBLAS(mesh, commandList)) {
        success = false;
    }
    if (success && !buildTLAS(commandList)) {
        success = false;
    }

    hr = commandList->Close();
    if (FAILED(hr)) {
        success = false;
    }

    if (success) {
        ID3D12CommandList* const ppCommandLists[] = {commandList};
        m_commandQueue->executeCommandLists(1, ppCommandLists);
        m_commandQueue->join();
        if (!createMeshBufferSRVs(mesh)) {
            OutputDebugStringW(L"Failed to create Mesh Buffer SRVs during AS build phase.\n");
            success = false; // Or handle differently
        }
    }

    commandList->Release();
    return success;
}

bool Renderer::buildBLAS(Mesh* mesh, ID3D12GraphicsCommandList5* commandList) {
    ID3D12Device5* device = nullptr;
    m_device->getDevice()->QueryInterface(IID_PPV_ARGS(&device));
    if (!device) {
        return false;
    }

    D3D12_RAYTRACING_GEOMETRY_DESC geometryDesc = {};
    geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
    geometryDesc.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
    geometryDesc.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE; // Assume opaque for now
    geometryDesc.Triangles.Transform3x4 = 0; // No local transform within BLAS
    geometryDesc.Triangles.IndexFormat = mesh->getIndexFormat();
    geometryDesc.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT; // Assuming position is float3
    geometryDesc.Triangles.IndexCount = mesh->getIndexCount();
    geometryDesc.Triangles.VertexCount = mesh->getVertexCount();
    geometryDesc.Triangles.IndexBuffer = mesh->getIndexBufferGPUVirtualAddress();
    geometryDesc.Triangles.VertexBuffer.StartAddress = mesh->getVertexBufferGPUVirtualAddress();
    geometryDesc.Triangles.VertexBuffer.StrideInBytes = mesh->getVertexStride();

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS blasInputs = {};
    blasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
    blasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE;
    blasInputs.NumDescs = 1;
    blasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    blasInputs.pGeometryDescs = &geometryDesc;

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO blasPrebuildInfo = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&blasInputs, &blasPrebuildInfo);
    if (blasPrebuildInfo.ResultDataMaxSizeInBytes == 0) {
        device->Release();
        return false; // Error or empty geometry
    }
    m_blasBuffers.scratch.Reset();
    m_blasBuffers.result.Reset();
    auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    auto uavBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(blasPrebuildInfo.ScratchDataSizeInBytes,
                                                       D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    HRESULT hr = device->CreateCommittedResource( // Use dxrDevice for consistency
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &uavBufferDesc,
        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&m_blasBuffers.scratch));
    if (FAILED(hr)) {
        device->Release();
        return false;
    }
    m_blasBuffers.scratch->SetName(L"BLAS Scratch Buffer");

    uavBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(blasPrebuildInfo.ResultDataMaxSizeInBytes,
                                                  D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    hr = device->CreateCommittedResource(
        &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &uavBufferDesc,
        D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr, IID_PPV_ARGS(&m_blasBuffers.result));
    if (FAILED(hr)) {
        device->Release();
        return false;
    }
    m_blasBuffers.result->SetName(L"BLAS Result Buffer");

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC blasDesc = {};
    blasDesc.Inputs = blasInputs;
    blasDesc.ScratchAccelerationStructureData = m_blasBuffers.scratch->GetGPUVirtualAddress();
    blasDesc.DestAccelerationStructureData = m_blasBuffers.result->GetGPUVirtualAddress();

    commandList->BuildRaytracingAccelerationStructure(&blasDesc, 0, nullptr); // No update or instance data for BLAS

    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_blasBuffers.result.Get());
    commandList->ResourceBarrier(1, &uavBarrier);
    device->Release();
    return true;
}

bool Renderer::buildTLAS(ID3D12GraphicsCommandList5* commandList) {
    if (!m_blasBuffers.result) {
        return false;
    }

    ID3D12Device5* device = nullptr;
    m_device->getDevice()->QueryInterface(IID_PPV_ARGS(&device));
    if (!device) {
        return false;
    }

    D3D12_RAYTRACING_INSTANCE_DESC tlasInstanceDesc = {};
    glm::mat4 transposedWorld = glm::transpose(glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.0f, 0.0f)));
    memcpy(tlasInstanceDesc.Transform, glm::value_ptr(transposedWorld), sizeof(tlasInstanceDesc.Transform));
    tlasInstanceDesc.InstanceID = 0;
    tlasInstanceDesc.InstanceMask = 1;
    tlasInstanceDesc.AccelerationStructure = m_blasBuffers.result->GetGPUVirtualAddress();
    tlasInstanceDesc.Flags = D3D12_RAYTRACING_INSTANCE_FLAG_NONE; // Disable culling for now

    /*
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto instanceDescBufferSize = sizeof(tlasInstanceDesc);
    auto instanceDescBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(instanceDescBufferSize);
    HRESULT hr = device->CreateCommittedResource(
        &uploadHeapProps, D3D12_HEAP_FLAG_NONE, &instanceDescBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m_tlasBuffers.instanceDesc));

    if (FAILED(hr)) {
        device->Release();
        return false;
    }
    */
    m_tlasBuffers.instanceDesc->SetName(L"TLAS Instance Buffer");

    void* mappedData = nullptr;
    HRESULT hr = m_tlasBuffers.instanceDesc->Map(0, nullptr, &mappedData);
    if (FAILED(hr)) {
        device->Release();
        return false;
    }
    memcpy(mappedData, &tlasInstanceDesc, sizeof(tlasInstanceDesc));
    m_tlasBuffers.instanceDesc->Unmap(0, nullptr);

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS tlasInputs = {};
    tlasInputs.Type = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL;
    tlasInputs.Flags = D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PREFER_FAST_TRACE |
                       D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_ALLOW_UPDATE;
    bool performUpdate = m_tlasBuffers.result != nullptr;

    if (performUpdate) { // If we have a previous TLAS, it's an update
        tlasInputs.Flags |= D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAG_PERFORM_UPDATE;
    }
    tlasInputs.NumDescs = 1;
    tlasInputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;
    tlasInputs.InstanceDescs = m_tlasBuffers.instanceDesc->GetGPUVirtualAddress();

    D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO tlasPrebuildInfo = {};
    device->GetRaytracingAccelerationStructurePrebuildInfo(&tlasInputs, &tlasPrebuildInfo);
    if (tlasPrebuildInfo.ResultDataMaxSizeInBytes == 0) {
        device->Release();
        return false; // Error or empty geometry
    }

    if (!m_tlasBuffers.scratch || m_tlasBuffers.scratch->GetDesc().Width < tlasPrebuildInfo.ScratchDataSizeInBytes) {
        auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto uavBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(tlasPrebuildInfo.ScratchDataSizeInBytes,
                                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = device->CreateCommittedResource(
            &defaultHeapProps, D3D12_HEAP_FLAG_NONE, &uavBufferDesc,
            D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&m_tlasBuffers.scratch));
        if (FAILED(hr)) {
            device->Release();
            return false;
        }
        m_tlasBuffers.scratch->SetName(L"TLAS Scratch Buffer");
    }

    ComPtr<ID3D12Resource> previousTLASResult = m_tlasBuffers.result;
    if (!performUpdate || !m_tlasBuffers.result ||
        m_tlasBuffers.result->GetDesc().Width < tlasPrebuildInfo.ResultDataMaxSizeInBytes) {
        m_tlasBuffers.result.Reset(); // Release if it's too small or doesn't exist
        auto defaultHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
        auto uavBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(tlasPrebuildInfo.ResultDataMaxSizeInBytes,
                                                           D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        hr = device->CreateCommittedResource(&defaultHeapProps, D3D12_HEAP_FLAG_NONE, &uavBufferDesc,
                                             D3D12_RESOURCE_STATE_RAYTRACING_ACCELERATION_STRUCTURE, nullptr,
                                             IID_PPV_ARGS(&m_tlasBuffers.result));
        if (FAILED(hr)) {
            device->Release();
            return false;
        }
        m_tlasBuffers.result->SetName(L"TLAS Result Buffer");
        performUpdate = false; // It's now an initial build into the new buffer
    }

    D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_DESC tlasBuildDesc = {};
    tlasBuildDesc.Inputs = tlasInputs;
    tlasBuildDesc.ScratchAccelerationStructureData = m_tlasBuffers.scratch->GetGPUVirtualAddress();
    tlasBuildDesc.DestAccelerationStructureData = m_tlasBuffers.result->GetGPUVirtualAddress();

    if (performUpdate) {
        tlasBuildDesc.SourceAccelerationStructureData = m_tlasBuffers.result->GetGPUVirtualAddress();
    } else {
        tlasBuildDesc.SourceAccelerationStructureData = 0; // No source for initial build
    }

    commandList->BuildRaytracingAccelerationStructure(&tlasBuildDesc, 0, nullptr);
    // No update or instance data for TLAS
    auto uavBarrier = CD3DX12_RESOURCE_BARRIER::UAV(m_tlasBuffers.result.Get());
    commandList->ResourceBarrier(1, &uavBarrier);
    device->Release();
    return true;
}

bool Renderer::createMeshBufferSRVs(Mesh* mesh) {
    ID3D12Device* device = m_device->getDevice();
    if (!mesh || !m_srvHeap || !mesh->getVertexBufferResource() || !mesh->getIndexBufferResource()) {
        OutputDebugStringW(L"Error: Missing prerequisites for CreateMeshBufferSRVs.\n");
        return false;
    }

    // --- Heap Layout Plan (Indices) ---
    // 0 to k-1: Per-Frame Light CBVs (Raster)
    // k:        Material CBV (Raster)
    // k+1:      DXR Camera CBV
    // k+2:      Texture SRV
    // k+3:      Mesh VB SRV  <--- Creating this
    // k+4:      Mesh IB SRV  <--- Creating this
    // k+5:      DXR Output UAV
    UINT vbSrvIndex = m_numFramesInFlight + 1 + 1; // After Light CBVs, Mat CBV, DXR Cam CBV
    UINT ibSrvIndex = vbSrvIndex + 1;

    D3D12_CPU_DESCRIPTOR_HANDLE cpuHandle = {0};
    D3D12_GPU_DESCRIPTOR_HANDLE gpuHandle = {0}; // Needed by AllocateDescriptor

    // 1. Create Vertex Buffer SRV
    if (!m_srvHeap->allocateDescriptor(cpuHandle, gpuHandle)) return false; // Allocate slot k+3
    m_meshVertexBufferSrvHandleGPU = gpuHandle; // Store GPU handle
    D3D12_SHADER_RESOURCE_VIEW_DESC vbSrvDesc = {};
    vbSrvDesc.Format = DXGI_FORMAT_UNKNOWN; // Structured buffer
    vbSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    vbSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    vbSrvDesc.Buffer.FirstElement = 0;
    vbSrvDesc.Buffer.NumElements = mesh->getVertexCount();
    vbSrvDesc.Buffer.StructureByteStride = mesh->getVertexStride();
    vbSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
    device->CreateShaderResourceView(mesh->getVertexBufferResource(), &vbSrvDesc, cpuHandle);

    // 2. Create Index Buffer SRV
    if (!m_srvHeap->allocateDescriptor(cpuHandle, gpuHandle)) {
        OutputDebugStringW(L"Error: Failed to allocate IB SRV descriptor.\n");
        return false;
    }
    m_meshIndexBufferSrvHandleGPU = gpuHandle; // Store GPU handle
    D3D12_SHADER_RESOURCE_VIEW_DESC ibSrvDesc = {};
    ibSrvDesc.Format = DXGI_FORMAT_R32_TYPELESS; // For ByteAddressBuffer
    ibSrvDesc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
    ibSrvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    ibSrvDesc.Buffer.FirstElement = 0;
    ibSrvDesc.Buffer.NumElements = mesh->getIndexCount(); // Number of indices
    ibSrvDesc.Buffer.StructureByteStride = 0; // Not structured
    ibSrvDesc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
    device->CreateShaderResourceView(mesh->getIndexBufferResource(), &ibSrvDesc, cpuHandle);

    return true;
}


void Renderer::renderRaster(ID3D12GraphicsCommandList* commandList,
                            Mesh* mesh,
                            Texture* texture) {
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_swapChain->getCurrentBackBufferView();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHandleCPU;
    commandList->SetGraphicsRootSignature(m_rasterRootSignature->getSignature());
    commandList->SetPipelineState(m_rasterPipelineState->getPipeline());
    commandList->RSSetViewports(1, &m_viewport);
    commandList->RSSetScissorRects(1, &m_scissorRect);
    commandList->OMSetRenderTargets(1, &rtvHandle, FALSE, &dsvHandle);

    // Clear Targets
    const float clearColor[] = {0.1f, 0.1f, 0.1f, 1.0f};
    commandList->ClearRenderTargetView(rtvHandle, clearColor, 0, nullptr);
    commandList->ClearDepthStencilView(dsvHandle, D3D12_CLEAR_FLAG_DEPTH, 1.0f, 0, 0, nullptr); //

    // Set IA Buffers using Mesh class
    if (mesh) {
        mesh->setupInputAssembler(commandList);
    }
    // Set Root Arguments
    // Param 0: Root CBV (Object Data)
    D3D12_GPU_VIRTUAL_ADDRESS cbGpuAddress = m_perFrameObjectCBs[m_currentFrameIndex]->getGPUVirtualAddress();
    commandList->SetGraphicsRootConstantBufferView(0, cbGpuAddress); // Offset 0 for the single object

    // Param 1: Texture SRV Table
    if (texture && texture->getResource()) {
        texture->TransitionToState(commandList, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        if (texture->getSRVGPUHandle().ptr != 0) {
            commandList->SetGraphicsRootDescriptorTable(1, texture->getSRVGPUHandle()); // Texture SRV
        }
    }

    D3D12_GPU_DESCRIPTOR_HANDLE lightCbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_srvHeap->getGPUHeapStart(), m_currentFrameIndex,
        m_srvHeap->getDescriptorSize());
    if (lightCbvHandle.ptr != 0) {
        commandList->SetGraphicsRootDescriptorTable(2, lightCbvHandle);
    }
    UINT materialCbvIndex = SwapChain::kBackBufferCount; // Material CBV is after frame light CBVs
    D3D12_GPU_DESCRIPTOR_HANDLE materialCbvHandle = CD3DX12_GPU_DESCRIPTOR_HANDLE(
        m_srvHeap->getGPUHeapStart(), materialCbvIndex,
        m_srvHeap->getDescriptorSize());
    if (materialCbvHandle.ptr != 0) {
        commandList->SetGraphicsRootDescriptorTable(3, materialCbvHandle);
    }


    for (UINT i = 0; i < 1; ++i) {
        D3D12_GPU_VIRTUAL_ADDRESS objectCbAddress =
                cbGpuAddress + i * ((sizeof(ObjectConstant) + D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1) &
                                    ~(D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT - 1));

        commandList->SetGraphicsRootConstantBufferView(0, objectCbAddress);

        // Draw using Mesh class
        if (mesh) {
            mesh->draw(commandList, 1); // Draw 1 instance
        }
    }
}

void Renderer::renderRaytraced(Camera* camera, Mesh* mesh, Texture* texture, ID3D12GraphicsCommandList5* commandList) {
    bool resourcesReady = true;
    if (!m_dxrStateObject) {
        OutputDebugStringW(L"DXR Error: m_dxrStateObject is null.\n");
        resourcesReady = false;
    }
    if (!m_shaderBindingTable) {
        OutputDebugStringW(L"DXR Error: m_shaderBindingTable is null.\n");
        resourcesReady = false;
    }
    if (!m_tlasBuffers.result) { // Check the TLAS result buffer
        OutputDebugStringW(L"DXR Error: m_tlas.pResult (TLAS result buffer) is null.\n");
        resourcesReady = false;
    }
    if (!m_dxrOutputTexture) {
        OutputDebugStringW(L"DXR Error: m_dxrOutputTexture is null.\n");
        resourcesReady = false;
    }
    if (m_dxrOutputUavGpuHandle.ptr == 0) { // Check the UAV handle for the output texture
        OutputDebugStringW(L"DXR Error: m_dxrOutputUavGpuHandle is null.\n");
        resourcesReady = false;
    }
    if (m_dxrCameraCbvHandleGPU.ptr == 0) {
        OutputDebugStringW(L"DXR Error: m_dxrCameraCbvHandleGPU is null.\n");
        resourcesReady = false;
    }
    if (m_dxrObjectCbvHandleGPU.ptr == 0) {
        OutputDebugStringW(L"DXR Error: m_dxrObjectCbvHandleGPU is null.\n");
        resourcesReady = false;
    }
    if (!texture || texture->getSRVGPUHandle().ptr == 0) { // Check pTexture itself first
        OutputDebugStringW(L"DXR Error: pTexture is null or its SRV GPU handle is null.\n");
        resourcesReady = false;
    }
    if (m_meshVertexBufferSrvHandleGPU.ptr == 0) {
        OutputDebugStringW(L"DXR Error: m_meshVertexBufferSrvHandleGPU is null.\n");
        resourcesReady = false;
    }
    if (m_meshIndexBufferSrvHandleGPU.ptr == 0) {
        OutputDebugStringW(L"DXR Error: m_meshIndexBufferSrvHandleGPU is null.\n");
        resourcesReady = false;
    }


    if (!resourcesReady) {
        OutputDebugStringW(L"Warning: DXR resources not ready, falling back to clear.\n");
        D3D12_CPU_DESCRIPTOR_HANDLE currentRtv = m_swapChain->getCurrentBackBufferView();
        const float clearColor[] = {0.4f, 0.1f, 0.4f, 1.0f}; // Purple for error/not ready
        commandList->ClearRenderTargetView(currentRtv, clearColor, 0, nullptr);
        return;
    }

    if (m_sbtEntrySize == 0) {
        OutputDebugStringW(L"Error: SBT Entry Size is zero in RenderRaytraced.\n");
        return; // Cannot proceed
    }
    UINT64 sbtBase = m_shaderBindingTable->GetGPUVirtualAddress();
    if (sbtBase == 0) {
        OutputDebugStringW(L"Error: SBT Base GPU Virtual Address is zero in RenderRaytraced.\n");
        return; // Cannot proceed
    }

    if (!buildTLAS(commandList)) {
        OutputDebugStringW(L"Error: Failed to build TLAS in RenderRaytraced.\n");
        return; // Cannot proceed
    }

    // Transition DXR output texture to UNORDERED_ACCESS
    auto barrierToUAV = CD3DX12_RESOURCE_BARRIER::Transition(
        m_dxrOutputTexture.Get(),
        D3D12_RESOURCE_STATE_COPY_SOURCE, // Ensure the correct "before" state
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS
    );
    if (texture) {
        texture->TransitionToState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    commandList->ResourceBarrier(1, &barrierToUAV);

    commandList->SetPipelineState1(m_dxrStateObject.Get());
    commandList->SetComputeRootSignature(m_dxrRootSignature.Get());

    commandList->SetComputeRootDescriptorTable(0, m_dxrOutputUavGpuHandle);
    commandList->SetComputeRootShaderResourceView(1, m_tlasBuffers.result->GetGPUVirtualAddress());

    commandList->SetComputeRootDescriptorTable(0, m_dxrOutputUavGpuHandle); // Param 0: Output UAV Table (u0)
    commandList->SetComputeRootShaderResourceView(1, m_tlasBuffers.result->GetGPUVirtualAddress());
    // Param 1: TLAS SRV (t0)
    commandList->SetComputeRootDescriptorTable(2, m_dxrCameraCbvHandleGPU); // Param 2: Camera CBV Table (b1)
    commandList->SetComputeRootDescriptorTable(3, texture->getSRVGPUHandle()); // Param 3: Texture SRV Table (t1)
    commandList->SetComputeRootDescriptorTable(4, m_meshVertexBufferSrvHandleGPU); // Param 4: VB SRV Table (t2)
    commandList->SetComputeRootDescriptorTable(5, m_meshIndexBufferSrvHandleGPU); // Param 5: IB SRV Table (t3)
    commandList->SetComputeRootDescriptorTable(6, m_dxrObjectCbvHandleGPU); // Param 5: IB SRV Table (t3)
    commandList->SetComputeRootDescriptorTable(7, m_dxrLightCbvHandleGPU); // Param 7: DXR Light CBV (b3)
    commandList->SetComputeRootDescriptorTable(8, m_dxrMaterialCbvHandleGPU); // Param 8: DXR Material CBV (b4)


    D3D12_DISPATCH_RAYS_DESC rayDesc = {};
    UINT recordAlignment = D3D12_RAYTRACING_SHADER_RECORD_BYTE_ALIGNMENT;
    UINT tableAlignment = D3D12_RAYTRACING_SHADER_TABLE_BYTE_ALIGNMENT;
    rayDesc.RayGenerationShaderRecord.StartAddress = sbtBase;
    rayDesc.RayGenerationShaderRecord.SizeInBytes = m_sbtEntrySize;
    rayDesc.MissShaderTable.StartAddress = AlignUp(sbtBase + m_sbtEntrySize, tableAlignment);
    rayDesc.MissShaderTable.SizeInBytes = m_sbtEntrySize;
    rayDesc.MissShaderTable.StrideInBytes = m_sbtEntrySize;
    rayDesc.HitGroupTable.StartAddress = AlignUp(
        rayDesc.MissShaderTable.StartAddress + rayDesc.MissShaderTable.SizeInBytes, tableAlignment);
    rayDesc.HitGroupTable.SizeInBytes = m_sbtEntrySize;
    rayDesc.HitGroupTable.StrideInBytes = m_sbtEntrySize;
    rayDesc.Width = m_swapChain->getWidth();
    rayDesc.Height = m_swapChain->getHeight();
    rayDesc.Depth = 1;

    commandList->DispatchRays(&rayDesc);

    // Transition DXR output texture to COPY_SOURCE
    auto barrierToCopySource = CD3DX12_RESOURCE_BARRIER::Transition(
        m_dxrOutputTexture.Get(),
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, // Ensure the correct "before" state
        D3D12_RESOURCE_STATE_COPY_SOURCE
    );

    if (texture) {
        texture->TransitionToState(commandList, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    }
    // Transition back buffer to COPY_DEST
    ID3D12Resource* currentBackBuffer = m_swapChain->getCurrentBackBufferResource();
    auto barrierToCopyDest = CD3DX12_RESOURCE_BARRIER::Transition(
        currentBackBuffer,
        D3D12_RESOURCE_STATE_RENDER_TARGET,
        D3D12_RESOURCE_STATE_COPY_DEST
    );

    D3D12_RESOURCE_BARRIER barriers[] = {barrierToCopySource, barrierToCopyDest};
    commandList->ResourceBarrier(_countof(barriers), barriers);

    // Copy DXR output to back buffer
    commandList->CopyResource(currentBackBuffer, m_dxrOutputTexture.Get());

    // Transition back buffer back to RENDER_TARGET
    auto barrierToRenderTarget = CD3DX12_RESOURCE_BARRIER::Transition(
        currentBackBuffer,
        D3D12_RESOURCE_STATE_COPY_DEST,
        D3D12_RESOURCE_STATE_RENDER_TARGET
    );
    commandList->ResourceBarrier(1, &barrierToRenderTarget);
}
