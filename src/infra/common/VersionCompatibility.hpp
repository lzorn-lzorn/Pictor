#pragma once

/**
 * @file VersionCompatibility.hpp
 * @brief 统一跨 C++ 版本兼容层：infra 模块所有头文件都应包含它。
 *
 * 设计目标
 * - infra 全部代码在 C++11 下可编译；C++20 为主要使用场景，可用时自动启用
 *   标准库设施（std::launder、std::construct_at、std::to_chars 等），不可用时
 *   退化为等价的手写实现。
 * - 所有跨版本组件（语言级别宏、属性宏、std 垫片）集中在本文件，各模块统一
 *   通过 infra::compat 命名空间与 PICTOR_* 宏使用，避免各自零散的特性探测。
 *
 * 约定
 * - 布尔型 trait 查询统一使用 C++11 风格的 std::xxx<T>::value；
 * - 类型别名统一使用本文件提供的 compat::xxx_t（alias template 为 C++11 语法，
 *   即使 C++14/17 的 std::xxx_t 不可用也可工作）。
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>       // snprintf（to_chars 垫片）
#include <cstdlib>      // strtod/strtoll（from_chars 垫片）
#include <limits>       // numeric_limits（to/from_chars 垫片）
#include <memory>       // unique_ptr / construct_at
#include <new>          // placement new / std::launder
#include <stdexcept>    // logic_error（Optional 垫片）
#include <string>       // char_traits / string
#include <system_error> // errc（to/from_chars 结果）
#include <tuple>        // index_sequence 垫片
#include <type_traits>
#include <utility>

#if defined(_MSC_VER)
#  include <intrin.h>   // _BitScanForward
#endif

// ========================================================================== //
// 语言级别                                                                   //
// ========================================================================== //
// @notice MSVC 上 __cplusplus 受 /Zc:__cplusplus 影响，_MSVC_LANG 始终正确。
#if defined(_MSVC_LANG)
#  define PICTOR_LANGUAGE_LEVEL _MSVC_LANG
#else
#  define PICTOR_LANGUAGE_LEVEL __cplusplus
#endif

#define PICTOR_HAS_CPP14 (PICTOR_LANGUAGE_LEVEL >= 201402L)
#define PICTOR_HAS_CPP17 (PICTOR_LANGUAGE_LEVEL >= 201703L)
#define PICTOR_HAS_CPP20 (PICTOR_LANGUAGE_LEVEL >= 202002L)
#define PICTOR_HAS_CPP23 (PICTOR_LANGUAGE_LEVEL >= 202302L)

// @notice 语言级别宏必须先于下面的条件包含与属性探测定义。
#if PICTOR_HAS_CPP17
#  include <charconv>
#  include <optional>
#  include <string_view>
#endif

// ========================================================================== //
// 属性宏                                                                     //
// ========================================================================== //
#if defined(__has_cpp_attribute)
#  define PICTOR_HAS_ATTRIBUTE(Attr) __has_cpp_attribute(Attr)
#else
#  define PICTOR_HAS_ATTRIBUTE(Attr) 0
#endif

// [[nodiscard]]（C++17）
#if PICTOR_HAS_CPP17 && PICTOR_HAS_ATTRIBUTE(nodiscard) >= 201907L
#  define PICTOR_NODISCARD [[nodiscard]]
#else
#  define PICTOR_NODISCARD
#endif

// [[maybe_unused]]（C++17）
#if PICTOR_HAS_CPP17 && PICTOR_HAS_ATTRIBUTE(maybe_unused) >= 201603L
#  define PICTOR_MAYBE_UNUSED [[maybe_unused]]
#else
#  define PICTOR_MAYBE_UNUSED
#endif

// [[no_unique_address]]（C++20）
#if PICTOR_HAS_CPP20 && PICTOR_HAS_ATTRIBUTE(no_unique_address) >= 201803L
#  define PICTOR_NO_UNIQUE_ADDRESS [[no_unique_address]]
#else
#  define PICTOR_NO_UNIQUE_ADDRESS
#endif

// [[likely]] / [[unlikely]]（C++20）
#if PICTOR_HAS_CPP20 && PICTOR_HAS_ATTRIBUTE(likely) >= 201803L
#  define PICTOR_LIKELY [[likely]]
#  define PICTOR_UNLIKELY [[unlikely]]
#else
#  define PICTOR_LIKELY
#  define PICTOR_UNLIKELY
#endif

// [[fallthrough]]（C++17）
#if PICTOR_HAS_CPP17 && PICTOR_HAS_ATTRIBUTE(fallthrough) >= 201603L
#  define PICTOR_FALLTHROUGH [[fallthrough]]
#else
#  define PICTOR_FALLTHROUGH
#endif

// ========================================================================== //
// 关键字宏                                                                   //
// ========================================================================== //
// @notice C++11 的 constexpr 只能单 return、成员函数隐式 const，无法表达循环体
//         与多语句，因此此类函数用 PICTOR_CONSTEXPR14（C++11 下退化为普通函数）。
#if PICTOR_HAS_CPP14
#  define PICTOR_CONSTEXPR14 constexpr
#else
#  define PICTOR_CONSTEXPR14
#endif

// @notice C++17 起 constexpr 静态数据成员隐式 inline；C++11/14 若被 odr-use
//         需要类外定义（配合使用）。
#if PICTOR_HAS_CPP17
#  define PICTOR_INLINE_VARIABLE inline
#else
#  define PICTOR_INLINE_VARIABLE
#endif

// ========================================================================== //
// std 垫片（namespace infra::compat）                                         //
// ========================================================================== //
namespace infra {
namespace compat {

// --------------------------------------------------------------------------
// C++17 std::launder
// --------------------------------------------------------------------------
template <typename Ty>
Ty *launder(Ty *Ptr) noexcept {
#if PICTOR_HAS_CPP17
    return std::launder(Ptr);
#else
    return Ptr;
#endif
}

template <typename Ty>
Ty const *launder(Ty const *Ptr) noexcept {
#if PICTOR_HAS_CPP17
    return std::launder(Ptr);
#else
    return Ptr;
#endif
}

// --------------------------------------------------------------------------
// C++20 std::construct_at（placement new 回退）
// --------------------------------------------------------------------------
template <typename Ty, typename... Args>
Ty *construct_at(Ty *Ptr, Args &&...Arguments) {
#if defined(__cpp_lib_construct_at) || PICTOR_HAS_CPP20
    return std::construct_at(Ptr, std::forward<Args>(Arguments)...);
#else
    return ::new (static_cast<void *>(Ptr)) Ty(std::forward<Args>(Arguments)...);
#endif
}

// --------------------------------------------------------------------------
// C++17 std::destroy_at（显式析构回退）
// --------------------------------------------------------------------------
template <typename Ty>
void destroy_at(Ty *Ptr) noexcept {
#if defined(__cpp_lib_destroy_at) || PICTOR_HAS_CPP17
    std::destroy_at(Ptr);
#else
    Ptr->~Ty();
#endif
}

// --------------------------------------------------------------------------
// C++20 位运算 std::countr_zero（尾随零计数；编译器内建回退）
// @notice 统一按 std::uint64_t 处理，任意无符号整型入参均可。
// --------------------------------------------------------------------------
template <typename UInt>
int countr_zero(UInt Value) noexcept {
    static_assert(std::is_unsigned<UInt>::value,
                  "infra::compat::countr_zero: 要求无符号整型");
    const std::uint64_t V = static_cast<std::uint64_t>(Value);
#if defined(__clang__) || defined(__GNUC__)
    return V != 0 ? __builtin_ctzll(static_cast<unsigned long long>(V)) : 64;
#elif defined(_MSC_VER)
    if (V != 0) {
        unsigned long Index = 0;
#  if defined(_M_X64) || defined(_M_ARM64)
        _BitScanForward64(&Index, V);
#  else
        if ((V & 0xFFFFFFFFULL) != 0) {
            _BitScanForward(&Index, static_cast<unsigned long>(V));
        } else {
            _BitScanForward(&Index, static_cast<unsigned long>(V >> 32));
            Index += 32;
        }
#  endif
        return static_cast<int>(Index);
    }
    return 64;
#else
    int Count = 0;
    std::uint64_t Cur = V;
    while (Cur != 0 && (Cur & 1ULL) == 0) {
        Cur >>= 1;
        ++Count;
    }
    return V != 0 ? Count : 64;
#endif
}

// --------------------------------------------------------------------------
// C++14 std::make_unique
// --------------------------------------------------------------------------
#if PICTOR_HAS_CPP14
using std::make_unique;
#else
template <typename Ty, typename... Args>
std::unique_ptr<Ty> make_unique(Args &&...Arguments) {
    return std::unique_ptr<Ty>(new Ty(std::forward<Args>(Arguments)...));
}
#endif

// --------------------------------------------------------------------------
// C++14 std::index_sequence / index_sequence_for
// --------------------------------------------------------------------------
template <std::size_t... Ints>
struct index_sequence {
    using type = index_sequence;
    static constexpr std::size_t size() { return sizeof...(Ints); }
};

template <typename Seq1, typename Seq2>
struct index_sequence_concat;

template <std::size_t... Ints1, std::size_t... Ints2>
struct index_sequence_concat<index_sequence<Ints1...>, index_sequence<Ints2...>> {
    using type = index_sequence<Ints1..., (sizeof...(Ints1) + Ints2)...>;
};

template <std::size_t N>
struct make_index_sequence_impl {
    using type = typename index_sequence_concat<
        typename make_index_sequence_impl<N / 2>::type,
        typename make_index_sequence_impl<N - N / 2>::type>::type;
};

template <>
struct make_index_sequence_impl<0> {
    using type = index_sequence<>;
};

template <>
struct make_index_sequence_impl<1> {
    using type = index_sequence<0>;
};

template <std::size_t N>
using make_index_sequence = typename make_index_sequence_impl<N>::type;

template <typename... Ts>
using index_sequence_for = make_index_sequence<sizeof...(Ts)>;

// --------------------------------------------------------------------------
// C++17 std::void_t（alias template 为 C++11 语法，可提前使用）
// --------------------------------------------------------------------------
template <typename... Ts>
using void_t = void;

// --------------------------------------------------------------------------
// C++20 std::remove_cvref
// --------------------------------------------------------------------------
template <typename Ty>
struct remove_cvref {
    using type =
        typename std::remove_cv<typename std::remove_reference<Ty>::type>::type;
};

template <typename Ty>
using remove_cvref_t = typename remove_cvref<Ty>::type;

// --------------------------------------------------------------------------
// 常用 _t 别名（alias template 为 C++11 语法）
// --------------------------------------------------------------------------
template <typename Ty>
using decay_t = typename std::decay<Ty>::type;

template <bool B, typename Ty = void>
using enable_if_t = typename std::enable_if<B, Ty>::type;

template <bool B, typename T, typename F>
using conditional_t = typename std::conditional<B, T, F>::type;

template <typename Ty>
using remove_reference_t = typename std::remove_reference<Ty>::type;

template <typename Ty>
using remove_cv_t = typename std::remove_cv<Ty>::type;

template <typename Ty>
using remove_pointer_t = typename std::remove_pointer<Ty>::type;

// --------------------------------------------------------------------------
// C++17 std::invoke_result（C++11 用 std::result_of 回退）
// --------------------------------------------------------------------------
#if PICTOR_HAS_CPP17
template <typename F, typename... Args>
struct invoke_result {
    using type = typename std::invoke_result<F, Args...>::type;
};
#else
template <typename F, typename... Args>
struct invoke_result {
    using type = typename std::result_of<F(Args...)>::type;
};
#endif

template <typename F, typename... Args>
using invoke_result_t = typename invoke_result<F, Args...>::type;

// --------------------------------------------------------------------------
// C++17 std::bool_constant
// --------------------------------------------------------------------------
template <bool B>
using bool_constant = std::integral_constant<bool, B>;

// --------------------------------------------------------------------------
// C++17 std::string_view 垫片（C++11 提供等价实现）
// @notice C++11 下 constexpr 函数仅支持单 return，故查找/子串类操作非 constexpr；
//         需要编译期执行的调用方请配合 PICTOR_CONSTEXPR14。
// --------------------------------------------------------------------------
#if PICTOR_HAS_CPP17
template <typename CharT, typename Traits = std::char_traits<CharT>>
using basic_string_view = std::basic_string_view<CharT, Traits>;

using string_view = std::string_view;
#else
template <typename CharT>
class basic_string_view {
public:
    using size_type = std::size_t;
    static constexpr size_type npos = static_cast<size_type>(-1);

    constexpr basic_string_view() : Data_(nullptr), Size_(0) {}

    constexpr basic_string_view(const CharT *Str)
        : Data_(Str), Size_(Str ? lengthOf(Str) : 0) {}

    constexpr basic_string_view(const CharT *Str, size_type N)
        : Data_(Str), Size_(N) {}

    constexpr const CharT *data() const { return Data_; }
    constexpr size_type size() const { return Size_; }
    constexpr bool empty() const { return Size_ == 0; }
    constexpr const CharT &operator[](size_type Index) const { return Data_[Index]; }
    constexpr const CharT &front() const { return Data_[0]; }
    constexpr const CharT &back() const { return Data_[Size_ - 1]; }

    size_type find_last_of(CharT C) const {
        for (size_type I = Size_; I > 0; --I)
            if (Data_[I - 1] == C)
                return I - 1;
        return npos;
    }

    size_type find(CharT C) const {
        for (size_type I = 0; I < Size_; ++I)
            if (Data_[I] == C)
                return I;
        return npos;
    }

    basic_string_view substr(size_type Pos = 0) const {
        return Pos > Size_
                   ? basic_string_view()
                   : basic_string_view(Data_ + Pos, Size_ - Pos);
    }

    friend bool operator==(basic_string_view Lhs, basic_string_view Rhs) {
        if (Lhs.Size_ != Rhs.Size_)
            return false;
        for (size_type I = 0; I < Lhs.Size_; ++I)
            if (Lhs.Data_[I] != Rhs.Data_[I])
                return false;
        return true;
    }

    friend bool operator!=(basic_string_view Lhs, basic_string_view Rhs) {
        return !(Lhs == Rhs);
    }

private:
    static constexpr size_type lengthOf(const CharT *Str) {
        return *Str != CharT() ? 1 + lengthOf(Str + 1) : 0;
    }

    const CharT *Data_;
    size_type Size_;
};

using string_view = basic_string_view<char>;
#endif

// --------------------------------------------------------------------------
// C++17 std::optional 垫片（C++11 提供等价实现）
// --------------------------------------------------------------------------
#if PICTOR_HAS_CPP17
using nullopt_t = std::nullopt_t;
// @notice std::nullopt_t 不可直接默认构造（实现定义为私密标记构造），
//         因此从标准常量 std::nullopt 拷贝初始化。
constexpr std::nullopt_t nullopt = std::nullopt;

template <typename Ty>
using Optional = std::optional<Ty>;
#else
struct nullopt_t {
    explicit constexpr nullopt_t(int) {}
};
constexpr nullopt_t nullopt = nullopt_t(0);

template <typename Ty>
class Optional {
public:
    Optional() : Has_(false) {}

    Optional(nullopt_t) : Has_(false) {}

    Optional(Ty const &Value) : Has_(false) { emplace(Value); }

    Optional(Ty &&Value) : Has_(false) { emplace(std::move(Value)); }

    Optional(Optional const &Other) : Has_(false) {
        if (Other.Has_)
            emplace(*Other.ptr());
    }

    Optional(Optional &&Other) : Has_(false) {
        if (Other.Has_)
            emplace(std::move(*Other.ptr()));
        Other.reset();
    }

    ~Optional() { reset(); }

    Optional &operator=(Optional const &Other) {
        if (this != &Other) {
            reset();
            if (Other.Has_)
                emplace(*Other.ptr());
        }
        return *this;
    }

    Optional &operator=(Optional &&Other) {
        if (this != &Other) {
            reset();
            if (Other.Has_)
                emplace(std::move(*Other.ptr()));
            Other.reset();
        }
        return *this;
    }

    Optional &operator=(Ty const &Value) {
        emplace(Value);
        return *this;
    }

    Optional &operator=(Ty &&Value) {
        emplace(std::move(Value));
        return *this;
    }

    Optional &operator=(nullopt_t) {
        reset();
        return *this;
    }

    bool has_value() const { return Has_; }
    explicit operator bool() const { return Has_; }

    Ty &operator*() { return value(); }
    Ty const &operator*() const { return value(); }
    Ty *operator->() { return ptr(); }
    Ty const *operator->() const { return ptr(); }

    Ty &value() {
        if (!Has_)
            throw std::logic_error("infra::compat::Optional::value: 空值");
        return *ptr();
    }

    Ty const &value() const {
        if (!Has_)
            throw std::logic_error("infra::compat::Optional::value: 空值");
        return *ptr();
    }

    template <typename... Args>
    Ty &emplace(Args &&...Arguments) {
        reset();
        ::new (static_cast<void *>(ptr())) Ty(std::forward<Args>(Arguments)...);
        Has_ = true;
        return value();
    }

    void reset() {
        if (Has_) {
            ptr()->~Ty();
            Has_ = false;
        }
    }

private:
    Ty *ptr() { return reinterpret_cast<Ty *>(Storage_); }
    Ty const *ptr() const { return reinterpret_cast<Ty const *>(Storage_); }

    alignas(Ty) unsigned char Storage_[sizeof(Ty)];
    bool Has_;
};
#endif

// --------------------------------------------------------------------------
// C++17 std::to_chars / std::from_chars 垫片（C++11 手写等价实现）
// @notice 垫片实现：整数为手写十进制解析/格式化；浮点经 strtod/snprintf，
//         不保证与 std::to_chars 的最短往返表示完全一致，仅作兼容回退。
// --------------------------------------------------------------------------
#if PICTOR_HAS_CPP17
using to_chars_result = std::to_chars_result;
using from_chars_result = std::from_chars_result;
using std::to_chars;
using std::from_chars;
#else
struct to_chars_result {
    char *ptr;
    std::errc ec;
};

struct from_chars_result {
    const char *ptr;
    std::errc ec;
};

namespace detail_parse {

inline bool parseDigits(const char *&Cur, const char *Last,
                        std::uint64_t &Out, int MaxDigits) {
    Out = 0;
    int Count = 0;
    while (Cur != Last && *Cur >= '0' && *Cur <= '9') {
        if (Count >= MaxDigits)
            return false; // 溢出或过长
        Out = Out * 10 + static_cast<std::uint64_t>(*Cur - '0');
        ++Cur;
        ++Count;
    }
    return Count > 0;
}

} // namespace detail_parse

// ---- 有符号整数 ----
template <typename Int>
typename std::enable_if<std::numeric_limits<Int>::is_integer &&
                            std::numeric_limits<Int>::is_signed,
                        from_chars_result>::type
from_chars(const char *First, const char *Last, Int &Value) {
    const char *Cur = First;
    bool Negative = false;
    if (Cur != Last && (*Cur == '-' || *Cur == '+')) {
        Negative = (*Cur == '-');
        ++Cur;
    }
    std::uint64_t Digits = 0;
    if (!detail_parse::parseDigits(Cur, Last, Digits,
                                   std::numeric_limits<Int>::digits10 + 1))
        return {First, std::errc::invalid_argument};
    const std::uint64_t Limit =
        Negative
            ? static_cast<std::uint64_t>(std::numeric_limits<Int>::max()) + 1ULL
            : static_cast<std::uint64_t>(std::numeric_limits<Int>::max());
    if (Digits > Limit)
        return {First, std::errc::result_out_of_range};
    Value = Negative
                ? static_cast<Int>(-static_cast<std::int64_t>(Digits))
                : static_cast<Int>(Digits);
    return {Cur, std::errc()};
}

// ---- 无符号整数 ----
template <typename Int>
typename std::enable_if<std::numeric_limits<Int>::is_integer &&
                            !std::numeric_limits<Int>::is_signed,
                        from_chars_result>::type
from_chars(const char *First, const char *Last, Int &Value) {
    const char *Cur = First;
    std::uint64_t Digits = 0;
    if (!detail_parse::parseDigits(Cur, Last, Digits,
                                   std::numeric_limits<Int>::digits10 + 1))
        return {First, std::errc::invalid_argument};
    if (Digits > static_cast<std::uint64_t>(std::numeric_limits<Int>::max()))
        return {First, std::errc::result_out_of_range};
    Value = static_cast<Int>(Digits);
    return {Cur, std::errc()};
}

// ---- 浮点 ----
template <typename Float>
typename std::enable_if<std::is_floating_point<Float>::value,
                        from_chars_result>::type
from_chars(const char *First, const char *Last, Float &Value) {
    // @notice std::from_chars 的精确语义（不跳空白、最接近取整）依赖 C++17
    //         浮点解析；垫片用 strtod 近似。将 [First, Last) 拷贝为 NUL 结尾
    //         的临时串，保证不会越过 Last 读取（strtod 会跳过前导空白）。
    if (First == Last)
        return {First, std::errc::invalid_argument};
    const std::string Text(First, static_cast<std::size_t>(Last - First));
    char *End = nullptr;
    Value = static_cast<Float>(std::strtod(Text.c_str(), &End));
    if (End == Text.c_str())
        return {First, std::errc::invalid_argument};
    return {First + (End - Text.c_str()), std::errc()};
}

// ---- 整数格式化（有符号 / 无符号分派，避免 %lld 吞掉无符号高位） ----
template <typename Int>
typename std::enable_if<std::numeric_limits<Int>::is_integer, to_chars_result>::type
to_charsImpl(char *First, char *Last, Int Value, std::true_type) {
    const int Written = std::snprintf(
        First, static_cast<std::size_t>(Last - First), "%lld",
        static_cast<long long>(Value));
    if (Written < 0 || First + Written >= Last)
        return {Last, std::errc::value_too_large};
    return {First + Written, std::errc()};
}

template <typename Int>
typename std::enable_if<std::numeric_limits<Int>::is_integer, to_chars_result>::type
to_charsImpl(char *First, char *Last, Int Value, std::false_type) {
    const int Written = std::snprintf(
        First, static_cast<std::size_t>(Last - First), "%llu",
        static_cast<unsigned long long>(Value));
    if (Written < 0 || First + Written >= Last)
        return {Last, std::errc::value_too_large};
    return {First + Written, std::errc()};
}

template <typename Int>
typename std::enable_if<std::numeric_limits<Int>::is_integer, to_chars_result>::type
to_chars(char *First, char *Last, Int Value) {
    return to_charsImpl(First, Last, Value, std::is_signed<Int>());
}

// ---- 浮点格式化 ----
template <typename Float>
typename std::enable_if<std::is_floating_point<Float>::value, to_chars_result>::type
to_chars(char *First, char *Last, Float Value) {
    const int Written = std::snprintf(
        First, static_cast<std::size_t>(Last - First), "%.17g",
        static_cast<double>(Value));
    if (Written < 0 || First + Written >= Last)
        return {Last, std::errc::value_too_large};
    return {First + Written, std::errc()};
}
#endif

} // namespace compat
} // namespace infra
