//
// Created by Lzorn on 2026/11/20.
//
// LinkedHashTable — a high-performance, std::unordered_map-compatible hash table.
//
//   * Lookup uses open addressing with linear probing, accelerated by 7-bit
//     fingerprint filtering over SIMD 16-slot control-byte groups
//     (Swiss-table style), so a probe rejects most keys without a KeyEqual call.
//   * Insertion order is preserved by an intrusive doubly-linked list, giving
//     O(1) begin()/end() iteration, stable iteration order, and O(1) erasure by
//     iterator without invalidating other iterators.
//   * Erasure uses backward-shift deletion, so the table never accumulates
//     tombstones and stays dense.
//
// The user-provided hash is passed through a splitmix64/murmur3 finalizer so the
// table stays well distributed even for weak hashes (e.g. the identity hash that
// std::hash<size_t> commonly is) under power-of-two bucket counts.
//
// Naming follows the project convention (verb-first lower camelCase). The few
// names that must match the standard library for ranges/algorithms interop —
// begin/end/cbegin/cend/rbegin/rend, size/empty, swap — keep their std spelling.
//

#ifndef PICTOR_LINKEDHASHTABLE_H
#define PICTOR_LINKEDHASHTABLE_H

#include <infra/common/SIMDControlByte.hpp>

#include <bit>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <limits>
#include <memory>
#include <stdexcept>
#include <tuple>
#include <type_traits>
#include <utility>

template <
    typename Key,
    typename T,
    typename Hash      = std::hash<Key>,
    typename KeyEqual  = std::equal_to<Key>,
    typename Allocator = std::allocator<std::pair<const Key, T>>
>
class LinkedHashTable
{
    // ----------------------------------------------------------------- nodes
    // The doubly-linked list links live in a base subobject so the end marker
    // (Sentinel) never has to construct a `std::pair<const Key, T>`. This keeps
    // the table usable with non-default-constructible key/value types, matching
    // the std::unordered_map requirement.
    struct NodeBase
    {
        NodeBase* Next = nullptr;
        NodeBase* Prev = nullptr;
    };

    struct Node : NodeBase
    {
        template <typename KTuple, typename TTuple>
        Node(std::piecewise_construct_t, KTuple&& KArgs, TTuple&& TArgs)
            : Value(std::piecewise_construct,
                    std::forward<KTuple>(KArgs),
                    std::forward<TTuple>(TArgs))
        {
        }

        std::pair<const Key, T> Value;
    };

    using NodeAllocator     = typename std::allocator_traits<Allocator>::template rebind_alloc<Node>;
    using NodeAllocTraits   = std::allocator_traits<NodeAllocator>;
    using BucketAllocator   = typename std::allocator_traits<NodeAllocator>::template rebind_alloc<Node*>;
    using BucketAllocTraits = std::allocator_traits<BucketAllocator>;
    using GroupAllocator    = typename std::allocator_traits<NodeAllocator>::template rebind_alloc<SIMDControlByte>;
    using GroupAllocTraits  = std::allocator_traits<GroupAllocator>;

    static constexpr std::size_t GroupSize = SIMDControlByte::GroupSize; // 16
    using MaskType = SIMDControlByte::MaskType;

public:
    using key_type        = Key;
    using mapped_type     = T;
    using value_type      = std::pair<const Key, T>;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using hasher          = Hash;
    using key_equal       = KeyEqual;
    using allocator_type  = Allocator;
    using reference       = value_type&;
    using const_reference = const value_type&;
    using pointer         = typename std::allocator_traits<Allocator>::pointer;
    using const_pointer   = typename std::allocator_traits<Allocator>::const_pointer;

    // -------------------------------------------------------------- iterator
    template <bool IsConst>
    class Iterator
    {
        friend class LinkedHashTable;
        template <bool> friend class Iterator;

        using BasePtr = std::conditional_t<IsConst, const NodeBase*, NodeBase*>;
        using NodePtr = std::conditional_t<IsConst, const Node*, Node*>;

        BasePtr Current = nullptr;

        explicit Iterator(BasePtr InCurrent) noexcept : Current(InCurrent) {}

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using iterator_concept  = std::bidirectional_iterator_tag;
        using value_type        = typename LinkedHashTable::value_type;
        using difference_type   = typename LinkedHashTable::difference_type;
        using pointer           = std::conditional_t<IsConst, const value_type*, value_type*>;
        using reference         = std::conditional_t<IsConst, const value_type&, value_type&>;

        Iterator() = default;

        template <bool OtherConst, std::enable_if_t<IsConst && !OtherConst, int> = 0>
        Iterator(const Iterator<OtherConst>& Other) noexcept : Current(Other.Current) {}

        reference operator*() const { return static_cast<NodePtr>(Current)->Value; }
        pointer   operator->() const { return std::addressof(operator*()); }

        Iterator& operator++() noexcept { Current = Current->Next; return *this; }
        Iterator  operator++(int) noexcept { Iterator Tmp = *this; ++*this; return Tmp; }
        Iterator& operator--() noexcept { Current = Current->Prev; return *this; }
        Iterator  operator--(int) noexcept { Iterator Tmp = *this; --*this; return Tmp; }

        // Mixed const/non-const comparisons (std::unordered_map interoperability).
        template <bool OtherConst>
        bool operator==(const Iterator<OtherConst>& Other) const noexcept { return Current == Other.Current; }
        template <bool OtherConst>
        bool operator!=(const Iterator<OtherConst>& Other) const noexcept { return !(*this == Other); }
    };

    using iterator               = Iterator<false>;
    using const_iterator         = Iterator<true>;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // -------------------------------------------------------- local iterator
    // Iterates the elements of a single bucket (elements whose home slot is n).
    // Open addressing scatters a bucket's elements, so this filters the global
    // insertion-order list. It is a forward iterator, as std::unordered_map
    // requires of local_iterator.
    template <bool IsConst>
    class LocalIterator
    {
        friend class LinkedHashTable;
        template <bool> friend class LocalIterator;

        using OwnerPtr = std::conditional_t<IsConst, const LinkedHashTable*, LinkedHashTable*>;
        using BasePtr  = std::conditional_t<IsConst, const NodeBase*, NodeBase*>;
        using NodePtr  = std::conditional_t<IsConst, const Node*, Node*>;

        OwnerPtr  Owner = nullptr;
        BasePtr   Current = nullptr;
        size_type BucketIndex = 0;

        LocalIterator(OwnerPtr InOwner, BasePtr InCurrent, size_type InBucket) noexcept
            : Owner(InOwner), Current(InCurrent), BucketIndex(InBucket)
        {
        }

        // Advance until Current is the sentinel or a node whose home is BucketIndex.
        void advanceToMatch() noexcept
        {
            while (Current != &Owner->Sentinel &&
                   Owner->bucketOfNode(static_cast<const Node*>(Current)) != BucketIndex)
            {
                Current = Current->Next;
            }
        }

    public:
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept  = std::forward_iterator_tag;
        using value_type        = typename LinkedHashTable::value_type;
        using difference_type   = typename LinkedHashTable::difference_type;
        using pointer           = std::conditional_t<IsConst, const value_type*, value_type*>;
        using reference         = std::conditional_t<IsConst, const value_type&, value_type&>;

        LocalIterator() = default;

        template <bool OtherConst, std::enable_if_t<IsConst && !OtherConst, int> = 0>
        LocalIterator(const LocalIterator<OtherConst>& Other) noexcept
            : Owner(Other.Owner), Current(Other.Current), BucketIndex(Other.BucketIndex)
        {
        }

        reference operator*() const { return static_cast<NodePtr>(Current)->Value; }
        pointer   operator->() const { return std::addressof(operator*()); }

        LocalIterator& operator++() noexcept
        {
            Current = Current->Next;
            advanceToMatch();
            return *this;
        }
        LocalIterator operator++(int) noexcept { LocalIterator Tmp = *this; ++*this; return Tmp; }

        template <bool OtherConst>
        bool operator==(const LocalIterator<OtherConst>& Other) const noexcept
        {
            return Owner == Other.Owner && Current == Other.Current;
        }
        template <bool OtherConst>
        bool operator!=(const LocalIterator<OtherConst>& Other) const noexcept
        {
            return !(*this == Other);
        }
    };

    using local_iterator       = LocalIterator<false>;
    using const_local_iterator = LocalIterator<true>;

    // --------------------------------------------------------- construction
    LinkedHashTable()
        : LinkedHashTable(0, Hash{}, KeyEqual{}, Allocator{}) {}

    explicit LinkedHashTable(
        size_type BucketHint,
        const Hash& Hasher = Hash{},
        const KeyEqual& Equal = KeyEqual{},
        const Allocator& Alloc = Allocator{})
        : Hasher(Hasher), Equal(Equal), Alloc(Alloc)
    {
        Sentinel.Next = &Sentinel;
        Sentinel.Prev = &Sentinel;
        allocateBuckets(BucketHint == 0 ? 16 : BucketHint);
    }

    LinkedHashTable(const LinkedHashTable& Other)
        : Hasher(Other.Hasher), Equal(Other.Equal), Alloc(Other.Alloc)
    {
        Sentinel.Next = &Sentinel;
        Sentinel.Prev = &Sentinel;
        copyFrom(Other);
    }

    LinkedHashTable(LinkedHashTable&& Other) noexcept(
        std::allocator_traits<Allocator>::is_always_equal::value)
        : Hasher(std::move(Other.Hasher)), Equal(std::move(Other.Equal)), Alloc(std::move(Other.Alloc))
    {
        stealFrom(std::move(Other));
    }

    ~LinkedHashTable()
    {
        clear();
        deallocateBuckets();
    }

    // Copy assignment: build a copy of the target allocator and swap it in.
    // This gives the strong guarantee and correct allocator propagation for
    // always-equal allocators (std::allocator); for stateful non-propagating
    // allocators we copy element-wise using our own allocator.
    LinkedHashTable& operator=(const LinkedHashTable& Other)
    {
        if (this == &Other)
        {
            return *this;
        }

        if constexpr (NodeAllocTraits::propagate_on_container_copy_assignment::value ||
                      NodeAllocTraits::is_always_equal::value)
        {
            LinkedHashTable Tmp(Other);
            swap(Tmp);
        }
        else
        {
            // Keep our allocator; copy Other's policy and elements into a temp.
            LinkedHashTable Tmp(0, Other.Hasher, Other.Equal, Alloc);
            Tmp.copyFrom(Other);
            swap(Tmp);
        }
        return *this;
    }

    LinkedHashTable& operator=(LinkedHashTable&& Other) noexcept(
        std::allocator_traits<Allocator>::is_always_equal::value)
    {
        if (this != &Other)
        {
            clear();
            deallocateBuckets();
            Hasher = std::move(Other.Hasher);
            Equal  = std::move(Other.Equal);
            if constexpr (NodeAllocTraits::propagate_on_container_move_assignment::value)
            {
                Alloc = std::move(Other.Alloc);
            }
            stealFrom(std::move(Other));
        }
        return *this;
    }

    // ------------------------------------------------------------- iterators
    iterator begin() noexcept { return iterator(Sentinel.Next); }
    const_iterator begin() const noexcept { return const_iterator(Sentinel.Next); }
    const_iterator cbegin() const noexcept { return begin(); }
    iterator end() noexcept { return iterator(&Sentinel); }
    const_iterator end() const noexcept { return const_iterator(&Sentinel); }
    const_iterator cend() const noexcept { return end(); }

    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator crbegin() const noexcept { return rbegin(); }
    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crend() const noexcept { return rend(); }

    // ------------------------------------------------------------- capacity
    [[nodiscard]] bool empty() const noexcept { return ElementCount == 0; }
    [[nodiscard]] size_type size() const noexcept { return ElementCount; }
    [[nodiscard]] size_type getMaxSize() const noexcept
    {
        return NodeAllocTraits::max_size(NodeAllocator{Alloc});
    }

    // ------------------------------------------------------------ modifiers
    void clear() noexcept
    {
        NodeBase* Curr = Sentinel.Next;
        while (Curr != &Sentinel)
        {
            NodeBase* Next = Curr->Next;
            deallocateNode(static_cast<Node*>(Curr));
            Curr = Next;
        }
        Sentinel.Next = &Sentinel;
        Sentinel.Prev = &Sentinel;
        ElementCount = 0;
        if (Buckets)
        {
            resetBuckets();
        }
    }

    std::pair<iterator, bool> insert(const value_type& Value)
    {
        return tryEmplace(Value.first, Value.second);
    }
    std::pair<iterator, bool> insert(value_type&& Value)
    {
        return tryEmplace(Value.first, std::move(Value.second));
    }

    template <typename P, std::enable_if_t<std::is_constructible_v<value_type, P&&>, int> = 0>
    std::pair<iterator, bool> insert(P&& Value)
    {
        value_type Tmp(std::forward<P>(Value));
        return tryEmplace(Tmp.first, std::move(Tmp.second));
    }

    template <typename... Args>
    std::pair<iterator, bool> emplace(Args&&... Args_)
    {
        return insert(value_type(std::forward<Args>(Args_)...));
    }

    template <typename... Args>
    std::pair<iterator, bool> tryEmplace(const key_type& KeyValue, Args&&... Args_)
    {
        ensureCapacityForInsert();
        return tryEmplaceImpl(KeyValue, std::forward<Args>(Args_)...);
    }

    template <typename... Args>
    std::pair<iterator, bool> tryEmplace(key_type&& KeyValue, Args&&... Args_)
    {
        ensureCapacityForInsert();
        return tryEmplaceImpl(std::move(KeyValue), std::forward<Args>(Args_)...);
    }

    template <typename M>
    std::pair<iterator, bool> insertOrAssign(const key_type& KeyValue, M&& Obj)
    {
        iterator It = find(KeyValue);
        if (It != end())
        {
            It->second = std::forward<M>(Obj);
            return {It, false};
        }
        return {tryEmplace(KeyValue, std::forward<M>(Obj)).first, true};
    }

    template <typename M>
    std::pair<iterator, bool> insertOrAssign(key_type&& KeyValue, M&& Obj)
    {
        iterator It = find(KeyValue);
        if (It != end())
        {
            It->second = std::forward<M>(Obj);
            return {It, false};
        }
        return {tryEmplace(std::move(KeyValue), std::forward<M>(Obj)).first, true};
    }

    iterator erase(const_iterator Pos)
    {
        if (Pos == cend()) [[unlikely]]
        {
            return end();
        }
        Node* Target = static_cast<Node*>(const_cast<NodeBase*>(Pos.Current));
        iterator Next(Target->Next);
        eraseNode(Target);
        return Next;
    }

    size_type erase(const key_type& KeyValue)
    {
        Node* Found = findNode(KeyValue);
        if (!Found) [[unlikely]]
        {
            return 0;
        }
        eraseNode(Found);
        return 1;
    }

    iterator erase(const_iterator First, const_iterator Last)
    {
        while (First != Last)
        {
            First = erase(First);
        }
        return iterator(const_cast<NodeBase*>(Last.Current));
    }

    void swap(LinkedHashTable& Other) noexcept(
        std::allocator_traits<Allocator>::is_always_equal::value)
    {
        using std::swap;
        swap(Hasher, Other.Hasher);
        swap(Equal, Other.Equal);
        if constexpr (NodeAllocTraits::propagate_on_container_swap::value)
        {
            swap(Alloc, Other.Alloc);
        }
        swap(Buckets, Other.Buckets);
        swap(Groups, Other.Groups);
        swap(BucketCount, Other.BucketCount);
        swap(ElementCount, Other.ElementCount);
        swap(GrowthThreshold, Other.GrowthThreshold);
        swap(MaxLoadFactor, Other.MaxLoadFactor);
        relinkAfterSwap(Other);
    }

    // --------------------------------------------------------------- lookup
    iterator find(const key_type& KeyValue)
    {
        Node* Found = findNode(KeyValue);
        return Found ? iterator(Found) : end();
    }
    const_iterator find(const key_type& KeyValue) const
    {
        const Node* Found = findNode(KeyValue);
        return Found ? const_iterator(Found) : cend();
    }
    [[nodiscard]] bool contains(const key_type& KeyValue) const
    {
        return findNode(KeyValue) != nullptr;
    }

    mapped_type& at(const key_type& KeyValue)
    {
        Node* Found = findNode(KeyValue);
        if (!Found) [[unlikely]]
        {
            throw std::out_of_range("LinkedHashTable::at");
        }
        return Found->Value.second;
    }
    const mapped_type& at(const key_type& KeyValue) const
    {
        const Node* Found = findNode(KeyValue);
        if (!Found) [[unlikely]]
        {
            throw std::out_of_range("LinkedHashTable::at");
        }
        return Found->Value.second;
    }

    mapped_type& operator[](const key_type& KeyValue)
    {
        return tryEmplace(KeyValue).first->second;
    }
    mapped_type& operator[](key_type&& KeyValue)
    {
        return tryEmplace(std::move(KeyValue)).first->second;
    }

    // ---------------------------------------------------------- hash policy
    [[nodiscard]] float getLoadFactor() const noexcept
    {
        return BucketCount
            ? static_cast<float>(ElementCount) / static_cast<float>(BucketCount)
            : 0.f;
    }
    [[nodiscard]] float getMaxLoadFactor() const noexcept { return MaxLoadFactor; }
    void setMaxLoadFactor(float ML)
    {
        if (!(ML > 0.f && ML <= 1.f)) [[unlikely]]
        {
            throw std::out_of_range("LinkedHashTable::setMaxLoadFactor");
        }
        MaxLoadFactor = ML;
        updateGrowthThreshold();
    }

    void rehash(size_type Count)
    {
        Count = (std::max)(Count,
            static_cast<size_type>(static_cast<double>(ElementCount) / MaxLoadFactor) + 1);
        const size_type Target = roundupPowerOfTwo(Count);
        if (Target > BucketCount)
        {
            rehashImpl(Target);
        }
    }
    void reserve(size_type Count)
    {
        rehash(static_cast<size_type>(static_cast<double>(Count) / MaxLoadFactor) + 1);
    }

    [[nodiscard]] size_type getBucketCount() const noexcept { return BucketCount; }
    [[nodiscard]] size_type getMaxBucketCount() const noexcept
    {
        return size_type{1} << (std::numeric_limits<size_type>::digits - 1);
    }
    [[nodiscard]] size_type getBucket(const key_type& KeyValue) const
    {
        return hashKey(KeyValue) & (BucketCount - 1);
    }
    [[nodiscard]] size_type getBucketSize(size_type N) const
    {
        size_type Count = 0;
        for (const NodeBase* C = Sentinel.Next; C != &Sentinel; C = C->Next)
        {
            if (bucketOfNode(static_cast<const Node*>(C)) == N)
            {
                ++Count;
            }
        }
        return Count;
    }

    local_iterator begin(size_type N) noexcept
    {
        local_iterator It(this, Sentinel.Next, N);
        It.advanceToMatch();
        return It;
    }
    const_local_iterator begin(size_type N) const noexcept
    {
        const_local_iterator It(this, Sentinel.Next, N);
        It.advanceToMatch();
        return It;
    }
    const_local_iterator cbegin(size_type N) const noexcept { return begin(N); }
    local_iterator end(size_type N) noexcept { return local_iterator(this, &Sentinel, N); }
    const_local_iterator end(size_type N) const noexcept { return const_local_iterator(this, &Sentinel, N); }
    const_local_iterator cend(size_type N) const noexcept { return end(N); }

    [[nodiscard]] hasher getHasher() const { return Hasher; }
    [[nodiscard]] key_equal getKeyEqual() const { return Equal; }
    [[nodiscard]] allocator_type getAllocator() const noexcept { return Alloc; }

private:
    // --------------------------------------------------------------- hashing
    // Finalizer spreads entropy so weak hashes (identity, strided, ...) still
    // distribute well under power-of-two bucket counts.
    static constexpr size_type mixHash(size_type X) noexcept
    {
        if constexpr (sizeof(size_type) == 8)
        {
            X += 0x9e3779b97f4a7c15ULL;                 // splitmix64
            X = (X ^ (X >> 30)) * 0xbf58476d1ce4e5b9ULL;
            X = (X ^ (X >> 27)) * 0x94d049bb133111ebULL;
            return X ^ (X >> 31);
        }
        else
        {
            X ^= X >> 16;                               // murmur3 fmix32
            X *= 0x85ebca6bU;
            X ^= X >> 13;
            X *= 0xc2b2ae35U;
            X ^= X >> 16;
            return X;
        }
    }

    [[nodiscard]] size_type hashKey(const key_type& KeyValue) const
    {
        return mixHash(Hasher(KeyValue));
    }

    // Top 7 bits of the (mixed) hash: 0..127, never the 0xFF empty marker.
    static constexpr std::uint8_t fingerprintOf(size_type HashValue) noexcept
    {
        return static_cast<std::uint8_t>(HashValue >> (sizeof(size_type) * 8 - 7));
    }

    // Home slot of a live node (used by getBucketSize / bucket iterators).
    [[nodiscard]] size_type bucketOfNode(const Node* N) const
    {
        return hashKey(N->Value.first) & (BucketCount - 1);
    }

    // Mask of control-byte slots at or after `From` within a 16-slot group.
    static constexpr MaskType tailMask(std::size_t From) noexcept
    {
        return static_cast<MaskType>(~((MaskType{1} << From) - 1));
    }

    static constexpr size_type roundupPowerOfTwo(size_type Value) noexcept
    {
        if (Value <= 16)
        {
            return 16;
        }
        const size_type Top = size_type{1} << (std::numeric_limits<size_type>::digits - 1);
        if (Value > Top)
        {
            return Top;
        }
        size_type P = 16;
        while (P < Value)
        {
            P <<= 1;
        }
        return P;
    }

    [[nodiscard]] size_type groupIndexOf(size_type SlotIndex) const noexcept
    {
        return SlotIndex / GroupSize;
    }
    [[nodiscard]] size_type slotInGroup(size_type SlotIndex) const noexcept
    {
        return SlotIndex % GroupSize;
    }

    // ------------------------------------------------------------ allocation
    void allocateBuckets(size_type Count)
    {
        BucketCount = roundupPowerOfTwo(Count);
        const size_type GroupCount = BucketCount / GroupSize;

        BucketAllocator BucketAlloc{Alloc};
        GroupAllocator  GroupAlloc{Alloc};

        Node** NewBuckets = BucketAllocTraits::allocate(BucketAlloc, BucketCount);
        SIMDControlByte* NewGroups = nullptr;
        try
        {
            std::fill_n(NewBuckets, BucketCount, static_cast<Node*>(nullptr));
            NewGroups = GroupAllocTraits::allocate(GroupAlloc, GroupCount);
            for (size_type I = 0; I < GroupCount; ++I)
            {
                GroupAllocTraits::construct(GroupAlloc, NewGroups + I); // default ctor => empty
            }
        }
        catch (...)
        {
            if (NewGroups)
            {
                GroupAllocTraits::deallocate(GroupAlloc, NewGroups, GroupCount);
            }
            BucketAllocTraits::deallocate(BucketAlloc, NewBuckets, BucketCount);
            throw;
        }

        Buckets = NewBuckets;
        Groups = NewGroups;
        updateGrowthThreshold();
    }

    void deallocateBuckets() noexcept
    {
        if (Buckets)
        {
            BucketAllocator BucketAlloc{Alloc};
            BucketAllocTraits::deallocate(BucketAlloc, Buckets, BucketCount);
            Buckets = nullptr;
        }
        if (Groups)
        {
            GroupAllocator GroupAlloc{Alloc};
            const size_type GroupCount = BucketCount / GroupSize;
            for (size_type I = 0; I < GroupCount; ++I)
            {
                GroupAllocTraits::destroy(GroupAlloc, Groups + I);
            }
            GroupAllocTraits::deallocate(GroupAlloc, Groups, GroupCount);
            Groups = nullptr;
        }
    }

    void resetBuckets() noexcept
    {
        std::fill_n(Buckets, BucketCount, static_cast<Node*>(nullptr));
        for (size_type I = 0; I < BucketCount / GroupSize; ++I)
        {
            Groups[I] = SIMDControlByte{};
        }
    }

    void updateGrowthThreshold() noexcept
    {
        GrowthThreshold = static_cast<size_type>(static_cast<double>(BucketCount) * MaxLoadFactor);
        // Always keep at least one empty slot so probes terminate.
        if (GrowthThreshold >= BucketCount)
        {
            GrowthThreshold = BucketCount - 1;
        }
    }

    // -------------------------------------------------------------- find/insert
    Node* findNode(const key_type& KeyValue) const
    {
        const size_type H = hashKey(KeyValue);
        const auto FP = fingerprintOf(H);
        const size_type Mask = BucketCount - 1;
        const size_type GroupCount = BucketCount / GroupSize;
        size_type Group = (H & Mask) / GroupSize;
        std::size_t Start = (H & Mask) % GroupSize;

        for (size_type Probe = 0; Probe <= GroupCount; ++Probe)
        {
            const SIMDControlByte& G = Groups[Group];
            const MaskType Tail = tailMask(Start);

            MaskType Match = G.matchFingerprint(FP) & Tail;
            while (Match)
            {
                const std::size_t Bit = static_cast<std::size_t>(std::countr_zero(Match));
                const size_type Slot = Group * GroupSize + Bit;
                Node* Candidate = Buckets[Slot];
                if (Candidate && Equal(Candidate->Value.first, KeyValue)) [[likely]]
                {
                    return Candidate;
                }
                Match &= static_cast<MaskType>(Match - 1);
            }

            if (G.matchEmpty() & Tail) [[unlikely]]
            {
                break;
            }

            Group = (Group + 1) & (GroupCount - 1);
            Start = 0;
        }
        return nullptr;
    }

    template <typename K, typename... Args>
    std::pair<iterator, bool> tryEmplaceImpl(K&& KeyValue, Args&&... Args_)
    {
        const size_type H = hashKey(KeyValue);
        const auto FP = fingerprintOf(H);
        const size_type Mask = BucketCount - 1;
        const size_type GroupCount = BucketCount / GroupSize;
        size_type Group = (H & Mask) / GroupSize;
        std::size_t Start = (H & Mask) % GroupSize;

        for (size_type Probe = 0; Probe <= GroupCount; ++Probe)
        {
            SIMDControlByte& G = Groups[Group];
            const MaskType Tail = tailMask(Start);

            MaskType Match = G.matchFingerprint(FP) & Tail;
            while (Match)
            {
                const std::size_t Bit = static_cast<std::size_t>(std::countr_zero(Match));
                const size_type Slot = Group * GroupSize + Bit;
                Node* Candidate = Buckets[Slot];
                if (Candidate && Equal(Candidate->Value.first, KeyValue))
                {
                    return {iterator{Candidate}, false};
                }
                Match &= static_cast<MaskType>(Match - 1);
            }

            const MaskType EmptyMask = G.matchEmpty() & Tail;
            if (EmptyMask)
            {
                const std::size_t Bit = static_cast<std::size_t>(std::countr_zero(EmptyMask));
                const size_type Slot = Group * GroupSize + Bit;
                Node* NewNode = allocateNode(std::forward<K>(KeyValue), std::forward<Args>(Args_)...);
                Buckets[Slot] = NewNode;
                G.set(Bit, FP);
                linkTail(NewNode);
                ++ElementCount;
                return {iterator{NewNode}, true};
            }

            Group = (Group + 1) & (GroupCount - 1);
            Start = 0;
        }
        throw std::overflow_error("LinkedHashTable: no empty slot");
    }

    // --------------------------------------------------------------- erasure
    // Backward-shift deletion keeps the probe clusters contiguous without
    // tombstones. For each successor, recompute its home; move it into the gap
    // only when its home is at-or-before the gap in cyclic probe order.
    void eraseFromBuckets(Node* Target)
    {
        const size_type Mask = BucketCount - 1;
        const size_type H = hashKey(Target->Value.first);
        size_type Slot = H & Mask;
        while (Buckets[Slot] != Target)
        {
            Slot = (Slot + 1) & Mask;
        }

        size_type I = Slot;
        size_type J = (Slot + 1) & Mask;
        while (Buckets[J] != nullptr)
        {
            const size_type JH = hashKey(Buckets[J]->Value.first);
            const size_type Home = JH & Mask;
            if (((I - Home) & Mask) < ((J - Home) & Mask))
            {
                Buckets[I] = Buckets[J];
                Groups[groupIndexOf(I)].set(slotInGroup(I), fingerprintOf(JH));
                I = J;
            }
            J = (J + 1) & Mask;
        }
        Buckets[I] = nullptr;
        Groups[groupIndexOf(I)].set(slotInGroup(I), SIMDControlByte::EmptyByte);
    }

    void eraseNode(Node* Target)
    {
        eraseFromBuckets(Target);
        unlink(Target);
        deallocateNode(Target);
        --ElementCount;
    }

    // ---------------------------------------------------------------- rehash
    void rehashImpl(size_type NewBucketCount)
    {
        NewBucketCount = roundupPowerOfTwo(NewBucketCount);
        const size_type NewGroupCount = NewBucketCount / GroupSize;

        BucketAllocator BucketAlloc{Alloc};
        GroupAllocator  GroupAlloc{Alloc};

        Node** NewBuckets = BucketAllocTraits::allocate(BucketAlloc, NewBucketCount);
        SIMDControlByte* NewGroups = nullptr;
        try
        {
            std::fill_n(NewBuckets, NewBucketCount, static_cast<Node*>(nullptr));
            NewGroups = GroupAllocTraits::allocate(GroupAlloc, NewGroupCount);
            for (size_type I = 0; I < NewGroupCount; ++I)
            {
                GroupAllocTraits::construct(GroupAlloc, NewGroups + I);
            }
        }
        catch (...)
        {
            if (NewGroups)
            {
                GroupAllocTraits::deallocate(GroupAlloc, NewGroups, NewGroupCount);
            }
            BucketAllocTraits::deallocate(BucketAlloc, NewBuckets, NewBucketCount);
            throw;
        }

        try
        {
            const size_type NewMask = NewBucketCount - 1;
            NodeBase* Curr = Sentinel.Next;
            while (Curr != &Sentinel)
            {
                Node* N = static_cast<Node*>(Curr);
                const size_type H = hashKey(N->Value.first);
                size_type Slot = H & NewMask;
                while (NewBuckets[Slot] != nullptr)
                {
                    Slot = (Slot + 1) & NewMask;
                }
                NewBuckets[Slot] = N;
                NewGroups[Slot / GroupSize].set(Slot % GroupSize, fingerprintOf(H));
                Curr = Curr->Next;
            }
        }
        catch (...)
        {
            for (size_type I = 0; I < NewGroupCount; ++I)
            {
                GroupAllocTraits::destroy(GroupAlloc, NewGroups + I);
            }
            GroupAllocTraits::deallocate(GroupAlloc, NewGroups, NewGroupCount);
            BucketAllocTraits::deallocate(BucketAlloc, NewBuckets, NewBucketCount);
            throw;
        }

        // Commit.
        Node** OldBuckets = Buckets;
        SIMDControlByte* OldGroups = Groups;
        const size_type OldCount = BucketCount;
        const size_type OldGroupCount = OldCount / GroupSize;

        Buckets = NewBuckets;
        Groups = NewGroups;
        BucketCount = NewBucketCount;
        updateGrowthThreshold();

        for (size_type I = 0; I < OldGroupCount; ++I)
        {
            GroupAllocTraits::destroy(GroupAlloc, OldGroups + I);
        }
        GroupAllocTraits::deallocate(GroupAlloc, OldGroups, OldGroupCount);
        BucketAllocTraits::deallocate(BucketAlloc, OldBuckets, OldCount);
    }

    void ensureCapacityForInsert()
    {
        if (ElementCount + 1 > GrowthThreshold) [[unlikely]]
        {
            rehashImpl(BucketCount >= (std::numeric_limits<size_type>::max() >> 1)
                ? BucketCount
                : BucketCount * 2);
        }
    }

    // ------------------------------------------------------------ node alloc
    template <typename K, typename... Args>
    Node* allocateNode(K&& KeyValue, Args&&... Args_)
    {
        NodeAllocator NodeAlloc{Alloc};
        Node* P = NodeAllocTraits::allocate(NodeAlloc, 1);
        try
        {
            NodeAllocTraits::construct(
                NodeAlloc, P,
                std::piecewise_construct,
                std::forward_as_tuple(std::forward<K>(KeyValue)),
                std::forward_as_tuple(std::forward<Args>(Args_)...));
        }
        catch (...)
        {
            NodeAllocTraits::deallocate(NodeAlloc, P, 1);
            throw;
        }
        return P;
    }

    void deallocateNode(Node* P) noexcept
    {
        NodeAllocator NodeAlloc{Alloc};
        NodeAllocTraits::destroy(NodeAlloc, P);
        NodeAllocTraits::deallocate(NodeAlloc, P, 1);
    }

    // -------------------------------------------------------- linked-list ops
    void linkTail(NodeBase* Target) noexcept
    {
        Target->Prev = Sentinel.Prev;
        Target->Next = &Sentinel;
        Sentinel.Prev->Next = Target;
        Sentinel.Prev = Target;
    }

    void unlink(NodeBase* Target) noexcept
    {
        Target->Prev->Next = Target->Next;
        Target->Next->Prev = Target->Prev;
        Target->Next = nullptr;
        Target->Prev = nullptr;
    }

    // ------------------------------------------------------------- ownership
    void copyFrom(const LinkedHashTable& Other)
    {
        allocateBuckets(Other.BucketCount == 0 ? 16 : Other.BucketCount);
        try
        {
            const NodeBase* Curr = Other.Sentinel.Next;
            while (Curr != &Other.Sentinel)
            {
                const Node* Src = static_cast<const Node*>(Curr);
                const size_type H = hashKey(Src->Value.first);
                Node* NewNode = allocateNode(Src->Value.first, Src->Value.second);
                size_type Slot = H & (BucketCount - 1);
                while (Buckets[Slot] != nullptr)
                {
                    Slot = (Slot + 1) & (BucketCount - 1);
                }
                Buckets[Slot] = NewNode;
                Groups[groupIndexOf(Slot)].set(slotInGroup(Slot), fingerprintOf(H));
                linkTail(NewNode);
                ++ElementCount;
                Curr = Curr->Next;
            }
        }
        catch (...)
        {
            clear();
            deallocateBuckets();
            throw;
        }
    }

    void stealFrom(LinkedHashTable&& Other) noexcept
    {
        Buckets = Other.Buckets;
        Groups = Other.Groups;
        BucketCount = Other.BucketCount;
        ElementCount = Other.ElementCount;
        GrowthThreshold = Other.GrowthThreshold;
        MaxLoadFactor = Other.MaxLoadFactor;

        if (Other.ElementCount == 0)
        {
            Sentinel.Next = &Sentinel;
            Sentinel.Prev = &Sentinel;
        }
        else
        {
            Sentinel.Next = Other.Sentinel.Next;
            Sentinel.Prev = Other.Sentinel.Prev;
            Sentinel.Next->Prev = &Sentinel;
            Sentinel.Prev->Next = &Sentinel;
        }

        Other.Buckets = nullptr;
        Other.Groups = nullptr;
        Other.BucketCount = 0;
        Other.ElementCount = 0;
        Other.GrowthThreshold = 0;
        Other.Sentinel.Next = &Other.Sentinel;
        Other.Sentinel.Prev = &Other.Sentinel;
    }

    // `swap` exchanges the array pointers and counters but each object's Sentinel
    // is a fixed subobject, so the lists must be re-linked to their new owner.
    void relinkAfterSwap(LinkedHashTable& Other) noexcept
    {
        NodeBase* const AHead = Sentinel.Next;
        NodeBase* const ATail = Sentinel.Prev;
        NodeBase* const BHead = Other.Sentinel.Next;
        NodeBase* const BTail = Other.Sentinel.Prev;

        if (BHead == &Other.Sentinel)
        {
            Sentinel.Next = &Sentinel;
            Sentinel.Prev = &Sentinel;
        }
        else
        {
            Sentinel.Next = BHead;
            Sentinel.Prev = BTail;
            BHead->Prev = &Sentinel;
            BTail->Next = &Sentinel;
        }

        if (AHead == &Sentinel)
        {
            Other.Sentinel.Next = &Other.Sentinel;
            Other.Sentinel.Prev = &Other.Sentinel;
        }
        else
        {
            Other.Sentinel.Next = AHead;
            Other.Sentinel.Prev = ATail;
            AHead->Prev = &Other.Sentinel;
            ATail->Next = &Other.Sentinel;
        }
    }

private:
    Hash Hasher;
    KeyEqual Equal;
    Allocator Alloc;

    NodeBase Sentinel;                 // end marker; carries links only, no value
    Node** Buckets = nullptr;
    SIMDControlByte* Groups = nullptr;
    size_type BucketCount = 0;
    size_type ElementCount = 0;
    size_type GrowthThreshold = 0;
    float MaxLoadFactor = 0.875f;
};

template <typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
void swap(LinkedHashTable<Key, T, Hash, KeyEqual, Allocator>& L,
          LinkedHashTable<Key, T, Hash, KeyEqual, Allocator>& R) noexcept(noexcept(L.swap(R)))
{
    L.swap(R);
}

#endif //PICTOR_LINKEDHASHTABLE_H
