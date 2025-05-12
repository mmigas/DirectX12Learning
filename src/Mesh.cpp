#include "Mesh.hpp"

#include <iostream>
#include <stdexcept>
#include <unordered_map>

#define TINYOBJLOADER_IMPLEMENTATION 
#include "libs/tiny_obj_loader/tiny_obj_loader.h"
using namespace Microsoft::WRL;

namespace std {
    template<>
    struct hash<tinyobj::index_t> {
        size_t operator()(const tinyobj::index_t& k) const noexcept {
            size_t h1 = std::hash<int>()(k.vertex_index);
            size_t h2 = std::hash<int>()(k.normal_index);
            size_t h3 = std::hash<int>()(k.texcoord_index);
            // Combine hashes (boost::hash_combine style)
            size_t seed = 0;
            seed ^= h1 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= h2 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            seed ^= h3 + 0x9e3779b9 + (seed << 6) + (seed >> 2);
            return seed;
        }
    };
}

namespace tinyobj {
    inline bool operator==(const index_t& a, const index_t& b) {
        return a.vertex_index == b.vertex_index &&
               a.normal_index == b.normal_index &&
               a.texcoord_index == b.texcoord_index;
    }
}

Mesh::Mesh() {
}

Mesh::~Mesh() {
}

std::pair<ComPtr<ID3D12Resource>, ComPtr<ID3D12Resource>> Mesh::LoadFromObjFile(
    ID3D12Device* device,
    ID3D12GraphicsCommandList* commandList,
    const std::string& filename) {
    if (!device || !commandList || filename.empty()) {
        throw std::invalid_argument("Invalid arguments for Mesh::LoadFromObjFile");
    }

    ComPtr<ID3D12Resource> vbUploadBuffer = nullptr;
    ComPtr<ID3D12Resource> ibUploadBuffer = nullptr;

    tinyobj::attrib_t attrib;
    std::vector<tinyobj::shape_t> shapes;
    std::vector<tinyobj::material_t> materials;
    std::string warn, err;

    bool ret = tinyobj::LoadObj(&attrib, &shapes, &materials, &warn, &err, filename.c_str());
    if (!warn.empty()) {
        std::cout << "TinyObj Warning: " << warn << std::endl;
        OutputDebugStringA(("TinyObj Warning: " + warn + "\n").c_str());
    }
    if (!err.empty()) {
        std::cerr << "TinyObj Error: " << err << std::endl;
        OutputDebugStringA(("TinyObj Error: " + err + "\n").c_str());
    }
    if (!ret) {
        throw std::runtime_error("Failed to load OBJ file: " + filename);
    }

    // --- Process vertices and indices ---
    std::vector<Vertex> finalVertices;
    std::vector<uint32_t> finalIndices; // Use 32-bit indices
    std::unordered_map<tinyobj::index_t, uint32_t> uniqueVertices;

    for (const auto& shape: shapes) {
        for (const auto& index: shape.mesh.indices) {
            // Check if this combination of pos/norm/uv index is already added
            if (uniqueVertices.count(index) == 0) {
                // If not found, create a new Vertex and add it
                uniqueVertices[index] = static_cast<uint32_t>(finalVertices.size());

                Vertex vertex = {};
                // Position
                vertex.position = {
                    attrib.vertices[3 * index.vertex_index + 0],
                    attrib.vertices[3 * index.vertex_index + 1],
                    attrib.vertices[3 * index.vertex_index + 2]
                };
                // Normals (check if present)
                if (index.normal_index >= 0) {
                    vertex.normal = {
                        attrib.normals[3 * index.normal_index + 0],
                        attrib.normals[3 * index.normal_index + 1],
                        attrib.normals[3 * index.normal_index + 2]
                    };
                } else {
                    // Provide default normal if none exists (e.g., facing up)
                    vertex.normal = {0.0f, 1.0f, 0.0f};
                }
                // Texture Coordinates (check if present)
                if (index.texcoord_index >= 0) {
                    vertex.texCoord = {
                        attrib.texcoords[2 * index.texcoord_index + 0],
                        // OBJ UVs often have Y inverted compared to DX, flip it
                        1.0f - attrib.texcoords[2 * index.texcoord_index + 1]
                    };
                } else {
                    vertex.texCoord = {0.0f, 0.0f}; // Default UV
                }
                // Color (set default or potentially load from material later)
                vertex.color = {1.0f, 1.0f, 1.0f, 1.0f}; // Default white

                finalVertices.push_back(vertex);
            }
            // Add index (either newly created or existing) to final index list
            finalIndices.push_back(uniqueVertices[index]);
        }
    }

    // --- Create Vertex Buffer ---
    if (finalVertices.empty()) {
        throw std::runtime_error("Failed to load OBJ file: " + filename);
    }

    m_vertexBuffer = std::make_unique<Buffer>();
    vbUploadBuffer = m_vertexBuffer->createAndUploadDefaultBuffer(
        device, commandList,
        finalVertices.data(), finalVertices.size() * sizeof(Vertex),
        D3D12_RESOURCE_STATE_VERTEX_AND_CONSTANT_BUFFER
    );

    if (!m_vertexBuffer->getResource()) {
        throw std::runtime_error("Failed to create vertex buffer upload resource");
    }
    m_vertexBuffer->getResource()->SetName((L"Mesh VB: " + std::wstring(filename.begin(), filename.end())).c_str());
    m_vertexStride = sizeof(Vertex);
    m_vertexCount = static_cast<UINT>(finalVertices.size());
    m_vertexBufferView = m_vertexBuffer->getVertexBufferView(m_vertexStride);

    // --- Create Index Buffer ---
    if (finalIndices.empty()) {
        throw std::runtime_error("No indices loaded from OBJ file: " + filename);
    }
    m_indexBuffer = std::make_unique<Buffer>();
    ibUploadBuffer = m_indexBuffer->createAndUploadDefaultBuffer(
        device, commandList, finalIndices.data(), finalIndices.size() * sizeof(uint32_t),
        D3D12_RESOURCE_STATE_INDEX_BUFFER
    );
    if (!m_indexBuffer->getResource()) {
        throw std::runtime_error("Failed to create mesh index buffer from OBJ.");
    }
    m_indexBuffer->getResource()->SetName((L"Mesh IB: " + std::wstring(filename.begin(), filename.end())).c_str());
    m_indexFormat = DXGI_FORMAT_R32_UINT; // Using 32-bit indices
    m_indexCount = static_cast<UINT>(finalIndices.size());
    m_indexBufferView = m_indexBuffer->getIndexBufferView(m_indexFormat);

    m_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST; // Assuming triangles

    // Return upload buffers for lifetime management by caller
    return {vbUploadBuffer, ibUploadBuffer};
}

void Mesh::setupInputAssembler(ID3D12GraphicsCommandList* commandList) const {
    if (!commandList || !m_vertexBuffer || !m_indexBuffer) {
        return;
    }
    commandList->IASetPrimitiveTopology(m_topology);
    if (m_vertexBuffer) {
        commandList->IASetVertexBuffers(0, 1, &m_vertexBufferView);
    }
    if (m_indexBuffer) {
        commandList->IASetIndexBuffer(&m_indexBufferView);
    }
}

void Mesh::draw(ID3D12GraphicsCommandList* commandList, UINT instanceCount = 1) const {
    if (!commandList || instanceCount == 0) return;

    // Assumes IA state is already set via SetupInputAssembler

    if (m_indexBuffer) {
        // Draw indexed
        commandList->DrawIndexedInstanced(m_indexCount, instanceCount, 0, 0, 0);
    } else if (m_vertexBuffer) {
        // Draw non-indexed (less common for complex meshes)
        commandList->DrawInstanced(m_vertexCount, instanceCount, 0, 0);
    }
}
