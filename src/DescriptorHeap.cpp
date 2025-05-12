#include "DescriptorHeap.hpp"

#include "d3dx12_root_signature.h"

DescriptorHeap::DescriptorHeap(): m_type(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV), // Default type
                                  m_descriptorSize(0),
                                  m_numDescriptorsInHeap(0),
                                  m_shaderVisible(false),
                                  m_startCPU({0}),
                                  m_startGPU({0}),
                                  m_currentDescriptorIndex(0) {
}

DescriptorHeap::~DescriptorHeap() = default;

bool DescriptorHeap::create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, int numDescriptors,
                            bool shaderVisible) {
    if (!device || numDescriptors <= 0) {
        return false;
    }

    m_type = type;
    m_numDescriptorsInHeap = numDescriptors;
    m_shaderVisible = shaderVisible;
    m_currentDescriptorIndex = 0;

    D3D12_DESCRIPTOR_HEAP_DESC desc = {};
    desc.NumDescriptors = numDescriptors;
    desc.Type = type;
    desc.Flags = shaderVisible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    desc.NodeMask = 0;

    HRESULT hr = device->CreateDescriptorHeap(&desc, IID_PPV_ARGS(&m_heap));
    if (FAILED(hr)) {
        OutputDebugStringW(L"Error: Failed to create descriptor heap.\n");
        return false;
    }

    m_descriptorSize = device->GetDescriptorHandleIncrementSize(m_type);

    m_startCPU = m_heap->GetCPUDescriptorHandleForHeapStart();
    if (shaderVisible) {
        m_startGPU = m_heap->GetGPUDescriptorHandleForHeapStart();
    } else {
        m_startGPU = {0}; // Invalid GPU handle
    }

    switch (m_type) {
        case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
            m_heap->SetName(L"CBV/SRV/UAV Heap");
            break;
        case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
            m_heap->SetName(L"Sampler Heap");
            break;
        case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
            m_heap->SetName(L"RTV Heap");
            break;
        case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
            m_heap->SetName(L"DSV Heap");
            break;
        default:
            m_heap->SetName(L"Unknown Heap");
            break;
    }

    return true;
}

bool DescriptorHeap::allocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& outCPUHandle,
                                        D3D12_GPU_DESCRIPTOR_HANDLE& outGPUHandle) {
    if (!m_heap || m_currentDescriptorIndex >= m_numDescriptorsInHeap) {
        outCPUHandle = {0};
        outGPUHandle = {0};
        OutputDebugStringW(L"Error: Descriptor Heap allocation failed (full or not created).\n");
        return false;
    }
    
    CD3DX12_CPU_DESCRIPTOR_HANDLE cpuHandle = {m_startCPU, m_currentDescriptorIndex, m_descriptorSize};
    outCPUHandle = cpuHandle;
    if (m_shaderVisible) {
        CD3DX12_GPU_DESCRIPTOR_HANDLE gpuHandle(m_startGPU, m_currentDescriptorIndex, m_descriptorSize);
        outGPUHandle = gpuHandle;
    } else {
        outGPUHandle = {0}; // Invalid handle if not shader visible
    }

    // Advance the index for the next allocation
    m_currentDescriptorIndex++;
    return true;
}
