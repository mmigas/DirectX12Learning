cmake_minimum_required(VERSION 3.30)
project(DirectX12Learning)

set(CMAKE_CXX_STANDARD 20)


set(HEADER_FILES
        libs/stb/stb_image.hpp
        libs/tiny_obj_loader/tiny_obj_loader.h
        src/Application.hpp
        src/Window.hpp
        src/DX12Device.hpp
        src/CommandQueue.hpp
        src/CommandListManager.hpp
        src/SwapChain.hpp
        src/Buffer.hpp
        src/Shader.hpp
        src/RootSignature.hpp
        src/PipelineStateObject.hpp
        src/Camera.hpp
        src/DescriptorHeap.hpp
        src/Texture.hpp
        src/Mesh.hpp
        src/Renderer.hpp
)

set(SRC_FILES
        src/Main.cpp
        src/Application.cpp
        src/Window.cpp
        src/DX12Device.cpp
        src/CommandQueue.cpp
        src/CommandListManager.cpp
        src/SwapChain.cpp
        src/Buffer.cpp
        src/Shader.cpp
        src/RootSignature.cpp
        src/PipelineStateObject.cpp
        src/Camera.cpp
        src/DescriptorHeap.cpp
        src/Texture.cpp
        src/Mesh.cpp
        src/Renderer.cpp
)

add_subdirectory(libs/DirectX-Headers)
add_subdirectory(libs/glm/glm)
add_executable(DirectX12Learning WIN32
        ${HEADER_FILES}
        ${SRC_FILES}
)


target_link_libraries(DirectX12Learning
        Microsoft::DirectX-Headers
        Microsoft::DirectX-Guids
        d3d12
        dxgi
        d3dcompiler
        glm::glm
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/dxcCompiler/lib/dxcompiler.lib"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/dxcCompiler/lib/dxil.lib"
)

target_include_directories(DirectX12Learning PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/DirectX-Headers/include/directx"
        "${CMAKE_CURRENT_SOURCE_DIR}/libs/DirectX-Headers/include/dxguids"
        "${CMAKE_SOURCE_DIR}/libs/glm"
        "${CMAKE_SOURCE_DIR}/dxcCompiler/include"
)

# Copy shaders to output directory

file(GLOB_RECURSE SHADER_FILES "${CMAKE_CURRENT_SOURCE_DIR}/*.hlsl")
foreach (SHADER_FILE ${SHADER_FILES})
    get_filename_component(SHADER_NAME ${SHADER_FILE} NAME)
    add_custom_command(
            TARGET DirectX12Learning POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SHADER_FILE}"
            $<TARGET_FILE_DIR:DirectX12Learning>/${SHADER_NAME}
    )

endforeach ()
