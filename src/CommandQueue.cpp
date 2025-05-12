#include "CommandQueue.hpp"

#include <stdexcept>

CommandQueue::CommandQueue() {
}

CommandQueue::~CommandQueue() {
    if (m_fenceEvent) {
        CloseHandle(m_fenceEvent);
        m_fenceEvent = nullptr;
    }
}

bool CommandQueue::create(ID3D12Device5* device, D3D12_COMMAND_LIST_TYPE type) {
    if (!device) {
        return false;
    }

    // 1. Create command queue
    D3D12_COMMAND_QUEUE_DESC queueDesc = {};
    queueDesc.Type = type;
    queueDesc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
    queueDesc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
    queueDesc.NodeMask = 0;

    HRESULT hr = device->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&m_commandQueue));
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to create command queue!", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }

    m_commandQueue->SetName(L"Main Command Queue");

    // 2. Create fence
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&m_fence));
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to create fence!", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }
    m_fence->SetName(L"Main Fence");
    m_nextFenceValue = 1; // Initialize fence value

    // 3. Create Fence Event Handle
    m_fenceEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (m_fenceEvent == nullptr) {
        MessageBoxW(nullptr, L"Failed to create fence event handle!", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

void CommandQueue::executeCommandLists(const UINT commandLists, ID3D12CommandList* const* numCommandLists) const {
    m_commandQueue->ExecuteCommandLists(commandLists, numCommandLists);
}

UINT64 CommandQueue::signal() {
    UINT64 fenceValueToSignal = m_nextFenceValue++;
    HRESULT hr = m_commandQueue->Signal(m_fence.Get(), fenceValueToSignal);
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to signal command queue fence!");
    }
    return fenceValueToSignal;
}

bool CommandQueue::isFenceComplete(UINT64 fenceValue) {
    return m_fence->GetCompletedValue() >= fenceValue;
}

void CommandQueue::waitForFence(UINT64 fenceValue) {
    if (!isFenceComplete(fenceValue)) {
        HRESULT hr = m_fence->SetEventOnCompletion(fenceValue, m_fenceEvent);
        if (SUCCEEDED(hr)) {
            WaitForSingleObject(m_fenceEvent, INFINITE);
        } else {
            throw std::runtime_error("Failed to set event on fence completion!");
        }
    }
}

void CommandQueue::join() {
    // Wait for the GPU to finish executing all commands in the queue
    UINT64 fenceValue = signal();
    waitForFence(fenceValue);
}
