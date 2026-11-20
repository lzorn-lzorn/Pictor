# InfraTracer — header-only tracing/benchmarking utilities (Benchmark).
# Depends on the containers module.
add_library(InfraTracer INTERFACE)

# Expose the src root so consumers include headers with the "infra/" prefix,
# e.g. #include "infra/tracer/Benchmark.h".
target_include_directories(InfraTracer INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(InfraTracer INTERFACE InfraCommon InfraContainers)
target_compile_features(InfraTracer INTERFACE cxx_std_23)
