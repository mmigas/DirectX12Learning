#include "Shader.hpp"
#include <d3dcompiler.h>
#include <d3dx12_core.h>
#include <iostream>

Shader::Shader() {
}

Shader::~Shader() {
}

bool Shader::loadAndCompile(const std::wstring& fileName, const std::string& entryPoint, const std::string& target) {
    UINT compileFlags = 0;

#if defined(_DEBUG) || defined(DEBUG)
    compileFlags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
#else
    compileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
#endif

    HRESULT hr = D3DCompileFromFile(fileName.c_str(),
                                    nullptr,
                                    D3D_COMPILE_STANDARD_FILE_INCLUDE,
                                    entryPoint.c_str(),
                                    target.c_str(),
                                    compileFlags,
                                    0,
                                    &m_shaderBlob,
                                    &m_errorBlob);

    OutputDebugStringW((L"D3DCompileFromFile Result for " + fileName + L": " + std::to_wstring(hr) + L"\n").c_str());

    if (FAILED(hr)) {
        OutputDebugStringW(L"Shader compilation failed!\n");
        if (m_errorBlob) {
            OutputDebugStringA(static_cast<char*>(m_errorBlob->GetBufferPointer()));
            std::cerr << "Shader Compiler Errors:\n" << static_cast<char*>(m_errorBlob->GetBufferPointer()) <<
                    std::endl;
            m_errorBlob.Reset(); // Release error blob
        } else {
            OutputDebugStringW(L"Unknown compilation error.\n");
            std::cerr << "Unknown shader compilation error." << std::endl;
        }
        m_shaderBlob.Reset(); // Ensure shader blob is null on failure
        return false;
    }

    if (!m_shaderBlob || m_shaderBlob->GetBufferSize() == 0) {
        OutputDebugStringW(L"Shader compilation SUCCEEDED but returned empty blob!\n");
        // Treat this as a failure
        m_shaderBlob.Reset(); // Ensure blob is null
        m_errorBlob.Reset(); // Just in case
        return false;
    }
    // --- END OF ADDED CHECK ---

    // Compilation successful and blob looks valid
    OutputDebugStringW(
        (L"Shader compilation SUCCEEDED for " + fileName + L". Size: " + std::to_wstring(m_shaderBlob->GetBufferSize())
         + L"\n").c_str());
    m_errorBlob.Reset(); // Release error blob if it exists
    return true;
}

CD3DX12_SHADER_BYTECODE Shader::getBytecode() const {
    if (m_shaderBlob) {
        return CD3DX12_SHADER_BYTECODE(m_shaderBlob.Get());
    }
    return {};
}
