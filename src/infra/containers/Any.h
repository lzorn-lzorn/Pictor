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
    Any(Any && Other)
    {

    }
};


#endif //HELLOVULKAN_ANY_H
