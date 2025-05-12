#pragma once
#include <d3d12.h>
#include <string>
#include <wrl/client.h>

#include "DescriptorHeap.hpp"


class Texture {
public:
    Texture();

    ~Texture();

    Microsoft::WRL::ComPtr<ID3D12Resource> LoadFromFile(
        ID3D12Device* device,
        ID3D12GraphicsCommandList* commandList,
        DescriptorHeap* descriptorHeap,
        const std::wstring& filename,
        const std::string& name = "Texture"
    );

    void TransitionToState(ID3D12GraphicsCommandList* pCmdList, D3D12_RESOURCE_STATES targetState);

    // Getters
    ID3D12Resource* getResource() const {
        return m_textureResource.Get();
    }

    D3D12_GPU_DESCRIPTOR_HANDLE getSRVGPUHandle() const {
        return m_srvHandleGPU;
    } // For binding
    UINT getWidth() const {
        return m_width;
    }

    UINT getHeight() const {
        return m_height;
    }

    DXGI_FORMAT getFormat() const {
        return m_format;
    }

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_textureResource; // The actual texture resource (DEFAULT heap)
    D3D12_CPU_DESCRIPTOR_HANDLE m_srvHandleCPU; // CPU handle where SRV lives in heap
    D3D12_GPU_DESCRIPTOR_HANDLE m_srvHandleGPU; // GPU handle where SRV lives in heap (if shader visible)
    std::string m_name;
    UINT m_width = 0;
    UINT m_height = 0;
    DXGI_FORMAT m_format = DXGI_FORMAT_UNKNOWN;
    D3D12_RESOURCE_STATES m_currentState = D3D12_RESOURCE_STATE_COMMON;
};
