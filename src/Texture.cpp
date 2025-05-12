#include "Texture.hpp"

#include <stdexcept>

#include "d3dx12_barriers.h"
#include "d3dx12_core.h"
#include "d3dx12_resource_helpers.h"
#include "../libs/stb/stb_image.hpp"

using namespace Microsoft::WRL;

Texture::Texture() : m_srvHandleCPU({0}),
                     m_srvHandleGPU({0}),
                     m_width(0),
                     m_height(0),
                     m_format(DXGI_FORMAT_UNKNOWN) {
}

Texture::~Texture() {
}

ComPtr<ID3D12Resource> Texture::LoadFromFile(ID3D12Device* device,
                                             ID3D12GraphicsCommandList* commandList,
                                             DescriptorHeap* descriptorHeap,
                                             const std::wstring& filename,
                                             const std::string& name) {
    if (!device || !commandList || !descriptorHeap || filename.empty()) {
        throw std::invalid_argument("Invalid arguments for Texture::LoadFromFile");
    }

    m_name = name;

    // --- 1. Load Image Data (stb_image) ---
    int width, height, channels;
    size_t convertedChars = 0;
    char narrowFilename[MAX_PATH];
    wcstombs_s(&convertedChars, narrowFilename, sizeof(narrowFilename), filename.c_str(), _TRUNCATE);

    unsigned char* pixels = stbi_load(narrowFilename, &width, &height, &channels, STBI_rgb_alpha);
    if (!pixels) {
        throw std::runtime_error("Failed to load texture file: " + std::string(narrowFilename));
    }
    m_width = static_cast<UINT>(width);
    m_height = static_cast<UINT>(height);
    m_format = DXGI_FORMAT_R8G8B8A8_UNORM;
    UINT imageSize = static_cast<UINT>(width * height * 4);

    // --- 2. Create Texture Resource (Default Heap) ---
    m_currentState = D3D12_RESOURCE_STATE_COPY_DEST;
    D3D12_RESOURCE_DESC textureDesc = CD3DX12_RESOURCE_DESC::Tex2D(m_format, m_width, m_height, 1, 1);
    auto defaultHeapPros = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
    HRESULT hr = device->CreateCommittedResource(
        &defaultHeapPros,
        D3D12_HEAP_FLAG_NONE,
        &textureDesc,
        m_currentState,
        nullptr,
        IID_PPV_ARGS(&m_textureResource));
    if (FAILED(hr)) {
        stbi_image_free(pixels);
        throw std::runtime_error("Failed to create texture resource");
    }
    m_textureResource->SetName(filename.c_str()); // Use filename as debug name

    ComPtr<ID3D12Resource> uploadBuffer;
    UINT64 uploadBufferSize = 0;
    device->GetCopyableFootprints(&textureDesc, 0, 1, 0, nullptr, nullptr, nullptr, &uploadBufferSize);

    // --- 3. Create Upload Buffer and Copy Data ---
    auto uploadHeapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
    auto uploadBufferDesc = CD3DX12_RESOURCE_DESC::Buffer(uploadBufferSize);
    hr = device->CreateCommittedResource(
        &uploadHeapProps,
        D3D12_HEAP_FLAG_NONE,
        &uploadBufferDesc,
        D3D12_RESOURCE_STATE_GENERIC_READ,
        nullptr,
        IID_PPV_ARGS(&uploadBuffer));

    if (FAILED(hr)) {
        stbi_image_free(pixels);
        throw std::runtime_error("Failed to create upload buffer");
    }
    uploadBuffer->SetName((filename + L" Upload Buffer").c_str()); // Use filename as debug name

    D3D12_SUBRESOURCE_DATA textureData;
    textureData.pData = pixels;
    textureData.RowPitch = static_cast<LONG_PTR>(m_width) * 4;
    textureData.SlicePitch = static_cast<LONG_PTR>(imageSize);

    UpdateSubresources(commandList, m_textureResource.Get(), uploadBuffer.Get(), 0, 0, 1, &textureData);
    D3D12_RESOURCE_STATES finalStateAfterLoad = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    TransitionToState(commandList, finalStateAfterLoad); // Use the new method


    // --- 4. Create Shader Resource View (SRV) ---
    if (!descriptorHeap->allocateDescriptor(m_srvHandleCPU, m_srvHandleGPU)) {
        stbi_image_free(pixels);
        throw std::runtime_error("Failed to allocate descriptor for texture");
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = m_format;
    srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(m_textureResource.Get(), &srvDesc, m_srvHandleCPU);

    stbi_image_free(pixels);

    return uploadBuffer;
}

void Texture::TransitionToState(
    ID3D12GraphicsCommandList* pCmdList,
    D3D12_RESOURCE_STATES targetState)
{
    if (!m_textureResource || !pCmdList) return;

    if (m_currentState != targetState) {
        auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(
            m_textureResource.Get(),
            m_currentState, // Current actual state
            targetState     // Desired state
        );
        pCmdList->ResourceBarrier(1, &barrier);
        m_currentState = targetState; // Update tracked state
    }
}
