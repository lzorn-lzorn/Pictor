# Rhi — the Vulkan render hardware interface.
add_library(Rhi STATIC Rhi.cpp)

target_include_directories(Rhi
    PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}
    PRIVATE ${SHADER_GENERATED_DIR}
)

# vulkan.hpp / vulkan_raii.hpp (via Vulkan::Headers) and SDL_video.h are part
# of Rhi.h's interface, so they propagate to consumers.
target_link_libraries(Rhi PUBLIC Vulkan::Headers SDL3::SDL3)

target_compile_features(Rhi PUBLIC cxx_std_23)

# The embedded shader headers must exist before Rhi.cpp is compiled.
add_dependencies(Rhi shaders)
