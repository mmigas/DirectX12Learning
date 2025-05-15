#include "BaseRenderer.hpp"

#include "d3dx12_barriers.h"
#include "d3dx12_core.h"
#include "glm/gtc/type_ptr.hpp"

BaseRenderer::BaseRenderer() : m_device(nullptr),
                               m_commandQueue(nullptr),
                               m_swapChain(nullptr),
                               m_numFramesInFlight(0),
                               m_currentFrameIndex(0),
                               m_viewport(),
                               m_scissorRect(),
                               m_totalTime(0.0f) {
    for (UINT i = 0; i < SwapChain::kBackBufferCount; ++i) { // Use constant from SwapChain
        m_frameFenceValues[i] = 0;
    }
    m_dsvHandleCPU = {0};
}

BaseRenderer::~BaseRenderer() {
    //shutdown();
}

void BaseRenderer::render(float deltaTime, Camera* camera, Mesh* mesh, Texture* texture) {
    if (!camera || !mesh || !texture) {
        return; // Need essential objects
    }

    // --- Wait & Update CB Data ---
    waitForGpu();
    updateConstantBuffers(deltaTime, camera, mesh); 

    if (!m_commandManager->resetAllocator(m_currentFrameIndex)) {
        return;
    }
    if (!m_commandManager->resetCommandList(m_currentFrameIndex, nullptr)) {
        return;
    }

    ID3D12GraphicsCommandList* commandList = m_commandManager->getCommandList();
    ID3D12Resource* currentBackBuffer = m_swapChain->getCurrentBackBufferResource();

    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(currentBackBuffer, D3D12_RESOURCE_STATE_PRESENT,
                                                        D3D12_RESOURCE_STATE_RENDER_TARGET);
    commandList->ResourceBarrier(1, &barrier);

    ID3D12DescriptorHeap* ppHeaps[] = {m_srvHeap->getHeapPointer()};
    commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);

    renderVariant(deltaTime, camera, mesh, texture, commandList);

    barrier = CD3DX12_RESOURCE_BARRIER::Transition(currentBackBuffer, D3D12_RESOURCE_STATE_RENDER_TARGET,
                                                   D3D12_RESOURCE_STATE_PRESENT);
    commandList->ResourceBarrier(1, &barrier);

    HRESULT hr = commandList->Close();
    if (FAILED(hr)) { // Handle error 
    } else {
        ID3D12CommandList* ppCommandLists[] = {commandList};
        m_commandQueue->executeCommandLists(1, ppCommandLists);
        if (!m_swapChain->present(0)) {
        }
        moveToNextFrame();
    }
}

void BaseRenderer::waitForGpu() {
    UINT64 fenceValue = m_frameFenceValues[m_currentFrameIndex];
    if (fenceValue > 0) {
        m_commandQueue->waitForFence(fenceValue);
    }
}

void BaseRenderer::moveToNextFrame() {
    const UINT64 currentFenceValue = m_commandQueue->signal();
    m_frameFenceValues[m_currentFrameIndex] = currentFenceValue;
    m_currentFrameIndex = m_swapChain->getCurrentBackBufferIndex();
}

bool BaseRenderer::createDescriptorHeaps() {
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

bool BaseRenderer::createDepthStencilResources() {
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

bool BaseRenderer::createConstantBuffersAndViews() {
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
    dxrMaterialConsts.specularColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
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


void BaseRenderer::setupViewportAndScissor(UINT width, UINT height) {
    m_viewport = CD3DX12_VIEWPORT(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height));
    m_scissorRect = CD3DX12_RECT(0, 0, width, height);
}

void BaseRenderer::updateConstantBuffers(float deltaTime, Camera* camera, Mesh* mesh) {
    m_totalTime += deltaTime; // Approximate time update - better to pass deltaTime

    // --- Update Per-Frame Light Constant Buffer ---
    Buffer* currentLightCB = m_perFrameLightCBs[m_currentFrameIndex].get();
    LightConstant lightConsts = {};
    lightConsts.ambientColor = glm::vec4(0.2f, 0.2f, 0.2f, 1.0f);
    float lightX = sin(m_totalTime * 0.5f) * 3.0f;
    float lightZ = cos(m_totalTime * 0.5f) * 3.0f;
    lightConsts.lightPosition = glm::vec3(0, 2.0f, 0);
    lightConsts.lightColor = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f);
    lightConsts.cameraPosition = camera->getPosition();
    void* lightMappedData = currentLightCB->map();
    if (lightMappedData) {
        memcpy(lightMappedData, &lightConsts, sizeof(lightConsts));
        currentLightCB->unmap(sizeof(lightConsts));
    }

    // --- Update Per-Object Constant Buffer ---
    Buffer* currentObjectCB = m_perFrameObjectCBs[m_currentFrameIndex].get();
    glm::mat4 viewProj = camera->getProjectionMatrix() * camera->getViewMatrix();
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
}
