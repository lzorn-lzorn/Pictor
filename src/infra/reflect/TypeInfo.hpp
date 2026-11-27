#pragma once

#include <infra/common/VersionCompatibility.hpp>
#include <cstddef>
#include <string>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace infra{
struct ReflectNullBase {
};

// ========================= 成员指针的类类型萃取 =========================
template<typename Ty>
struct ReflectMemberPointerClass;

template<typename Class, typename Ty>
struct ReflectMemberPointerClass<Ty Class::*> {
    using type = Class;
};

template<typename Ty>
using ReflectMemberPointerClass_t = typename ReflectMemberPointerClass<Ty>::type;

// ========================= 变量类型萃取 =========================
template<typename Ty>
struct ReflectVariableType {
    using OriginalType = Ty;
    using PureType     = infra::compat::remove_cvref_t<Ty>;
};

template<typename Class, typename Ty>
struct ReflectVariableType<Ty Class::*> {
    using OriginalType = Ty;
    using PureType     = infra::compat::remove_cvref_t<Ty>;
};

// ========================= FunctionTraits =========================
template<typename Ty>
struct ReflectFunctionTraits;

template<typename Ret, typename... Args>
struct ReflectFunctionTraits<Ret(Args...)> {
    using ReturnType                       = Ret;
    using ArgumentTypes                    = std::tuple<Args...>;
    using ClassType                        = void;
    static constexpr std::size_t Arity     = sizeof...(Args);
    static constexpr bool IsMemberFunction = false;
    static constexpr bool IsConst          = false;
    static constexpr bool IsFunction       = true;
    static constexpr bool IsVariable       = false;
};

template<typename Ret, typename Class, typename... Args>
struct ReflectFunctionTraits<Ret(Class::*)(Args...)> {
    using ReturnType                       = Ret;
    using ArgumentTypes                    = std::tuple<Class *, Args...>;
    using ClassType                        = Class;
    static constexpr std::size_t Arity     = sizeof...(Args);
    static constexpr bool IsMemberFunction = true;
    static constexpr bool IsConst          = false;
    static constexpr bool IsFunction       = true;
    static constexpr bool IsVariable       = false;
};

template<typename Ret, typename Class, typename... Args>
struct ReflectFunctionTraits<Ret(Class::*)(Args...) const> {
    using ReturnType                       = Ret;
    using ArgumentTypes                    = std::tuple<const Class *, Args...>;
    using ClassType                        = Class;
    static constexpr std::size_t Arity     = sizeof...(Args);
    static constexpr bool IsMemberFunction = true;
    static constexpr bool IsConst          = true;
    static constexpr bool IsFunction       = true;
    static constexpr bool IsVariable       = false;
};

// ========================= VariableTraits =========================
// @notice 主模板带第二默认参数，成员对象指针用 enable_if 偏特化
//         （C++11 兼容，替代 C++20 的 requires 偏特化约束）。
template<typename Ty, typename = void>
struct ReflectVariableTraits {
    using OriginalType                     = typename ReflectVariableType<Ty>::OriginalType;
    using PureType                         = typename ReflectVariableType<Ty>::PureType;
    using ClassType                        = void;
    using ReturnType                       = void;
    using ArgumentTypes                    = std::tuple<>;
    static constexpr std::size_t Arity     = 0;
    static constexpr bool IsMemberFunction = false;
    static constexpr bool IsConst          = false;
    static constexpr bool IsFunction       = false;
    static constexpr bool IsVariable       = true;
    static constexpr bool IsMemberPointer  = false;
    static constexpr bool IsPointer        = std::is_pointer<Ty>::value;
    static constexpr bool IsReference      = std::is_reference<Ty>::value;
};

template<typename Ty>
struct ReflectVariableTraits<Ty, infra::compat::enable_if_t<std::is_member_object_pointer<Ty>::value> > {
    using OriginalType                     = typename ReflectVariableType<Ty>::OriginalType;
    using PureType                         = typename ReflectVariableType<Ty>::PureType;
    using ClassType                        = ReflectMemberPointerClass_t<Ty>;
    using ReturnType                       = void;
    using ArgumentTypes                    = std::tuple<>;
    static constexpr std::size_t Arity     = 0;
    static constexpr bool IsMemberFunction = false;
    static constexpr bool IsConst          = std::is_const<infra::compat::remove_reference_t<OriginalType> >::value;
    static constexpr bool IsFunction       = false;
    static constexpr bool IsVariable       = true;
    static constexpr bool IsMemberPointer  = true;
    static constexpr bool IsPointer        = false;
    static constexpr bool IsReference      = false;
};

// ========================= BasicFieldTraits =========================
template<typename Ty>
struct ReflectBasicFieldTraits {
private:
    static constexpr bool IsMemFn = std::is_member_function_pointer<Ty>::value;
    using Traits                  = infra::compat::conditional_t<IsMemFn, ReflectFunctionTraits<Ty>, ReflectVariableTraits<Ty> >;

public:
    using ReturnType                       = typename Traits::ReturnType;
    using ArgumentTypes                    = typename Traits::ArgumentTypes;
    using ClassType                        = typename Traits::ClassType;
    static constexpr std::size_t Arity     = Traits::Arity;
    static constexpr bool IsMemberFunction = Traits::IsMemberFunction;
    static constexpr bool IsConst          = Traits::IsConst;
    static constexpr bool IsFunction       = Traits::IsFunction;
    static constexpr bool IsVariable       = Traits::IsVariable;
};

// ========================= FieldTraits =========================
template<typename Ty>
struct ReflectFieldTraits : ReflectBasicFieldTraits<Ty> {
    Ty Pointer;
    infra::compat::string_view Name;

    // @notice C++11 垫片 string_view 的 find_last_of/substr 非 constexpr，故用
    //         PICTOR_CONSTEXPR14：C++14 起为 constexpr，C++11 下退化为普通函数。
    PICTOR_CONSTEXPR14 static infra::compat::string_view realName(infra::compat::string_view raw) noexcept {
        auto pos = raw.find_last_of(':');
        return (pos == infra::compat::string_view::npos) ? raw : raw.substr(pos + 1);
    }

    PICTOR_CONSTEXPR14 ReflectFieldTraits(Ty Ptr, infra::compat::string_view raw) noexcept
        : Pointer(Ptr),
          Name(realName(raw)) {
    }
};

// ========================= 检测 Ty 是否有 reflect_* 静态成员 =========================
namespace detail{
template<typename Ty, typename = void>
struct NameOf {
    static constexpr infra::compat::string_view Value = {};
};

template<typename Ty>
struct NameOf<Ty, infra::compat::void_t<decltype(Ty::reflect_name)> > {
    static constexpr infra::compat::string_view Value = Ty::reflect_name;
};

template<typename Ty, typename = void>
struct BaseOf {
    using Type = ReflectNullBase;
};

template<typename Ty>
struct BaseOf<Ty, infra::compat::void_t<typename Ty::reflect_base> > {
    using Type = typename Ty::reflect_base;
};

// @notice C++11/14 下 std::tuple 无 constexpr 构造，Value 退化为普通静态成员
//         （类外定义经 Ty::reflectVariables()/reflectFunctions() 取宏展开的
//         函数内静态局部变量，保证有定义且无 ODR 问题）；C++17 起保持原
//         constexpr 静态成员写法。
template<typename Ty, typename = void>
struct VariablesOf {
    using Tuple = std::tuple<>;
#if PICTOR_HAS_CPP17
    static constexpr Tuple Value = Tuple();
#else
    static const Tuple Value;
#endif
};

template<typename Ty>
struct VariablesOf<Ty, infra::compat::void_t<decltype(Ty::reflectVariables())> > {
    using Tuple = infra::compat::remove_cvref_t<decltype(Ty::reflectVariables())>;
#if PICTOR_HAS_CPP17
    static constexpr Tuple Value = Ty::reflect_variables;
#else
    static const Tuple Value;
#endif
};

template<typename Ty, typename = void>
struct FunctionsOf {
    using Tuple = std::tuple<>;
#if PICTOR_HAS_CPP17
    static constexpr Tuple Value = Tuple();
#else
    static const Tuple Value;
#endif
};

template<typename Ty>
struct FunctionsOf<Ty, infra::compat::void_t<decltype(Ty::reflectFunctions())> > {
    using Tuple = infra::compat::remove_cvref_t<decltype(Ty::reflectFunctions())>;
#if PICTOR_HAS_CPP17
    static constexpr Tuple Value = Ty::reflect_functions;
#else
    static const Tuple Value;
#endif
};
}

// ========================= 通用 TypeInfo =========================
template<typename Ty>
struct TypeInfo {
    PICTOR_INLINE_VARIABLE static constexpr char TypeId{}; // 每类型唯一标识（取地址作运行时 ID，不依赖 RTTI）
    static constexpr infra::compat::string_view Name     = detail::NameOf<Ty>::Value;
    using Base                                 = typename detail::BaseOf<Ty>::Type;
    static constexpr bool HasBase              = !std::is_same<Base, ReflectNullBase>::value;
    // 直接基类名：无基类时 Base == ReflectNullBase，其 Name 为空串，故这里天然得到空串
    static constexpr infra::compat::string_view BaseName = TypeInfo<Base>::Name;
#if PICTOR_HAS_CPP17
    static constexpr auto Functions            = detail::FunctionsOf<Ty>::Value;
    static constexpr auto Variables            = detail::VariablesOf<Ty>::Value;
#else
    // @notice C++11/14 无 constexpr tuple，退化为普通静态成员（类外定义见文件末尾）
    static const typename detail::FunctionsOf<Ty>::Tuple Functions;
    static const typename detail::VariablesOf<Ty>::Tuple Variables;
#endif

    static constexpr const char *getClassName() {
        return Name.data();
    }

    // ---- 运行时查询：基类 / 子类信息（父/子关系由 ReflectRegistry 在注册时汇总） ----
    static infra::compat::string_view baseName() noexcept {
        return BaseName;
    }                                                 // 直接基类名（编译期即知）
    static std::vector<std::string> derivedNames();   // 直接子类名（定义见 ReflectRegistry 之后）
    static std::vector<std::string> baseChain();      // 基类全链（自近及远，不含自身）
    static bool isDerivedFrom(infra::compat::string_view Base); // 是否（直接或间接）派生自 Base

    template<typename Callable>
    static void forEachMembers(Ty &Obj, Callable &&Fn) {
        forEachMembersImpl(Obj, std::forward<Callable>(Fn), std::integral_constant<bool, HasBase>());
    }

    template<typename Callable>
    static void forEachMembers(const Ty &Obj, Callable &&Fn) {
        forEachMembersImpl(Obj, std::forward<Callable>(Fn), std::integral_constant<bool, HasBase>());
    }

    template<typename Callable>
    static void forEachFunction(Callable &&Fn) {
        forEachFunctionImpl(std::forward<Callable>(Fn), std::integral_constant<bool, HasBase>());
    }

private:
    // ---- if constexpr (HasBase) 的 C++11 等价：标签分派 ----
    template<typename Callable>
    static void forEachMembersImpl(Ty &Obj, Callable &&Fn, std::true_type) {
        TypeInfo<Base>::forEachMembers(Obj, std::forward<Callable>(Fn));
        applyMembers(Obj, Fn, Variables, infra::compat::make_index_sequence<std::tuple_size<typename detail::VariablesOf<Ty>::Tuple>::value>());
    }

    template<typename Callable>
    static void forEachMembersImpl(Ty &Obj, Callable &&Fn, std::false_type) {
        applyMembers(Obj, Fn, Variables, infra::compat::make_index_sequence<std::tuple_size<typename detail::VariablesOf<Ty>::Tuple>::value>());
    }

    template<typename Callable>
    static void forEachMembersImpl(const Ty &Obj, Callable &&Fn, std::true_type) {
        TypeInfo<Base>::forEachMembers(Obj, std::forward<Callable>(Fn));
        applyMembers(Obj, Fn, Variables, infra::compat::make_index_sequence<std::tuple_size<typename detail::VariablesOf<Ty>::Tuple>::value>());
    }

    template<typename Callable>
    static void forEachMembersImpl(const Ty &Obj, Callable &&Fn, std::false_type) {
        applyMembers(Obj, Fn, Variables, infra::compat::make_index_sequence<std::tuple_size<typename detail::VariablesOf<Ty>::Tuple>::value>());
    }

    template<typename Callable>
    static void forEachFunctionImpl(Callable &&Fn, std::true_type) {
        TypeInfo<Base>::forEachFunction(std::forward<Callable>(Fn));
        applyFunctions(Fn, Functions, infra::compat::make_index_sequence<std::tuple_size<typename detail::FunctionsOf<Ty>::Tuple>::value>());
    }

    template<typename Callable>
    static void forEachFunctionImpl(Callable &&Fn, std::false_type) {
        applyFunctions(Fn, Functions, infra::compat::make_index_sequence<std::tuple_size<typename detail::FunctionsOf<Ty>::Tuple>::value>());
    }

    // ---- std::apply + 折叠表达式的 C++11 等价：index_sequence + 数组初始化展开 ----
    template<typename Ty2, typename Callable, typename Tuple, std::size_t... I>
    static void applyMembers(Ty2 &Obj, Callable &Fn, Tuple const &T, infra::compat::index_sequence<I...>) {
        int Dummy[] = {0, (Fn(std::get<I>(T).Name.data(), Obj.*(std::get<I>(T).Pointer)), 0)...};
        (void)Dummy;
    }

    template<typename Callable, typename Tuple, std::size_t... I>
    static void applyFunctions(Callable &Fn, Tuple const &T, infra::compat::index_sequence<I...>) {
        int Dummy[] = {0, (Fn(std::get<I>(T)), 0)...};
        (void)Dummy;
    }
};

// ========================= C++11/14 静态成员类外定义 =========================
// @notice C++17 起 constexpr 静态数据成员隐式 inline，无需类外定义；C++11/14 下
//         被 odr-use 的成员必须在此给出定义（类内已有初始化的不带初始化器）。
#if !PICTOR_HAS_CPP17
template<typename Ty>
constexpr char TypeInfo<Ty>::TypeId;

template<typename Ty>
constexpr infra::compat::string_view TypeInfo<Ty>::Name;

template<typename Ty>
constexpr infra::compat::string_view TypeInfo<Ty>::BaseName;

template<typename Ty>
const typename detail::FunctionsOf<Ty>::Tuple TypeInfo<Ty>::Functions = detail::FunctionsOf<Ty>::Value;

template<typename Ty>
const typename detail::VariablesOf<Ty>::Tuple TypeInfo<Ty>::Variables = detail::VariablesOf<Ty>::Value;

namespace detail{
// @notice clang 在 C++11 模式下要求主模板成员的类外定义写全模板参数列表
//         （默认参数不算数），故主模板成员定义用 (Ty, Dummy) 两参形式。
template<typename Ty, typename Dummy>
constexpr infra::compat::string_view NameOf<Ty, Dummy>::Value;

template<typename Ty>
constexpr infra::compat::string_view NameOf<Ty, infra::compat::void_t<decltype(Ty::reflect_name)> >::Value;

template<typename Ty, typename Dummy>
const typename VariablesOf<Ty, Dummy>::Tuple VariablesOf<Ty, Dummy>::Value;

template<typename Ty>
const typename VariablesOf<Ty, infra::compat::void_t<decltype(Ty::reflectVariables())> >::Tuple
    VariablesOf<Ty, infra::compat::void_t<decltype(Ty::reflectVariables())> >::Value = Ty::reflectVariables();

template<typename Ty, typename Dummy>
const typename FunctionsOf<Ty, Dummy>::Tuple FunctionsOf<Ty, Dummy>::Value;

template<typename Ty>
const typename FunctionsOf<Ty, infra::compat::void_t<decltype(Ty::reflectFunctions())> >::Tuple
    FunctionsOf<Ty, infra::compat::void_t<decltype(Ty::reflectFunctions())> >::Value = Ty::reflectFunctions();
}
#endif

template<typename Ty>
constexpr TypeInfo<Ty> StaticClass() {
    return TypeInfo<Ty>{};
}
}
