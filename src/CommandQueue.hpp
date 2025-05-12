#pragma once
#include "DX12Device.hpp"


class CommandQueue {
public:
    CommandQueue();

    ~CommandQueue();

    bool create(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE type);

    void executeCommandLists(UINT commandLists, ID3D12CommandList* const* numCommandLists) const;

    UINT64 signal();

    bool isFenceComplete(UINT64 fenceValue);

    void waitForFence(UINT64 fenceValue);

    //Wait for the GPU to finish executing all commands in the queue
    void join();

    ID3D12CommandQueue* getCommandQueue() const {
        return m_commandQueue.Get();
    }

    ID3D12Fence* getFence() const {
        return m_fence.Get();
    }

private:
    Microsoft::WRL::ComPtr<ID3D12CommandQueue> m_commandQueue;
    Microsoft::WRL::ComPtr<ID3D12Fence> m_fence;
    HANDLE m_fenceEvent = nullptr; // Win32 event handle
    UINT64 m_nextFenceValue = 1; // Start fence values at 1
};
