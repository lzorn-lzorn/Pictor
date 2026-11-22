
add_library(InfraContainers INTERFACE
        LinkedHashTable.hpp
        Any.hpp
        IntrusiveRBTree.hpp
)

target_include_directories(InfraContainers INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/../..)
target_compile_features(InfraContainers INTERFACE cxx_std_23)

target_link_libraries(InfraContainers INTERFACE InfraCommon)
