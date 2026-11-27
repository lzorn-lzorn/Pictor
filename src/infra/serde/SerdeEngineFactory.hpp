//
// SerdeEngineFactory.hpp — 后端工厂
//
// 按 ESerdeBackend 分派到具体引擎实例。引擎为函数内静态对象, 线程安全;
// 未知后端返回 nullptr。SerdeServer 通过本头文件取引擎。
//
// C++11 兼容(2026/11 重构): TOML 引擎依赖 vendored toml++(要求 C++17),
// C++11 下不注册(对应 ESerdeBackend::Toml 落入默认分支返回 nullptr)。
//

#ifndef INFRA_SERDE_ENGINE_FACTORY_HPP
#define INFRA_SERDE_ENGINE_FACTORY_HPP

#include <infra/common/VersionCompatibility.hpp>
#include <infra/serde/SerdeEngine.hpp>
#include <infra/serde/SerdeJsonEngine.hpp>
#include <infra/serde/SerdeTomlEngine.hpp>
#include <infra/serde/SerdeIniEngine.hpp>

namespace infra{
inline SerdeEngine *getEngineFor(ESerdeBackend Backend) {
    static SerdeJsonEngine JsonEngine;
#if PICTOR_HAS_CPP17
    static SerdeTomlEngine TomlEngine;
#endif
    static SerdeIniEngine IniEngine;
    switch (Backend) {
        case ESerdeBackend::Json:
            return &JsonEngine;
#if PICTOR_HAS_CPP17
        case ESerdeBackend::Toml:
            return &TomlEngine;
#endif
        case ESerdeBackend::Ini:
            return &IniEngine;
        default:
            return nullptr;
    }
}
} // namespace infra

#endif // INFRA_SERDE_ENGINE_FACTORY_HPP
