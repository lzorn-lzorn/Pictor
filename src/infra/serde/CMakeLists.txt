# InfraSerde — serialization framework: format-agnostic core (SerdeADT /
# SerdeEngine / SerdeReflect / SerdeServer, header-only) plus the INI backend,
# which doubles as the standalone configuration-file parser.
add_library(InfraSerde STATIC
        parser/Ini.cpp
        parser/IniFile.cpp
)

# Expose the src root so consumers include headers with the "infra/" prefix,
# e.g. #include "infra/serde/parser/Ini.hpp" / #include <infra/serde/Serde.hpp>.
target_include_directories(InfraSerde PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/../..)

target_link_libraries(InfraSerde PUBLIC InfraCommon InfraContainers)
target_compile_features(InfraSerde PUBLIC cxx_std_23)

# infra/reflect 的注册宏依赖 __VA_OPT__ 等 C++20 预处理特性
if(MSVC)
    target_compile_options(InfraSerde INTERFACE /Zc:preprocessor)
endif()
