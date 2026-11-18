# InfraSerde — serialization/deserialization utilities (currently the INI
# reader). Depends on the containers module.
add_library(InfraSerde STATIC
        reader/Ini.cpp
)

# Expose the src root so consumers include headers with the "infra/" prefix,
# e.g. #include "infra/serde/reader/Ini.h".
target_include_directories(InfraSerde PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(InfraSerde PUBLIC InfraContainers)
target_compile_features(InfraSerde PUBLIC cxx_std_23)
