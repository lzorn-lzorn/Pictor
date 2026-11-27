#ifndef INFRA_SERDE_SERDEREFLECT_HPP_
#define INFRA_SERDE_SERDEREFLECT_HPP_

#include <infra/reflect/TypeInfo.hpp>
#include <infra/serde/SerdeADT.hpp>

#include <map>
#include <stdexcept>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#if PICTOR_HAS_CPP17
#  include <optional>
#endif

namespace infra{
// ========================= 类型检测（toAdt/fromAdt 用） =========================
namespace detail{
// 说明：std::false_type / std::true_type 的成员是 value（小写），本文件统一以 Value 暴露
//       （与 NameOf / VariablesOf / FunctionsOf 保持一致），否则 ::Value 不是合法成员。
template<typename Ty, typename = void>
struct IsReflected : std::false_type {
    static constexpr bool Value = false;
};

template<typename Ty>
struct IsReflected<Ty, infra::compat::void_t<decltype(Ty::reflect_variables)> > : std::true_type {
    static constexpr bool Value = true;
};

template<typename Ty>
struct IsStdVector : std::false_type {
    static constexpr bool Value = false;
};

template<typename Ty, typename A>
struct IsStdVector<std::vector<Ty, A> > : std::true_type {
    static constexpr bool Value = true;
};

template<typename Ty>
struct IsStdOptional : std::false_type {
    static constexpr bool Value = false;
};

// std::optional 为 C++17 设施: C++11 下该特化不可见(所有类型 IsStdOptional 为 false)
#if PICTOR_HAS_CPP17
template<typename Ty>
struct IsStdOptional<std::optional<Ty> > : std::true_type {
    static constexpr bool Value = true;
};
#endif

template<typename Ty>
struct IsStringMap : std::false_type {
    static constexpr bool Value = false;
};

template<typename V, typename... R>
struct IsStringMap<std::map<std::string, V, R...> > : std::true_type {
    static constexpr bool Value = true;
};

template<typename V, typename... R>
struct IsStringMap<std::unordered_map<std::string, V, R...> > : std::true_type {
    static constexpr bool Value = true;
};

/**
 * @brief 优先级分派标签: 数字越大优先级越高。替代 if constexpr 分支链,
 *         靠"派生 -> 基类"的转换排名保持原有分支顺序。
 */
template <int N>
struct SerdePriority : SerdePriority<N - 1> {
};

template <>
struct SerdePriority<0> {
};
} // namespace detail

// ========================= 业务类型 -> SerdeADT（反射驱动） =========================
template<typename Ty>
SerdeADT toAdt(const Ty &Obj);

namespace detail{
// @notice 反射成员遍历的仿函数: 替代 C++14 泛型 lambda
template<typename Ty>
struct ToAdtMemberCollector {
    SerdeObject *Members;

    template<typename Member>
    void operator()(const char *Name, const Member &Value) const {
        Members->emplace_back(Name, toAdt(Value));
    }
};

// 反射类型(最高优先级)
template<typename Ty>
infra::compat::enable_if_t<IsReflected<Ty>::Value, SerdeADT>
toAdtImpl(const Ty &Obj, SerdePriority<4>) {
    SerdeObject Members;
    ToAdtMemberCollector<Ty> Collector;
    Collector.Members = &Members;
    TypeInfo<Ty>::forEachMembers(Obj, Collector);
    return SerdeADT{std::move(Members)};
}

// std::vector
template<typename Ty>
infra::compat::enable_if_t<!IsReflected<Ty>::Value && IsStdVector<Ty>::Value, SerdeADT>
toAdtImpl(const Ty &Obj, SerdePriority<3>) {
    SerdeArray Arr;
    for (const auto &E: Obj)
        Arr.push_back(toAdt(E));
    return SerdeADT{std::move(Arr)};
}

// std::optional
template<typename Ty>
infra::compat::enable_if_t<!IsReflected<Ty>::Value && !IsStdVector<Ty>::Value &&
                               IsStdOptional<Ty>::Value,
                           SerdeADT>
toAdtImpl(const Ty &Obj, SerdePriority<2>) {
    return Obj ? toAdt(*Obj) : SerdeADT{nullptr};
}

// 字符串键映射(map / unordered_map)
template<typename Ty>
infra::compat::enable_if_t<!IsReflected<Ty>::Value && !IsStdVector<Ty>::Value &&
                               !IsStdOptional<Ty>::Value && IsStringMap<Ty>::Value,
                           SerdeADT>
toAdtImpl(const Ty &Obj, SerdePriority<1>) {
    SerdeObject M;
    for (const auto &Pair: Obj)
        M.emplace_back(Pair.first, toAdt(Pair.second));
    return SerdeADT{std::move(M)};
}

// 其余视为标量(最低优先级)
template<typename Ty>
infra::compat::enable_if_t<!IsReflected<Ty>::Value && !IsStdVector<Ty>::Value &&
                               !IsStdOptional<Ty>::Value && !IsStringMap<Ty>::Value,
                           SerdeADT>
toAdtImpl(const Ty &Obj, SerdePriority<0>) {
    return SerdeADT{Obj}; // 标量
}
} // namespace detail

template<typename Ty>
SerdeADT toAdt(const Ty &Obj) {
    return detail::toAdtImpl(Obj, detail::SerdePriority<4>());
}

// ========================= SerdeADT -> 业务类型（反射驱动） =========================
template<typename Ty>
serde_detail::Expected<Ty, std::string> fromAdt(const SerdeADT &Value);

namespace detail{
// @notice 反射成员写回的仿函数: 替代 C++14 泛型 lambda; 失败抛异常由调用方捕获
template<typename Ty>
struct FromAdtMemberSetter {
    const SerdeADT *Value;

    template<typename Member>
    void operator()(const char *Name, Member &MemberRef) const {
        if (const SerdeADT *F = Value->find(Name)) {
            serde_detail::Expected<infra::compat::decay_t<Member>, std::string> R =
                fromAdt<infra::compat::decay_t<Member> >(*F);
            if (!R)
                throw std::runtime_error(R.error());
            MemberRef = std::move(*R);
        }
    }
};

// 反射类型(最高优先级)
template<typename Ty>
infra::compat::enable_if_t<IsReflected<Ty>::Value, serde_detail::Expected<Ty, std::string> >
fromAdtImpl(const SerdeADT &Value, SerdePriority<4>) {
    try {
        if (!Value.isObject())
            return serde_detail::unexpected("expected object for reflected type");
        Ty Obj{};
        FromAdtMemberSetter<Ty> Setter;
        Setter.Value = &Value;
        TypeInfo<Ty>::forEachMembers(Obj, Setter);
        return Obj;
    }
    catch (const std::exception &E) {
        return serde_detail::unexpected(E.what());
    }
}

// std::vector
template<typename Ty>
infra::compat::enable_if_t<!IsReflected<Ty>::Value && IsStdVector<Ty>::Value,
                           serde_detail::Expected<Ty, std::string> >
fromAdtImpl(const SerdeADT &Value, SerdePriority<3>) {
    if (!Value.isArray())
        return serde_detail::unexpected("expected array");
    Ty Vec;
    for (const auto &E: Value.asArray()) {
        serde_detail::Expected<typename Ty::value_type, std::string> R =
            fromAdt<typename Ty::value_type>(E);
        if (!R)
            return serde_detail::unexpected(R.error());
        Vec.push_back(std::move(*R));
    }
    return Vec;
}

// std::optional(该分支依赖 std::nullopt, 仅 C++17 起存在)
#if PICTOR_HAS_CPP17
template<typename Ty>
infra::compat::enable_if_t<!IsReflected<Ty>::Value && !IsStdVector<Ty>::Value &&
                               IsStdOptional<Ty>::Value,
                           serde_detail::Expected<Ty, std::string> >
fromAdtImpl(const SerdeADT &Value, SerdePriority<2>) {
    if (Value.isNull())
        return Ty{std::nullopt};
    serde_detail::Expected<typename Ty::value_type, std::string> R =
        fromAdt<typename Ty::value_type>(Value);
    if (!R)
        return serde_detail::unexpected(R.error());
    return Ty{std::move(*R)};
}
#endif

// 字符串键映射(map / unordered_map)
template<typename Ty>
infra::compat::enable_if_t<!IsReflected<Ty>::Value && !IsStdVector<Ty>::Value &&
                               !IsStdOptional<Ty>::Value && IsStringMap<Ty>::Value,
                           serde_detail::Expected<Ty, std::string> >
fromAdtImpl(const SerdeADT &Value, SerdePriority<1>) {
    if (!Value.isObject())
        return serde_detail::unexpected("expected object");
    Ty M;
    for (const auto &Pair: Value.asObject()) {
        serde_detail::Expected<typename Ty::mapped_type, std::string> R =
            fromAdt<typename Ty::mapped_type>(Pair.second);
        if (!R)
            return serde_detail::unexpected(R.error());
        M.emplace(Pair.first, std::move(*R));
    }
    return M;
}

// ---- 标量分派: 字符串 > 布尔 > 整数 > 浮点 > 不支持 ----
template<typename Ty>
infra::compat::enable_if_t<std::is_same<Ty, std::string>::value,
                           serde_detail::Expected<Ty, std::string> >
fromAdtScalar(const SerdeADT &Value, SerdePriority<4>) {
    if (!Value.isString())
        return serde_detail::unexpected("expected string");
    return Value.asString();
}

template<typename Ty>
infra::compat::enable_if_t<std::is_same<Ty, bool>::value,
                           serde_detail::Expected<Ty, std::string> >
fromAdtScalar(const SerdeADT &Value, SerdePriority<3>) {
    if (!Value.isBool())
        return serde_detail::unexpected("expected bool");
    return Value.asBool();
}

template<typename Ty>
infra::compat::enable_if_t<std::is_integral<Ty>::value && !std::is_same<Ty, bool>::value,
                           serde_detail::Expected<Ty, std::string> >
fromAdtScalar(const SerdeADT &Value, SerdePriority<2>) {
    if (Value.isInt())
        return static_cast<Ty>(Value.asInt());
    if (Value.isUint())
        return static_cast<Ty>(Value.asUint());
    if (Value.isDouble())
        return static_cast<Ty>(Value.asDouble());
    return serde_detail::unexpected("expected integer");
}

template<typename Ty>
infra::compat::enable_if_t<std::is_floating_point<Ty>::value,
                           serde_detail::Expected<Ty, std::string> >
fromAdtScalar(const SerdeADT &Value, SerdePriority<1>) {
    if (Value.isDouble())
        return static_cast<Ty>(Value.asDouble());
    if (Value.isInt())
        return static_cast<Ty>(Value.asInt());
    if (Value.isUint())
        return static_cast<Ty>(Value.asUint());
    return serde_detail::unexpected("expected number");
}

template<typename Ty>
infra::compat::enable_if_t<!std::is_same<Ty, std::string>::value && !std::is_same<Ty, bool>::value &&
                               !std::is_integral<Ty>::value && !std::is_floating_point<Ty>::value,
                           serde_detail::Expected<Ty, std::string> >
fromAdtScalar(const SerdeADT &Value, SerdePriority<0>) {
    (void) Value;
    return serde_detail::unexpected("fromAdt: unsupported type");
}

// 其余类型(最低优先级): 落入标量分派
template<typename Ty>
infra::compat::enable_if_t<!IsReflected<Ty>::Value && !IsStdVector<Ty>::Value &&
                               !IsStdOptional<Ty>::Value && !IsStringMap<Ty>::Value,
                           serde_detail::Expected<Ty, std::string> >
fromAdtImpl(const SerdeADT &Value, SerdePriority<0>) {
    return fromAdtScalar<Ty>(Value, SerdePriority<4>());
}
} // namespace detail

template<typename Ty>
serde_detail::Expected<Ty, std::string> fromAdt(const SerdeADT &Value) {
    // @notice fromAdtImpl 的 Ty 仅出现在返回值中, 必须显式给出模板实参
    return detail::fromAdtImpl<Ty>(Value, detail::SerdePriority<4>());
}

// 抛异常版（供运行时 invoke 内部使用）
template<typename Ty>
Ty valueCast(const SerdeADT &Value) {
    serde_detail::Expected<Ty, std::string> R = fromAdt<Ty>(Value);
    if (!R)
        throw std::runtime_error(R.error());
    return std::move(*R);
}
} // namespace infra
#endif
