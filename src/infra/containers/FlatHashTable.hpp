/**
 * @file FlatHashTable.hpp
 * @brief 扁平（开放寻址）哈希映射：在不需要迭代顺序稳定时，作为 LinkedHashTable
 *        最快的直接替代。
 *
 *   * 值内联存放在一个连续槽位数组中（无逐节点分配、无指针追踪）。查找采用
 *     线性探测，并通过 7 位 SIMD 指纹组加速，与 LinkedHashTable 完全一致。
 *   * insert/tryEmplace 直接在找到的空槽中构造值：零逐元素分配。删除采用
 *     向后移位（backward-shift deletion），搬移值以保持探测簇连续（无墓碑、
 *     无释放）。
 *   * 迭代按内存顺序遍历槽位数组（顺序未指定，与插入顺序不同）。按扁平映射
 *     的约定，insert（重哈希）与 erase 会使迭代器/指针/引用失效。
 *   * 键必须可移动或可拷贝，以便移位删除/重哈希能搬移它们。
 *
 * 公共接口、命名约定以及与 std/range 的互操作点均与 LinkedHashTable 一致。
 * 用户哈希经过 splitmix64/murmur3 终结器，使弱哈希（例如恒等 std::hash<size_t>）
 * 保持良好分布。
 */

#ifndef PICTOR_FLATHASHTABLE_H
#define PICTOR_FLATHASHTABLE_H

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
class FlatHashTable
{
    // 槽位存放非 const 的 Key，以便向后移位删除能整体移动赋值槽位。公共
    // value_type（pair<const Key, T>）在所有支持的 ABI 上布局完全相同；迭代器
    // 通过 reinterpret_cast 暴露槽位，并由这些 static_assert 守护（与 abseil /
    // ankerl 扁平映射相同的技巧）。
    using SlotPair = std::pair<Key, T>;

    using SlotAllocator     = typename std::allocator_traits<Allocator>::template rebind_alloc<SlotPair>;
    using SlotAllocTraits   = std::allocator_traits<SlotAllocator>;
    using GroupAllocator    = typename std::allocator_traits<SlotAllocator>::template rebind_alloc<SIMDControlByte>;
    using GroupAllocTraits  = std::allocator_traits<GroupAllocator>;

    static constexpr size_t GroupSize = SIMDControlByte::GroupSize; // 16
    using MaskType = SIMDControlByte::MaskType;

public:
    using key_type        = Key;
    using mapped_type     = T;
    using value_type      = std::pair<const Key, T>;
    using size_type       = size_t;
    using difference_type = std::ptrdiff_t;
    using hasher          = Hash;
    using key_equal       = KeyEqual;
    using allocator_type  = Allocator;
    using reference       = value_type&;
    using const_reference = const value_type&;
    using pointer         = typename std::allocator_traits<Allocator>::pointer;
    using const_pointer   = typename std::allocator_traits<Allocator>::const_pointer;

    static_assert(sizeof(SlotPair) == sizeof(value_type),
        "FlatHashTable: pair<Key,T> and pair<const Key,T> layouts differ — unsupported");
    static_assert(alignof(SlotPair) == alignof(value_type),
        "FlatHashTable: pair<Key,T> and pair<const Key,T> alignments differ — unsupported");

    // -------------------------------------------------------------- 迭代器
    // 按内存顺序迭代被占用的槽位。insert 与 erase 可能使迭代器失效（扁平映射约定）。
    template <bool IsConst>
    class Iterator
    {
        friend class FlatHashTable;
        template <bool> friend class Iterator;

        using OwnerPtr = std::conditional_t<IsConst, const FlatHashTable*, FlatHashTable*>;
        using SlotPtr  = std::conditional_t<IsConst, const SlotPair*, SlotPair*>;
        using ValuePtr = std::conditional_t<IsConst, const value_type*, value_type*>;

        OwnerPtr Owner = nullptr;
        SlotPtr  Current = nullptr;

        explicit Iterator(OwnerPtr InOwner, SlotPtr InCurrent) noexcept
            : Owner(InOwner), Current(InCurrent) {}

        void skipEmptyForward() noexcept
        {
            if (!Owner->Slots) [[unlikely]] { return; }
            const SlotPtr End = Owner->Slots + Owner->BucketCount;
            while (Current != End &&
                   !Owner->isOccupiedSlot(static_cast<size_type>(Current - Owner->Slots)))
            {
                ++Current;
            }
        }

        void skipEmptyBackward() noexcept
        {
            if (!Owner->Slots) [[unlikely]] { return; }
            while (Current != Owner->Slots &&
                   !Owner->isOccupiedSlot(static_cast<size_type>(Current - Owner->Slots)))
            {
                --Current;
            }
        }

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using iterator_concept  = std::bidirectional_iterator_tag;
        using value_type        = typename FlatHashTable::value_type;
        using difference_type   = typename FlatHashTable::difference_type;
        using pointer           = std::conditional_t<IsConst, const value_type*, value_type*>;
        using reference         = std::conditional_t<IsConst, const value_type&, value_type&>;

        Iterator() = default;

        template <bool OtherConst, std::enable_if_t<IsConst && !OtherConst, int> = 0>
        Iterator(const Iterator<OtherConst>& Other) noexcept
            : Owner(Other.Owner), Current(Other.Current) {}

        reference operator*() const { return *reinterpret_cast<ValuePtr>(Current); }
        pointer   operator->() const { return std::addressof(operator*()); }

        Iterator& operator++() noexcept { ++Current; skipEmptyForward(); return *this; }
        Iterator  operator++(int) noexcept { Iterator Tmp = *this; ++*this; return Tmp; }
        Iterator& operator--() noexcept { --Current; skipEmptyBackward(); return *this; }
        Iterator  operator--(int) noexcept { Iterator Tmp = *this; --*this; return Tmp; }

        template <bool OtherConst>
        bool operator==(const Iterator<OtherConst>& Other) const noexcept { return Current == Other.Current; }
        template <bool OtherConst>
        bool operator!=(const Iterator<OtherConst>& Other) const noexcept { return !(*this == Other); }
    };

    using iterator               = Iterator<false>;
    using const_iterator         = Iterator<true>;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // 局部迭代器 --------------------------------------------------------
    // 迭代 home slot 为 N 的元素（前向迭代器，与 LinkedHashTable 的桶迭代器一致）。
    template <bool IsConst>
    class LocalIterator
    {
        friend class FlatHashTable;
        template <bool> friend class LocalIterator;

        using OwnerPtr = std::conditional_t<IsConst, const FlatHashTable*, FlatHashTable*>;
        using SlotPtr  = std::conditional_t<IsConst, const SlotPair*, SlotPair*>;
        using ValuePtr = std::conditional_t<IsConst, const value_type*, value_type*>;

        OwnerPtr  Owner = nullptr;
        SlotPtr   Current = nullptr;
        size_type BucketIndex = 0;

        explicit LocalIterator(OwnerPtr InOwner, SlotPtr InCurrent, size_type InBucket) noexcept
            : Owner(InOwner), Current(InCurrent), BucketIndex(InBucket) {}

        void advanceToMatch() noexcept
        {
            if (!Owner->Slots) [[unlikely]] { return; }
            const SlotPtr End = Owner->Slots + Owner->BucketCount;
            while (Current != End)
            {
                if (const auto I = static_cast<size_type>(Current - Owner->Slots); 
                    Owner->isOccupiedSlot(I) && Owner->homeOfSlot(I) == BucketIndex)
                {
                    return;
                }
                ++Current;
            }
        }

    public:
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept  = std::forward_iterator_tag;
        using value_type        = typename FlatHashTable::value_type;
        using difference_type   = typename FlatHashTable::difference_type;
        using pointer           = std::conditional_t<IsConst, const value_type*, value_type*>;
        using reference         = std::conditional_t<IsConst, const value_type&, value_type&>;

        LocalIterator() = default;

        template <bool OtherConst, std::enable_if_t<IsConst && !OtherConst, int> = 0>
        LocalIterator(const LocalIterator<OtherConst>& Other) noexcept
            : Owner(Other.Owner), Current(Other.Current), BucketIndex(Other.BucketIndex) {}

        reference operator*() const { return *reinterpret_cast<ValuePtr>(Current); }
        pointer   operator->() const { return std::addressof(operator*()); }

        LocalIterator& operator++() noexcept { ++Current; advanceToMatch(); return *this; }
        LocalIterator  operator++(int) noexcept { LocalIterator Tmp = *this; ++*this; return Tmp; }

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

    // 构造 ---------------------------------------------------------
    FlatHashTable()
        : FlatHashTable(0, Hash{}, KeyEqual{}, Allocator{}) {}

    explicit FlatHashTable(
        size_type BucketHint,
        const Hash& Hasher = Hash{},
        const KeyEqual& Equal = KeyEqual{},
        const Allocator& Alloc = Allocator{})
        : Hasher(Hasher), Equal(Equal), Alloc(Alloc)
    {
        allocateStorage(BucketHint == 0 ? 16 : BucketHint);
    }

    FlatHashTable(const FlatHashTable& Other)
        : Hasher(Other.Hasher), Equal(Other.Equal), Alloc(Other.Alloc)
    {
        copyFrom(Other);
    }

    FlatHashTable(FlatHashTable&& Other) noexcept(
        std::allocator_traits<Allocator>::is_always_equal::value)
        : Hasher(std::move(Other.Hasher)), Equal(std::move(Other.Equal)), Alloc(std::move(Other.Alloc))
    {
        stealFrom(std::move(Other));
    }

    ~FlatHashTable()
    {
        clear();
        deallocateStorage();
    }

    FlatHashTable& operator=(const FlatHashTable& Other)
    {
        if (this == &Other)
        {
            return *this;
        }

        if constexpr (SlotAllocTraits::propagate_on_container_copy_assignment::value ||
                      SlotAllocTraits::is_always_equal::value)
        {
            FlatHashTable Tmp(Other);
            swap(Tmp);
        }
        else
        {
            // 保留自身分配器；把 Other 的策略与元素拷贝进临时对象。
            FlatHashTable Tmp(0, Other.Hasher, Other.Equal, Alloc);
            Tmp.copyFrom(Other);
            swap(Tmp);
        }
        return *this;
    }

    FlatHashTable& operator=(FlatHashTable&& Other) noexcept(
        std::allocator_traits<Allocator>::is_always_equal::value)
    {
        if (this != &Other)
        {
            clear();
            deallocateStorage();
            Hasher = std::move(Other.Hasher);
            Equal  = std::move(Other.Equal);
            if constexpr (SlotAllocTraits::propagate_on_container_move_assignment::value)
            {
                Alloc = std::move(Other.Alloc);
            }
            stealFrom(std::move(Other));
        }
        return *this;
    }

    // ------------------------------------------------------------- 迭代器
    iterator begin() noexcept
    {
        iterator It(this, Slots);
        It.skipEmptyForward();
        return It;
    }
    const_iterator begin() const noexcept
    {
        const_iterator It(this, Slots);
        It.skipEmptyForward();
        return It;
    }
    const_iterator cbegin() const noexcept { return begin(); }
    iterator end() noexcept { return iterator(this, BucketCount ? Slots + BucketCount : nullptr); }
    const_iterator end() const noexcept { return const_iterator(this, BucketCount ? Slots + BucketCount : nullptr); }
    const_iterator cend() const noexcept { return end(); }

    reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    const_reverse_iterator crbegin() const noexcept { return rbegin(); }
    reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
    const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
    const_reverse_iterator crend() const noexcept { return rend(); }

    // ------------------------------------------------------------- 容量
    [[nodiscard]] bool empty() const noexcept { return ElementCount == 0; }
    [[nodiscard]] size_type size() const noexcept { return ElementCount; }
    [[nodiscard]] size_type getMaxSize() const noexcept
    {
        return SlotAllocTraits::max_size(SlotAllocator{Alloc});
    }

    // ------------------------------------------------------------ 修改器
    void clear() noexcept
    {
        for (size_type I = 0; I < BucketCount; ++I)
        {
            if (isOccupiedSlot(I))
            {
                destroySlot(I);
            }
        }
        ElementCount = 0;
        if (Slots)
        {
            resetGroups();
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
        const auto Slot = static_cast<size_type>(Pos.Current - Slots);
        eraseSlot(Slot);
        iterator Next(this, Slots + Slot);
        Next.skipEmptyForward();
        return Next;
    }

    size_type erase(const key_type& KeyValue)
    {
        SlotPair* Found = findSlot(KeyValue);
        if (!Found) [[unlikely]]
        {
            return 0;
        }
        eraseSlot(static_cast<size_type>(Found - Slots));
        return 1;
    }

    // 按槽位顺序删除 [First, Last)。从右向左删除可保证当前元素以下的每个位置
    // 都不受影响，因此每次删除都命中区间内原本的元素。
    iterator erase(const_iterator First, const_iterator Last)
    {
        const auto FirstPos = static_cast<size_type>(First.Current - Slots);
        const auto LastPos  = static_cast<size_type>(Last.Current - Slots);
        for (size_type P = LastPos; P > FirstPos; --P)
        {
            if (isOccupiedSlot(P - 1))
            {
                eraseSlot(P - 1);
            }
        }
        iterator Result(this, Slots + FirstPos);
        Result.skipEmptyForward();
        return Result;
    }

    void swap(FlatHashTable& Other) noexcept(
        std::allocator_traits<Allocator>::is_always_equal::value)
    {
        using std::swap;
        swap(Hasher, Other.Hasher);
        swap(Equal, Other.Equal);
        if constexpr (SlotAllocTraits::propagate_on_container_swap::value)
        {
            swap(Alloc, Other.Alloc);
        }
        swap(Slots, Other.Slots);
        swap(Groups, Other.Groups);
        swap(BucketCount, Other.BucketCount);
        swap(ElementCount, Other.ElementCount);
        swap(GrowthThreshold, Other.GrowthThreshold);
        swap(MaxLoadFactor, Other.MaxLoadFactor);
    }

    // --------------------------------------------------------------- 查找
    iterator find(const key_type& KeyValue)
    {
        SlotPair* Found = findSlot(KeyValue);
        return Found ? iterator(this, Found) : end();
    }
    const_iterator find(const key_type& KeyValue) const
    {
        const SlotPair* Found = findSlot(KeyValue);
        return Found ? const_iterator(this, Found) : cend();
    }
    [[nodiscard]] bool contains(const key_type& KeyValue) const
    {
        return findSlot(KeyValue) != nullptr;
    }

    mapped_type& at(const key_type& KeyValue)
    {
        SlotPair* Found = findSlot(KeyValue);
        if (!Found) [[unlikely]]
        {
            throw std::out_of_range("FlatHashTable::at");
        }
        return Found->second;
    }
    const mapped_type& at(const key_type& KeyValue) const
    {
        const SlotPair* Found = findSlot(KeyValue);
        if (!Found) [[unlikely]]
        {
            throw std::out_of_range("FlatHashTable::at");
        }
        return Found->second;
    }

    mapped_type& operator[](const key_type& KeyValue)
    {
        return tryEmplace(KeyValue).first->second;
    }
    mapped_type& operator[](key_type&& KeyValue)
    {
        return tryEmplace(std::move(KeyValue)).first->second;
    }

    // ---------------------------------------------------------- 哈希策略
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
            throw std::out_of_range("FlatHashTable::setMaxLoadFactor");
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
        for (size_type I = 0; I < BucketCount; ++I)
        {
            if (isOccupiedSlot(I) && homeOfSlot(I) == N)
            {
                ++Count;
            }
        }
        return Count;
    }

    local_iterator begin(size_type N) noexcept
    {
        local_iterator It(this, Slots, N);
        It.advanceToMatch();
        return It;
    }
    const_local_iterator begin(size_type N) const noexcept
    {
        const_local_iterator It(this, Slots, N);
        It.advanceToMatch();
        return It;
    }
    const_local_iterator cbegin(size_type N) const noexcept { return begin(N); }
    local_iterator end(size_type N) noexcept
    {
        return local_iterator(this, BucketCount ? Slots + BucketCount : nullptr, N);
    }
    const_local_iterator end(size_type N) const noexcept
    {
        return const_local_iterator(this, BucketCount ? Slots + BucketCount : nullptr, N);
    }
    const_local_iterator cend(size_type N) const noexcept { return end(N); }

    [[nodiscard]] hasher getHasher() const { return Hasher; }
    [[nodiscard]] key_equal getKeyEqual() const { return Equal; }
    [[nodiscard]] allocator_type getAllocator() const noexcept { return Alloc; }

private:
    // --------------------------------------------------------------- 哈希
    // 终结器扩散熵，使弱哈希（恒等、步进等）在 2 的幂桶数下仍能良好分布。
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

    // 取（混合后）哈希的高 7 位：0..127，永不为 0xFF 空标记。
    static constexpr std::uint8_t fingerprintOf(size_type HashValue) noexcept
    {
        return static_cast<std::uint8_t>(HashValue >> (sizeof(size_type) * 8 - 7));
    }

    // 16 槽组内位于 `From` 及其之后的控制字节槽位掩码。
    static constexpr MaskType tailMask(size_t From) noexcept
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

    [[nodiscard]] bool isOccupiedSlot(size_type I) const noexcept
    {
        return Groups[I / GroupSize].get(I % GroupSize) != SIMDControlByte::EmptyByte;
    }
    [[nodiscard]] size_type homeOfSlot(size_type I) const
    {
        return hashKey(Slots[I].first) & (BucketCount - 1);
    }

    // ------------------------------------------------------------ 分配
    // 分配槽位数组（值在插入时惰性原地构造）与控制字节组；二者均来自用户分配器。
    void allocateStorage(size_type Count)
    {
        BucketCount = roundupPowerOfTwo(Count);
        const size_type GroupCount = BucketCount / GroupSize;

        SlotAllocator  SA{Alloc};
        GroupAllocator GA{Alloc};

        SlotPair* NewSlots = SlotAllocTraits::allocate(SA, BucketCount);
        SIMDControlByte* NewGroups = nullptr;
        try
        {
            NewGroups = GroupAllocTraits::allocate(GA, GroupCount);
            for (size_type I = 0; I < GroupCount; ++I)
            {
                GroupAllocTraits::construct(GA, NewGroups + I); // 默认构造 => 空
            }
        }
        catch (...)
        {
            if (NewGroups)
            {
                GroupAllocTraits::deallocate(GA, NewGroups, GroupCount);
            }
            SlotAllocTraits::deallocate(SA, NewSlots, BucketCount);
            throw;
        }

        Slots = NewSlots;
        Groups = NewGroups;
        updateGrowthThreshold();
    }

    void deallocateStorage() noexcept
    {
        if (Slots)
        {
            SlotAllocator SA{Alloc};
            SlotAllocTraits::deallocate(SA, Slots, BucketCount);
            Slots = nullptr;
        }
        if (Groups)
        {
            GroupAllocator GA{Alloc};
            const size_type GroupCount = BucketCount / GroupSize;
            for (size_type I = 0; I < GroupCount; ++I)
            {
                GroupAllocTraits::destroy(GA, Groups + I);
            }
            GroupAllocTraits::deallocate(GA, Groups, GroupCount);
            Groups = nullptr;
        }
    }

    void resetGroups() noexcept
    {
        for (size_type I = 0; I < BucketCount / GroupSize; ++I)
        {
            Groups[I] = SIMDControlByte{};
        }
    }

    void updateGrowthThreshold() noexcept
    {
        GrowthThreshold = static_cast<size_type>(static_cast<double>(BucketCount) * MaxLoadFactor);
        // 始终至少保留一个空槽，以保证探测能终止。
        if (GrowthThreshold >= BucketCount)
        {
            GrowthThreshold = BucketCount - 1;
        }
    }

    // 槽位生命周期 -----------------------------------------------------------
    template <typename K, typename... Args>
    void constructSlot(size_type Slot, K&& KeyValue, Args&&... Args_)
    {
        SlotAllocator SA{Alloc};
        SlotAllocTraits::construct(SA, Slots + Slot,
            std::piecewise_construct,
            std::forward_as_tuple(std::forward<K>(KeyValue)),
            std::forward_as_tuple(std::forward<Args>(Args_)...));
    }

    void destroySlot(size_type Slot) noexcept
    {
        SlotAllocator SA{Alloc};
        SlotAllocTraits::destroy(SA, Slots + Slot);
    }

    // 查找/插入 --------------------------------------------------------------
    SlotPair* findSlot(const key_type& KeyValue) const
    {
        const size_type H = hashKey(KeyValue);
        const auto FP = fingerprintOf(H);
        const size_type Mask = BucketCount - 1;
        const size_type GroupCount = BucketCount / GroupSize;
        size_type Group = (H & Mask) / GroupSize;
        size_t Start = (H & Mask) % GroupSize;

        for (size_type Probe = 0; Probe <= GroupCount; ++Probe)
        {
            const SIMDControlByte& G = Groups[Group];
            const MaskType Tail = tailMask(Start);

            // 指纹匹配即意味着槽位被占用，因此无需判空。
            MaskType Match = G.matchFingerprint(FP) & Tail;
            while (Match)
            {
                const auto Bit = static_cast<size_t>(std::countr_zero(Match));
                const size_type Slot = Group * GroupSize + Bit;
                if (Equal(Slots[Slot].first, KeyValue)) [[likely]]
                {
                    return Slots + Slot;
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
        size_t Start = (H & Mask) % GroupSize;

        for (size_type Probe = 0; Probe <= GroupCount; ++Probe)
        {
            SIMDControlByte& G = Groups[Group];
            const MaskType Tail = tailMask(Start);

            MaskType Match = G.matchFingerprint(FP) & Tail;
            while (Match)
            {
                const auto Bit = static_cast<size_t>(std::countr_zero(Match));
                const size_type Slot = Group * GroupSize + Bit;
                if (Equal(Slots[Slot].first, KeyValue))
                {
                    return {iterator{this, Slots + Slot}, false};
                }
                Match &= static_cast<MaskType>(Match - 1);
            }

            const MaskType EmptyMask = G.matchEmpty() & Tail;
            if (EmptyMask)
            {
                const auto Bit = static_cast<size_t>(std::countr_zero(EmptyMask));
                const size_type Slot = Group * GroupSize + Bit;
                constructSlot(Slot, std::forward<K>(KeyValue), std::forward<Args>(Args_)...);
                G.set(Bit, FP);
                ++ElementCount;
                return {iterator{this, Slots + Slot}, true};
            }

            Group = (Group + 1) & (GroupCount - 1);
            Start = 0;
        }
        throw std::overflow_error("FlatHashTable: no empty slot");
    }

    // --------------------------------------------------------------- 删除
    // 向后移位删除：左移值以填补空洞（noexcept 可移动则移动，否则拷贝），并销毁
    // 末尾的被移动源值。不释放内存、不留墓碑。
    void eraseSlot(size_type Slot)
    {
        const size_type Mask = BucketCount - 1;

        size_type I = Slot;
        size_type J = (Slot + 1) & Mask;
        while (isOccupiedSlot(J))
        {
            const size_type JH = hashKey(Slots[J].first);
            const size_type Home = JH & Mask;
            if (((I - Home) & Mask) < ((J - Home) & Mask))
            {
                Slots[I] = std::move_if_noexcept(Slots[J]);
                Groups[groupIndexOf(I)].set(slotInGroup(I), fingerprintOf(JH));
                I = J;
            }
            J = (J + 1) & Mask;
        }
        destroySlot(I);
        Groups[groupIndexOf(I)].set(slotInGroup(I), SIMDControlByte::EmptyByte);
        --ElementCount;
    }

    // ---------------------------------------------------------------- 重哈希
    void rehashImpl(size_type NewBucketCount)
    {
        NewBucketCount = roundupPowerOfTwo(NewBucketCount);
        const size_type NewGroupCount = NewBucketCount / GroupSize;

        SlotAllocator  SA{Alloc};
        GroupAllocator GA{Alloc};

        SlotPair* NewSlots = SlotAllocTraits::allocate(SA, NewBucketCount);
        SIMDControlByte* NewGroups = nullptr;
        try
        {
            NewGroups = GroupAllocTraits::allocate(GA, NewGroupCount);
            for (size_type I = 0; I < NewGroupCount; ++I)
            {
                GroupAllocTraits::construct(GA, NewGroups + I);
            }
        }
        catch (...)
        {
            if (NewGroups)
            {
                GroupAllocTraits::deallocate(GA, NewGroups, NewGroupCount);
            }
            SlotAllocTraits::deallocate(SA, NewSlots, NewBucketCount);
            throw;
        }

        try
        {
            const size_type NewMask = NewBucketCount - 1;
            for (size_type I = 0; I < BucketCount; ++I)
            {
                if (!isOccupiedSlot(I))
                {
                    continue;
                }
                const size_type H = hashKey(Slots[I].first);
                size_type S = H & NewMask;
                while (NewGroups[S / GroupSize].get(S % GroupSize) != SIMDControlByte::EmptyByte)
                {
                    S = (S + 1) & NewMask;
                }
                SlotAllocTraits::construct(SA, NewSlots + S, std::move_if_noexcept(Slots[I]));
                NewGroups[S / GroupSize].set(S % GroupSize, fingerprintOf(H));
            }
        }
        catch (...)
        {
            // 新组的控制字节精确记录了哪些新槽位已被构造。
            for (size_type I = 0; I < NewBucketCount; ++I)
            {
                if (NewGroups[I / GroupSize].get(I % GroupSize) != SIMDControlByte::EmptyByte)
                {
                    SlotAllocTraits::destroy(SA, NewSlots + I);
                }
            }
            for (size_type I = 0; I < NewGroupCount; ++I)
            {
                GroupAllocTraits::destroy(GA, NewGroups + I);
            }
            GroupAllocTraits::deallocate(GA, NewGroups, NewGroupCount);
            SlotAllocTraits::deallocate(SA, NewSlots, NewBucketCount);
            throw;
        }

        // 提交：销毁旧值、交换数组、释放旧存储。
        SlotPair* OldSlots = Slots;
        SIMDControlByte* OldGroups = Groups;
        const size_type OldCount = BucketCount;
        const size_type OldGroupCount = OldCount / GroupSize;

        for (size_type I = 0; I < OldCount; ++I)
        {
            if (isOccupiedSlot(I))
            {
                SlotAllocTraits::destroy(SA, Slots + I);
            }
        }

        Slots = NewSlots;
        Groups = NewGroups;
        BucketCount = NewBucketCount;
        updateGrowthThreshold();

        for (size_type I = 0; I < OldGroupCount; ++I)
        {
            GroupAllocTraits::destroy(GA, OldGroups + I);
        }
        GroupAllocTraits::deallocate(GA, OldGroups, OldGroupCount);
        SlotAllocTraits::deallocate(SA, OldSlots, OldCount);
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

    // ------------------------------------------------------------- 所有权
    void copyFrom(const FlatHashTable& Other)
    {
        allocateStorage(Other.BucketCount == 0 ? 16 : Other.BucketCount);
        try
        {
            SlotAllocator SA{Alloc};
            for (size_type I = 0; I < Other.BucketCount; ++I)
            {
                if (!Other.isOccupiedSlot(I))
                {
                    continue;
                }
                const size_type H = hashKey(Other.Slots[I].first);
                size_type Slot = H & (BucketCount - 1);
                while (isOccupiedSlot(Slot))
                {
                    Slot = (Slot + 1) & (BucketCount - 1);
                }
                SlotAllocTraits::construct(SA, Slots + Slot, Other.Slots[I]);
                Groups[groupIndexOf(Slot)].set(slotInGroup(Slot), fingerprintOf(H));
                ++ElementCount;
            }
        }
        catch (...)
        {
            clear();
            deallocateStorage();
            throw;
        }
    }

    void stealFrom(FlatHashTable&& Other) noexcept
    {
        Slots = Other.Slots;
        Groups = Other.Groups;
        BucketCount = Other.BucketCount;
        ElementCount = Other.ElementCount;
        GrowthThreshold = Other.GrowthThreshold;
        MaxLoadFactor = Other.MaxLoadFactor;

        Other.Slots = nullptr;
        Other.Groups = nullptr;
        Other.BucketCount = 0;
        Other.ElementCount = 0;
        Other.GrowthThreshold = 0;
    }

private:
    Hash Hasher;
    KeyEqual Equal;
    Allocator Alloc;

    SlotPair* Slots = nullptr;          // 连续值存储，惰性构造
    SIMDControlByte* Groups = nullptr;  // 16 槽控制字节组
    size_type BucketCount = 0;
    size_type ElementCount = 0;
    size_type GrowthThreshold = 0;
    float MaxLoadFactor = 0.875f;
};

template <typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
void swap(FlatHashTable<Key, T, Hash, KeyEqual, Allocator>& L,
          FlatHashTable<Key, T, Hash, KeyEqual, Allocator>& R) noexcept(noexcept(L.swap(R)))
{
    L.swap(R);
}

#endif //PICTOR_FLATHASHTABLE_H
