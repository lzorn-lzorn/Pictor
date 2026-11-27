//
// SerdeEngine.hpp — 序列化后端引擎(抽象基类 + 后端类型 + 共享文本工具)
//
// SerdeEngine 是“SerdeADT <-> 文本”的抽象后端，与反射、业务类型完全解耦。
// 具体格式各自一个文件:
//   - SerdeJsonEngine.hpp  (JSON 后端)
//   - SerdeTomlEngine.hpp  (TOML 后端)
//   - SerdeIniEngine.hpp   (INI 后端)
// 后端工厂见 SerdeEngineFactory.hpp; SerdeServer 通过 getEngineFor() 分派。
//
// C++11 兼容(2026/11 重构): string_view 统一走 infra::compat::string_view;
// 文本规整逻辑不再使用 starts_with / remove_prefix 等 C++20 成员。
//

#ifndef INFRA_SERDE_ENGINE_HPP
#define INFRA_SERDE_ENGINE_HPP
#pragma once

#include <string>

#include <infra/common/VersionCompatibility.hpp>
#include <infra/serde/SerdeADT.hpp>

namespace infra{
// ========================= 模式 / 后端类型 =========================
enum class ESerdeMode {
    Append,
    Overwrite
};

enum class ESerdeBackend {
    None,   // 初始化空状态
    Json,   // 将 SerdeADT 转化为 Json 文件
    Toml,   // 将 SerdeADT 转化为 Toml 文件
    Ini,    // 将 SerdeADT 转化为 Ini 文件
    Binary, // 直接将 SerdeADT 转化为二进制文件
};

inline infra::compat::string_view backendExtension(ESerdeBackend Backend) {
    switch (Backend) {
        case ESerdeBackend::Json:
            return "json";
        case ESerdeBackend::Toml:
            return "toml";
        case ESerdeBackend::Ini:
            return "ini";
        case ESerdeBackend::Binary:
            return "bin";
        default:
            return "";
    }
}

// ========================= 文本预处理（读取前先规整） =========================
// 序列化写出的是带 \n / \t 的文本；读取时先做一次规整，避免下列情况影响解析：
//   1) 文件带 UTF-8 BOM（Windows 编辑器常见）
//   2) CRLF / 单独的 CR（Windows 记事本换行）
//   3) 首尾多余空白（末尾换行、行尾空格）
inline bool isTextSpace(char C) noexcept {
    return C == ' ' || C == '\t' || C == '\n' || C == '\r' || C == '\f' || C == '\v';
}

inline std::string getNormalizeText(infra::compat::string_view Text) {
    constexpr infra::compat::string_view Bom = "\xEF\xBB\xBF"; // UTF-8 BOM
    // @notice starts_with / remove_prefix 为 C++20 成员, 这里按 C++11 可用的写法判断
    if (Text.size() >= Bom.size() &&
        infra::compat::string_view(Text.data(), Bom.size()) == Bom) {
        Text = Text.substr(Bom.size());
    }

    std::string Out;
    Out.reserve(Text.size());
    for (const char C: Text) {
        if (C != '\r') // CRLF / CR -> LF
        {
            Out.push_back(C);
        }
    }

    std::size_t Beg = 0;
    std::size_t End = Out.size();
    while (Beg < End && isTextSpace(Out[Beg])) {
        ++Beg;
    }
    while (End > Beg && isTextSpace(Out[End - 1])) {
        --End;
    }
    Out.resize(End);
    Out.erase(0, Beg);
    return Out;
}

// ========================= 抽象后端 =========================
class SerdeEngine {
public:
    virtual ~SerdeEngine() = default;

    PICTOR_NODISCARD virtual SerdeADT parse(infra::compat::string_view Text) const = 0;

    PICTOR_NODISCARD virtual std::string serialize(const SerdeADT &Adt) const = 0;
};
} // namespace infra

#endif // INFRA_SERDE_ENGINE_HPP
