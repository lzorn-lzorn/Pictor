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

#ifndef INFRA_SERDE_ENGINE_HPP
#define INFRA_SERDE_ENGINE_HPP
#pragma once

#include <string>
#include <string_view>

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

inline std::string_view backendExtension(ESerdeBackend Backend) {
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

inline std::string getNormalizeText(std::string_view Text) {
    constexpr std::string_view Bom = "\xEF\xBB\xBF"; // UTF-8 BOM
    if (Text.starts_with(Bom)) {
        Text.remove_prefix(Bom.size());
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

    [[nodiscard]] virtual SerdeADT parse(std::string_view Text) const = 0;

    [[nodiscard]] virtual std::string serialize(const SerdeADT &Adt) const = 0;
};
} // namespace infra

#endif // INFRA_SERDE_ENGINE_HPP
