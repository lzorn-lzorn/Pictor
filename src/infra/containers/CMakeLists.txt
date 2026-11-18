# InfraContainers — header-only container utilities (Any, LinkedHashTable,
# ObjectPool). Self-contained: depends only on the C++ standard library.
add_library(InfraContainers INTERFACE)

# Expose the src root so consumers include headers with the "infra/" prefix,
# e.g. #include "infra/containers/Any.h".
target_include_directories(InfraContainers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_compile_features(InfraContainers INTERFACE cxx_std_23)
