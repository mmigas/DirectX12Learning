#include "CommandListManager.hpp"

CommandListManager::CommandListManager() : m_type(D3D12_COMMAND_LIST_TYPE_DIRECT),
                                           m_numAllocators(0) {
    // Initialize command allocators and command list to nullptr
}

CommandListManager::~CommandListManager() {
}

bool CommandListManager::create(ID3D12Device* device, D3D12_COMMAND_LIST_TYPE type, UINT numAllocators) {
    if (device == nullptr) {
        return false;
    }

    m_type = type;
    m_numAllocators = numAllocators;

    // 1. Create command allocator
    m_commandAllocators.resize(m_numAllocators);
    for (UINT i = 0; i < m_numAllocators; ++i) {
        HRESULT hr = device->CreateCommandAllocator(m_type, IID_PPV_ARGS(&m_commandAllocators[i]));
        if (FAILED(hr)) {
            MessageBoxW(nullptr, L"Failed to create command allocator!", L"Error", MB_OK | MB_ICONERROR);
            return false;
        }
        wchar_t name[40];
        swprintf_s(name, L"Command Allocator %u", i);
        m_commandAllocators[i]->SetName(name);
    }
    // 2. Create command list (using the first allocator initially)
    HRESULT hr = device->CreateCommandList(0, m_type, m_commandAllocators[0].Get(), nullptr,
                                           IID_PPV_ARGS(&m_commandList));
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to create command list!", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }
    m_commandList->SetName(L"Main Command List");

    // Command lists are created in the recording state, but there's nothing
    // to record yet. Close it initially; the main loop will reset it.
    hr = m_commandList->Close();
    if (FAILED(hr)) {
        MessageBoxW(nullptr, L"Failed to close command list!", L"Error", MB_OK | MB_ICONERROR);
        return false;
    }
    return true;
}

bool CommandListManager::resetAllocator(UINT frameIndex) const {
    if (frameIndex >= m_numAllocators) {
        OutputDebugStringW(L"Error: Invalid frame index for ResetAllocator.\n");
        return false; // Invalid index
    }

    HRESULT hr = m_commandAllocators[frameIndex]->Reset();
    if (FAILED(hr)) {
        // This usually means the GPU hasn't finished with commands using this allocator
        OutputDebugStringW(L"Error: Failed to reset command allocator.\n");
        return false;
    }
    return true;
}

bool CommandListManager::resetCommandList(UINT frameIndex, ID3D12PipelineState* pipelineState) const {
    if (frameIndex >= m_numAllocators) {
        OutputDebugStringW(L"Error: Invalid frame index for ResetAllocator.\n");
        return false; // Invalid index
    }
    if (!m_commandAllocators[frameIndex]) {
        OutputDebugStringW(L"Error: Command Allocator pointer is null for ResetAllocator.\n");
        return false;
    }
    HRESULT hr = m_commandList->Reset(m_commandAllocators[frameIndex].Get(), pipelineState);
    if (FAILED(hr)) {
        OutputDebugStringW(L"Error: Failed to reset command list!\n");
        return false;
    }
    return true;
}

bool CommandListManager::closeCommandList() {
    HRESULT hr = m_commandList->Close();
    if (FAILED(hr)) {
        OutputDebugStringW(L"Error: Failed to close command list!\n");
        return false;
    }
    return true;
}
