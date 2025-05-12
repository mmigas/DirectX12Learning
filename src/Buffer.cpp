#include "Buffer.hpp"

#include <d3dx12_barriers.h>
#include <d3dx12_core.h>
#include <stdexcept>

using namespace Microsoft::WRL;

Buffer::Buffer() : m_size(0), m_alignedSize(0), m_heapType(D3D12_HEAP_TYPE_DEFAULT), m_mappedData(nullptr) {
}

Buffer::~Buffer() {
    if (m_mappedData) {
        unmap();
    }
}

bool Buffer::create(ID3D12Device* device,
                    size_t size,
                    D3D12_HEAP_TYPE heapType,
                    D3D12_RESOURCE_STATES initialState,
                    bool isConstantBuffer,
                    D3D12_RESOURCE_FLAGS flags) {
    if (!device || size == 0) {
        return false;
    }

    m_size = size;
    m_heapType = heapType;
    m_alignedSize = isConstantBuffer ? alignUp(size, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT) : size;

    auto heapProperties = CD3DX12_HEAP_PROPERTIES(heapType);
    auto bufferDescription = CD3DX12_RESOURCE_DESC::Buffer(m_alignedSize, flags);

    D3D12_RESOURCE_STATES actualInitialState = (heapType == D3D12_HEAP_TYPE_UPLOAD)
                                                   ? D3D12_RESOURCE_STATE_GENERIC_READ
                                                   : initialState;

    HRESULT hr = device->CreateCommittedResource(
        &heapProperties,
        D3D12_HEAP_FLAG_NONE,
        &bufferDescription,
        actualInitialState,
        nullptr,
        IID_PPV_ARGS(&m_resource));

    if (FAILED(hr)) {
        OutputDebugStringW(L"Error: Failed to create committed resource (Buffer).\n");
        m_resource.Reset();
        m_size = 0;
        return false;
    }

    return true;
}

ComPtr<ID3D12Resource> Buffer::createAndUploadDefaultBuffer(ID3D12Device* device,
                                                            ID3D12GraphicsCommandList* cmdList,
                                                            const void* data, size_t size,
                                                            D3D12_RESOURCE_STATES finalState) {
    if (!device || !cmdList || !data || size == 0) {
        throw std::invalid_argument("Invalid arguments provided to createAndUploadDefaultBuffer.");
    }

    // 1. Create the default buffer (target)
    if (!create(device, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COPY_DEST, false,
                D3D12_RESOURCE_FLAG_NONE)) {
        throw std::runtime_error("Failed to create default buffer for upload.");
    }
    m_resource->SetName(L"Default Buffer (Target)");

    // 2. Create the upload buffer (intermediate)
    ComPtr<ID3D12Resource> uploadBuffer;
    auto uploadHeapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uploadBufferDescription = CD3DX12_RESOURCE_DESC::Buffer(size);

    HRESULT hr = device->CreateCommittedResource(
        &uploadHeapProperties,
        D3D12_HEAP_FLAG_NONE,
        &uploadBufferDescription,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer));
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to create upload buffer for initialization.");
    }
    uploadBuffer->SetName(L"Upload Buffer (Intermediate)");

    // 3. Map, Copy data to upload buffer, unmap
    void* mappedData = nullptr;
    hr = uploadBuffer->Map(0, nullptr, &mappedData);
    if (FAILED(hr)) {
        throw std::runtime_error("Failed to map upload buffer.");
    }
    memcpy(mappedData, data, size);
    uploadBuffer->Unmap(0, nullptr);

    // 4. Record command to copy from upload buffer to default buffer
    cmdList->CopyBufferRegion(
        m_resource.Get(), // Destination (default buffer)
        0, // Dest offset
        uploadBuffer.Get(), // Source (upload buffer)
        0, // Source offset
        size // Size to copy
    );

    // 5. Record barrier to transition the default buffer to the final state
    auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
        m_resource.Get(),
        D3D12_RESOURCE_STATE_COPY_DEST,
        finalState
    );
    cmdList->ResourceBarrier(1, &barrier);

    return uploadBuffer;
}

void* Buffer::map() {
    if (m_heapType != D3D12_HEAP_TYPE_UPLOAD && m_heapType != D3D12_HEAP_TYPE_READBACK) {
        OutputDebugStringW(L"Warning: Mapping buffer not on upload or readback heap.\n");
    }
    if (!m_resource || m_mappedData) {
        return m_mappedData;
    }

    D3D12_RANGE readRange = {}; // We don't intend to read from CPU side for Upload heap
    HRESULT hr = m_resource->Map(0, &readRange, &m_mappedData);
    if (FAILED(hr)) {
        OutputDebugStringW(L"Error: Failed to map buffer.\n");
        m_mappedData = nullptr;
        // Or throw exception
    }
    return m_mappedData;
}

void Buffer::unmap(size_t writtenSize) {
    if (m_resource && m_mappedData) {
        D3D12_RANGE* writtenRangePtr = nullptr;
        D3D12_RANGE writtenRange = {};
        if (writtenSize != static_cast<size_t>(-1) && writtenSize <= m_alignedSize) {
            writtenRange.Begin = 0;
            writtenRange.End = writtenSize;
            writtenRangePtr = &writtenRange;
        }
        m_resource->Unmap(0, writtenRangePtr);
        m_mappedData = nullptr;
    } else {
        OutputDebugStringW(L"Warning: Attempting to unmap buffer that is not mapped.\n");
    }
}

D3D12_GPU_VIRTUAL_ADDRESS Buffer::getGPUVirtualAddress() const {
    return m_resource ? m_resource->GetGPUVirtualAddress() : 0;
}

D3D12_VERTEX_BUFFER_VIEW Buffer::getVertexBufferView(UINT stride) const {
    D3D12_VERTEX_BUFFER_VIEW vbv = {};
    vbv.BufferLocation = getGPUVirtualAddress();
    vbv.SizeInBytes = static_cast<UINT>(m_size);
    vbv.StrideInBytes = stride;
    return vbv;
}

D3D12_INDEX_BUFFER_VIEW Buffer::getIndexBufferView(DXGI_FORMAT format) const {
    D3D12_INDEX_BUFFER_VIEW ibv = {};
    ibv.BufferLocation = getGPUVirtualAddress();
    ibv.SizeInBytes = static_cast<UINT>(m_size);
    ibv.Format = format; // Typically DXGI_FORMAT_R16_UINT or DXGI_FORMAT_R32_UINT
    return ibv;
}
