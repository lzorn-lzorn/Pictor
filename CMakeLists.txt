cmake_minimum_required(VERSION 3.28)

# Hello Vulkan — a minimal Vulkan 1.0 triangle rendered into an SDL3 window.
project(HelloVulkan LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Modules
# ---------------------------------------------------------------------------
# Each module is a self-contained CMake directory that owns its own build
# logic and dependencies. The renderer module vendors SDL3, fetches the Vulkan
# headers and compiles the HLSL shaders internally; this file only wires the
# public module targets together.
add_subdirectory(src/infra)
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

# Copy the runtime DLLs of the executable's transitive dependencies (e.g.
# SDL3.dll, supplied by the renderer module) next to it, so it runs out of the
# box. This is generic: the root does not know which DLLs the modules need.
if(WIN32)
    add_custom_command(TARGET HelloVulkan POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                $<TARGET_RUNTIME_DLLS:HelloVulkan>
                $<TARGET_FILE_DIR:HelloVulkan>
        COMMAND_EXPAND_LISTS
        COMMENT "Copying runtime DLLs next to the executable"
        VERBATIM
    )
endif()

# Place the final binary in a predictable location.
set_target_properties(HelloVulkan PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin
)
