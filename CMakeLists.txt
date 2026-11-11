cmake_minimum_required(VERSION 3.28)

# Hello Vulkan — a minimal Vulkan 1.0 triangle rendered into an SDL3 window.
project(HelloVulkan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)

# ---------------------------------------------------------------------------
# SDL3 (vendored under external/SDL)
# ---------------------------------------------------------------------------
# Build only the shared SDL3 library, skip tests/examples/docs for speed.
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_INSTALL OFF CACHE BOOL "" FORCE)
add_subdirectory(external/SDL EXCLUDE_FROM_ALL)

# ---------------------------------------------------------------------------
# Vulkan — headers (vulkan.h / vulkan.hpp / vulkan_raii.hpp), pinned tag.
#
# No Vulkan SDK is required: vk::raii loads the runtime loader (vulkan-1.dll,
# shipped by the GPU driver) dynamically, so we only need the headers here.
# ---------------------------------------------------------------------------
set(VULKAN_HEADERS_TAG "vulkan-sdk-1.4.350.0")

FetchContent_Declare(Vulkan-Headers
    GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
    GIT_TAG        ${VULKAN_HEADERS_TAG}
    GIT_SHALLOW    TRUE
)
FetchContent_MakeAvailable(Vulkan-Headers)

# ---------------------------------------------------------------------------
# DXC (DirectX Shader Compiler) — prebuilt Windows binary, pinned release.
#
# A prebuilt package (dxc.exe + dxcompiler.dll) is downloaded and extracted
# once at configure time; the hash pins the exact artifact. Set -DDXC_EXECUTABLE
# to a local dxc.exe to skip the download.
# ---------------------------------------------------------------------------
set(DXC_VERSION "v1.9.2609")
set(DXC_URL "https://github.com/microsoft/DirectXShaderCompiler/releases/download/${DXC_VERSION}/dxc_2026_09_29.zip")
set(DXC_SHA256 "ad31b1fc8443175d204f77a611fdb3ef2ec42759bdc2f1167368de24a4a7e7f1")

set(DXC_EXECUTABLE "" CACHE FILEPATH "Path to dxc.exe (empty = auto-download a pinned release)")

if(NOT DXC_EXECUTABLE)
    set(DXC_EXTRACT_DIR "${CMAKE_BINARY_DIR}/_dxc")
    if(NOT EXISTS "${DXC_EXTRACT_DIR}")
        file(DOWNLOAD "${DXC_URL}" "${CMAKE_BINARY_DIR}/dxc.zip"
             EXPECTED_HASH "SHA256=${DXC_SHA256}"
             STATUS download_status)
        list(GET download_status 0 download_code)
        if(NOT download_code EQUAL 0)
            message(FATAL_ERROR "Failed to download DXC from ${DXC_URL}: ${download_status}")
        endif()
        file(MAKE_DIRECTORY "${DXC_EXTRACT_DIR}")
        file(ARCHIVE_EXTRACT INPUT "${CMAKE_BINARY_DIR}/dxc.zip" DESTINATION "${DXC_EXTRACT_DIR}")
    endif()

    # The package ships bin/{arm64,x64,x86}/dxc.exe; pick the host architecture.
    if(CMAKE_SIZEOF_VOID_P EQUAL 8)
        set(_dxc_arch "x64")
    else()
        set(_dxc_arch "x86")
    endif()
    set(DXC_EXECUTABLE "${DXC_EXTRACT_DIR}/bin/${_dxc_arch}/dxc.exe")
    if(NOT EXISTS "${DXC_EXECUTABLE}")
        file(GLOB_RECURSE DXC_EXE_CANDIDATES "${DXC_EXTRACT_DIR}/dxc.exe")
        if(NOT DXC_EXE_CANDIDATES)
            message(FATAL_ERROR "dxc.exe not found in the downloaded DXC package.")
        endif()
        list(GET DXC_EXE_CANDIDATES 0 DXC_EXECUTABLE)
    endif()
endif()

# ---------------------------------------------------------------------------
# Shaders: compile HLSL -> SPIR-V -> embed as C++ headers
# ---------------------------------------------------------------------------
set(SHADER_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/shaders)
set(SHADER_GENERATED_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/shaders)
file(MAKE_DIRECTORY ${SHADER_GENERATED_DIR})

set(SHADER_FILES
    triangle.vert.hlsl
    triangle.frag.hlsl
)

set(GENERATED_SHADER_HEADERS "")

foreach(shader IN LISTS SHADER_FILES)
    # "triangle.vert.hlsl" -> "triangle.vert" -> "triangle_vert"
    string(REPLACE ".hlsl" "" shader_stem "${shader}")
    string(REPLACE "." "_" shader_var "${shader_stem}")

    # Choose the DXC target profile from the shader stage.
    if(shader MATCHES "vert")
        set(dxc_profile "vs_6_0")
    elseif(shader MATCHES "frag")
        set(dxc_profile "ps_6_0")
    else()
        message(FATAL_ERROR "Cannot infer shader stage from '${shader}'.")
    endif()

    set(spv    ${SHADER_GENERATED_DIR}/${shader_var}.spv)
    set(header ${SHADER_GENERATED_DIR}/${shader_var}_spv.h)

    add_custom_command(
        OUTPUT ${spv}
        COMMAND ${DXC_EXECUTABLE} -spirv -T ${dxc_profile} -E main -Fo ${spv}
                ${SHADER_SOURCE_DIR}/${shader}
        DEPENDS ${SHADER_SOURCE_DIR}/${shader}
        COMMENT "Compiling ${shader} to SPIR-V (DXC, ${dxc_profile})"
        VERBATIM
    )

    add_custom_command(
        OUTPUT ${header}
        COMMAND ${CMAKE_COMMAND}
                -DINPUT=${spv}
                -DOUTPUT=${header}
                -DNAME=${shader_var}_spv
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/spv_to_header.cmake
        DEPENDS ${spv} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/spv_to_header.cmake
        COMMENT "Embedding ${shader} SPIR-V into ${shader_var}_spv.h"
        VERBATIM
    )

    list(APPEND GENERATED_SHADER_HEADERS ${header})
endforeach()

add_custom_target(shaders ALL DEPENDS ${GENERATED_SHADER_HEADERS})

# ---------------------------------------------------------------------------
# Subsystems
# ---------------------------------------------------------------------------
add_subdirectory(src/runtime/platform)
add_subdirectory(src/runtime/renderer)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(HelloVulkan src/main.cpp)

if(MSVC)
    target_compile_options(HelloVulkan PRIVATE /W4 /utf-8)
else()
    target_compile_options(HelloVulkan PRIVATE -Wall -Wextra)
endif()

target_link_libraries(HelloVulkan PRIVATE Renderer GenericPlatform)

# Copy the SDL3 runtime DLL next to the executable so it runs out of the box.
if(WIN32)
    add_custom_command(TARGET HelloVulkan POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_FILE:SDL3::SDL3>
                $<TARGET_FILE_DIR:HelloVulkan>
        COMMENT "Copying SDL3.dll next to the executable"
        VERBATIM
    )
endif()

# Place the final binary in a predictable location.
set_target_properties(HelloVulkan PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
