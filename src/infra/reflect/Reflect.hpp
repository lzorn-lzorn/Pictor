#pragma once

#include <infra/common/VersionCompatibility.hpp>
#include <infra/reflect/ReflectObject.hpp>
#include <infra/serde/SerdeReflect.hpp>
#include <functional>

namespace infra{
// ========================= 运行时反射：参数元组（去掉隐式 this） =========================
template<typename Tuple>
struct ReflectArgsTuple;

template<typename First, typename... Rest>
struct ReflectArgsTuple<std::tuple<First, Rest...> > {
    using type = std::tuple<Rest...>;
};

// ---- 原显式模板 lambda 的 C++11 等价：命名 helper + 标签分派 + index_sequence 展开 ----
template<typename Class, typename MemFn, typename Array, typename Tuple, std::size_t... I>
SerdeADT invokeAndPack(Class &self, MemFn Fn, const Array &Arr, Tuple *,
                       infra::compat::index_sequence<I...>, std::true_type) {
    // Ret == void：调用后返回 null
    (self.*Fn)(valueCast<infra::compat::remove_cvref_t<typename std::tuple_element<I, Tuple>::type> >(Arr[I])...);
    return SerdeADT{nullptr};
}

template<typename Class, typename MemFn, typename Array, typename Tuple, std::size_t... I>
SerdeADT invokeAndPack(Class &self, MemFn Fn, const Array &Arr, Tuple *,
                       infra::compat::index_sequence<I...>, std::false_type) {
    return toAdt((self.*Fn)(valueCast<infra::compat::remove_cvref_t<typename std::tuple_element<I, Tuple>::type> >(Arr[I])...));
}

template<typename Class, typename MemFn, typename Ret, typename... Args>
SerdeADT invokeMemberImpl(Class &self, MemFn Fn, const SerdeADT &args, std::tuple<Args...> *) {
    if (!args.isArray())
        throw std::invalid_argument("invoke: arguments must be an array");
    const auto &Arr = args.asArray();
    if (Arr.size() != sizeof...(Args))
        throw std::invalid_argument("invoke: argument count mismatch");
    return invokeAndPack<Class, MemFn>(self,
                                       Fn,
                                       Arr,
                                       static_cast<std::tuple<Args...> *>(nullptr),
                                       infra::compat::index_sequence_for<Args...>(),
                                       std::integral_constant<bool, std::is_void<Ret>::value>());
}

template<typename Class, typename MemFn>
SerdeADT invokeMember(Class &self, MemFn Fn, const SerdeADT &args) {
    using Traits       = ReflectFunctionTraits<MemFn>;
    using ExplicitArgs = typename ReflectArgsTuple<typename Traits::ArgumentTypes>::type;
    return invokeMemberImpl<Class, MemFn, typename Traits::ReturnType>(self,
                                                                       Fn,
                                                                       args,
                                                                       static_cast<ExplicitArgs *>(nullptr));
}

// ========================= 运行时反射：全局注册表 =========================
class ReflectRegistry {
public:
    using Factory = std::function<ReflectObject(const SerdeADT &)>;
    using Invoker = std::function<SerdeADT(ReflectObject &, const SerdeADT &)>;
    using Getter  = std::function<SerdeADT(ReflectObject &)>;
    using Setter  = std::function<void(ReflectObject &, const SerdeADT &)>;

    static ReflectRegistry &self() {
        static ReflectRegistry Instance;
        return Instance;
    }

    void registerEntry(infra::compat::string_view Name,
                       Factory factory,
                       std::unordered_map<std::string, Invoker> invokers,
                       std::unordered_map<std::string, Getter> Getters,
                       std::unordered_map<std::string, Setter> Setters,
                       infra::compat::string_view BaseName,
                       bool HasBase) {
        const std::string Key{Name.data(), Name.size()};
        Types.emplace(Key,
                      Entry{std::move(factory), std::move(invokers), std::move(Getters), std::move(Setters)});
        if (HasBase) {
            BaseOf[Key] = std::string(BaseName.data(), BaseName.size());                  // 类型名 -> 直接基类名
            DerivedOf[std::string(BaseName.data(), BaseName.size())].push_back(Key);      // 基类名 -> 直接子类名
        }
    }

    PICTOR_NODISCARD bool has(infra::compat::string_view Name) const {
        return Types.find(std::string(Name.data(), Name.size())) != Types.end();
    }

    // 通过字符串构造对象
    PICTOR_NODISCARD ReflectObject create(infra::compat::string_view Name) const {
        return create(Name, SerdeADT{nullptr});
    }

    PICTOR_NODISCARD ReflectObject create(infra::compat::string_view Name, const SerdeADT &args) const {
        auto it = Types.find(std::string(Name.data(), Name.size()));
        if (it == Types.end()) {
            throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(Name.data(), Name.size()) + "'");
        }
        return it->second.Factory(args);
    }

    // 通过字符串调用成员方法
    SerdeADT invoke(infra::compat::string_view type, infra::compat::string_view Fn, ReflectObject &Obj, const SerdeADT &args) const {
        auto it = Types.find(std::string(type.data(), type.size()));
        if (it == Types.end()) {
            throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(type.data(), type.size()) + "'");
        }
        auto fit = it->second.Invokers.find(std::string(Fn.data(), Fn.size()));
        if (fit == it->second.Invokers.end()) {
            throw std::invalid_argument("ReflectRegistry: unknown function '" + std::string(Fn.data(), Fn.size()) + "'");
        }
        return fit->second(Obj, args);
    }

    // 通过字符串访问成员, 读
    SerdeADT get(infra::compat::string_view type, infra::compat::string_view Member, ReflectObject &Obj) const {
        auto it = Types.find(std::string(type.data(), type.size()));
        if (it == Types.end()) {
            throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(type.data(), type.size()) + "'");
        }
        auto git = it->second.Getters.find(std::string(Member.data(), Member.size()));
        if (git == it->second.Getters.end()) {
            throw std::invalid_argument("ReflectRegistry: unknown Member '" + std::string(Member.data(), Member.size()) + "'");
        }
        return git->second(Obj);
    }

    // 通过字符串访问成员, 写
    void set(infra::compat::string_view type, infra::compat::string_view Member, ReflectObject &Obj, const SerdeADT &Value) const {
        auto it = Types.find(std::string(type.data(), type.size()));
        if (it == Types.end()) {
            throw std::invalid_argument("ReflectRegistry: unknown type '" + std::string(type.data(), type.size()) + "'");
        }
        auto sit = it->second.Setters.find(std::string(Member.data(), Member.size()));
        if (sit == it->second.Setters.end()) {
            throw std::invalid_argument("ReflectRegistry: unknown Member '" + std::string(Member.data(), Member.size()) + "'");
        }
        sit->second(Obj, Value);
    }

    // ---- 继承信息查询 ----
    PICTOR_NODISCARD infra::compat::Optional<std::string> baseOf(infra::compat::string_view Name) const {
        auto It = BaseOf.find(std::string(Name.data(), Name.size()));
        return It == BaseOf.end() ? infra::compat::nullopt : infra::compat::Optional<std::string>{It->second};
    }

    PICTOR_NODISCARD std::vector<std::string> derivedOf(infra::compat::string_view Name) const {
        auto It = DerivedOf.find(std::string(Name.data(), Name.size()));
        return It == DerivedOf.end() ? std::vector<std::string>{} : It->second;
    }

    // 基类全链（自近及远，不含自身）
    PICTOR_NODISCARD std::vector<std::string> baseChainOf(infra::compat::string_view Name) const {
        std::vector<std::string> Chain;
        std::string Cur{Name.data(), Name.size()};
        while (auto B = baseOf(infra::compat::string_view(Cur.data(), Cur.size()))) {
            Chain.push_back(*B);
            Cur = *B;
        }
        return Chain;
    }

    // 是否（直接或间接）派生自 Base
    PICTOR_NODISCARD bool isDerivedFrom(infra::compat::string_view Name, infra::compat::string_view Base) const {
        std::string Cur{Name.data(), Name.size()};
        while (auto B = baseOf(infra::compat::string_view(Cur.data(), Cur.size()))) {
            if (infra::compat::string_view(B->data(), B->size()) == Base)
                return true;
            Cur = *B;
        }
        return false;
    }

    PICTOR_NODISCARD std::vector<std::string> typeNames() const {
        std::vector<std::string> Names;
        Names.reserve(Types.size());
        for (auto const &K: Types)
            Names.push_back(K.first);
        return Names;
    }

    PICTOR_NODISCARD std::size_t size() const noexcept {
        return Types.size();
    }

private:
    struct Entry {
        Factory Factory;
        std::unordered_map<std::string, Invoker> Invokers;
        std::unordered_map<std::string, Getter> Getters;
        std::unordered_map<std::string, Setter> Setters;
    };

    std::unordered_map<std::string, Entry> Types;
    std::unordered_map<std::string, std::string> BaseOf;                  // 类型名 -> 直接基类名
    std::unordered_map<std::string, std::vector<std::string> > DerivedOf; // 基类名 -> 直接子类名
};

// ---- TypeInfo 的运行时继承查询实现（须在 ReflectRegistry 完整定义之后） ----
template<typename Ty>
std::vector<std::string> TypeInfo<Ty>::derivedNames() {
    return ReflectRegistry::self().derivedOf(Name);
}

template<typename Ty>
std::vector<std::string> TypeInfo<Ty>::baseChain() {
    return ReflectRegistry::self().baseChainOf(Name);
}

template<typename Ty>
bool TypeInfo<Ty>::isDerivedFrom(infra::compat::string_view Base) {
    return ReflectRegistry::self().isDerivedFrom(Name, Base);
}

namespace detail{
// ---- buildInvokers 的 C++11 可调用对象（本地类不能有成员模板，故放命名空间内） ----
template<typename Ty>
struct InvokerBuilder {
    std::unordered_map<std::string, ReflectRegistry::Invoker> &Invokers;

    template<typename FieldTy>
    void operator()(const FieldTy &Field) const {
        const auto Ptr = Field.Pointer;
        Invokers[std::string(Field.Name.data(), Field.Name.size())] =
            [Ptr](ReflectObject &Obj, const SerdeADT &args) -> SerdeADT {
                return invokeMember(Obj.cast<Ty>(), Ptr, args);
            };
    }
};
}

// ========================= 由 TypeInfo<Ty> 生成工厂 / 调用表 / 成员表 =========================
template<typename Ty>
ReflectObject factory(const SerdeADT &args) {
    if (args.isNull())
        return ReflectObject{Ty{}};
    auto R = fromAdt<Ty>(args);
    if (!R) {
        throw std::runtime_error(R.error());
    }
    return ReflectObject{std::move(*R)};
}

template<typename Ty>
std::unordered_map<std::string, ReflectRegistry::Invoker> buildInvokers() {
    std::unordered_map<std::string, ReflectRegistry::Invoker> Invokers;
    TypeInfo<Ty>::forEachFunction(detail::InvokerBuilder<Ty>{Invokers});
    return Invokers;
}

template<typename Ty, typename FieldType>
void addMemberField(std::unordered_map<std::string, ReflectRegistry::Getter> &Getters,
                    std::unordered_map<std::string, ReflectRegistry::Setter> &Setters,
                    const FieldType &Field) {
    auto Ptr                         = Field.Pointer;
    Getters[std::string(Field.Name.data(), Field.Name.size())] = [Ptr](ReflectObject &Obj) -> SerdeADT {
        return toAdt(Obj.cast<Ty>().*Ptr);
    };
    Setters[std::string(Field.Name.data(), Field.Name.size())] = [Ptr](ReflectObject &Obj, const SerdeADT &Value) {
        using FieldT = infra::compat::decay_t<decltype(Obj.cast<Ty>().*Ptr)>;
        auto R       = fromAdt<FieldT>(Value);
        if (!R)
            throw std::runtime_error(R.error());
        Obj.cast<Ty>().*Ptr = std::move(*R);
    };
}

// ---- if constexpr (HasBase) + std::apply + 折叠表达式的 C++11 等价：标签分派 + index_sequence ----
template<typename Ty, typename Tuple, std::size_t... I>
void addAllMemberFieldsImpl(std::unordered_map<std::string, ReflectRegistry::Getter> &Getters,
                            std::unordered_map<std::string, ReflectRegistry::Setter> &Setters,
                            Tuple const &T, infra::compat::index_sequence<I...>) {
    int Dummy[] = {0, (addMemberField<Ty>(Getters, Setters, std::get<I>(T)), 0)...};
    (void)Dummy;
}

template<typename Ty, typename Tuple>
void addAllMemberFields(std::unordered_map<std::string, ReflectRegistry::Getter> &Getters,
                        std::unordered_map<std::string, ReflectRegistry::Setter> &Setters,
                        Tuple const &T) {
    addAllMemberFieldsImpl<Ty>(Getters, Setters, T,
                               infra::compat::make_index_sequence<std::tuple_size<Tuple>::value>());
}

// @notice 前置声明：buildMembersIntoImpl 需要递归调用 buildMembersInto
//         （默认模板实参只在后面的定义处给出）
template<typename Ty, typename ReflectedType>
void buildMembersInto(std::unordered_map<std::string, ReflectRegistry::Getter> &Getters,
                      std::unordered_map<std::string, ReflectRegistry::Setter> &Setters);

template<typename Ty, typename ReflectedType>
void buildMembersIntoImpl(std::unordered_map<std::string, ReflectRegistry::Getter> &Getters,
                          std::unordered_map<std::string, ReflectRegistry::Setter> &Setters,
                          std::true_type) {
    buildMembersInto<Ty, typename TypeInfo<ReflectedType>::Base>(Getters, Setters);
    addAllMemberFields<Ty>(Getters, Setters, TypeInfo<ReflectedType>::Variables);
}

template<typename Ty, typename ReflectedType>
void buildMembersIntoImpl(std::unordered_map<std::string, ReflectRegistry::Getter> &Getters,
                          std::unordered_map<std::string, ReflectRegistry::Setter> &Setters,
                          std::false_type) {
    addAllMemberFields<Ty>(Getters, Setters, TypeInfo<ReflectedType>::Variables);
}

template<typename Ty, typename ReflectedType = Ty>
void buildMembersInto(std::unordered_map<std::string, ReflectRegistry::Getter> &Getters,
                      std::unordered_map<std::string, ReflectRegistry::Setter> &Setters) {
    buildMembersIntoImpl<Ty, ReflectedType>(Getters, Setters,
                                            std::integral_constant<bool, TypeInfo<ReflectedType>::HasBase>());
}

template<typename Ty>
void registerReflect() {
    constexpr infra::compat::string_view Name = TypeInfo<Ty>::Name;
    if (ReflectRegistry::self().has(Name))
        return; // 幂等：自动注册可能被多次触发（多 TU / 显式调用）

    auto Invokers = buildInvokers<Ty>();
    std::unordered_map<std::string, ReflectRegistry::Getter> Getters;
    std::unordered_map<std::string, ReflectRegistry::Setter> Setters;
    buildMembersInto<Ty>(Getters, Setters);
    ReflectRegistry::self().registerEntry(Name,
                                          &factory<Ty>,
                                          std::move(Invokers),
                                          std::move(Getters),
                                          std::move(Setters),
                                          TypeInfo<Ty>::BaseName,
                                          TypeInfo<Ty>::HasBase);
}

// ========================= 自动注册 =========================
// REFLECT_CLASS 展开出的 inline static 成员在本翻译单元的静态初始化阶段构造，
// 其构造函数统一调用 registerReflect<Ty>()：无需为每个反射类型手写注册代码，
// 且所有注册均在进入 main 之前完成。
namespace detail{
template<typename Ty>
struct ReflectAutoRegistrar {
    ReflectAutoRegistrar() {
        registerReflect<Ty>();
    }
};
}
} // namespace infra

// 全局别名：让用户写 REFLECT_CLASS(Person, ReflectNullBase) 时无需 infra:: 前缀
using ReflectNullBase = infra::ReflectNullBase;

// ========================= 注册宏 =========================
#define PP_CAT(a, b) PP_CAT_I(a, b)
#define PP_CAT_I(a, b) a##b

// ---- C++11 兼容的空参数检测（标准预处理语义；MSVC 需 /Zc:preprocessor） ----
// 思路：参数列表只按逗号分割。探测一 PP_HA 统计参数个数（>=2 为 1）；
// 探测二在参数后拼 PP_TRIGGER 与空括号：仅当参数为空时 PP_TRIGGER 才会被
// 调用并展开为逗号（多出一个空槽位），据此区分空与非空。
// （PP_NARG_I 由下方 PP_NARG 区定义，宏展开发生在用户调用处，顺序无关）
#define PP_HA(...) PP_NARG_I(__VA_ARGS__, 1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,0)
#define PP_TRIGGER(...) ,

#define PP_IS_EMPTY(...) PP_IS_EMPTY_I(PP_HA(__VA_ARGS__), PP_HA(PP_TRIGGER __VA_ARGS__ ()))
#define PP_IS_EMPTY_I(p1, p2) PP_CAT(PP_IS_EMPTY_CASE_, PP_CAT(p1, p2))
#define PP_IS_EMPTY_CASE_01 1
#define PP_IS_EMPTY_CASE_00 0
#define PP_IS_EMPTY_CASE_10 0
#define PP_IS_EMPTY_CASE_11 0

// REFLECT_FIRST：取可变参数第一项；参数为空时回退为 ReflectNullBase（替代 __VA_OPT__）
#define REFLECT_FIRST(...) REFLECT_FIRST_I(PP_IS_EMPTY(__VA_ARGS__), __VA_ARGS__)
#define REFLECT_FIRST_I(IsEmpty, ...) PP_CAT(REFLECT_FIRST_SELECT_, IsEmpty)(__VA_ARGS__)
#define REFLECT_FIRST_SELECT_1(...) ReflectNullBase
#define REFLECT_FIRST_SELECT_0(first, ...) first

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
// reflect_registrar：静态初始化阶段自动调用 registerReflect<Type>()，无需手写注册。
// @notice C++11/14 不允许类内初始化非整型/非字面量类型的静态常量成员，
//         reflect_registrar 退化为纯声明（自动注册仅 C++17+ 生效；C++11/14 下
//         可显式调用 registerReflect<Type>()，其幂等检查保证每个类型只注册一次）。
#if PICTOR_HAS_CPP17
#  define REFLECT_CLASS(Type, ...)                              \
    static constexpr infra::compat::string_view reflect_name = #Type;   \
    using reflect_self = Type;                                \
    using reflect_base = REFLECT_FIRST(__VA_ARGS__);          \
    static infra::TypeInfo<Type> StaticClass() { return infra::TypeInfo<Type>{}; } \
    inline static const infra::detail::ReflectAutoRegistrar<Type> reflect_registrar{}
#else
#  define REFLECT_CLASS(Type, ...)                              \
    static constexpr infra::compat::string_view reflect_name = #Type;   \
    using reflect_self = Type;                                \
    using reflect_base = REFLECT_FIRST(__VA_ARGS__);          \
    static infra::TypeInfo<Type> StaticClass() { return infra::TypeInfo<Type>{}; } \
    static const infra::detail::ReflectAutoRegistrar<Type> reflect_registrar
#endif

#define REFLECT_FIELD_DECL(P)  REFLECT_FIELD_DECL_I P
#define REFLECT_FIELD_DECL_I(Ty, N) Ty N;
#define REFLECT_FIELD_ENTRY(P) REFLECT_FIELD_ENTRY_I P
// @notice 显式给出 ReflectFieldTraits<成员指针类型>，避免依赖 C++17 CTAD 推导
#define REFLECT_FIELD_ENTRY_I(Ty, N) infra::ReflectFieldTraits<decltype(&reflect_self::N)>(&reflect_self::N, #N)

// @notice C++17+ 保持原 constexpr 静态成员；C++11/14 下 std::tuple 无 constexpr
//         构造，reflect_variables/reflect_functions 退化为纯声明（仅供 decltype
//         探测），实际取值经 reflectVariables()/reflectFunctions() 的函数内静态
//         局部变量提供（每个类恰好初始化一次，无 ODR 问题）。
#if PICTOR_HAS_CPP17
#  define REFLECT_FIELDS(...)                                   \
    PP_FOR_EACH(REFLECT_FIELD_DECL, __VA_ARGS__)                \
    static constexpr auto reflect_variables =                   \
        std::make_tuple(PP_MAP(REFLECT_FIELD_ENTRY, __VA_ARGS__)); \
    static constexpr decltype(reflect_variables) const &reflectVariables() { return reflect_variables; }
#else
#  define REFLECT_FIELDS(...)                                   \
    PP_FOR_EACH(REFLECT_FIELD_DECL, __VA_ARGS__)                \
    static const decltype(std::make_tuple(PP_MAP(REFLECT_FIELD_ENTRY, __VA_ARGS__))) reflect_variables; \
    static const decltype(reflect_variables) &reflectVariables() {                            \
        static const decltype(reflect_variables) V = std::make_tuple(PP_MAP(REFLECT_FIELD_ENTRY, __VA_ARGS__)); \
        return V;                                                                             \
    }
#endif

#define REFLECT_FN_ENTRY(N) infra::ReflectFieldTraits<decltype(&reflect_self::N)>(&reflect_self::N, #N)

// REFLECT_FUNCTIONS 允许空参数列表（__VA_OPT__ 的 C++11 等价）
#define REFLECT_FN_TUPLE(IsEmpty, ...) PP_CAT(REFLECT_FN_TUPLE_, IsEmpty)(__VA_ARGS__)
#define REFLECT_FN_TUPLE_1(...) std::make_tuple()
#define REFLECT_FN_TUPLE_0(...) std::make_tuple(PP_MAP(REFLECT_FN_ENTRY, __VA_ARGS__))

#if PICTOR_HAS_CPP17
#  define REFLECT_FUNCTIONS(...)                                \
    static constexpr auto reflect_functions =                   \
        REFLECT_FN_TUPLE(PP_IS_EMPTY(__VA_ARGS__), __VA_ARGS__); \
    static constexpr decltype(reflect_functions) const &reflectFunctions() { return reflect_functions; }
#else
#  define REFLECT_FUNCTIONS(...)                                \
    static const decltype(REFLECT_FN_TUPLE(PP_IS_EMPTY(__VA_ARGS__), __VA_ARGS__)) reflect_functions; \
    static const decltype(reflect_functions) &reflectFunctions() {                            \
        static const decltype(reflect_functions) V = REFLECT_FN_TUPLE(PP_IS_EMPTY(__VA_ARGS__), __VA_ARGS__); \
        return V;                                                                             \
    }
#endif
