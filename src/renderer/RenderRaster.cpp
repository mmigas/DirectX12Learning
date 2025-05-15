#include "RenderRaster.hpp"

#include "Shader.hpp"

RenderRaster::RenderRaster() : BaseRenderer() {
    m_rootSignature = nullptr;
    m_pipelineState = nullptr;
}

RenderRaster::~RenderRaster() {
}

bool RenderRaster::init(DX12Device* pDevice, CommandQueue* pCommandQueue, SwapChain* pSwapChain, UINT numFrames) {
    m_device = pDevice;
    m_commandQueue = pCommandQueue;
    m_swapChain = pSwapChain;
    m_numFramesInFlight = numFrames; // Should match swap chain buffer count

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
    if (!createRootSignature()) {
        return false;
    }
    if (!createPipelineStateObject()) {
        return false;
    }

    // Get initial state
    m_currentFrameIndex = m_swapChain->getCurrentBackBufferIndex();
    setupViewportAndScissor(m_swapChain->getWidth(), m_swapChain->getHeight()); // Use SwapChain dimensions

    return true;
}

void RenderRaster::shutdown() {
}

bool RenderRaster::createRootSignature() {
    ID3D12Device* device = m_device->getDevice();
    m_rootSignature = std::make_unique<RootSignature>();

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

    ComPtr<ID3DBlob> signatureBlob, errorBlob;
    HRESULT hr;
    hr = D3DX12SerializeVersionedRootSignature(&rootSignatureDesc, D3D_ROOT_SIGNATURE_VERSION_1_1, &signatureBlob,
                                               &errorBlob);
    if (FAILED(hr)) {
        return false;
    }
    hr = device->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
                                     IID_PPV_ARGS(m_rootSignature->GetSignatureAddressOf()));
    if (FAILED(hr)) {
        return false;
    }
    m_rootSignature->getSignature()->SetName(L"Main Root Sig (Lighting)");
    return true;
}

bool RenderRaster::createPipelineStateObject() {
    ID3D12Device* device = m_device->getDevice();
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

    m_pipelineState = std::make_unique<PipelineStateObject>();
    D3D12_GRAPHICS_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.InputLayout = inputLayoutDesc;
    psoDesc.pRootSignature = m_rootSignature->getSignature(); // Use the NEW root signature
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


    if (!m_pipelineState->create(device, psoDesc)) {
        return false;
    }
    m_pipelineState->getPipeline()->SetName(L"Main PSO");

    return true;
}

void RenderRaster::renderVariant(float deltaTime, Camera* camera, Mesh* mesh, Texture* texture,
                                 ID3D12GraphicsCommandList* commandList) {
    // Set common descriptor heap (SRV/CBV heap)
    ID3D12DescriptorHeap* ppHeaps[] = {m_srvHeap->getHeapPointer()};
    commandList->SetDescriptorHeaps(_countof(ppHeaps), ppHeaps);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle = m_swapChain->getCurrentBackBufferView();
    D3D12_CPU_DESCRIPTOR_HANDLE dsvHandle = m_dsvHandleCPU;
    commandList->SetGraphicsRootSignature(m_rootSignature->getSignature());
    commandList->SetPipelineState(m_pipelineState->getPipeline());
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
