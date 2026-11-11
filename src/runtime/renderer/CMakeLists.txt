# Renderer — SDL window + event loop glue, delegating drawing to the Rhi.
add_subdirectory(rhi)

add_library(Renderer STATIC Renderer.cpp)

target_include_directories(Renderer PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

target_link_libraries(Renderer PUBLIC Rhi SDL3::SDL3)

target_compile_features(Renderer PUBLIC cxx_std_23)
