//
// Created by Lzorn on 2026/11/20.
//

#ifndef PICTOR_LINKEDHASHTABLE_H
#define PICTOR_LINKEDHASHTABLE_H

#include <infra/common/SIMDControlByte.hpp>

#include <functional>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <utility>

template <
    typename Key,
    typename T,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>,
    typename Allocator = std::allocator<std::pair<const Key, T>>
>
class LinkedHashTable
{
    struct Node
    {
        std::pair<const Key, T> Value;
        Node* Next = { nullptr };
        Node* Prev = { nullptr };

        template <typename ...Args>
        explicit Node(Args&& ...args) : Value(std::forward<Args>(args)...) {}
    };

    using NodeAllocator     = typename std::allocator_traits<Allocator>::template rebind_alloc<Node>;
    using NodeAllocTraits   = std::allocator_traits<NodeAllocator>;
    using BucketAllocator   = typename std::allocator_traits<NodeAllocator>::template rebind_alloc<Node*>;
    using BucketAllocTraits = std::allocator_traits<BucketAllocator>;

public:
    using key_type        = Key;
    using mapped_type     = T;
    using value_type      = std::pair<const Key, T>;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using hasher          = Hash;
    using key_equal       = KeyEqual;
    using allocator_type  = Allocator;

    template <bool IsConst>
    class Iterator
    {
        friend class LinkedHashTable;

        using NodePtr = typename std::conditional<IsConst, const Node*, Node*>::type;

        NodePtr Current;

        explicit Iterator(NodePtr InCurrent) : Current(InCurrent) {}

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = typename LinkedHashTable::value_type;
        using difference_type   = typename LinkedHashTable::difference_type;
        using pointer           = std::conditional_t<IsConst, const value_type*, value_type*>;
        using reference         = std::conditional_t<IsConst, const value_type&, value_type&>;

        Iterator() = default;

        template <bool OtherConst, std::enable_if_t<IsConst && !OtherConst, int32_t> = 0>
        explicit Iterator(const Iterator<OtherConst>& Other) : Current(Other.Current) {}

        reference operator*() const { return Current->Value; }
        pointer   operator->() const { return std::addressof(Current->Value); }

        Iterator& operator++() { Current = Current->NextGlobal; return *this; }
        Iterator  operator++(int) { Iterator Tmp = *this; ++(*this); return Tmp; }
        Iterator& operator--() { Current = Current->PrevGlobal; return *this; }
        Iterator  operator--(int) { Iterator Tmp = *this; --(*this); return Tmp; }

        friend bool operator==(const Iterator& L, const Iterator& R) { return L.Current == R.Current; }
        friend bool operator!=(const Iterator& L, const Iterator& R) { return !(L == R); }
    };

    using iterator       = Iterator<false>;
    using const_iterator = Iterator<true>;




private:
    Hash Hasher;
    KeyEqual Equal;
    Allocator Alloc;

    Node** Buckets          = { nullptr };
    SIMDControlByte* Groups = { nullptr };
    size_type BucketSize    = { 0 };
    float MaxLoadFactor     = { 0.875 };
    size_type ElementSize   = { 0 };
    Node Sentinel;
};


#endif //PICTOR_LINKEDHASHTABLE_H
