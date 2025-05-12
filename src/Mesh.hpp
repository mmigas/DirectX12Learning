#pragma once
#include <d3d12.h>
#include <memory>
#include <string>
#include <utility>
#include <wrl/client.h>

#include "Buffer.hpp"
#include "glm/glm.hpp"

struct Vertex {
    glm::vec3 position;
    glm::vec4 color;
    glm::vec2 texCoord;
    glm::vec3 normal;
};

class Mesh {
public:
    Mesh();

    ~Mesh();

    std::pair<Microsoft::WRL::ComPtr<ID3D12Resource>, Microsoft::WRL::ComPtr<ID3D12Resource>> LoadFromObjFile(
        ID3D12Device* pDevice,
        ID3D12GraphicsCommandList* pCmdList,
        const std::string& filename // Use std::string for tinyobj compatibility
    );

    void setupInputAssembler(ID3D12GraphicsCommandList* commandList) const;

    void draw(ID3D12GraphicsCommandList* commandList, UINT instanceCount) const;

    ID3D12Resource* getVertexBufferResource() const {
        return m_vertexBuffer ? m_vertexBuffer->getResource() : nullptr;
    }

    ID3D12Resource* getIndexBufferResource() const {
        return m_indexBuffer ? m_indexBuffer->getResource() : nullptr;
    }

    D3D12_GPU_VIRTUAL_ADDRESS getVertexBufferGPUVirtualAddress() const {
        return m_vertexBufferView.BufferLocation;
    }

    D3D12_GPU_VIRTUAL_ADDRESS getIndexBufferGPUVirtualAddress() const {
        return m_indexBufferView.BufferLocation;
    }

    UINT getVertexStride() const {
        return m_vertexStride;
    }

    UINT getVertexCount() const {
        return m_vertexCount;
    }

    UINT getIndexCount() const {
        return m_indexCount;
    }

    DXGI_FORMAT getIndexFormat() const {
        return m_indexFormat;
    }

private:
    std::unique_ptr<Buffer> m_vertexBuffer;
    std::unique_ptr<Buffer> m_indexBuffer; // Can be nullptr if not indexed

    D3D12_VERTEX_BUFFER_VIEW m_vertexBufferView;
    D3D12_INDEX_BUFFER_VIEW m_indexBufferView; // Only valid if m_indexBuffer exists

    UINT m_vertexCount; // Needed if drawing non-indexed
    UINT m_indexCount; // Number of indices to draw
    UINT m_vertexStride;
    DXGI_FORMAT m_indexFormat;
    D3D12_PRIMITIVE_TOPOLOGY m_topology;
};
