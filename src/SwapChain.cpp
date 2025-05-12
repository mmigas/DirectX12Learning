#include "SwapChain.hpp"

#include <stdexcept>
#include <string>
#include <d3dx12.h>

SwapChain::SwapChain() {
}

SwapChain::~SwapChain() {
}

bool SwapChain::create(IDXGIFactory6* factory, ID3D12CommandQueue* commandQueue, ID3D12Device* device, HWND hwnd,
                       UINT width, UINT height, DXGI_FORMAT format) {
    if (!factory || !commandQueue || !hwnd || !device) {
        return false;
    }

    m_width = width;
    m_height = height;
    m_format = format;

    // 1. Create RTV Description heap
    D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
    rtvHeapDesc.NumDescriptors = kBackBufferCount;
    rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtvHeapDesc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE; // No shader visibility
    rtvHeapDesc.NodeMask = 0; // Default node mask

    HRESULT hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&m_rtvHeap));
    if (FAILED(hr)) {
        MessageBoxW(hwnd, L"Failed to create rtv heap.", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }
    m_rtvHeap->SetName(L"Swap Chain RTV Heap");
    m_rtvDescriptorSize = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

    // 2. Describe and Create the Swap Chain
    DXGI_SWAP_CHAIN_DESC1 swapChainDesc = {};
    swapChainDesc.BufferCount = kBackBufferCount;
    swapChainDesc.Width = m_width;
    swapChainDesc.Height = m_height;
    swapChainDesc.Format = m_format;
    swapChainDesc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swapChainDesc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swapChainDesc.SampleDesc.Count = 1; // No multi-sampling
    swapChainDesc.SampleDesc.Quality = 0; // No multi-sampling
    swapChainDesc.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH; // Allow mode switching

    Microsoft::WRL::ComPtr<IDXGISwapChain1> tempSwapChain;
    hr = factory->CreateSwapChainForHwnd(commandQueue,
                                         hwnd,
                                         &swapChainDesc,
                                         nullptr, // Optional: fullscreen descriptor
                                         nullptr, // Optional: restrict output
                                         &tempSwapChain);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to create swap chain.", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Upgrade to IDXGISwapChain3 for GetCurrentBackBufferIndex etc.
    hr = tempSwapChain.As(&m_swapChain);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to cast Swap Chain to IDXGISwapChain3.", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    // Disable Alt+Enter fullscreen toggle
    hr = factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);

    // 3. Get initial back buffer index
    m_currentBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

    // 4. Create RTVs for each back buffer
    if (!createRTV(device)) {
        return false;
    }
    return true;
}

bool SwapChain::present(UINT vsync) {
    UINT presentFlags = 0;

    HRESULT hr = m_swapChain->Present(vsync, presentFlags);
    if (FAILED(hr)) {
        // Handle device removed or other errors
        HRESULT removedReason = m_swapChain->GetDevice(IID_PPV_ARGS(reinterpret_cast<ID3D12Device**>(nullptr)));
        // Trick to get device removed reason
        if (removedReason == DXGI_ERROR_DEVICE_REMOVED) {
            throw std::runtime_error("Device removed! SwapChain::Present failed.");
        } else {
            throw std::runtime_error("SwapChain::Present failed with HRESULT: " + std::to_string(hr));
        }
        return false;
    }
    m_currentBufferIndex = m_swapChain->GetCurrentBackBufferIndex();
    return true;
}

bool SwapChain::resize(UINT width, UINT height, ID3D12Device* device) {
    if (!m_swapChain || !device) {
        return false;
    }
    if (width != m_width || height != m_height) {
        return true;
    }

    // TODO: Proper resize requires:
    // 1. Flushing the command queue to ensure back buffers are not in use.
    // 2. Releasing the existing m_backBuffers ComPtrs.
    // 3. Calling m_swapChain->ResizeBuffers(...).
    // 4. Re-creating the RTVs using CreateRTVs().

    OutputDebugStringW(L"Warning: SwapChain::Resize not fully implemented yet.\n");
    // Placeholder simple resize (will likely crash without proper sync/release)
    releaseBuffer(); // Release existing resources

    m_width = width;
    m_height = height;

    HRESULT hr = m_swapChain->ResizeBuffers(kBackBufferCount, m_width, m_height, m_format,
                                            DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING);
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to resize Swap Chain buffers.", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    m_currentBufferIndex = m_swapChain->GetCurrentBackBufferIndex();

    if (!createRTV(device)) {
        MessageBoxW(nullptr, L"Failed to recreate RTVs after resize.", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    return true;
}

ID3D12Resource* SwapChain::getCurrentBackBufferResource() const {
    if (m_currentBufferIndex < kBackBufferCount) {
        return m_backBuffers[m_currentBufferIndex].Get();
    }
    return nullptr;
}

D3D12_CPU_DESCRIPTOR_HANDLE SwapChain::getCurrentBackBufferView() const {
    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    rtvHandle.Offset(m_currentBufferIndex, m_rtvDescriptorSize);
    return rtvHandle;
}

bool SwapChain::createRTV(ID3D12Device* device) {
    if (!device || !m_rtvHeap) {
        return false;
    }

    CD3DX12_CPU_DESCRIPTOR_HANDLE rtvHandle(m_rtvHeap->GetCPUDescriptorHandleForHeapStart());
    for (UINT i = 0; i < kBackBufferCount; ++i) {
        HRESULT hr = m_swapChain->GetBuffer(i, IID_PPV_ARGS(&m_backBuffers[i]));
        if (FAILED(hr)) {
            MessageBoxW(nullptr, L"Failed to get back buffer.", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }
        device->CreateRenderTargetView(m_backBuffers[i].Get(), nullptr, rtvHandle);

        wchar_t name[32];
        swprintf_s(name, L"Back Buffer %u", i);
        m_backBuffers[i]->SetName(name);

        // Move the handle to the next descriptor
        rtvHandle.Offset(1, m_rtvDescriptorSize);
    }
    return true;
}

void SwapChain::releaseBuffer() {
    for (UINT i = 0; i < kBackBufferCount; ++i) {
        m_backBuffers[i].Reset();
    }
}
