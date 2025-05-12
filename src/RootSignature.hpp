#pragma once
#include <d3d12.h>
#include <wrl/client.h> // ComPtr
#include "d3dx12.h"     // For helpers like CD3DX12_ROOT_SIGNATURE_DESC


class RootSignature {
public:
    RootSignature();

    ~RootSignature();

    bool create(ID3D12Device* device, const D3D12_ROOT_SIGNATURE_DESC& desc,
                D3D_ROOT_SIGNATURE_VERSION version = D3D_ROOT_SIGNATURE_VERSION_1_0);

    bool createEmpty(ID3D12Device* device);

    ID3D12RootSignature* getSignature() const {
        return m_rootSignature.Get();
    }

    ID3D12RootSignature** GetSignatureAddressOf() {
        return m_rootSignature.ReleaseAndGetAddressOf();
    }

private:
    Microsoft::WRL::ComPtr<ID3D12RootSignature> m_rootSignature;
    Microsoft::WRL::ComPtr<ID3DBlob> m_signatureBlob; // Serialized version
    Microsoft::WRL::ComPtr<ID3DBlob> m_errorBlob; // Errors during serialization
};
