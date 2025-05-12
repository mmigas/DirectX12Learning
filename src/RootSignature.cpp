#include "RootSignature.hpp"
#include <d3dcompiler.h> // For D3DSerializeRootSignature
#include <stdexcept>     // For runtime_error

RootSignature::RootSignature() {
}

RootSignature::~RootSignature() {
}

bool RootSignature::create(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc,
                           D3D_ROOT_SIGNATURE_VERSION version) {
    if (!device) {
        return false;
    }

    m_signatureBlob.Reset();
    m_errorBlob.Reset();

    HRESULT hr = D3D12SerializeRootSignature(&desc, version, &m_signatureBlob, &m_errorBlob);
    if (FAILED(hr)) {
        OutputDebugStringW(L"Error: Failed to serialize root signature.\n");
        if (m_errorBlob) {
            OutputDebugStringW(L"Error: Failed to parse root signature.\n");
            OutputDebugStringA(static_cast<char*>(m_errorBlob->GetBufferPointer()));
            m_errorBlob.Reset(); // Release error blob
        } else {
            OutputDebugStringW(L"Unknown serialization error.\n");
        }
        return false;
    }

    hr = device->CreateRootSignature(0,
                                     m_signatureBlob->GetBufferPointer(),
                                     m_signatureBlob->GetBufferSize(),
                                     IID_PPV_ARGS(&m_rootSignature));
    if (FAILED(hr)) {
        OutputDebugStringW(L"Error: Failed to create root signature.\n");
        m_signatureBlob.Reset();
        m_rootSignature.Reset();
        return false;
    }

    m_rootSignature->SetName(L"Main Root Signature");
    return true;
}

bool RootSignature::createEmpty(ID3D12Device* device) {
    if (!device) {
        return false;
    }

    CD3DX12_ROOT_SIGNATURE_DESC desc(0,
                                     nullptr,
                                     0,
                                     nullptr,
                                     D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT);
    return create(device, desc, D3D_ROOT_SIGNATURE_VERSION_1_0);
}
