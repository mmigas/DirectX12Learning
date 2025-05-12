#include "DX12Device.hpp"

#include <corecrt_wstdio.h>
#include <dxgidebug.h>
using namespace Microsoft::WRL;
#ifdef _DEBUG
// Provide the definition for DXGI_DEBUG_ALL as declared in dxgidebug.h
EXTERN_C const IID DXGI_DEBUG_ALL =
        {0xe48ae283, 0xda80, 0x490b, {0x87, 0x1c, 0xc7, 0x23, 0xd2, 0xbb, 0x3f, 0x2c}};
#endif

DX12Device::DX12Device() {
}

DX12Device::~DX12Device() {
#if defined(DEBUG) || defined(_DEBUG)
    ComPtr<IDXGIDebug1> debugController;
    if (SUCCEEDED(DXGIGetDebugInterface1(0, IID_PPV_ARGS(&debugController)))) {
        OutputDebugStringW(L"DXGI Report Live Objects:\n");
        debugController->ReportLiveObjects(DXGI_DEBUG_ALL,
                                           DXGI_DEBUG_RLO_FLAGS(
                                               DXGI_DEBUG_RLO_SUMMARY | DXGI_DEBUG_RLO_IGNORE_INTERNAL));
        OutputDebugStringW(L"--------------------------\n");
    }
#endif
}

bool DX12Device::create(bool enableDebugLayer) {
    HRESULT hr;
    UINT dxgiFactorFlags = 0;

    // 1. Enable Debug Layer (if requested and in debug build)
#if defined(DEBUG) || defined(_DEBUG)
    if (enableDebugLayer) {
        ComPtr<ID3D12Debug> debugController;
        hr = D3D12GetDebugInterface(IID_PPV_ARGS(&debugController));
        if (SUCCEEDED(hr)) {
            debugController->EnableDebugLayer();
            dxgiFactorFlags |= DXGI_CREATE_FACTORY_DEBUG;
            OutputDebugStringW(L"D3D12 Debug Layer Enabled\n");
        } else {
            OutputDebugStringW(L"Warning: Failed to enable D3D12 Debug Layer. Install Graphics Tools.\n");
        }
    }
#endif

    // 2. Create DXGI Factory
    hr = CreateDXGIFactory2(dxgiFactorFlags, IID_PPV_ARGS(&m_factory));
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to create DXGIFactory2", L"Error", MB_OK);
        return false;
    }

    // 3. Select Hardware Adapter
    if (!selectAdapter()) {
        MessageBoxW(nullptr, L"Failed to create DXGIAdapter", L"Error", MB_OK);
        return false;
    }

    // 4. Create D3D12 Device
    hr = D3D12CreateDevice(
        m_adapter.Get(),
        D3D_FEATURE_LEVEL_11_1,
        IID_PPV_ARGS(&m_device));

    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to create D3D12 Device.", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

#if defined(DEBUG) || defined(_DEBUG)
    if (enableDebugLayer) {
        ComPtr<ID3D12InfoQueue> infoQueue;
        if (SUCCEEDED(m_device.As(&infoQueue))) {
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, true);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_CORRUPTION, true);
            infoQueue->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_WARNING, false);
        }
    }
#endif
    return true;
}

bool DX12Device::selectAdapter() {
    m_adapter = nullptr;

    ComPtr<IDXGIAdapter1> adapter;
    UINT adapterIndex = 0;
    size_t maxDedicatedVideoMemory = 0;

    while (m_factory->EnumAdapters1(adapterIndex++, &adapter) != DXGI_ERROR_NOT_FOUND) {
        DXGI_ADAPTER_DESC1 desc;
        adapter->GetDesc1(&desc);

        if (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) {
            continue;
        }

        if (SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_12_2, IID_PPV_ARGS(&m_device)))) {
            if (desc.DedicatedVideoMemory > maxDedicatedVideoMemory) {
                maxDedicatedVideoMemory = desc.DedicatedVideoMemory;
                m_adapter = adapter;
#if defined(DEBUG) || defined(_DEBUG)
                wchar_t buffer[256];
                swprintf_s(buffer, L"Selected GPU: %s (%zu MB VRAM)\n", desc.Description,
                           desc.DedicatedVideoMemory / 1024 / 1024);
                OutputDebugStringW(buffer);
#endif
            }
        }
    }
    if (!m_adapter) {
        // If no suitable hardware adapter found, maybe try WARP?
        // For now, we just fail.
        OutputDebugStringW(L"No suitable D3D12 hardware adapter found.\n.");
        return false;
    }
    return true;
}
