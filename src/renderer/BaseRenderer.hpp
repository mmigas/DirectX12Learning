#pragma once
#include "Camera.hpp"
#include "CommandListManager.hpp"
#include "CommandQueue.hpp"
#include "DX12Device.hpp"
#include "Mesh.hpp"
#include "PipelineStateObject.hpp"
#include "SwapChain.hpp"
#include "Texture.hpp"
using Microsoft::WRL::ComPtr;

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

struct DXRCameraConstants {
    glm::mat4 inverseViewProjectMatrix;
    glm::vec3 position;
};

struct DXRObjectConstants {
    glm::mat4 worldMatrix;
    glm::mat4 invTransposeWorldMatrix; // For transforming normals
};

inline size_t AlignUp(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}

class BaseRenderer {
public:
    BaseRenderer();

    virtual ~BaseRenderer();

    virtual bool init(DX12Device* device, CommandQueue* commandQueue, SwapChain* swapChain, UINT numFrames) = 0;

    virtual void render(float deltaTime, Camera* camera, Mesh* mesh, Texture* texture);

    virtual void shutdown() = 0;

    void waitForGpu();

    void moveToNextFrame();

    DX12Device* getDevice() const {
        return m_device;
    }

    CommandQueue* getCommandQueue() const {
        return m_commandQueue;
    }

    SwapChain* getSwapChain() const {
        return m_swapChain;
    }

    CommandListManager* getCommandManager() const {
        return m_commandManager.get();
    }

    std::shared_ptr<DescriptorHeap> getSrvHeap() const {
        return m_srvHeap;
    } // Combined heap
    DescriptorHeap* getDsvHeap() const {
        return m_dsvHeap.get();
    }

    D3D12_CPU_DESCRIPTOR_HANDLE getCurrentDsvHandle() const {
        return m_dsvHandleCPU;
    }

    const D3D12_VIEWPORT& getViewport() const {
        return m_viewport;
    }

    const D3D12_RECT& getScissorRect() const {
        return m_scissorRect;
    }

    UINT getCurrentFrameIndex() const {
        return m_currentFrameIndex;
    }

    UINT getNumFramesInFlight() const {
        return m_numFramesInFlight;
    }

    void setTotalTime(float totalTime) {
        m_totalTime = totalTime;
    }

    float getTotalTime() const {
        return m_totalTime;
    }

protected:
    DX12Device* m_device = nullptr;
    CommandQueue* m_commandQueue = nullptr;
    SwapChain* m_swapChain = nullptr;
    UINT m_numFramesInFlight = 0;

    std::unique_ptr<CommandListManager> m_commandManager;
    std::shared_ptr<DescriptorHeap> m_srvHeap;
    std::unique_ptr<DescriptorHeap> m_dsvHeap;
    Microsoft::WRL::ComPtr<ID3D12Resource> m_depthStencilBuffer;
    D3D12_CPU_DESCRIPTOR_HANDLE m_dsvHandleCPU = {};

    UINT64 m_frameFenceValues[SwapChain::kBackBufferCount] = {};
    UINT m_currentFrameIndex = 0;

    D3D12_VIEWPORT m_viewport;
    D3D12_RECT m_scissorRect;

    float m_totalTime = 0.0f;


    std::vector<std::unique_ptr<Buffer>> m_perFrameObjectCBs;
    std::vector<std::unique_ptr<Buffer>> m_perFrameLightCBs;
    std::unique_ptr<Buffer> m_materialCB;

    std::vector<D3D12_GPU_DESCRIPTOR_HANDLE> m_frameLightCbvHandlesGPU;
    D3D12_GPU_DESCRIPTOR_HANDLE m_materialCbvHandleGPU = {};

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

    bool createDescriptorHeaps();

    bool createDepthStencilResources();

    bool createConstantBuffersAndViews();

    void setupViewportAndScissor(UINT width, UINT height);

    virtual void renderVariant(float deltaTime, Camera* camera, Mesh* mesh, Texture* texture, ID3D12GraphicsCommandList* commandList) = 0;

    virtual void updateConstantBuffers(float delta_time, Camera* camera, Mesh* mesh);
};
