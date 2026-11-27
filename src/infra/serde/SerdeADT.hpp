//
// SerdeADT.hpp — 序列化/反序列化的格式无关中间数据结构（Serde Abstract Data Type）
//
// 外部对象经由反射（SerdeReflect.hpp 的 toAdt/fromAdt）填充本结构；序列化后端
// （SerdeEngine）只认识 SerdeADT，不认识任何业务类型，从而实现“反射与序列化解耦”。
// 标量用 std::variant 存储，数组/对象用 std::unique_ptr 打破递归。
//
// C++11 兼容（2026/11 重构）：
//   1. 本文件同时承载 serde 模块的小型跨版本垫片（统一兼容层 VersionCompatibility.hpp
//      之外的 serde 专属部分）：
//      - serde_detail::Expected<T, E>：std::expected 为 C++23 设施，C++11~C++20
//        均不可用，此处提供接口按需对齐的最小等价实现（unexpected / operator bool /
//        has_value / operator* / operator-> / value / error）；
//      - serde_detail::Byte：std::byte 为 C++17 设施，C++11 下用 enum class 模拟；
//      - C++11 下 string_view 垫片的 range-for 支持（begin/end 自由函数）。
//   2. SerdeADT 的存储依赖 std::variant / std::visit（C++17 设施）：C++17 起保持
//      原实现不变；C++11/14 下类接口照常声明，任何值构造/取值抛出 std::logic_error
//      优雅降级（配置解析器 infra/serde/parser/Ini.hpp 不受影响，在 C++11 下完整可用）。
//

#ifndef INFRA_SERDE_ADT_HPP
#define INFRA_SERDE_ADT_HPP
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <infra/common/VersionCompatibility.hpp>

#if PICTOR_HAS_CPP17
#  include <optional>
#  include <string_view>
#  include <variant>
#endif

#if PICTOR_HAS_CPP23
#  include <expected>
#endif

namespace infra{

// ============================================================================ //
// serde 专属跨版本垫片                                                         //
// ============================================================================ //
namespace serde_detail {

// ----------------------------------------------------------------------------
// std::byte（C++17）垫片：C++11 用 enum class 模拟非整数字节类型
// ----------------------------------------------------------------------------
#if PICTOR_HAS_CPP17
using Byte = std::byte;
#else
enum class Byte : unsigned char {
};
#endif

// ----------------------------------------------------------------------------
// std::expected（C++23）垫片：C++23 起直接别名 std::expected，公共 API 与
// 标准库完全一致；C++11~C++20 提供接口按需对齐的最小等价实现。
// 错误侧统一经 unexpected(...) 构造。
// ----------------------------------------------------------------------------
#if PICTOR_HAS_CPP23
template <typename T, typename E>
using Expected = std::expected<T, E>;

template <typename E>
std::unexpected<typename std::decay<E>::type> unexpected(E &&Err) {
    return std::unexpected<typename std::decay<E>::type>(std::forward<E>(Err));
}
#else

/** @brief std::unexpected 的简化替身：只持有错误值，可隐式转换成 Expected。 */
template <typename E>
class UnexpectedHolder {
public:
    explicit UnexpectedHolder(const E &Err) : Error(Err) {
    }

    explicit UnexpectedHolder(E &&Err) : Error(std::move(Err)) {
    }

    E Error;
};

template <typename E>
UnexpectedHolder<typename std::decay<E>::type> unexpected(E &&Err) {
    return UnexpectedHolder<typename std::decay<E>::type>(std::forward<E>(Err));
}

// ---- 非 void 版本（std::expected<T, E> 的对偶）----
template <typename T, typename E>
class Expected {
public:
    Expected() : HasValue_(true) {
        ::new (static_cast<void *>(&S_.Value_)) T();
    }

    Expected(const T &V) : HasValue_(true) {
        ::new (static_cast<void *>(&S_.Value_)) T(V);
    }

    Expected(T &&V) : HasValue_(true) {
        ::new (static_cast<void *>(&S_.Value_)) T(std::move(V));
    }

    template <typename E2>
    Expected(const UnexpectedHolder<E2> &U) : HasValue_(false) {
        ::new (static_cast<void *>(&S_.Error_)) E(U.Error);
    }

    template <typename E2>
    Expected(UnexpectedHolder<E2> &&U) : HasValue_(false) {
        ::new (static_cast<void *>(&S_.Error_)) E(std::move(U.Error));
    }

    Expected(const Expected &Other) : HasValue_(Other.HasValue_) {
        if (HasValue_) {
            ::new (static_cast<void *>(&S_.Value_)) T(Other.S_.Value_);
        }
        else {
            ::new (static_cast<void *>(&S_.Error_)) E(Other.S_.Error_);
        }
    }

    Expected(Expected &&Other) : HasValue_(Other.HasValue_) {
        if (HasValue_) {
            ::new (static_cast<void *>(&S_.Value_)) T(std::move(Other.S_.Value_));
        }
        else {
            ::new (static_cast<void *>(&S_.Error_)) E(std::move(Other.S_.Error_));
        }
    }

    ~Expected() {
        destroy();
    }

    Expected &operator=(const Expected &Other) {
        if (this != &Other) {
            destroy();
            HasValue_ = Other.HasValue_;
            if (HasValue_) {
                ::new (static_cast<void *>(&S_.Value_)) T(Other.S_.Value_);
            }
            else {
                ::new (static_cast<void *>(&S_.Error_)) E(Other.S_.Error_);
            }
        }
        return *this;
    }

    Expected &operator=(Expected &&Other) {
        if (this != &Other) {
            destroy();
            HasValue_ = Other.HasValue_;
            if (HasValue_) {
                ::new (static_cast<void *>(&S_.Value_)) T(std::move(Other.S_.Value_));
            }
            else {
                ::new (static_cast<void *>(&S_.Error_)) E(std::move(Other.S_.Error_));
            }
        }
        return *this;
    }

    template <typename E2>
    Expected &operator=(const UnexpectedHolder<E2> &U) {
        destroy();
        HasValue_ = false;
        ::new (static_cast<void *>(&S_.Error_)) E(U.Error);
        return *this;
    }

    template <typename E2>
    Expected &operator=(UnexpectedHolder<E2> &&U) {
        destroy();
        HasValue_ = false;
        ::new (static_cast<void *>(&S_.Error_)) E(std::move(U.Error));
        return *this;
    }

    bool has_value() const {
        return HasValue_;
    }

    explicit operator bool() const {
        return HasValue_;
    }

    T &operator*() {
        return S_.Value_;
    }

    const T &operator*() const {
        return S_.Value_;
    }

    T *operator->() {
        return &S_.Value_;
    }

    const T *operator->() const {
        return &S_.Value_;
    }

    T &value() {
        if (!HasValue_) {
            throw std::logic_error("serde_detail::Expected: 空值");
        }
        return S_.Value_;
    }

    const T &value() const {
        if (!HasValue_) {
            throw std::logic_error("serde_detail::Expected: 空值");
        }
        return S_.Value_;
    }

    E &error() {
        return S_.Error_;
    }

    const E &error() const {
        return S_.Error_;
    }

private:
    void destroy() {
        if (HasValue_) {
            S_.Value_.~T();
        }
        else {
            S_.Error_.~E();
        }
    }

    union Storage {
        T Value_;
        E Error_;
        Storage() {
        }
        ~Storage() {
        }
    } S_;
    bool HasValue_;
};

// ---- void 版本（std::expected<void, E> 的对偶）----
template <typename E>
class Expected<void, E> {
public:
    Expected() : HasValue_(true) {
    }

    template <typename E2>
    Expected(const UnexpectedHolder<E2> &U) : Error_(U.Error), HasValue_(false) {
    }

    template <typename E2>
    Expected(UnexpectedHolder<E2> &&U) : Error_(std::move(U.Error)), HasValue_(false) {
    }

    Expected(const Expected &) = default;

    Expected(Expected &&) = default;

    Expected &operator=(const Expected &) = default;

    Expected &operator=(Expected &&) = default;

    template <typename E2>
    Expected &operator=(const UnexpectedHolder<E2> &U) {
        HasValue_ = false;
        Error_    = U.Error;
        return *this;
    }

    template <typename E2>
    Expected &operator=(UnexpectedHolder<E2> &&U) {
        HasValue_ = false;
        Error_    = std::move(U.Error);
        return *this;
    }

    bool has_value() const {
        return HasValue_;
    }

    explicit operator bool() const {
        return HasValue_;
    }

    E &error() {
        return Error_;
    }

    const E &error() const {
        return Error_;
    }

private:
    E Error_;
    bool HasValue_;
};

#endif // PICTOR_HAS_CPP23

} // namespace serde_detail

// ============================================================================ //
// 基础类型                                                                      //
// ============================================================================ //

class SerdeADT;

using SerdeArray  = std::vector<SerdeADT>;
using SerdeObject = std::vector<std::pair<std::string, SerdeADT> >; // 保持插入顺序
using SerdeBinary = std::vector<serde_detail::Byte>;

// 格式特定 / 值级覆盖信息（Optional 统一走兼容层，C++17 起即 std::optional）
struct SerdeMetadata {
    infra::compat::Optional<std::string> Tag;            // 类型标签（YAML/CBOR）
    std::vector<std::string> Comments;                   // 注释
    infra::compat::Optional<std::string> StringEncoding; // 本值字符串编码
    infra::compat::Optional<std::string> Endianness;     // 本值字节序
    infra::compat::Optional<std::string> Timezone;       // 本值时区
};

#if PICTOR_HAS_CPP17
// ========================= C++17+ 完整实现 =========================
class SerdeADT {
public:
    using Variant = std::variant<
        std::monostate,                        // null
        bool,                                  // boolean
        std::int64_t,                          // signed integer
        std::uint64_t,                         // unsigned integer
        double,                                // floating point
        std::string,                           // string（纯字节）
        SerdeBinary,                           // binary data
        std::chrono::system_clock::time_point, // date/time
        std::unique_ptr<SerdeArray>,           // array
        std::unique_ptr<SerdeObject> >;        // object

private:
    Variant Data;
    std::unique_ptr<SerdeMetadata> Metadata;

public:
    SerdeADT() : Data(std::monostate{}) {
    }

    SerdeADT(std::nullptr_t) : Data(std::monostate{}) {
    }

    SerdeADT(bool B) : Data(B) {
    }

    SerdeADT(double d) : Data(d) {
    }

    SerdeADT(float f) : Data(static_cast<double>(f)) {
    }

    // 所有整型统一收敛为 int64/uint64，避免 int -> {int64,uint64,double,bool} 二义性
    template<typename T, typename std::enable_if<std::is_integral<T>::value &&
                                                     !std::is_same<T, bool>::value,
                                                 int>::type = 0>
    SerdeADT(T V) {
        if constexpr (std::is_signed<T>::value)
            Data = static_cast<std::int64_t>(V);
        else
            Data = static_cast<std::uint64_t>(V);
    }

    SerdeADT(std::string S) : Data(std::move(S)) {
    }

    SerdeADT(const char *S) : Data(std::string(S)) {
    }

    SerdeADT(SerdeBinary B) : Data(std::move(B)) {
    }

    SerdeADT(std::chrono::system_clock::time_point Tb) : Data(Tb) {
    }

    SerdeADT(SerdeArray Arr) : Data(infra::compat::make_unique<SerdeArray>(std::move(Arr))) {
    }

    SerdeADT(SerdeObject Obj) : Data(infra::compat::make_unique<SerdeObject>(std::move(Obj))) {
    }

    SerdeADT(const SerdeADT &Other)
        : Data(copyVariant(Other.Data)),
          Metadata(Other.Metadata ? infra::compat::make_unique<SerdeMetadata>(*Other.Metadata)
                                  : nullptr) {
    }

    SerdeADT &operator=(const SerdeADT &Other) {
        if (this != &Other) {
            Data     = copyVariant(Other.Data);
            Metadata = Other.Metadata ? infra::compat::make_unique<SerdeMetadata>(*Other.Metadata)
                                      : nullptr;
        }
        return *this;
    }

    SerdeADT(SerdeADT &&) noexcept = default;

    SerdeADT &operator=(SerdeADT &&) noexcept = default;

    ~SerdeADT() = default;

    // ---------- 类型判断 ----------
    PICTOR_NODISCARD bool isNull() const {
        return std::holds_alternative<std::monostate>(Data);
    }

    PICTOR_NODISCARD bool isBool() const {
        return std::holds_alternative<bool>(Data);
    }

    PICTOR_NODISCARD bool isInt() const {
        return std::holds_alternative<std::int64_t>(Data);
    }

    PICTOR_NODISCARD bool isUint() const {
        return std::holds_alternative<std::uint64_t>(Data);
    }

    PICTOR_NODISCARD bool isDouble() const {
        return std::holds_alternative<double>(Data);
    }

    PICTOR_NODISCARD bool isNumber() const {
        return isInt() || isUint() || isDouble();
    }

    PICTOR_NODISCARD bool isString() const {
        return std::holds_alternative<std::string>(Data);
    }

    PICTOR_NODISCARD bool isBinary() const {
        return std::holds_alternative<SerdeBinary>(Data);
    }

    PICTOR_NODISCARD bool isDatetime() const {
        return std::holds_alternative<std::chrono::system_clock::time_point>(Data);
    }

    PICTOR_NODISCARD bool isArray() const {
        return std::holds_alternative<std::unique_ptr<SerdeArray> >(Data);
    }

    PICTOR_NODISCARD bool isObject() const {
        return std::holds_alternative<std::unique_ptr<SerdeObject> >(Data);
    }

    // ---------- 取值 ----------
    PICTOR_NODISCARD bool asBool() const {
        return std::get<bool>(Data);
    }

    PICTOR_NODISCARD std::int64_t asInt() const {
        return std::get<std::int64_t>(Data);
    }

    PICTOR_NODISCARD std::uint64_t asUint() const {
        return std::get<std::uint64_t>(Data);
    }

    PICTOR_NODISCARD double asDouble() const {
        return std::get<double>(Data);
    }

    PICTOR_NODISCARD const std::string &asString() const {
        return std::get<std::string>(Data);
    }

    PICTOR_NODISCARD const SerdeBinary &asBinary() const {
        return std::get<SerdeBinary>(Data);
    }

    PICTOR_NODISCARD const std::chrono::system_clock::time_point &asDatetime() const {
        return std::get<std::chrono::system_clock::time_point>(Data);
    }

    PICTOR_NODISCARD const SerdeArray &asArray() const {
        return *std::get<std::unique_ptr<SerdeArray> >(Data);
    }

    SerdeArray &asArray() {
        return *std::get<std::unique_ptr<SerdeArray> >(Data);
    }

    PICTOR_NODISCARD const SerdeObject &asObject() const {
        return *std::get<std::unique_ptr<SerdeObject> >(Data);
    }

    SerdeObject &asObject() {
        return *std::get<std::unique_ptr<SerdeObject> >(Data);
    }

    // ---------- 便捷访问 ----------
    PICTOR_NODISCARD const SerdeADT *find(infra::compat::string_view Key) const {
        if (!isObject())
            return nullptr;
        for (const auto &[K, V]: asObject())
            if (K == Key)
                return &V;
        return nullptr;
    }

    SerdeADT *find(infra::compat::string_view Key) {
        if (!isObject())
            return nullptr;
        for (auto &[K, V]: asObject())
            if (K == Key)
                return &V;
        return nullptr;
    }

    // ---------- 元数据 ----------
    SerdeMetadata &metadata() {
        if (!Metadata)
            Metadata = infra::compat::make_unique<SerdeMetadata>();
        return *Metadata;
    }

    PICTOR_NODISCARD const SerdeMetadata &metadata() const {
        static const SerdeMetadata Empty{};
        return Metadata ? *Metadata : Empty;
    }

    PICTOR_NODISCARD bool hasMetadata() const {
        return Metadata != nullptr;
    }

private:
    static Variant copyVariant(const Variant &V) {
        return std::visit([](const auto &Arg) -> Variant {
                              using T = infra::compat::decay_t<decltype(Arg)>;
                              if constexpr (std::is_same<T, std::unique_ptr<SerdeArray> >::value) {
                                  return Arg ? infra::compat::make_unique<SerdeArray>(*Arg) : nullptr;
                              }
                              else if constexpr (std::is_same<T, std::unique_ptr<SerdeObject> >::value) {
                                  return Arg ? infra::compat::make_unique<SerdeObject>(*Arg) : nullptr;
                              }
                              else {
                                  return Arg;
                              }
                          },
                          V);
    }
};

#else // !PICTOR_HAS_CPP17
// ========================= C++11 优雅降级 =========================
// @notice SerdeADT 的存储依赖 std::variant / std::visit（C++17 设施）。C++11/14 下
//         类接口照常声明以便各后端/服务器头文件编译通过，但任何值构造/取值都会
//         抛出 std::logic_error —— 序列化层在 C++11 下整体不可用（配置解析器
//         infra/serde/parser/Ini.hpp 不受影响，仍完整可用）。
class SerdeADT {
public:
    // @notice 统一报错信息见文末 throwRequired(); 构造/取值直接内联抛出,
    //         保证编译器可见"最后一条语句是无条件 throw"而不产生返回路径告警。
    SerdeADT() {
    }

    SerdeADT(std::nullptr_t) {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    SerdeADT(bool) {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    SerdeADT(double) {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    SerdeADT(float) {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    // 整型/枚举等杂项类型统一在此退化（复制/移动构造由下方精确重载优先匹配）
    template<typename T>
    SerdeADT(T) {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    SerdeADT(std::string) {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    SerdeADT(const char *) {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    SerdeADT(SerdeBinary) {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    SerdeADT(std::chrono::system_clock::time_point) {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    SerdeADT(SerdeArray) {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    SerdeADT(SerdeObject) {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    SerdeADT(const SerdeADT &) = default;

    SerdeADT &operator=(const SerdeADT &) = default;

    SerdeADT(SerdeADT &&) noexcept = default;

    SerdeADT &operator=(SerdeADT &&) noexcept = default;

    ~SerdeADT() = default;

    // ---------- 类型判断（恒为 false） ----------
    PICTOR_NODISCARD bool isNull() const {
        return false;
    }

    PICTOR_NODISCARD bool isBool() const {
        return false;
    }

    PICTOR_NODISCARD bool isInt() const {
        return false;
    }

    PICTOR_NODISCARD bool isUint() const {
        return false;
    }

    PICTOR_NODISCARD bool isDouble() const {
        return false;
    }

    PICTOR_NODISCARD bool isNumber() const {
        return false;
    }

    PICTOR_NODISCARD bool isString() const {
        return false;
    }

    PICTOR_NODISCARD bool isBinary() const {
        return false;
    }

    PICTOR_NODISCARD bool isDatetime() const {
        return false;
    }

    PICTOR_NODISCARD bool isArray() const {
        return false;
    }

    PICTOR_NODISCARD bool isObject() const {
        return false;
    }

    // ---------- 取值（无存储可用，一律抛错） ----------
    PICTOR_NODISCARD bool asBool() const {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    PICTOR_NODISCARD std::int64_t asInt() const {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    PICTOR_NODISCARD std::uint64_t asUint() const {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    PICTOR_NODISCARD double asDouble() const {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    PICTOR_NODISCARD const std::string &asString() const {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    PICTOR_NODISCARD const SerdeBinary &asBinary() const {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    PICTOR_NODISCARD const std::chrono::system_clock::time_point &asDatetime() const {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    PICTOR_NODISCARD const SerdeArray &asArray() const {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    SerdeArray &asArray() {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    PICTOR_NODISCARD const SerdeObject &asObject() const {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    SerdeObject &asObject() {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    // ---------- 便捷访问 ----------
    PICTOR_NODISCARD const SerdeADT *find(infra::compat::string_view Key) const {
        (void) Key;
        return nullptr;
    }

    SerdeADT *find(infra::compat::string_view Key) {
        (void) Key;
        return nullptr;
    }

    // ---------- 元数据 ----------
    SerdeMetadata &metadata() {
        throw std::logic_error("SerdeADT requires C++17 (serde 层在 C++11 下优雅降级)");
    }

    PICTOR_NODISCARD const SerdeMetadata &metadata() const {
        static const SerdeMetadata Empty{};
        return Empty;
    }

    PICTOR_NODISCARD bool hasMetadata() const {
        return false;
    }
};
#endif // PICTOR_HAS_CPP17

} // namespace infra

// ============================================================================ //
// C++11 下 string_view 垫片的 range-for 支持                                   //
// ============================================================================ //
// @notice 标准 std::string_view 自带 begin/end（C++17）；C++11 垫片只有 data/size，
//         这里以自由函数补齐，使 `for (char C : View)` 在两种标准下写法一致。
//         位于 compat 命名空间以借助 ADL 查找。
#if !PICTOR_HAS_CPP17
namespace infra {
namespace compat {

template <typename CharT>
inline const CharT *begin(const basic_string_view<CharT> &View) noexcept {
    return View.data();
}

template <typename CharT>
inline const CharT *end(const basic_string_view<CharT> &View) noexcept {
    return View.data() + View.size();
}

} // namespace compat
} // namespace infra
#endif

#endif // INFRA_SERDE_ADT_HPP
