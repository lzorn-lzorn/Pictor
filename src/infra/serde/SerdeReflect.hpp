#pragma once

#include <infra/reflect/TypeInfo.hpp>
#include <infra/serde/SerdeADT.hpp>
#include <expected>
#include <map>
#include <optional>
#include <stdexcept>
#include <unordered_map>
#include <utility>

namespace infra
{

// ========================= 类型检测（toAdt/fromAdt 用） =========================
namespace detail
{
    // 说明：std::false_type / std::true_type 的成员是 value（小写），本文件统一以 Value 暴露
    //       （与 NameOf / VariablesOf / FunctionsOf 保持一致），否则 ::Value 不是合法成员。
    template<typename Ty, typename = void>
    struct IsReflected : std::false_type { static constexpr bool Value = false; };
    template<typename Ty>
    struct IsReflected<Ty, std::void_t<decltype(Ty::reflect_variables)>> : std::true_type { static constexpr bool Value = true; };

    template<typename Ty> struct IsStdVector : std::false_type { static constexpr bool Value = false; };
    template<typename Ty, typename A> struct IsStdVector<std::vector<Ty, A>> : std::true_type { static constexpr bool Value = true; };

    template<typename Ty> struct IsStdOptional : std::false_type { static constexpr bool Value = false; };
    template<typename Ty> struct IsStdOptional<std::optional<Ty>> : std::true_type { static constexpr bool Value = true; };

    template<typename Ty> struct IsStringMap : std::false_type { static constexpr bool Value = false; };
    template<typename V, typename... R> struct IsStringMap<std::map<std::string, V, R...>> : std::true_type { static constexpr bool Value = true; };
    template<typename V, typename... R> struct IsStringMap<std::unordered_map<std::string, V, R...>> : std::true_type { static constexpr bool Value = true; };
}

// ========================= 业务类型 -> SerdeADT（反射驱动） =========================
template<typename Ty>
SerdeADT toAdt(const Ty &Obj)
{
    if constexpr (detail::IsReflected<Ty>::Value)
    {
        SerdeObject Members;
        TypeInfo<Ty>::forEachMembers(Obj, [&Members](const char *Name, const auto &Member) {
            Members.emplace_back(Name, toAdt(Member));
        });
        return SerdeADT{ std::move(Members) };
    }
    else if constexpr (detail::IsStdVector<Ty>::Value)
    {
        SerdeArray Arr;
        for (const auto &E : Obj) Arr.push_back(toAdt(E));
        return SerdeADT{ std::move(Arr) };
    }
    else if constexpr (detail::IsStdOptional<Ty>::Value)
    {
        return Obj ? toAdt(*Obj) : SerdeADT{ nullptr };
    }
    else if constexpr (detail::IsStringMap<Ty>::Value)
    {
        SerdeObject M;
        for (const auto &[K, Value] : Obj) M.emplace_back(K, toAdt(Value));
        return SerdeADT{ std::move(M) };
    }
    else
    {
        return SerdeADT{ Obj }; // 标量
    }
}

// ========================= SerdeADT -> 业务类型（反射驱动） =========================
template<typename Ty>
std::expected<Ty, std::string> fromAdt(const SerdeADT &Value)
{
    if constexpr (detail::IsReflected<Ty>::Value)
    {
        try
        {
            if (!Value.isObject()) return std::unexpected("expected object for reflected type");
            Ty Obj{};
            TypeInfo<Ty>::forEachMembers(Obj, [&Value](const char *Name, auto &Member) {
                if (const SerdeADT *F = Value.find(Name))
                {
                    auto R = fromAdt<std::decay_t<decltype(Member)>>(*F);
                    if (!R) throw std::runtime_error(R.error());
                    Member = std::move(*R);
                }
            });
            return Obj;
        }
        catch (const std::exception &E)
        {
            return std::unexpected(E.what());
        }
    }
    else if constexpr (detail::IsStdVector<Ty>::Value)
    {
        if (!Value.isArray()) return std::unexpected("expected array");
        Ty Vec;
        for (const auto &E : Value.asArray())
        {
            auto R = fromAdt<typename Ty::value_type>(E);
            if (!R) return std::unexpected(R.error());
            Vec.push_back(std::move(*R));
        }
        return Vec;
    }
    else if constexpr (detail::IsStdOptional<Ty>::Value)
    {
        if (Value.isNull()) return Ty{ std::nullopt };
        auto R = fromAdt<typename Ty::value_type>(Value);
        if (!R) return std::unexpected(R.error());
        return Ty{ std::move(*R) };
    }
    else if constexpr (detail::IsStringMap<Ty>::Value)
    {
        if (!Value.isObject()) return std::unexpected("expected object");
        Ty M;
        for (const auto &[K, E] : Value.asObject())
        {
            auto R = fromAdt<typename Ty::mapped_type>(E);
            if (!R) return std::unexpected(R.error());
            M.emplace(K, std::move(*R));
        }
        return M;
    }
    else
    {
        if constexpr (std::is_same_v<Ty, std::string>)
        {
            if (!Value.isString()) return std::unexpected("expected string");
            return Value.asString();
        }
        else if constexpr (std::is_same_v<Ty, bool>)
        {
            if (!Value.isBool()) return std::unexpected("expected bool");
            return Value.asBool();
        }
        else if constexpr (std::is_integral_v<Ty>)
        {
            if (Value.isInt()) return static_cast<Ty>(Value.asInt());
            if (Value.isUint()) return static_cast<Ty>(Value.asUint());
            if (Value.isDouble()) return static_cast<Ty>(Value.asDouble());
            return std::unexpected("expected integer");
        }
        else if constexpr (std::is_floating_point_v<Ty>)
        {
            if (Value.isDouble()) return static_cast<Ty>(Value.asDouble());
            if (Value.isInt()) return static_cast<Ty>(Value.asInt());
            if (Value.isUint()) return static_cast<Ty>(Value.asUint());
            return std::unexpected("expected number");
        }
        else
        {
            return std::unexpected("fromAdt: unsupported type");
        }
    }
}

// 抛异常版（供运行时 invoke 内部使用）
template<typename Ty>
Ty valueCast(const SerdeADT &Value)
{
    auto R = fromAdt<Ty>(Value);
    if (!R) throw std::runtime_error(R.error());
    return std::move(*R);
}


}
