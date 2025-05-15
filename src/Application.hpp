#pragma once

#include "CommandQueue.hpp"
#include "DX12Device.hpp"
#include "PipelineStateObject.hpp"
#include "SwapChain.hpp"
#include "Window.hpp"
#include "Camera.hpp"
#include "Mesh.hpp"
//#include "Renderer.hpp"
#include "Texture.hpp"
#include "renderer/RenderRaster.hpp"
#include "renderer/RenderRayTracing.hpp"


// This header won't be included in other files so its ok to use namespaces here
using namespace Microsoft::WRL;


class Application {
public:
    Application(HINSTANCE hInstance);

    ~Application();

    bool init();

    int run();

    void shutdown();

    HINSTANCE getInstanceHandle() const {
        return m_hInstance;
    }

    HWND getWindowHandle();

private:
    void update(float deltaTime);

    // RenderFrame now just calls Renderer::Render

    // --- Core Application Members ---
    HINSTANCE m_hInstance;
    std::unique_ptr<Window> m_window;
    bool m_isRunning = true;

    // --- Core DX12 Components (Still owned by Application) ---
    std::unique_ptr<DX12Device> m_device;
    std::unique_ptr<CommandQueue> m_commandQueue;
    std::unique_ptr<SwapChain> m_swapChain;

    // --- Renderer (Owns pipeline state, heaps, CBs, etc.) ---
    std::unique_ptr<RenderRaster> m_rendererRaster;
    std::unique_ptr<RenderRayTracing> m_rendererRayTracing;

    // --- High-Level Assets (Owned by Application) ---
    std::unique_ptr<Mesh> m_modelMesh;
    std::unique_ptr<Texture> m_textureRaster;
    std::unique_ptr<Texture> m_textureRayTracing;

    // --- Camera ---
    std::unique_ptr<Camera> m_camera;

    // --- Timing ---
    LARGE_INTEGER m_lastFrameTime = {};
    LARGE_INTEGER m_frequency = {};
    // float m_totalTime = 0.0f; // Time might be managed by Renderer or passed in

    // Keep upload buffers alive until GPU is done with copies
    std::vector<ComPtr<ID3D12Resource>> m_uploadBuffers;
    bool m_useRaytracing = true;

    void trackUploadBuffer(const ComPtr<ID3D12Resource>& uploadBuffer);

    void waitForGpuIdleAndClearUploads(); // Helper to wait and clear

    bool initDirectX(); // Creates Device, Queue, SwapChain

    bool loadAssets(); // Creates Mesh, Texture (and their upload buffers)

    void updateMatrices(); // Updates Camera Projection

    float calculateDeltaTime();
};
