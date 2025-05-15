#pragma once

#include <d3d12.h>
#include <string>
#include <wrl/client.h> // ComPtr

class PipelineStateObject {
public:
    PipelineStateObject();

    ~PipelineStateObject();

    bool create(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& description);

    ID3D12PipelineState* getPipeline() const {
        return m_pipelineState.Get();
    }

    void setName(std::wstring name) {
        m_pipelineState->SetName(name.c_str());
    }

private:
    Microsoft::WRL::ComPtr<ID3D12PipelineState> m_pipelineState;
};
