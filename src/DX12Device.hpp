#pragma once
#include <directx/d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
class DX12Device {
public:

    DX12Device();
    ~DX12Device();

    bool create(bool enableDebugLayer = false);

    ID3D12Device5* getDevice() const {
        return m_device.Get();
    }

    IDXGIFactory6* getFactory() const {
        return m_factory.Get();
    }

    IDXGIAdapter1* getAdapter() const {
        return m_adapter.Get();
    }
private:

    bool selectAdapter();

    Microsoft::WRL::ComPtr<IDXGIFactory6> m_factory;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> m_adapter;
    Microsoft::WRL::ComPtr<ID3D12Device5> m_device;
};

