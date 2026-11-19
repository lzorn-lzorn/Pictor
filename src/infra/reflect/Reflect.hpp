#pragma once

#include <infra/reflect/ReflectObject.hpp>
#include <infra/serde/SerdeReflect.hpp>
#include <functional>

namespace infra
{

// ========================= 运行时反射：参数元组（去掉隐式 this） =========================
template<typename Tuple> struct ReflectArgsTuple;
template<typename First, typename... Rest>
struct ReflectArgsTuple<std::tuple<First, Rest...>> { using type = std::tuple<Rest...>; };

template<typename Class, typename MemFn, typename Ret, typename... Args>
SerdeADT invokeMemberImpl(Class &self, MemFn Fn, const SerdeADT &args, std::tuple<Args...> *)
{
    if (!args.isArray()) throw std::invalid_argument("invoke: arguments must be an array");
    const auto &Arr = args.asArray();
    if (Arr.size() != sizeof...(Args)) throw std::invalid_argument("invoke: argument count mismatch");
    return [&]<std::size_t... I>(std::index_sequence<I...>) -> SerdeADT {
        if constexpr (std::is_void_v<Ret>)
        {
            (self.*Fn)(valueCast<std::remove_cvref_t<Args>>(Arr[I])...);
            return SerdeADT{ nullptr };
        }
        else
        {
            return toAdt((self.*Fn)(valueCast<std::remove_cvref_t<Args>>(Arr[I])...));
        }
    }(std::index_sequence_for<Args...>{});
}

template<typename Class, typename MemFn>
SerdeADT invokeMember(Class &self, MemFn Fn, const SerdeADT &args)
{
    using Traits       = ReflectFunctionTraits<MemFn>;
    using ExplicitArgs = typename ReflectArgsTuple<typename Traits::ArgumentTypes>::type;
    return invokeMemberImpl<Class, MemFn, typename Traits::ReturnType>(self, Fn, args, static_cast<ExplicitArgs *>(nullptr));
}

// ========================= 运行时反射：全局注册表 =========================
class ReflectRegistry
{
public:
    using Factory = std::function<ReflectObject(const SerdeADT &)>;
    using Invoker = std::function<SerdeADT(ReflectObject &, const SerdeADT &)>;
    using Getter  = std::function<SerdeADT(ReflectObject &)>;
    using Setter  = std::function<void(ReflectObject &, const SerdeADT &)>;

    static ReflectRegistry &self()
    {
        static ReflectRegistry Instance;
        return Instance;
    }

    void registerEntry(std::string_view Name, Factory factory,
                       std::unordered_map<std::string, Invoker> invokers,
                       std::unordered_map<std::string, Getter> Getters,
                       std::unordered_map<std::string, Setter> Setters,
                       std::string_view BaseName, bool HasBase)
    {
        const std::string Key{ Name };
        Types.emplace(Key,
                       Entry{ std::move(factory), std::move(invokers), std::move(Getters), std::move(Setters) });
        if (HasBase)
        {
            BaseOf[Key] = std::string(BaseName);              // 类型名 -> 直接基类名
            DerivedOf[std::string(BaseName)].push_back(Key);  // 基类名 -> 直接子类名
        }
    }

    [[nodiscard]] bool has(std::string_view Name) const
    {
        return Types.find(std::string(Name)) != Types.end();
    }

    // 通过字符串构造对象
    ReflectObject create(std::string_view Name) const
    {
        return create(Name, SerdeADT{ nullptr });
    }
    ReflectObject create(std::string_view Name, const SerdeADT &args) const
    {
        auto it = Types.find(std::string(Name));
        if (it == Types.end()) 
        {
            throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(Name) + "'");
        }
        return it->second.Factory(args);
    }

    // 通过字符串调用成员方法
    SerdeADT invoke(std::string_view type, std::string_view Fn, ReflectObject &Obj, const SerdeADT &args) const
    {
        auto it = Types.find(std::string(type));
        if (it == Types.end())
        {
            throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(type) + "'");
        }
        auto fit = it->second.Invokers.find(std::string(Fn));
        if (fit == it->second.Invokers.end()) 
        {
            throw std::invalid_argument("ReflectRegistry: unknown function '" + std::string(Fn) + "'");
        }
        return fit->second(Obj, args);
    }

    // 通过字符串访问成员, 读
    SerdeADT get(std::string_view type, std::string_view Member, ReflectObject &Obj) const
    {
        auto it = Types.find(std::string(type));
        if (it == Types.end()) 
        {
            throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(type) + "'");
        }
        auto git = it->second.Getters.find(std::string(Member));
        if (git == it->second.Getters.end()) 
        {
            throw std::invalid_argument("ReflectRegistry: unknown Member '" + std::string(Member) + "'");
        }
        return git->second(Obj);
    }

    // 通过字符串访问成员, 写
    void set(std::string_view type, std::string_view Member, ReflectObject &Obj, const SerdeADT &Value) const
    {
        auto it = Types.find(std::string(type));
        if (it == Types.end()) 
        {
            throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(type) + "'");
        }
        auto sit = it->second.Setters.find(std::string(Member));
        if (sit == it->second.Setters.end()) 
        {
            throw std::invalid_argument("ReflectRegistry: unknown Member '" + std::string(Member) + "'");
        }
        sit->second(Obj, Value);
    }

    // ---- 继承信息查询 ----
    [[nodiscard]] std::optional<std::string> baseOf(std::string_view Name) const
    {
        auto It = BaseOf.find(std::string(Name));
        return It == BaseOf.end() ? std::nullopt : std::optional<std::string>{ It->second };
    }

    [[nodiscard]] std::vector<std::string> derivedOf(std::string_view Name) const
    {
        auto It = DerivedOf.find(std::string(Name));
        return It == DerivedOf.end() ? std::vector<std::string>{} : It->second;
    }

    // 基类全链（自近及远，不含自身）
    [[nodiscard]] std::vector<std::string> baseChainOf(std::string_view Name) const
    {
        std::vector<std::string> Chain;
        std::string Cur{ Name };
        while (auto B = baseOf(Cur))
        {
            Chain.push_back(*B);
            Cur = *B;
        }
        return Chain;
    }

    // 是否（直接或间接）派生自 Base
    [[nodiscard]] bool isDerivedFrom(std::string_view Name, std::string_view Base) const
    {
        std::string Cur{ Name };
        while (auto B = baseOf(Cur))
        {
            if (*B == Base) return true;
            Cur = *B;
        }
        return false;
    }

    [[nodiscard]] std::vector<std::string> typeNames() const
    {
        std::vector<std::string> Names;
        Names.reserve(Types.size());
        for (const auto &[K, V] : Types) Names.push_back(K);
        return Names;
    }

    [[nodiscard]] std::size_t size() const noexcept { return Types.size(); }

private:
    struct Entry
    {
        Factory Factory;
        std::unordered_map<std::string, Invoker> Invokers;
        std::unordered_map<std::string, Getter> Getters;
        std::unordered_map<std::string, Setter> Setters;
    };
    std::unordered_map<std::string, Entry> Types;
    std::unordered_map<std::string, std::string> BaseOf;                  // 类型名 -> 直接基类名
    std::unordered_map<std::string, std::vector<std::string>> DerivedOf;  // 基类名 -> 直接子类名
};

// ---- TypeInfo 的运行时继承查询实现（须在 ReflectRegistry 完整定义之后） ----
template<typename Ty>
std::vector<std::string> TypeInfo<Ty>::derivedNames() { return ReflectRegistry::self().derivedOf(Name); }
template<typename Ty>
std::vector<std::string> TypeInfo<Ty>::baseChain() { return ReflectRegistry::self().baseChainOf(Name); }
template<typename Ty>
bool TypeInfo<Ty>::isDerivedFrom(std::string_view Base) { return ReflectRegistry::self().isDerivedFrom(Name, Base); }

// ========================= 由 TypeInfo<Ty> 生成工厂 / 调用表 / 成员表 =========================
template<typename Ty>
ReflectObject factory(const SerdeADT &args)
{
    if (args.isNull()) return ReflectObject{ Ty{} };
    auto R = fromAdt<Ty>(args);
    if (!R)
    {
        throw std::runtime_error(R.error());
    }
    return ReflectObject{ std::move(*R) };
}

template<typename Ty>
std::unordered_map<std::string, ReflectRegistry::Invoker> buildInvokers()
{
    std::unordered_map<std::string, ReflectRegistry::Invoker> Invokers;
    TypeInfo<Ty>::forEachFunction([&Invokers](const auto &Field) {
        Invokers[std::string(Field.Name)] =
            [Ptr = Field.Pointer](ReflectObject &Obj, const SerdeADT &args) -> SerdeADT {
                return invokeMember(Obj.cast<Ty>(), Ptr, args);
            };
    });
    return Invokers;
}

template<typename Ty, typename FieldType>
void addMemberField(std::unordered_map<std::string, ReflectRegistry::Getter> &Getters,
                    std::unordered_map<std::string, ReflectRegistry::Setter> &Setters,
                    const FieldType &Field)
{
    auto Ptr = Field.Pointer;
    Getters[std::string(Field.Name)] = [Ptr](ReflectObject &Obj) -> SerdeADT {
        return toAdt(Obj.cast<Ty>().*Ptr);
    };
    Setters[std::string(Field.Name)] = [Ptr](ReflectObject &Obj, const SerdeADT &Value) {
        using FieldT = std::decay_t<decltype(Obj.cast<Ty>().*Ptr)>;
        auto R = fromAdt<FieldT>(Value);
        if (!R) throw std::runtime_error(R.error());
        Obj.cast<Ty>().*Ptr = std::move(*R);
    };
}

template<typename Ty, typename ReflectedType = Ty>
void buildMembersInto(std::unordered_map<std::string, ReflectRegistry::Getter> &Getters,
                      std::unordered_map<std::string, ReflectRegistry::Setter> &Setters)
{
    if constexpr (TypeInfo<ReflectedType>::HasBase)
        buildMembersInto<Ty, typename TypeInfo<ReflectedType>::Base>(Getters, Setters);
    std::apply([&](auto &...Field) {
        ((addMemberField<Ty>(Getters, Setters, Field)), ...);
    }, TypeInfo<ReflectedType>::Variables);
}

template<typename Ty>
void registerReflect()
{
    constexpr std::string_view Name = TypeInfo<Ty>::Name;
    if (ReflectRegistry::self().has(Name)) return; // 幂等：自动注册可能被多次触发（多 TU / 显式调用）

    auto Invokers = buildInvokers<Ty>();
    std::unordered_map<std::string, ReflectRegistry::Getter> Getters;
    std::unordered_map<std::string, ReflectRegistry::Setter> Setters;
    buildMembersInto<Ty>(Getters, Setters);
    ReflectRegistry::self().registerEntry(Name, &factory<Ty>,
                                          std::move(Invokers), std::move(Getters), std::move(Setters),
                                          TypeInfo<Ty>::BaseName, TypeInfo<Ty>::HasBase);
}

// ========================= 自动注册 =========================
// REFLECT_CLASS 展开出的 inline static 成员在本翻译单元的静态初始化阶段构造，
// 其构造函数统一调用 registerReflect<Ty>()：无需为每个反射类型手写注册代码，
// 且所有注册均在进入 main 之前完成。
namespace detail
{
    template<typename Ty>
    struct ReflectAutoRegistrar
    {
        ReflectAutoRegistrar() { registerReflect<Ty>(); }
    };
}

} // namespace infra

// 全局别名：让用户写 REFLECT_CLASS(Person, ReflectNullBase) 时无需 infra:: 前缀
using ReflectNullBase = infra::ReflectNullBase;

// ========================= 注册宏 =========================
#define REFLECT_FIRST_SELECT(first, ...) first
#define REFLECT_FIRST(...) REFLECT_FIRST_SELECT(__VA_ARGS__ __VA_OPT__(,) ReflectNullBase)

#define PP_CAT(a, b) PP_CAT_I(a, b)
#define PP_CAT_I(a, b) a##b
#define PP_NARG(...) PP_NARG_I(__VA_ARGS__, 16,15,14,13,12,11,10,9,8,7,6,5,4,3,2,1)
#define PP_NARG_I(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,N,...) N

#define PP_FOR_EACH(F, ...) PP_CAT(PP_FOR_EACH_, PP_NARG(__VA_ARGS__))(F, __VA_ARGS__)
#define PP_FOR_EACH_1(F,a) F(a)
#define PP_FOR_EACH_2(F,a,b) F(a) F(b)
#define PP_FOR_EACH_3(F,a,b,c) F(a) F(b) F(c)
#define PP_FOR_EACH_4(F,a,b,c,d) F(a) F(b) F(c) F(d)
#define PP_FOR_EACH_5(F,a,b,c,d,E) F(a) F(b) F(c) F(d) F(E)
#define PP_FOR_EACH_6(F,a,b,c,d,E,f) F(a) F(b) F(c) F(d) F(E) F(f)
#define PP_FOR_EACH_7(F,a,b,c,d,E,f,g) F(a) F(b) F(c) F(d) F(E) F(f) F(g)
#define PP_FOR_EACH_8(F,a,b,c,d,E,f,g,h) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h)
#define PP_FOR_EACH_9(F,a,b,c,d,E,f,g,h,i) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i)
#define PP_FOR_EACH_10(F,a,b,c,d,E,f,g,h,i,j) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j)
#define PP_FOR_EACH_11(F,a,b,c,d,E,f,g,h,i,j,K) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j) F(K)
#define PP_FOR_EACH_12(F,a,b,c,d,E,f,g,h,i,j,K,l) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j) F(K) F(l)
#define PP_FOR_EACH_13(F,a,b,c,d,E,f,g,h,i,j,K,l,M) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j) F(K) F(l) F(M)
#define PP_FOR_EACH_14(F,a,b,c,d,E,f,g,h,i,j,K,l,M,n) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j) F(K) F(l) F(M) F(n)
#define PP_FOR_EACH_15(F,a,b,c,d,E,f,g,h,i,j,K,l,M,n,o) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j) F(K) F(l) F(M) F(n) F(o)
#define PP_FOR_EACH_16(F,a,b,c,d,E,f,g,h,i,j,K,l,M,n,o,p) F(a) F(b) F(c) F(d) F(E) F(f) F(g) F(h) F(i) F(j) F(K) F(l) F(M) F(n) F(o) F(p)

#define PP_MAP(F, ...) PP_CAT(PP_MAP_, PP_NARG(__VA_ARGS__))(F, __VA_ARGS__)
#define PP_MAP_1(F,a) F(a)
#define PP_MAP_2(F,a,b) F(a), F(b)
#define PP_MAP_3(F,a,b,c) F(a), F(b), F(c)
#define PP_MAP_4(F,a,b,c,d) F(a), F(b), F(c), F(d)
#define PP_MAP_5(F,a,b,c,d,E) F(a), F(b), F(c), F(d), F(E)
#define PP_MAP_6(F,a,b,c,d,E,f) F(a), F(b), F(c), F(d), F(E), F(f)
#define PP_MAP_7(F,a,b,c,d,E,f,g) F(a), F(b), F(c), F(d), F(E), F(f), F(g)
#define PP_MAP_8(F,a,b,c,d,E,f,g,h) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h)
#define PP_MAP_9(F,a,b,c,d,E,f,g,h,i) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i)
#define PP_MAP_10(F,a,b,c,d,E,f,g,h,i,j) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j)
#define PP_MAP_11(F,a,b,c,d,E,f,g,h,i,j,K) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j), F(K)
#define PP_MAP_12(F,a,b,c,d,E,f,g,h,i,j,K,l) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j), F(K), F(l)
#define PP_MAP_13(F,a,b,c,d,E,f,g,h,i,j,K,l,M) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j), F(K), F(l), F(M)
#define PP_MAP_14(F,a,b,c,d,E,f,g,h,i,j,K,l,M,n) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j), F(K), F(l), F(M), F(n)
#define PP_MAP_15(F,a,b,c,d,E,f,g,h,i,j,K,l,M,n,o) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j), F(K), F(l), F(M), F(n), F(o)
#define PP_MAP_16(F,a,b,c,d,E,f,g,h,i,j,K,l,M,n,o,p) F(a), F(b), F(c), F(d), F(E), F(f), F(g), F(h), F(i), F(j), F(K), F(l), F(M), F(n), F(o), F(p)

// 类元信息 + staticClass；须写在类内、先于 REFLECT_FIELDS / REFLECT_FUNCTIONS
// reflect_registrar：静态初始化阶段自动调用 registerReflect<Type>()，无需手写注册
#define REFLECT_CLASS(Type, ...)                              \
    static constexpr std::string_view reflect_name = #Type;   \
    using reflect_self = Type;                                \
    using reflect_base = REFLECT_FIRST(__VA_ARGS__);          \
    static auto StaticClass() { return infra::TypeInfo<Type>{}; } \
    inline static const infra::detail::ReflectAutoRegistrar<Type> reflect_registrar{}

#define REFLECT_FIELD_DECL(P)  REFLECT_FIELD_DECL_I P
#define REFLECT_FIELD_DECL_I(Ty, N) Ty N;
#define REFLECT_FIELD_ENTRY(P) REFLECT_FIELD_ENTRY_I P
#define REFLECT_FIELD_ENTRY_I(Ty, N) infra::ReflectFieldTraits{ &reflect_self::N, #N }
#define REFLECT_FIELDS(...)                                   \
    PP_FOR_EACH(REFLECT_FIELD_DECL, __VA_ARGS__)              \
    static constexpr auto reflect_variables =                 \
        std::make_tuple(PP_MAP(REFLECT_FIELD_ENTRY, __VA_ARGS__))

#define REFLECT_FN_ENTRY(N) infra::ReflectFieldTraits{ &reflect_self::N, #N }
#define REFLECT_FUNCTIONS(...)                                \
    static constexpr auto reflect_functions =                 \
        std::make_tuple(__VA_OPT__(PP_MAP(REFLECT_FN_ENTRY, __VA_ARGS__)))

