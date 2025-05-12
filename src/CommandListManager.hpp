#pragma once
#include <vector>

#include "DX12Device.hpp"


class CommandListManager {
public:
    CommandListManager();

    ~CommandListManager();

    bool create(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, UINT numAllocators);

    bool resetAllocator(UINT frameIndex) const;

    bool resetCommandList(UINT frameIndex, ID3D12PipelineState* pipelineState = nullptr) const;

    bool closeCommandList();

    // Getters
    ID3D12GraphicsCommandList* getCommandList() const {
        return m_commandList.Get();
    }

private:
    std::vector<Microsoft::WRL::ComPtr<ID3D12CommandAllocator>> m_commandAllocators;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> m_commandList; // Assuming graphics for now
    D3D12_COMMAND_LIST_TYPE m_type;
    UINT m_numAllocators = 0;
};
