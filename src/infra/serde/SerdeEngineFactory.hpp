//
// SerdeEngineFactory.hpp — 后端工厂
//
// 按 ESerdeBackend 分派到具体引擎实例。引擎为函数内静态对象, 线程安全;
// 未知后端返回 nullptr。SerdeServer 通过本头文件取引擎。
//

#ifndef INFRA_SERDE_ENGINE_FACTORY_HPP
#define INFRA_SERDE_ENGINE_FACTORY_HPP

#include <infra/serde/SerdeEngine.hpp>
#include <infra/serde/SerdeJsonEngine.hpp>
#include <infra/serde/SerdeTomlEngine.hpp>
#include <infra/serde/SerdeIniEngine.hpp>

namespace infra
{

inline SerdeEngine *getEngineFor(ESerdeBackend Backend)
{
    static SerdeJsonEngine JsonEngine;
    static SerdeTomlEngine TomlEngine;
    static SerdeIniEngine IniEngine;
    switch (Backend)
    {
        case ESerdeBackend::Json: return &JsonEngine;
        case ESerdeBackend::Toml: return &TomlEngine;
        case ESerdeBackend::Ini:  return &IniEngine;
        default:                  return nullptr;
    }
}

} // namespace infra

#endif // INFRA_SERDE_ENGINE_FACTORY_HPP
