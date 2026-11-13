# Infra — header-only foundation utilities (containers, tracing).
#
# A self-contained module that depends only on the C++ standard library.
add_library(Infra INTERFACE)

target_include_directories(Infra INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(Infra INTERFACE cxx_std_23)
