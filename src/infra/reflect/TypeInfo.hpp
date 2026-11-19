#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace infra
{

struct ReflectNullBase {};

// ========================= 成员指针的类类型萃取 =========================
template<typename Ty> struct ReflectMemberPointerClass;
template<typename Class, typename Ty>
struct ReflectMemberPointerClass<Ty Class::*> { using type = Class; };
template<typename Ty>
using ReflectMemberPointerClass_t = typename ReflectMemberPointerClass<Ty>::type;

// ========================= 变量类型萃取 =========================
template<typename Ty>
struct ReflectVariableType
{
    using OriginalType = Ty;
    using PureType     = std::remove_cvref_t<Ty>;
};
template<typename Class, typename Ty>
struct ReflectVariableType<Ty Class::*>
{
    using OriginalType = Ty;
    using PureType     = std::remove_cvref_t<Ty>;
};

// ========================= FunctionTraits =========================
template<typename Ty> struct ReflectFunctionTraits;

template<typename Ret, typename... Args>
struct ReflectFunctionTraits<Ret(Args...)>
{
    using ReturnType = Ret;
    using ArgumentTypes = std::tuple<Args...>;
    using ClassType = void;
    static constexpr std::size_t Arity = sizeof...(Args);
    static constexpr bool IsMemberFunction = false;
    static constexpr bool IsConst = false;
    static constexpr bool IsFunction = true;
    static constexpr bool IsVariable = false;
};

template<typename Ret, typename Class, typename... Args>
struct ReflectFunctionTraits<Ret(Class::*)(Args...)>
{
    using ReturnType = Ret;
    using ArgumentTypes = std::tuple<Class *, Args...>;
    using ClassType = Class;
    static constexpr std::size_t Arity = sizeof...(Args);
    static constexpr bool IsMemberFunction = true;
    static constexpr bool IsConst = false;
    static constexpr bool IsFunction = true;
    static constexpr bool IsVariable = false;
};

template<typename Ret, typename Class, typename... Args>
struct ReflectFunctionTraits<Ret(Class::*)(Args...) const>
{
    using ReturnType = Ret;
    using ArgumentTypes = std::tuple<const Class *, Args...>;
    using ClassType = Class;
    static constexpr std::size_t Arity = sizeof...(Args);
    static constexpr bool IsMemberFunction = true;
    static constexpr bool IsConst = true;
    static constexpr bool IsFunction = true;
    static constexpr bool IsVariable = false;
};

// ========================= VariableTraits =========================
template<typename Ty>
struct ReflectVariableTraits
{
    using OriginalType = typename ReflectVariableType<Ty>::OriginalType;
    using PureType     = typename ReflectVariableType<Ty>::PureType;
    using ClassType    = void;
    using ReturnType   = void;
    using ArgumentTypes = std::tuple<>;
    static constexpr std::size_t Arity = 0;
    static constexpr bool IsMemberFunction = false;
    static constexpr bool IsConst = false;
    static constexpr bool IsFunction = false;
    static constexpr bool IsVariable = true;
    static constexpr bool IsMemberPointer = false;
    static constexpr bool IsPointer = std::is_pointer_v<Ty>;
    static constexpr bool IsReference = std::is_reference_v<Ty>;
};

template<typename Ty>
    requires std::is_member_object_pointer_v<Ty>
struct ReflectVariableTraits<Ty>
{
    using OriginalType = typename ReflectVariableType<Ty>::OriginalType;
    using PureType     = typename ReflectVariableType<Ty>::PureType;
    using ClassType    = ReflectMemberPointerClass_t<Ty>;
    using ReturnType   = void;
    using ArgumentTypes = std::tuple<>;
    static constexpr std::size_t Arity = 0;
    static constexpr bool IsMemberFunction = false;
    static constexpr bool IsConst = std::is_const_v<std::remove_reference_t<OriginalType>>;
    static constexpr bool IsFunction = false;
    static constexpr bool IsVariable = true;
    static constexpr bool IsMemberPointer = true;
    static constexpr bool IsPointer = false;
    static constexpr bool IsReference = false;
};

// ========================= BasicFieldTraits =========================
template<typename Ty>
struct ReflectBasicFieldTraits
{
private:
    static constexpr bool IsMemFn = std::is_member_function_pointer_v<Ty>;
    using Traits = std::conditional_t<IsMemFn, ReflectFunctionTraits<Ty>, ReflectVariableTraits<Ty>>;
public:
    using ReturnType = typename Traits::ReturnType;
    using ArgumentTypes = typename Traits::ArgumentTypes;
    using ClassType = typename Traits::ClassType;
    static constexpr std::size_t Arity = Traits::Arity;
    static constexpr bool IsMemberFunction = Traits::IsMemberFunction;
    static constexpr bool IsConst = Traits::IsConst;
    static constexpr bool IsFunction = Traits::IsFunction;
    static constexpr bool IsVariable = Traits::IsVariable;
};

// ========================= FieldTraits =========================
template<typename Ty>
struct ReflectFieldTraits : ReflectBasicFieldTraits<Ty>
{
    Ty Pointer;
    std::string_view Name;

    static constexpr std::string_view realName(std::string_view raw) noexcept
    {
        auto pos = raw.find_last_of(':');
        return (pos == std::string_view::npos) ? raw : raw.substr(pos + 1);
    }
    constexpr ReflectFieldTraits(Ty Ptr, std::string_view raw) noexcept
        : Pointer(Ptr), Name(realName(raw)) {}
};
template<typename Ty>
ReflectFieldTraits(Ty, std::string_view) -> ReflectFieldTraits<Ty>;

// ========================= 检测 Ty 是否有 reflect_* 静态成员 =========================
namespace detail
{
    template<typename Ty, typename = void>
    struct NameOf { static constexpr std::string_view Value = ""; };
    template<typename Ty>
    struct NameOf<Ty, std::void_t<decltype(Ty::reflect_name)>> { static constexpr std::string_view Value = Ty::reflect_name; };

    template<typename Ty, typename = void>
    struct BaseOf { using Type = ReflectNullBase; };
    template<typename Ty>
    struct BaseOf<Ty, std::void_t<typename Ty::reflect_base>> { using Type = typename Ty::reflect_base; };

    template<typename Ty, typename = void>
    struct VariablesOf { static constexpr auto Value = std::tuple{}; };
    template<typename Ty>
    struct VariablesOf<Ty, std::void_t<decltype(Ty::reflect_variables)>> { static constexpr auto Value = Ty::reflect_variables; };

    template<typename Ty, typename = void>
    struct FunctionsOf { static constexpr auto Value = std::tuple{}; };
    template<typename Ty>
    struct FunctionsOf<Ty, std::void_t<decltype(Ty::reflect_functions)>> { static constexpr auto Value = Ty::reflect_functions; };
}

// ========================= 通用 TypeInfo =========================
template<typename Ty>
struct TypeInfo
{
    static const inline char TypeId{};   // 每类型唯一标识（取地址作运行时 ID，不依赖 RTTI）
    static constexpr std::string_view Name = detail::NameOf<Ty>::Value;
    using Base = typename detail::BaseOf<Ty>::Type;
    static constexpr bool HasBase = !std::is_same_v<Base, ReflectNullBase>;
    // 直接基类名：无基类时 Base == ReflectNullBase，其 Name 为空串，故这里天然得到空串
    static constexpr std::string_view BaseName = TypeInfo<Base>::Name;
    static constexpr auto Functions = detail::FunctionsOf<Ty>::Value;
    static constexpr auto Variables = detail::VariablesOf<Ty>::Value;

    static constexpr const char *getClassName() { return Name.data(); }

    // ---- 运行时查询：基类 / 子类信息（父/子关系由 ReflectRegistry 在注册时汇总） ----
    static std::string_view baseName() noexcept { return BaseName; }  // 直接基类名（编译期即知）
    static std::vector<std::string> derivedNames();                    // 直接子类名（定义见 ReflectRegistry 之后）
    static std::vector<std::string> baseChain();                       // 基类全链（自近及远，不含自身）
    static bool isDerivedFrom(std::string_view Base);                  // 是否（直接或间接）派生自 Base

    template<typename Callable>
    static void forEachMembers(Ty &Obj, Callable &&Fn)
    {
        if constexpr (HasBase) TypeInfo<Base>::forEachMembers(Obj, std::forward<Callable>(Fn));
        std::apply([&](auto &...Field) {
            (Fn(Field.Name.data(), Obj.*(Field.Pointer)), ...);
        }, Variables);
    }
    template<typename Callable>
    static void forEachMembers(const Ty &Obj, Callable &&Fn)
    {
        if constexpr (HasBase) TypeInfo<Base>::forEachMembers(Obj, std::forward<Callable>(Fn));
        std::apply([&](auto &...Field) {
            (Fn(Field.Name.data(), Obj.*(Field.Pointer)), ...);
        }, Variables);
    }
    template<typename Callable>
    static void forEachFunction(Callable &&Fn)
    {
        if constexpr (HasBase) TypeInfo<Base>::forEachFunction(std::forward<Callable>(Fn));
        std::apply([&](auto &...Field) { (Fn(Field), ...); }, Functions);
    }
};

template<typename Ty>
constexpr TypeInfo<Ty> StaticClass() { return TypeInfo<Ty>{}; }


}
