#define STB_IMAGE_IMPLEMENTATION
#include "Application.hpp"
#include "CommandQueue.hpp"

#include <d3dx12_barriers.h>
#include <stdexcept>
#include "../libs/stb/stb_image.hpp"

Application::Application(HINSTANCE hInstance) : m_hInstance(hInstance),
                                                m_window(nullptr),
                                                m_isRunning(true),
                                                m_device(nullptr),
                                                m_commandQueue(nullptr),
                                                m_swapChain(nullptr),
                                                m_renderer(nullptr), // Init new ptr
                                                m_modelMesh(nullptr),
                                                m_texture(nullptr),
                                                m_camera(nullptr) {
    QueryPerformanceFrequency(&m_frequency);
    QueryPerformanceCounter(&m_lastFrameTime);
}

Application::~Application() {
    shutdown();
}

bool Application::init() {
    m_window = std::make_unique<Window>(m_hInstance, L"DX12 Framework", 1280, 720);
    if (!m_window || !m_window->create()) {
        MessageBoxW(nullptr, L"Window creation failed", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    if (!initDirectX()) {
        return false;
    }

    m_camera = std::make_unique<Camera>(m_window->getWidth(), m_window->getHeight());

    m_renderer = std::make_unique<Renderer>();
    if (!m_renderer || !m_renderer->init(m_device.get(), m_commandQueue.get(), m_swapChain.get(),
                                         SwapChain::kBackBufferCount)) {
        MessageBoxW(nullptr, L"Failed to initialize Renderer!", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    if (!loadAssets()) {
        MessageBoxW(nullptr, L"Failed to load content!", L"Initialization Error", MB_OK | MB_ICONERROR);
        return false;
    }
    m_renderer->buildAccelerationStructures(m_modelMesh.get());
    updateMatrices();

    m_window->show(SW_SHOWDEFAULT);
    return true;
}

int Application::run() {
    MSG msg = {};
    m_isRunning = true;
    while (m_isRunning) {
        // Process all pending messages in the queue
        while (PeekMessage(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) {
                m_isRunning = false;
                break;
            }
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }

        if (m_isRunning) {
            float deltaTime = calculateDeltaTime();
            update(deltaTime);
            if (m_renderer) {
                m_renderer->render(deltaTime, m_camera.get(), m_modelMesh.get(), m_texture.get(), m_useRaytracing);
            }
        }
    }
    // Return the exit code from WM_QUIT message
    return (int)(msg.wParam);
}

void Application::shutdown() {
    // Ensure GPU is idle before releasing anything
    waitForGpuIdleAndClearUploads(); // Use helper

    // Shutdown renderer first (releases its DX objects)
    if (m_renderer) m_renderer->shutdown();
    m_renderer.reset();

    // Release Application owned resources
    m_modelMesh.reset();
    m_texture.reset();
    m_camera.reset();
    m_swapChain.reset(); // Release before queue/device
    m_commandQueue.reset();
    m_device.reset();

    if (m_window) {
        m_window->destroy();
    }
    m_window.reset();
}

HWND Application::getWindowHandle() {
    if (m_window) {
        return m_window->getWindowHandle();
    }
    return nullptr;
}

void Application::update(float deltaTime) {
    static float timer = 0.0f;
    static int frameCount = 0;
    timer += deltaTime;
    frameCount++;
    if (timer >= 1.0f) {
        std::wstring title = L"DX12 Renderer - Mode: ";
        title += (m_useRaytracing ? L"Raytracing" : L"Rasterization");
        title += L" - " + std::to_wstring(m_window->getWidth()) + L"x" + std::to_wstring(m_window->getHeight());
        title += L" - " + std::to_wstring(frameCount) + L" fps";
        m_window->setTitle(title); // Need to add SetTitle method to Window class
        timer -= 1.0f;
        frameCount = 0;
    }

    m_renderer->setTotalTime(m_renderer->getTotalTime() + deltaTime);

    float scrollDelta = m_window->getAndResetMouseWheelDelta();
    float mouseDeltaX = 0.0f, mouseDeltaY = 0.0f;
    if (m_window->isLeftMouseButtonDown()) { // Only get mouse delta if button is down
        m_window->getAndResetMouseDelta(mouseDeltaX, mouseDeltaY);
    }
    if (m_window->wasSpaceBarPressed()) {
        m_useRaytracing = !m_useRaytracing;
        OutputDebugStringW(m_useRaytracing ? L"Switching to Raytracing\n" : L"Switching to Rasterization\n");
        // Update window title maybe?
        std::wstring title = L"DX12 Renderer - Mode: ";
        title += (m_useRaytracing ? L"Raytracing" : L"Rasterization");
        title += L" - " + std::to_wstring(m_window->getWidth()) + L"x" + std::to_wstring(m_window->getHeight());
        title += L" - " + std::to_wstring(frameCount) + L" fps";
        m_window->setTitle(title);
    }

    // Pass input to camera
    if (scrollDelta != 0.0f) {
        m_camera->processMouseScroll(scrollDelta);
    }
    if (mouseDeltaX != 0.0f || mouseDeltaY != 0.0f) {
        // Only process orbit if there was movement while button was down
        m_camera->processOrbit(mouseDeltaX, mouseDeltaY);
    }

    // Update camera's view matrix based on new position/orientation
    m_camera->updateViewMatrix();

    
}

bool Application::initDirectX() {
    bool enableDebug = false;
#if defined(_DEBUG)
    enableDebug = true;
#endif
    m_device = std::make_unique<DX12Device>();
    if (!m_device || !m_device->create(enableDebug)) {
        return false;
    }

    m_commandQueue = std::make_unique<CommandQueue>();
    if (!m_commandQueue || !m_commandQueue->create(m_device->getDevice(),
                                                   D3D12_COMMAND_LIST_TYPE_DIRECT)) {
        return false;
    }

    m_swapChain = std::make_unique<SwapChain>();
    DXGI_FORMAT backBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;
    if (!m_swapChain || !m_swapChain->create(m_device->getFactory(), m_commandQueue->getCommandQueue(),
                                             m_device->getDevice(), m_window->getWindowHandle(), m_window->getWidth(),
                                             m_window->getHeight(), backBufferFormat)) {
        return false;
    }

    return true;
}

bool Application::loadAssets() {
    ID3D12Device* device = m_device->getDevice();

    // Need a temporary command list/allocator for asset uploads
    ComPtr<ID3D12CommandAllocator> tempAllocator;
    HRESULT hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&tempAllocator));
    if (FAILED(hr)) return false;
    tempAllocator->SetName(L"Asset Load Allocator");

    ComPtr<ID3D12GraphicsCommandList> tempCommandList;
    hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tempAllocator.Get(), nullptr,
                                   IID_PPV_ARGS(&tempCommandList));
    if (FAILED(hr)) return false;
    tempCommandList->SetName(L"Asset Load Command List");

    m_uploadBuffers.clear(); // Clear previous tracking

    try {
        m_modelMesh = std::make_unique<Mesh>();
        // Replace "model.obj" with the path to your OBJ file
        auto meshUploadBuffers = m_modelMesh->LoadFromObjFile(device, tempCommandList.Get(), "mitsuba.obj");
        if (meshUploadBuffers.first) {
            trackUploadBuffer(meshUploadBuffers.first);
        }
        if (meshUploadBuffers.second) {
            trackUploadBuffer(meshUploadBuffers.second);
        }
        m_texture = std::make_unique<Texture>();

        ComPtr<ID3D12Resource> texUploadBuffer = m_texture->LoadFromFile(
            device, tempCommandList.Get(), m_renderer->getSrvHeap().get(), L"texture.png"); // Use Renderer's heap
        trackUploadBuffer(texUploadBuffer);
    } catch (const std::exception& e) {
        OutputDebugStringA("Error loading assets: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
        // Need to close command list even on error? Maybe just return false
        return false;
    }
    // --- Execute Asset Upload Commands ---
    hr = tempCommandList->Close();
    if (FAILED(hr)) return false;
    ID3D12CommandList* ppCommandLists[] = {tempCommandList.Get()};
    m_commandQueue->executeCommandLists(1, ppCommandLists);

    // Wait for GPU to finish uploads and clear tracking list
    waitForGpuIdleAndClearUploads();

    // Signal renderer that uploads are done (optional, if needed)
    if (m_renderer) {
        m_renderer->signalAssetUploadComplete();
    }

    return true;
}

void Application::trackUploadBuffer(const ComPtr<ID3D12Resource>& uploadBuffer) {
    if (uploadBuffer) m_uploadBuffers.push_back(uploadBuffer);
}

// Wait for GPU Idle and Clear Upload Buffers
void Application::waitForGpuIdleAndClearUploads() {
    if (m_commandQueue) {
        m_commandQueue->join(); // Wait for all commands to complete
    }
    m_uploadBuffers.clear(); // Release upload buffers now
}

void Application::updateMatrices() {
    if (!m_camera || !m_window) return;
    m_camera->updateProjectionMatrix(
        static_cast<float>(m_window->getWidth()) / static_cast<float>(m_window->getHeight()));
}

float Application::calculateDeltaTime() {
    LARGE_INTEGER currentTime;
    QueryPerformanceCounter(&currentTime);

    float deltaTime = (float)(currentTime.QuadPart - m_lastFrameTime.QuadPart) / (float)m_frequency.QuadPart;
    m_lastFrameTime = currentTime;
    return std::max(0.0f, std::min(deltaTime, 0.1f)); // Prevent timestep > 0.1 seconds
}
