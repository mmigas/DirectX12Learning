#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>


class SwapChain {
public:
    // Define the number of back buffers (usually 2 for double buffering, 3 for triple buffering)
    static const UINT kBackBufferCount = 3;

    SwapChain();

    ~SwapChain();

    bool create(IDXGIFactory6* factory, ID3D12CommandQueue* commandQueue, ID3D12Device* device, HWND hwnd, UINT width,
                UINT height, DXGI_FORMAT format);

    bool present(UINT vsync);

    bool resize(UINT width, UINT height, ID3D12Device* device);

    ID3D12Resource* getCurrentBackBufferResource() const;

    D3D12_CPU_DESCRIPTOR_HANDLE getCurrentBackBufferView() const;

    UINT getCurrentBackBufferIndex() const {
        return m_currentBufferIndex;
    }

    DXGI_FORMAT getFormat() const {
        return m_format;
    }

    UINT getWidth() const {
        return m_width;
    }

    UINT getHeight() const {
        return m_height;
    }

private:
    bool createRTV(ID3D12Device* device);

    void releaseBuffer();

    Microsoft::WRL::ComPtr<IDXGISwapChain3> m_swapChain;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_rtvHeap; // Descriptor heap for RTVs
    Microsoft::WRL::ComPtr<ID3D12Resource> m_backBuffers[kBackBufferCount];
    UINT m_rtvDescriptorSize = 0; // Size of one RTV descriptor
    UINT m_currentBufferIndex = 0; // Which back buffer is current (0 or 1)
    DXGI_FORMAT m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT m_width = 0;
    UINT m_height = 0;
};
