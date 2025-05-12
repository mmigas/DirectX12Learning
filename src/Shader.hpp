#pragma once
#include <d3d12.h>
#include <d3dx12_core.h>
#include <wrl/client.h> // ComPtr
#include <string>


class Shader {
public:
    Shader();

    ~Shader();

    bool loadAndCompile(const std::wstring& fileName, const std::string& entryPoint, const std::string& target);

    // Getters
    ID3DBlob* getBlob() const {
        return m_shaderBlob.Get();
    }

    CD3DX12_SHADER_BYTECODE getBytecode() const;

private:
    Microsoft::WRL::ComPtr<ID3DBlob> m_shaderBlob;
    Microsoft::WRL::ComPtr<ID3DBlob> m_errorBlob; // To store compilation errors
};
