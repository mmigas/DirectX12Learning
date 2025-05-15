#pragma once
#include "BaseRenderer.hpp"

struct AccelerationStructureBuffers {
    ComPtr<ID3D12Resource> scratch = nullptr; // Scratch memory for build
    ComPtr<ID3D12Resource> result = nullptr; // Stores final AS
    ComPtr<ID3D12Resource> instanceDesc = nullptr; // Holds instance descs for TLAS build (Upload heap)
};


class RenderRayTracing : public BaseRenderer {
public:
    RenderRayTracing();

    ~RenderRayTracing() override;

    bool init(DX12Device* device, CommandQueue* commandQueue, SwapChain* swapChain, UINT numFrames) override;

    void shutdown() override;

    void renderVariant(float deltaTime, Camera* camera, Mesh* mesh, Texture* texture,
                       ID3D12GraphicsCommandList* commandList) override;

    bool buildAccelerationStructures(Mesh* mesh);

private:
    AccelerationStructureBuffers m_blasBuffers;
    AccelerationStructureBuffers m_tlasBuffers;
    bool m_rayTracingSupported = false;

    ComPtr<ID3D12RootSignature> m_rootSignature;
    ComPtr<ID3D12StateObject> m_stateObject;
    ComPtr<ID3D12Resource> m_outputTexture;
    D3D12_CPU_DESCRIPTOR_HANDLE m_outputUavCpuHandle = {}; // CPU Handle for UAV creation
    D3D12_GPU_DESCRIPTOR_HANDLE m_outputUavGpuHandle = {}; // GPU Handle for binding UAV
    ComPtr<ID3D12Resource> m_shaderBindingTable;
    UINT m_sbtEntrySize = 0;

    bool checkRayTracingSupport();

    bool createResources();

    bool createRootSignature();

    bool createStateObject(const std::wstring& shaderPath);

    bool buildShaderBindingTable();

    bool buildBLAS(Mesh* mesh, ID3D12GraphicsCommandList5* commandList);

    bool buildTLAS(ID3D12GraphicsCommandList5* commandList);

    bool createMeshBufferSRVs(Mesh* mesh);

    void updateConstantBuffers(float deltaTime, Camera* camera, Mesh* mesh);
};
