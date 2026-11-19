//
// SerdeADT.hpp — 序列化/反序列化的格式无关中间数据结构（Serde Abstract Data Type）
//
// 外部对象经由反射（SerdeReflect.hpp 的 toAdt/fromAdt）填充本结构；序列化后端
// （SerdeEngine）只认识 SerdeADT，不认识任何业务类型，从而实现“反射与序列化解耦”。
// 标量用 std::variant 存储，数组/对象用 std::unique_ptr 打破递归。
//

#ifndef CORE_SERDE_ADT_HPP
#define CORE_SERDE_ADT_HPP
#pragma once

#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace infra
{

class SerdeADT;

using SerdeArray  = std::vector<SerdeADT>;
using SerdeObject = std::vector<std::pair<std::string, SerdeADT>>; // 保持插入顺序
using SerdeBinary = std::vector<std::byte>;

// 格式特定 / 值级覆盖信息
struct SerdeMetadata
{
    std::optional<std::string> Tag;             // 类型标签（YAML/CBOR）
    std::vector<std::string>   Comments;        // 注释
    std::optional<std::string> StringEncoding;  // 本值字符串编码
    std::optional<std::string> Endianness;      // 本值字节序
    std::optional<std::string> Timezone;        // 本值时区
};

class SerdeADT
{
public:
    using Variant = std::variant<
        std::monostate,                          // null
        bool,                                    // boolean
        std::int64_t,                            // signed integer
        std::uint64_t,                           // unsigned integer
        double,                                  // floating point
        std::string,                             // string（纯字节）
        SerdeBinary,                             // binary data
        std::chrono::system_clock::time_point,   // date/time
        std::unique_ptr<SerdeArray>,             // array
        std::unique_ptr<SerdeObject>>;           // object

private:
    Variant Data;
    std::unique_ptr<SerdeMetadata> Metadata;

public:
    SerdeADT() : Data(std::monostate{}) {}
    SerdeADT(std::nullptr_t) : Data(std::monostate{}) {}
    SerdeADT(bool B) : Data(B) {}
    SerdeADT(double d) : Data(d) {}
    SerdeADT(float f) : Data(static_cast<double>(f)) {}

    // 所有整型统一收敛为 int64/uint64，避免 int -> {int64,uint64,double,bool} 二义性
    template<typename T>
        requires (std::integral<T> && !std::same_as<T, bool>)
    SerdeADT(T V)
    {
        if constexpr (std::is_signed_v<T>) Data = static_cast<std::int64_t>(V);
        else Data = static_cast<std::uint64_t>(V);
    }

    SerdeADT(std::string S) : Data(std::move(S)) {}
    SerdeADT(const char *S) : Data(std::string(S)) {}
    SerdeADT(SerdeBinary B) : Data(std::move(B)) {}
    SerdeADT(std::chrono::system_clock::time_point Tb) : Data(Tb) {}
    SerdeADT(SerdeArray Arr) : Data(std::make_unique<SerdeArray>(std::move(Arr))) {}
    SerdeADT(SerdeObject Obj) : Data(std::make_unique<SerdeObject>(std::move(Obj))) {}

    SerdeADT(const SerdeADT &Other)
        : Data(copyVariant(Other.Data)),
          Metadata(Other.Metadata ? std::make_unique<SerdeMetadata>(*Other.Metadata) : nullptr) {}
    SerdeADT &operator=(const SerdeADT &Other)
    {
        if (this != &Other)
        {
            Data = copyVariant(Other.Data);
            Metadata = Other.Metadata ? std::make_unique<SerdeMetadata>(*Other.Metadata) : nullptr;
        }
        return *this;
    }
    SerdeADT(SerdeADT &&) noexcept = default;
    SerdeADT &operator=(SerdeADT &&) noexcept = default;
    ~SerdeADT() = default;

    // ---------- 类型判断 ----------
    [[nodiscard]] bool isNull()     const { return std::holds_alternative<std::monostate>(Data); }
    [[nodiscard]] bool isBool()     const { return std::holds_alternative<bool>(Data); }
    [[nodiscard]] bool isInt()      const { return std::holds_alternative<std::int64_t>(Data); }
    [[nodiscard]] bool isUint()     const { return std::holds_alternative<std::uint64_t>(Data); }
    [[nodiscard]] bool isDouble()   const { return std::holds_alternative<double>(Data); }
    [[nodiscard]] bool isNumber()   const { return isInt() || isUint() || isDouble(); }
    [[nodiscard]] bool isString()   const { return std::holds_alternative<std::string>(Data); }
    [[nodiscard]] bool isBinary()   const { return std::holds_alternative<SerdeBinary>(Data); }
    [[nodiscard]] bool isDatetime() const { return std::holds_alternative<std::chrono::system_clock::time_point>(Data); }
    [[nodiscard]] bool isArray()    const { return std::holds_alternative<std::unique_ptr<SerdeArray>>(Data); }
    [[nodiscard]] bool isObject()   const { return std::holds_alternative<std::unique_ptr<SerdeObject>>(Data); }

    // ---------- 取值 ----------
    [[nodiscard]] bool asBool() const { return std::get<bool>(Data); }
    [[nodiscard]] std::int64_t asInt() const { return std::get<std::int64_t>(Data); }
    [[nodiscard]] std::uint64_t asUint() const { return std::get<std::uint64_t>(Data); }
    [[nodiscard]] double asDouble() const { return std::get<double>(Data); }
    [[nodiscard]] const std::string &asString() const { return std::get<std::string>(Data); }
    [[nodiscard]] const SerdeBinary &asBinary() const { return std::get<SerdeBinary>(Data); }
    [[nodiscard]] const std::chrono::system_clock::time_point &asDatetime() const
    {
        return std::get<std::chrono::system_clock::time_point>(Data);
    }
    [[nodiscard]] const SerdeArray &asArray() const { return *std::get<std::unique_ptr<SerdeArray>>(Data); }
    SerdeArray &asArray() { return *std::get<std::unique_ptr<SerdeArray>>(Data); }
    [[nodiscard]] const SerdeObject &asObject() const { return *std::get<std::unique_ptr<SerdeObject>>(Data); }
    SerdeObject &asObject() { return *std::get<std::unique_ptr<SerdeObject>>(Data); }

    // ---------- 便捷访问 ----------
    [[nodiscard]] const SerdeADT *find(std::string_view Key) const
    {
        if (!isObject()) return nullptr;
        for (const auto &[K, V] : asObject())
            if (K == Key) return &V;
        return nullptr;
    }
    SerdeADT *find(std::string_view Key)
    {
        if (!isObject()) return nullptr;
        for (auto &[K, V] : asObject())
            if (K == Key) return &V;
        return nullptr;
    }

    // ---------- 元数据 ----------
    SerdeMetadata &metadata()
    {
        if (!Metadata) Metadata = std::make_unique<SerdeMetadata>();
        return *Metadata;
    }
    [[nodiscard]] const SerdeMetadata &metadata() const
    {
        static const SerdeMetadata Empty{};
        return Metadata ? *Metadata : Empty;
    }
    [[nodiscard]] bool hasMetadata() const { return Metadata != nullptr; }

private:
    static Variant copyVariant(const Variant &V)
    {
        return std::visit([](const auto &Arg) -> Variant 
        {
            using T = std::decay_t<decltype(Arg)>;
            if constexpr (std::is_same_v<T, std::unique_ptr<SerdeArray>>)
            {
                return Arg ? std::make_unique<SerdeArray>(*Arg) : nullptr;
            }
            else if constexpr (std::is_same_v<T, std::unique_ptr<SerdeObject>>)
            {
                return Arg ? std::make_unique<SerdeObject>(*Arg) : nullptr;
            }
            else
            {
                return Arg;
            }
        }, V);
    }
};

} // namespace infra

#endif // CORE_SERDE_ADT_HPP
