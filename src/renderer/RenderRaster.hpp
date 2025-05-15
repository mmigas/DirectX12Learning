#pragma once
#include "BaseRenderer.hpp"
#include "src/PipelineStateObject.hpp"
#include "src/RootSignature.hpp"


class RenderRaster final : public BaseRenderer {
public:
    RenderRaster();

    ~RenderRaster() override;

    bool init(DX12Device* pDevice, CommandQueue* pCommandQueue, SwapChain* pSwapChain, UINT numFrames) override;

    void shutdown() override;

private:
    std::unique_ptr<RootSignature> m_rootSignature;
    std::unique_ptr<PipelineStateObject> m_pipelineState;

    bool createRootSignature();

    bool createPipelineStateObject();

    void renderVariant(float deltaTime, Camera* camera, Mesh* mesh, Texture* texture, ID3D12GraphicsCommandList* commandList) override;
};
