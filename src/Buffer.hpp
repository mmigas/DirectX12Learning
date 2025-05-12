#pragma once
#include <d3d12.h>
#include <wrl/client.h>

// Helper function to calculate aligned buffer sizes
inline size_t alignUp(size_t size, size_t alignment) {
    return (size + alignment - 1) & ~(alignment - 1);
}


class Buffer {
public:
    Buffer();

    ~Buffer();

    // Creates a committed resource (buffer) on the GPU.
    bool create(ID3D12Device* device, // D3D12 device
                size_t size, // Size of the buffer in bytes
                D3D12_HEAP_TYPE heapType, // Memory heap type (Default, Upload, Readback)
                D3D12_RESOURCE_STATES initialState,
                // The starting resource state (e.g., Common, Generic_Read, Copy_Dest)
                bool isConstantBuffer = false, // If true, size will be aligned to 256 bytes
                D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE // Optional resource flags
    );

    // Helper to create a default heap buffer and upload data via an upload heap.
    Microsoft::WRL::ComPtr<ID3D12Resource> createAndUploadDefaultBuffer(
        ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, const void* data, size_t size,
        D3D12_RESOURCE_STATES finalState);

    void* map();

    void unmap(size_t writtenSize = -1);

    ID3D12Resource* getResource() const {
        return m_resource.Get();
    }

    size_t getSize() const {
        return m_size;
    }

    size_t getAlignedSize() const {
        return m_alignedSize;
    }

    D3D12_GPU_VIRTUAL_ADDRESS getGPUVirtualAddress() const;

    // --- Views (Specific to buffer type) ---
    D3D12_VERTEX_BUFFER_VIEW getVertexBufferView(UINT stride) const;

    D3D12_INDEX_BUFFER_VIEW getIndexBufferView(DXGI_FORMAT format = DXGI_FORMAT_R32_UINT) const;

private:
    Microsoft::WRL::ComPtr<ID3D12Resource> m_resource;
    size_t m_size = 0;
    size_t m_alignedSize = 0; // Aligned size for constant buffer alignment
    D3D12_HEAP_TYPE m_heapType;
    void* m_mappedData = nullptr;
};
