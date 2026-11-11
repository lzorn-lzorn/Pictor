# GenericPlatform — header-only platform abstraction interface.
add_library(GenericPlatform INTERFACE)

target_include_directories(GenericPlatform INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(GenericPlatform INTERFACE cxx_std_23)
