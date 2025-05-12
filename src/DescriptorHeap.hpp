#pragma once
#include <d3d12.h>
#include <wrl/client.h>


class DescriptorHeap {
public:
    DescriptorHeap();

    ~DescriptorHeap();

    bool create(ID3D12Device* device, D3D12_DESCRIPTOR_HEAP_TYPE type, int numDescriptors, bool shaderVisability);

    bool allocateDescriptor(D3D12_CPU_DESCRIPTOR_HANDLE& outCPUHandle,
                            D3D12_GPU_DESCRIPTOR_HANDLE& outGPUHandle);

    // Getters
    ID3D12DescriptorHeap* getHeapPointer() const {
        return m_heap.Get();
    }

    D3D12_GPU_DESCRIPTOR_HANDLE getGPUHeapStart() const {
        return m_startGPU;
    } // Only valid if shaderVisible
    UINT getDescriptorSize() const {
        return m_descriptorSize;
    }

    UINT getCapacity() const {
        return m_numDescriptorsInHeap;
    }

    UINT getCurrentSize() const {
        return m_currentDescriptorIndex;
    } // How many allocated

private:
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> m_heap;
    D3D12_DESCRIPTOR_HEAP_TYPE m_type;
    UINT m_descriptorSize;
    UINT m_numDescriptorsInHeap;
    bool m_shaderVisible;

    D3D12_CPU_DESCRIPTOR_HANDLE m_startCPU;
    D3D12_GPU_DESCRIPTOR_HANDLE m_startGPU; 

    // Simple linear allocator state
    INT m_currentDescriptorIndex;
};
