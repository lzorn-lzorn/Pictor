# InfraCommon — shared low-level building blocks used across the infra modules
# (e.g. SIMDControlByte). Header-only, no dependencies.
add_library(InfraCommon INTERFACE
        SIMDControlByte.hpp
)

# Expose the src root so consumers include headers with the "infra/" prefix,
# e.g. #include <infra/common/SIMDControlByte.hpp>.
target_include_directories(InfraCommon INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_compile_features(InfraCommon INTERFACE cxx_std_23)
