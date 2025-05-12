#pragma once

#include <d3d12.h>
#include <wrl/client.h> // ComPtr
#include <memory>       // unique_ptr
#include <string>
#include <vector>
#include <glm/glm.hpp>

#include "SwapChain.hpp"

// Forward Declarations (Core DX12/App)
class DX12Device;
class CommandQueue;
class SwapChain;
class CommandListManager;
class RootSignature;
class PipelineStateObject;
class DescriptorHeap;
class Buffer;
class Camera;
class Mesh;
class Texture;

using Microsoft::WRL::ComPtr;
using namespace glm;

struct FrameConstant {
    glm::mat4 viewProjectMatrix;
};

struct ObjectConstant {
    glm::mat4 worldMatrix;
    glm::mat4 mvpMatrix;
};

struct LightConstant {
    glm::vec4 ambientColor;
    glm::vec4 lightColor;
    glm::vec3 lightPosition;
    glm::vec3 cameraPosition;
};

struct MaterialConstant {
    glm::vec4 specularColor;
    float specularPower;
};

struct AccelerationStructureBuffers {
    ComPtr<ID3D12Resource> scratch = nullptr; // Scratch memory for build
    ComPtr<ID3D12Resource> result = nullptr; // Stores final AS
    ComPtr<ID3D12Resource> instanceDesc = nullptr; // Holds instance descs for TLAS build (Upload heap)
};

struct DXRCameraConstants {
    glm::mat4 inverseViewProjectMatrix;
    glm::vec3 position;
};

struct DXRObjectConstants {
    glm::mat4 worldMatrix;
    glm::mat4 invTransposeWorldMatrix; // For transforming normals
};

class Renderer {
public:
    Renderer();

    ~Renderer();

    // Initialization
    bool init(
        DX12Device* pDevice,
        CommandQueue* pCommandQueue,
        SwapChain* pSwapChain,
        UINT numFrames // Typically SwapChain::kBackBufferCount
    );

    // Called before exiting
    void shutdown();

    bool buildAccelerationStructures(Mesh* mesh); // Main entry point

    // Main render function
    void render(float deltaTime, Camera* camera, Mesh* mesh, Texture* texture, bool useRaytracing);

    // Pass scene objects

    // Signal that assets requiring GPU upload have finished loading
    void signalAssetUploadComplete();

    std::shared_ptr<DescriptorHeap> getSrvHeap() {
        return m_srvHeap;
    }

    void setTotalTime(float totalTime) {
        m_totalTime = totalTime;
    }

    float getTotalTime() const {
        return m_totalTime;
    }

private:
    // --- Core Dependencies (Raw pointers - Renderer doesn't own these) ---
    DX12Device* m_device = nullptr;
    CommandQueue* m_commandQueue = nullptr;
    SwapChain* m_swapChain = nullptr;
    UINT m_numFramesInFlight = 0;

    // --- Owned DX12 Objects ---
    std::unique_ptr<CommandListManager> m_commandManager;
    std::unique_ptr<RootSignature> m_rasterRootSignature;
    std::unique_ptr<PipelineStateObject> m_rasterPipelineState;
    std::shared_ptr<DescriptorHeap> m_srvHeap;
    std::unique_ptr<DescriptorHeap> m_dsvHeap;
    ComPtr<ID3D12Resource> m_depthStencilBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandleCPU = {};

    // --- Owned Constant Buffers ---
    std::vector<std::unique_ptr<Buffer>> m_perFrameObjectCBs;
    std::vector<std::unique_ptr<Buffer>> m_perFrameLightCBs;
    std::unique_ptr<Buffer> m_materialCB;

    // --- Synchronization ---
    UINT64 m_frameFenceValues[SwapChain::kBackBufferCount] = {}; // Max frames
    UINT m_currentFrameIndex = 0;

    // --- Viewport/Scissor ---
    D3D12_VIEWPORT m_viewport;
    D3D12_RECT m_scissorRect;

    // --- Temp state for Update/Render ---
    float m_totalTime = 0.0f; // Keep track of time for animations


    // --- Raytracing ---
    AccelerationStructureBuffers m_blasBuffers; // TLAS buffers for raytracing
    AccelerationStructureBuffers m_tlasBuffers; // TLAS buffers for raytracing
    bool m_rayTracingSupported = false;

    ComPtr<ID3D12RootSignature> m_dxrRootSignature; // DXR needs ComPtr directly often
    ComPtr<ID3D12StateObject> m_dxrStateObject; // The RTPSO
    ComPtr<ID3D12Resource> m_dxrOutputTexture; // UAV for output
    D3D12_CPU_DESCRIPTOR_HANDLE m_dxrOutputUavCpuHandle = {}; // CPU Handle for UAV creation
    D3D12_GPU_DESCRIPTOR_HANDLE m_dxrOutputUavGpuHandle = {}; // GPU Handle for binding UAV
    ComPtr<ID3D12Resource> m_shaderBindingTable; // Buffer for SBT
    UINT m_sbtEntrySize = 0; // Size of one SBT entry
    std::unique_ptr<Buffer> m_dxrCameraCB; // Camera CB for raytracing
    std::unique_ptr<Buffer> m_dxrObjectCB;
    std::unique_ptr<Buffer> m_dxrLightCB; // Single buffer, updated per frame
    std::unique_ptr<Buffer> m_dxrMaterialCB; // Single buffer, can be static
    D3D12_GPU_DESCRIPTOR_HANDLE m_dxrObjectCbvHandleGPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_dxrCameraCbvHandleGPU = {}; // GPU Handle for binding
    D3D12_GPU_DESCRIPTOR_HANDLE m_meshVertexBufferSrvHandleGPU = {}; // GPU Handle for binding VB SRV
    D3D12_GPU_DESCRIPTOR_HANDLE m_meshIndexBufferSrvHandleGPU = {}; // GPU Handle for binding IB SRV
    D3D12_GPU_DESCRIPTOR_HANDLE m_dxrLightCbvHandleGPU = {};
    D3D12_GPU_DESCRIPTOR_HANDLE m_dxrMaterialCbvHandleGPU = {};


    // --- Private Helper Methods ---
    bool checkRayTracingSupport();

    bool createDescriptorHeaps();

    bool createDepthStencilResources();

    bool createConstantBuffersAndViews();

    bool createRootSignatureAndPSO();

    void setupViewportAndScissor(UINT width, UINT height);

    void updateConstantBuffers(float deltaTime, Camera* camera, Mesh* mesh);

    void waitForGpu();

    void moveToNextFrame();

    D3D12_GPU_DESCRIPTOR_HANDLE GetGpuHandleFromHeap(DescriptorHeap* heap, UINT index);

    bool CreateDXRResources();

    bool CreateDXRRootSignature();

    bool CreateDXRStateObject(const std::wstring& shaderPath);

    bool BuildShaderBindingTable();

    bool buildBLAS(Mesh* mesh, ID3D12GraphicsCommandList5* cmdList);

    bool buildTLAS(ID3D12GraphicsCommandList5* cmdList);

    bool createMeshBufferSRVs(Mesh* mesh);

    void renderRaster(ID3D12GraphicsCommandList* commandList,
                      Mesh
                      * mesh, Texture* texture);

    void renderRaytraced(Camera* camera, Mesh* mesh, Texture* texture, ID3D12GraphicsCommandList5* commandList);
};
