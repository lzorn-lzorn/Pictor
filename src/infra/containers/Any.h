//
// Created by Administrator on 2026/11/16.
//

#ifndef HELLOVULKAN_ANY_H
#define HELLOVULKAN_ANY_H

#include <cstddef>
#include <cstring>
#include <initializer_list>
#include <variant>
#include <memory>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>

#if defined(__cpp_rtti) || defined(__GXX_RTTT) || (defined(_CPPRTTI) && _CPPRTTI)
#   include <typeinfo>
#   define HAS_RTTI 1
#else
#   define HAS_RTTI 0
#endif

// @notice 用 __attribute__((visibility("default"))) 让 TypeId 跨动态库唯一.
// @notice MSVC 则没有类似的方式
#if defined(__GUNC__) || defined(__clang__)
#   define ANY_EXPORT __attribute__((visibility("default")))
#else
#   define ANY_EXPORT
#endif

namespace any_detail
{
using TypeId_t = const void*;

template <typename Ty>
inline constexpr char TypeIdTag = 0;

template <typename Ty>
ANY_EXPORT TypeId_t getTypeId() noexcept
{
    return &TypeIdTag<std::decay_t<Ty>>;
}

constexpr std::size_t SmallBufferOptimizeSize = 3 * sizeof(void*);
constexpr std::size_t SmallBufferOptimizeAlign = alignof(void*);

union Storage_t
{
    void* Ptr;
    std::byte Buffer[SmallBufferOptimizeSize];
    Storage_t() noexcept : Ptr(nullptr) {}
    ~Storage_t() {}
};

struct VTable_t
{
    void (*Destructor)(Storage_t&) noexcept;
    void (*Copy)(Storage_t &, Storage_t const&) noexcept;
    void (*Move)(Storage_t &, Storage_t &) noexcept;
    const void* (*Cast)(const Storage_t &, TypeId_t
#if HAS_RTTI
        , const std::type_info*
#endif
    ) noexcept;

    TypeId_t (*GetTypeId)() noexcept;

#if HAS_RTTI
    const std::type_info& (GetTypeInfo)() noexcept;
#endif
};

template <typename Ty>
struct Ops
{
    static constexpr bool IsUseSmallBufferOptimize
        = sizeof(Ty) <= SmallBufferOptimizeSize &&
            alignof(Ty) <= SmallBufferOptimizeAlign &&
                std::is_nothrow_move_constructible_v<Ty>;

    static Ty* getPtr(Storage_t& Storage) noexcept
    {
        if constexpr (IsUseSmallBufferOptimize)
        {
            return std::launder(reinterpret_cast<Ty*>(Storage.Buffer));
        }
        else
        {
            return static_cast<Ty*>(Storage.Ptr);
        }
    }

    static Ty* getPtr(Storage_t const& Storage) noexcept
    {
        if constexpr (IsUseSmallBufferOptimize)
        {
            return std::launder(reinterpret_cast<const Ty*>(Storage.Buffer));
        }
        else
        {
            return static_cast<const Ty*>(Storage.Ptr);
        }
    }

    template <typename ...Args>
    static void construct(Storage_t& Storage, Args&& ...InArgs)
    {
        if constexpr (IsUseSmallBufferOptimize)
        {
            std::construct_at(reinterpret_cast<const Ty*>(Storage.Buffer), std::forward<Args>(InArgs)...);
        }
        else
        {
            Storage.Ptr = new Ty(std::forward<Args>(InArgs)...);
        }
    }

    static void destroy(Storage_t& Storage) noexcept
    {
        if constexpr (IsUseSmallBufferOptimize)
        {
            std::destroy_at(std::launder(reinterpret_cast<Ty*>(Storage.Buffer)));
        }
        else
        {
            delete static_cast<Ty*>(Storage.Ptr);
        }
    }

    static void copy(Storage_t& Dst, Storage_t const& Src) noexcept
    {
        if constexpr (IsUseSmallBufferOptimize)
        {
            std::construct_at(reinterpret_cast<Ty*>(Dst.Ptr), Src.Ptr);
        }
        else
        {
            Dst.Ptr = new Ty(*getPtr(Src));
        }
    }

    static void move(Storage_t& Dst, Storage_t & Src) noexcept
    {
        if constexpr (IsUseSmallBufferOptimize)
        {
            std::construct_at(reinterpret_cast<Ty*>(Dst.Buffer), std::move(*getPtr(Src)));
            std::destroy_at(std::launder(reinterpret_cast<Ty*>(Dst.Ptr)));
        }
        else
        {
            Dst.Ptr = Src.Ptr;
            Src.Ptr = nullptr;
        }
    }

    static const void* cast(Storage_t const& Storage, TypeId_t Id
#if HAS_RTTI
        , const std::type_info* QueryTypeInfo
#endif
    ) noexcept
    {
        if (Id == getTypeId<Ty>())
        {
            return getPtr(Storage);
        }
#if HAS_RTTI
      if (QueryTypeInfo && *QueryTypeInfo == typeid(Ty))
      {
          return getPtr(Storage);
      }
#endif
        return nullptr;
    }

    static TypeId_t getTypeId() noexcept { return getTypeId<Ty>(); }

#if HAS_RTTI
    static const std::type_info& getTypeInfo() noexcept { return typeid(Ty); }
#endif
};

template <typename Ty>
inline constexpr VTable_t VTableFor = {
    &Ops<Ty>::destroy,
    &Ops<Ty>::copy,
    &Ops<Ty>::mvoe,
    &Ops<Ty>::cast,
    &Ops<Ty>::getTypeId,
#if HAS_RTTI
    &Ops<Ty>::getTypeInfo,
#endif
};

}

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
        typename DT = std::decay_t<T>,
        std::enable_if_t<!std::is_same_v<typename DT, Any>, int32_t> = 0,
        std::enable_if_t<std::is_copy_constructible_v<DT>, int32_t> = 0
    >
    Any(Any && Other) noexcept
    {
        any_detail::Ops<DT>::construct(Storage, std::forward<T>(Other));
        VTable = &any_detail::VTableFor<DT>;
    }

    template <typename Ty, typename ...Args>
    explicit Any(std::in_place_type_t<Ty>, Args&&... ConstructParams)
    {
        using DT = std::decay_t<Ty>;
        static_assert(std::is_copy_constructible_v<DT>, "Type must be copy constructible");
        any_detail::Ops<DT>::construct(Storage, std::forward<Args>(ConstructParams)...);
        VTable = &any_detail::VTableFor<DT>;
    }

    template <typename T, typename U, typename ...Args>
    explicit Any(std::in_place_type_t<T>, std::initializer_list<U> InitParamList, Args&&... ConstructParams)
    {
        using DT = std::decay_t<Ty>;
        static_assert(std::is_copy_constructible_v<DT>, "Type must be copy constructible");
        any_detail::Ops<DT>::construct(Storage, InitParamList, std::forward<Args>(ConstructParams)...);
        VTable = &any_detail::VTableFor<DT>;
    }

    Any& operator=(const Any& Other) {
        if (this != &Other) {
            Any Tmp(Other);
            swap(Tmp);
        }
        return *this;
    }

    Any& operator=(Any&& Other) noexcept {
        if (this != &Other) {
            reset();
            if (Other.VTable) {
                Other.VTable->move(Storage, Other.Storage);
                VTable = Other.VTable;
                Other.VTable = nullptr;
            }
        }
        return *this;
    }

    template <class T,
          class DT = std::decay_t<T>,
          std::enable_if_t<!std::is_same_v<DT, Any>, int> = 0>
    Any& operator=(T&& Value) {
        Any Tmp(std::forward<T>(Value));
        swap(Tmp);
        return *this;
    }
    
    template <class T, class... Args>
    std::decay_t<T>& emplace(Args&&... ConstructParams)
    {
        using DT = std::decay_t<T>;
        static_assert(std::is_copy_constructible_v<DT>);
        reset();
        fast_any_detail::Ops<DT>::construct(Storage, std::forward<Args>(ConstructParams)...);
        vtable_ = &fast_any_detail::vtable_for<DT>;
        return *fast_any_detail::Ops<DT>::ptr(Storage);
    }

    template <class T, class U, class... Args>
    std::decay_t<T>& emplace(std::initializer_list<U> InitParamList, Args&&... ConstructParams)
    {
        using DT = std::decay_t<T>;
        static_assert(std::is_copy_constructible_v<DT>);
        reset();
        any_detail::Ops<DT>::construct(Storage, InitParamList, std::forward<Args>(ConstructParams)...);
        VTable = &any_detail::VTableFor<DT>;
        return *any_detail::Ops<DT>::getPtr(Storage);
    }

    void reset() noexcept
    {
        if (VTable)
        {
            VTable->destroy(Storage);
            VTable = nullptr;
        }
    }

    void swap(Any& Other) noexcept
    {
        Storage Tmp;
        std::memcpy(&Tmp, &Storage, sizeof(Storage));
        std::memcpy(&Storage, &Other.Storage, sizeof(Storage));
        std::memcpy(&Other.Storage, &Tmp, sizeof(Storage));
        std::swap(VTable, Other.VTable);
    }

    [[nodiscard]] bool HasValue() const noexcept { return VTable != nullptr; }
    explicit operator bool() const noexcept { return HasValue(); }

    TypeId_t getTypeId() const noexcept { return VTable ? VTable->getTypeId() : nullptr; }

#if HAS_RTTI
    const std::type_info& type() const noexcept
    {
        return VTable ? VTable->getTypeInfo() : typeid(void);
    }
#endif

    template <typename Ty>
    std::decay_t<Ty>* castPtr() noexcept
    {
        return const_cast<std::decay_t<Ty>*>(static_cast<const Any*>(this)->castPtr<Ty>());
    }
};

inline void swap(Any& A, Any& B) noexcept
{
    A.swap(B);
}

template <class T>
const std::decay_t<T>* doAnyCast(const Any* A) noexcept {
    return A ? A->castPtr<T>() : nullptr;
}

template <class T>
std::decay_t<T>* doAnyCast(Any* A) noexcept {
    return A ? A->castPtr<T>() : nullptr;
}

template <class T>
std::decay_t<T> doAnyCast(const Any& A) {
    using DT = std::decay_t<T>;
    auto* Ptr = A.castPtr<DT>();
    if (!Ptr)
    {
        throw std::bad_cast{};
    }
    return *Ptr;
}

template <class T>
std::decay_t<T> doAnyCast(Any& A) {
    using DT = std::decay_t<T>;
    auto* Ptr = A.castPtr<DT>();
    if (!Ptr)
    {
        throw std::bad_cast{};
    }
    return *Ptr;
}

template <class T>
std::decay_t<T> doAnyCast(Any&& A) {
    using DT = std::decay_t<T>;
    auto* Ptr = A.castPtr<DT>();
    if (!Ptr)    
    {
        throw std::bad_cast{};
    }
    return std::move(*Ptr);
}

template <class T, class... Args>
Any makeAny(Args&&... ConsturctParams) {
    return Any(std::in_place_type<T>, std::forward<Args>(ConsturctParams)...);
}

template <class T, class U, class... Args>
Any makeAny(std::initializer_list<U> InitParamList, Args&&... ConsturctParams) {
    return Any(std::in_place_type<T>, InitParamList, std::forward<Args>(ConsturctParams)...);
}

#endif //HELLOVULKAN_ANY_H
