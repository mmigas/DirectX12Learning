#include "PipelineStateObject.hpp"

PipelineStateObject::PipelineStateObject() {
}

PipelineStateObject::~PipelineStateObject() {
}

bool PipelineStateObject::create(ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC& description) {
    if (!device) {
        return false;
    }

    HRESULT hr = device->CreateGraphicsPipelineState(&description, IID_PPV_ARGS(&m_pipelineState));
    if (FAILED(hr)) {
        OutputDebugStringW(L"Error: Failed to create Graphics Pipeline State Object.\n");
        m_pipelineState.Reset();
        return false;
    }

    m_pipelineState->SetName(L"Main Pipeline State Object");
    return true;
}
