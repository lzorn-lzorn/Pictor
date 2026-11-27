//
// Created by Administrator on 2026/11/16.
//

#ifndef PICTOR_INFRA_ANY_H
#define PICTOR_INFRA_ANY_H

#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <typeinfo>
#include <utility>

// 统一跨版本兼容层: PICTOR_NODISCARD / infra::compat::launder 等(C++11 ~ C++23)。
#include <infra/common/VersionCompatibility.hpp>

// std::bad_cast / std::type_info 声明于 <typeinfo>, 该头文件在任何 RTTI 设置下都可用;
// 只有 typeid / dynamic_cast 这类语言特性才依赖 RTTI, 因此这里始终引入是安全的,
// 也让 doAnyCast 抛出的 std::bad_cast 在 -fno-rtti 下依然可用。
#if defined(__cpp_rtti) || defined(__GXX_RTTI) || (defined(_CPPRTTI) && _CPPRTTI)
#   define HAS_RTTI 1
#else
#   define HAS_RTTI 0
#endif

// @notice 用 __attribute__((visibility("default"))) 让 TypeId 跨动态库唯一.
// @notice MSVC 则没有类似的方式
#if defined(__GNUC__) || defined(__clang__)
#   define ANY_EXPORT __attribute__((visibility("default")))
#else
#   define ANY_EXPORT
#endif

namespace any_detail
{

using TypeId_t = const void*;

// ---------------------------------------------------------------------------
// 标准库兼容层: 用预处理指令在 C++11 ~ C++23 之间抹平差异, 不改变功能。
// ---------------------------------------------------------------------------

// std::byte  (C++17)  ->  用 enum class 模拟相同的「非整数字节类型」
#if defined(__cpp_lib_byte) && __cpp_lib_byte >= 201603L
    using Byte = std::byte;
#else
    enum class Byte : unsigned char {};
#endif

// std::launder (C++17) ->  统一经 infra::compat::launder: C++17 起调用 std::launder,
// C++11 无 lifetime 优化, 直接返回指针即可。
// 注意: 命名为 launderPtr 以免与 std::launder 经 ADL 产生歧义(C++17 起)。
template <typename T>
T* launderPtr(T* P) noexcept
{
    return infra::compat::launder(P);
}

// std::in_place_type_t (C++17) ->  自定义等价的标签类型
template <typename T>
struct in_place_type_t
{
    explicit in_place_type_t() = default;
};

// ---------------------------------------------------------------------------
// TypeId: 无 RTTI 的类型标识 —— 取「每个类型一份」的静态对象地址作为标签。
// C++11 用类模板的静态数据成员(跨 TU 经 COMDAT 合并)代替 C++17 的 inline 变量模板。
// ---------------------------------------------------------------------------
template <typename T>
struct TypeIdTag
{
    static char value;
};
template <typename T>
char TypeIdTag<T>::value = 0;

template <typename T>
ANY_EXPORT TypeId_t getTypeId() noexcept
{
    return &TypeIdTag<typename std::decay<T>::type>::value;
}

constexpr std::size_t SmallBufferOptimizeSize = 3 * sizeof(void*);
constexpr std::size_t SmallBufferOptimizeAlign = alignof(void*);

union Storage_t
{
    void* Ptr;
    Byte Buffer[SmallBufferOptimizeSize];
    Storage_t() noexcept : Ptr(nullptr) {}
};
/* @notice ~Storage_t() {} 是用户提供的析构函数, 会使得 trivially copyable 丢失, 导致之后的 memcpy 是 UB */
static_assert(std::is_trivially_copyable<Storage_t>::value, "Storage_t must be trivially copyable to allow memcpy-based swap");

struct VTable_t
{
    void (*Destructor)(Storage_t&) noexcept;
    void (*Copy)(Storage_t &, Storage_t const&);   // 拷贝构造 / new 可能抛出, 不能标 noexcept
    void (*Move)(Storage_t &, Storage_t &) noexcept;
    const void* (*Cast)(const Storage_t &, TypeId_t
#if HAS_RTTI
        , const std::type_info*
#endif
    ) noexcept;
    TypeId_t (*GetTypeId)() noexcept;
    // 整个 Storage_t 是否可安全地 memcpy 交换(见 Ops<T>::CanMemcpy)
    bool CanMemcpy;

#if HAS_RTTI
    const std::type_info& (*GetTypeInfo)() /* noexcept */;
#endif
};

template <typename T>
struct Ops
{
    static constexpr bool IsUseSmallBufferOptimize
        = sizeof(T) <= SmallBufferOptimizeSize &&
            alignof(T) <= SmallBufferOptimizeAlign &&
                std::is_nothrow_move_constructible<T>::value;

    // memcpy 整块交换 Storage_t 是否安全:
    //   - 堆分配: 只交换 void* 指针, 对象本身不移动, 恒安全;
    //   - SBO:     仅当 T 本身 trivially copyable 时, 字节搬移才等价于一次合法拷贝。
    // 关键结论: 只有「SBO 且 T 非 trivially copyable」才需要退回到搬移交换。
    static constexpr bool CanMemcpy
        = !IsUseSmallBufferOptimize || std::is_trivially_copyable<T>::value;

    // 析构默认即 noexcept; 显式约束让 Move/Destroy 标 noexcept 是成立的。
    static_assert(std::is_nothrow_destructible<T>::value,
        "Any 要求 T 的析构为 noexcept(默认即满足)");

    // C++11 无 if constexpr, 用 tag dispatch 在 SBO / 堆分配之间分派。
    using SboTag = std::integral_constant<bool, IsUseSmallBufferOptimize>;

    // ---- getPtr ----
    static T* getPtr(Storage_t& Storage) noexcept
    {
        return getPtrImpl(Storage, SboTag());
    }
    static const T* getPtr(Storage_t const& Storage) noexcept
    {
        return getPtrConstImpl(Storage, SboTag());
    }

    // ---- construct ----
    template <typename... Args>
    static void construct(Storage_t& Storage, Args&&... InArgs)
    {
        constructImpl(Storage, SboTag(), std::forward<Args>(InArgs)...);
    }

    // ---- destroy ----
    static void destroy(Storage_t& Storage) noexcept
    {
        destroyImpl(Storage, SboTag());
    }

    // ---- copy (可能抛出) ----
    static void copy(Storage_t& Dst, Storage_t const& Src)
    {
        copyImpl(Dst, Src, SboTag());
    }

    // ---- move (noexcept) ----
    static void move(Storage_t& Dst, Storage_t & Src) noexcept
    {
        moveImpl(Dst, Src, SboTag());
    }

    // ---- cast ----
    static const void* cast(Storage_t const& Storage, TypeId_t Id
#if HAS_RTTI
        , const std::type_info* QueryTypeInfo
#endif
    ) noexcept
    {
        if (Id == any_detail::getTypeId<T>())
        {
            return getPtr(Storage);
        }
#if HAS_RTTI
        if (QueryTypeInfo && *QueryTypeInfo == typeid(T))
        {
            return getPtr(Storage);
        }
#endif
        return nullptr;
    }

    static TypeId_t getTypeId() noexcept { return any_detail::getTypeId<T>(); }

#if HAS_RTTI
    static const std::type_info& getTypeInfo() /* noexcept */ { return typeid(T); }
#endif

private:
    // ---- SBO 分支 (true_type) ----
    static T* getPtrImpl(Storage_t& Storage, std::true_type) noexcept
    {
        return launderPtr(reinterpret_cast<T*>(Storage.Buffer));
    }
    static const T* getPtrConstImpl(Storage_t const& Storage, std::true_type) noexcept
    {
        return launderPtr(reinterpret_cast<const T*>(Storage.Buffer));
    }

    template <typename... Args>
    static void constructImpl(Storage_t& Storage, std::true_type, Args&&... InArgs)
    {
        infra::compat::construct_at(reinterpret_cast<T*>(Storage.Buffer), std::forward<Args>(InArgs)...);
    }

    static void destroyImpl(Storage_t& Storage, std::true_type) noexcept
    {
        infra::compat::destroy_at(getPtr(Storage));
    }

    static void copyImpl(Storage_t& Dst, Storage_t const& Src, std::true_type)
    {
        infra::compat::construct_at(reinterpret_cast<T*>(Dst.Buffer), *getPtr(Src));
    }

    static void moveImpl(Storage_t& Dst, Storage_t& Src, std::true_type) noexcept
    {
        // SBO 无法像堆分配那样"偷指针", 只能搬移构造到 Dst, 并立刻结束源对象生命周期;
        // 否则源对象(即便已搬移)的析构永远不会被调用, 造成资源泄漏。
        // 搬移构造是 noexcept 的(由 IsUseSmallBufferOptimize 保证), 析构也是 noexcept。
        infra::compat::construct_at(reinterpret_cast<T*>(Dst.Buffer), std::move(*getPtr(Src)));
        infra::compat::destroy_at(getPtr(Src));
    }

    // ---- 堆分配分支 (false_type) ----
    static T* getPtrImpl(Storage_t& Storage, std::false_type) noexcept
    {
        return static_cast<T*>(Storage.Ptr);
    }
    static const T* getPtrConstImpl(Storage_t const& Storage, std::false_type) noexcept
    {
        return static_cast<const T*>(Storage.Ptr);
    }

    template <typename... Args>
    static void constructImpl(Storage_t& Storage, std::false_type, Args&&... InArgs)
    {
        Storage.Ptr = new T(std::forward<Args>(InArgs)...);
    }

    static void destroyImpl(Storage_t& Storage, std::false_type) noexcept
    {
        delete static_cast<T*>(Storage.Ptr);
    }

    static void copyImpl(Storage_t& Dst, Storage_t const& Src, std::false_type)
    {
        Dst.Ptr = new T(*getPtr(Src));
    }

    static void moveImpl(Storage_t& Dst, Storage_t& Src, std::false_type) noexcept
    {
        Dst.Ptr = Src.Ptr;
        Src.Ptr = nullptr;
    }
};

// C++11 用类模板静态数据成员代替 C++17 的 inline constexpr 变量模板 VTableFor<T>。
template <typename T>
struct VTableFor
{
    static const VTable_t value;
};

template <typename T>
const VTable_t VTableFor<T>::value = {
    &Ops<T>::destroy,
    &Ops<T>::copy,
    &Ops<T>::move,
    &Ops<T>::cast,
    &Ops<T>::getTypeId,
    Ops<T>::CanMemcpy,
#if HAS_RTTI
    &Ops<T>::getTypeInfo,
#endif
};

} // namespace any_detail

class Any
{
    using Storage_t = any_detail::Storage_t;
    using VTable_t = any_detail::VTable_t;
    using TypeId_t = any_detail::TypeId_t;

    Storage_t Storage;
    VTable_t const* VTable = nullptr;

public:
    Any() noexcept = default;

    Any(const Any& Other) : VTable(Other.VTable)
    {
        if (VTable)
        {
            VTable->Copy(Storage, Other.Storage);
        }
    }

    Any(Any && Other) noexcept : VTable(Other.VTable)
    {
        if (VTable)
        {
            VTable->Move(Storage, Other.Storage);
            Other.VTable = nullptr;
        }
    }

    ~Any()
    {
        if (VTable)
        {
            VTable->Destructor(Storage);
        }
    }

    template <typename T,
        typename DT = typename std::decay<T>::type,
        typename std::enable_if<!std::is_same<DT, Any>::value, int>::type = 0,
        typename std::enable_if<std::is_copy_constructible<DT>::value, int>::type = 0
    >
    explicit Any(T&& Value) // 构造可能抛出(拷贝构造 / 堆分配), 不能标 noexcept
    {
        any_detail::Ops<DT>::construct(Storage, std::forward<T>(Value));
        VTable = &any_detail::VTableFor<DT>::value;
    }

    template <typename T, typename ...Args>
    explicit Any(any_detail::in_place_type_t<T>, Args&&... ConstructParams)
    {
        using DT = typename std::decay<T>::type;
        static_assert(std::is_copy_constructible<DT>::value, "Type must be copy constructible");
        any_detail::Ops<DT>::construct(Storage, std::forward<Args>(ConstructParams)...);
        VTable = &any_detail::VTableFor<DT>::value;
    }

    template <typename T, typename U, typename ...Args>
    explicit Any(any_detail::in_place_type_t<T>, std::initializer_list<U> InitParamList, Args&&... ConstructParams)
    {
        using DT = typename std::decay<T>::type;
        static_assert(std::is_copy_constructible<DT>::value, "Type must be copy constructible");
        any_detail::Ops<DT>::construct(Storage, InitParamList, std::forward<Args>(ConstructParams)...);
        VTable = &any_detail::VTableFor<DT>::value;
    }

    Any& operator=(const Any& Other)
    {
        if (this != &Other)
        {
            Any Tmp(Other); // 可能抛出, 失败则 *this 保持不变(强异常安全)
            swap(Tmp);
        }
        return *this;
    }

    Any& operator=(Any&& Other) noexcept
    {
        if (this != &Other)
        {
            reset();
            if (Other.VTable)
            {
                Other.VTable->Move(Storage, Other.Storage);
                VTable = Other.VTable;
                Other.VTable = nullptr;
            }
        }
        return *this;
    }

    template <class T,
        class DT = typename std::decay<T>::type,
        typename std::enable_if<!std::is_same<DT, Any>::value, int>::type = 0,
        typename std::enable_if<std::is_copy_constructible<DT>::value, int>::type = 0>
    Any& operator=(T&& Value)
    {
        Any Tmp(std::forward<T>(Value));
        swap(Tmp);
        return *this;
    }

    template <class T, class... Args>
    typename std::decay<T>::type& emplace(Args&&... ConstructParams)
    {
        using DT = typename std::decay<T>::type;
        static_assert(std::is_copy_constructible<DT>::value, "Type must be copy constructible");
        reset();
        any_detail::Ops<DT>::construct(Storage, std::forward<Args>(ConstructParams)...);
        VTable = &any_detail::VTableFor<DT>::value;
        return *any_detail::Ops<DT>::getPtr(Storage);
    }

    template <class T, class U, class... Args>
    typename std::decay<T>::type& emplace(std::initializer_list<U> InitParamList, Args&&... ConstructParams)
    {
        using DT = typename std::decay<T>::type;
        static_assert(std::is_copy_constructible<DT>::value, "Type must be copy constructible");
        reset();
        any_detail::Ops<DT>::construct(Storage, InitParamList, std::forward<Args>(ConstructParams)...);
        VTable = &any_detail::VTableFor<DT>::value;
        return *any_detail::Ops<DT>::getPtr(Storage);
    }

    void reset() noexcept
    {
        if (VTable)
        {
            VTable->Destructor(Storage);
            VTable = nullptr;
        }
    }

    void swap(Any& Other) noexcept
    {
        if (this == &Other)
        {
            return;
        }

        // 快速路径: 两侧都允许字节级交换时, 直接 memcpy 整个 Storage_t, 不触碰存储对象本身。
        // 只有「SBO 且 T 非 trivially copyable」才落到搬移路径, 规避字节搬移造成的生命周期 UB。
        if (CanMemcpySwap() && Other.CanMemcpySwap())
        {
            Storage_t Tmp;
            std::memcpy(&Tmp, &Storage, sizeof(Storage_t));
            std::memcpy(&Storage, &Other.Storage, sizeof(Storage_t));
            std::memcpy(&Other.Storage, &Tmp, sizeof(Storage_t));
            std::swap(VTable, Other.VTable);
            return;
        }

        // 兜底路径: 通过搬移实现交换, 对任意类型(含 SBO 非 trivially copyable)都正确。
        Any Tmp(std::move(*this));
        *this = std::move(Other);
        Other = std::move(Tmp);
    }

    PICTOR_NODISCARD bool hasValue() const noexcept { return VTable != nullptr; }
    explicit operator bool() const noexcept { return hasValue(); }

    PICTOR_NODISCARD TypeId_t getTypeId() const noexcept { return VTable ? VTable->GetTypeId() : nullptr; }

#if HAS_RTTI
    PICTOR_NODISCARD const std::type_info& type() const noexcept
    {
        return VTable ? VTable->GetTypeInfo() : typeid(void);
    }
#endif

    template <typename Ty>
    const typename std::decay<Ty>::type* castPtr() const noexcept
    {
        using DT = typename std::decay<Ty>::type;
        if (!VTable) return nullptr;
        const void* P = VTable->Cast(Storage, any_detail::getTypeId<DT>()
#if HAS_RTTI
            , &typeid(DT)
#endif
        );
        return static_cast<const DT*>(P);
    }

    template <typename Ty>
    typename std::decay<Ty>::type* castPtr() noexcept
    {
        return const_cast<typename std::decay<Ty>::type*>(static_cast<const Any*>(this)->castPtr<Ty>());
    }

private:
    PICTOR_NODISCARD bool CanMemcpySwap() const noexcept
    {
        // 空 Any 的 Storage 只是 null 指针, 字节交换安全; 否则看该类型对应的存储策略。
        return !VTable || VTable->CanMemcpy;
    }
};

inline void swap(Any& A, Any& B) noexcept
{
    A.swap(B);
}

template <class T>
const typename std::decay<T>::type* doAnyCast(const Any* A) noexcept {
    return A ? A->castPtr<T>() : nullptr;
}

template <class T>
typename std::decay<T>::type* doAnyCast(Any* A) noexcept {
    return A ? A->castPtr<T>() : nullptr;
}

template <class T>
typename std::decay<T>::type doAnyCast(const Any& A) {
    using DT = typename std::decay<T>::type;
    auto* Ptr = A.castPtr<DT>();
    if (!Ptr)
    {
        throw std::bad_cast{};
    }
    return *Ptr;
}

template <class T>
typename std::decay<T>::type doAnyCast(Any& A) {
    using DT = typename std::decay<T>::type;
    auto* Ptr = A.castPtr<DT>();
    if (!Ptr)
    {
        throw std::bad_cast{};
    }
    return *Ptr;
}

template <class T>
typename std::decay<T>::type doAnyCast(Any&& A) {
    using DT = typename std::decay<T>::type;
    auto* Ptr = A.castPtr<DT>();
    if (!Ptr)    
    {
        throw std::bad_cast{};
    }
    return std::move(*Ptr);
}

template <class T, class... Args>
Any makeAny(Args&&... ConsturctParams) {
    return Any(any_detail::in_place_type_t<T>(), std::forward<Args>(ConsturctParams)...);
}

template <class T, class U, class... Args>
Any makeAny(std::initializer_list<U> InitParamList, Args&&... ConsturctParams) {
    return Any(any_detail::in_place_type_t<T>(), InitParamList, std::forward<Args>(ConsturctParams)...);
}

#endif //PICTOR_INFRA_ANY_H
