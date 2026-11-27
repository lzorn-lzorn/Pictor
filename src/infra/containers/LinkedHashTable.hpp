/**
 * @file LinkedHashTable.hpp
 * @brief 高性能、兼容 std::unordered_map 的哈希表。
 *
 *   * 查找采用开放寻址 + 线性探测，并通过 16 槽控制字节组上的 7 位指纹过滤
 *     （Swiss-table 风格）做 SIMD 加速，使一次探测即可在不调用 KeyEqual 的
 *     前提下排除绝大多数键。
 *   * 插入顺序由侵入式双向链表保持，因此 begin()/end() 迭代为 O(1)、迭代顺序
 *     稳定，且可按迭代器 O(1) 删除而不使其它迭代器失效。
 *   * 删除采用向后移位（backward-shift deletion），因此表不会累积墓碑，始终
 *     保持稠密。
 *
 * 用户提供的哈希会经过 splitmix64/murmur3 终结器（finalizer），使表即使在
 * 2 的幂桶数下、面对弱哈希（例如 std::hash<size_t> 常见的恒等哈希）也能保持
 * 良好分布。
 *
 * 命名遵循项目约定（动词开头的小驼峰）。少数必须与标准库匹配、以便与
 * range/算法互操作的名字 —— begin/end/cbegin/cend/rbegin/rend、size/empty、
 * swap —— 保留其标准拼写。
 */

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

template<
    typename Key,
    typename T,
    typename Hash = std::hash<Key>,
    typename KeyEqual = std::equal_to<Key>,
    typename Allocator = std::allocator<std::pair<const Key, T> > >
class LinkedHashTable {
    // 节点 -----------------------------------------------------------------
    // 双向链表的链接指针放在基类子对象中，使末尾哨兵（Sentinel）无需构造
    // `std::pair<const Key, T>`。这使得表可用于键/值类型不可默认构造的情形，
    // 与 std::unordered_map 的要求一致。
    struct NodeBase {
        NodeBase *Next = nullptr;
        NodeBase *Prev = nullptr;
    };

    struct Node : NodeBase {
        template<typename KTuple, typename TTuple>
        Node(std::piecewise_construct_t, KTuple &&KArgs, TTuple &&TArgs)
            : Value(std::piecewise_construct, std::forward<KTuple>(KArgs), std::forward<TTuple>(TArgs)) {
        }

        std::pair<const Key, T> Value;
    };

    using NodeAllocator     = typename std::allocator_traits<Allocator>::template rebind_alloc<Node>;
    using NodeAllocTraits   = std::allocator_traits<NodeAllocator>;
    using BucketAllocator   = typename std::allocator_traits<NodeAllocator>::template rebind_alloc<Node *>;
    using BucketAllocTraits = std::allocator_traits<BucketAllocator>;
    using GroupAllocator    = typename std::allocator_traits<NodeAllocator>::template rebind_alloc<SIMDControlByte>;
    using GroupAllocTraits  = std::allocator_traits<GroupAllocator>;

    static constexpr size_t GroupSize = SIMDControlByte::GroupSize; // 16
    using MaskType                    = SIMDControlByte::MaskType;

public:
    using key_type        = Key;
    using mapped_type     = T;
    using value_type      = std::pair<const Key, T>;
    using size_type       = size_t;
    using difference_type = std::ptrdiff_t;
    using hasher          = Hash;
    using key_equal       = KeyEqual;
    using allocator_type  = Allocator;
    using reference       = value_type &;
    using const_reference = const value_type &;
    using pointer         = typename std::allocator_traits<Allocator>::pointer;
    using const_pointer   = typename std::allocator_traits<Allocator>::const_pointer;

    // 迭代器 --------------------------------------------------------------
    template<bool IsConst>
    class Iterator {
        friend class LinkedHashTable;
        template<bool>
        friend class Iterator;

        using BasePtr = std::conditional_t<IsConst, const NodeBase *, NodeBase *>;
        using NodePtr = std::conditional_t<IsConst, const Node *, Node *>;

        BasePtr Current = nullptr;

        explicit Iterator(BasePtr InCurrent) noexcept : Current(InCurrent) {
        }

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using iterator_concept  = std::bidirectional_iterator_tag;
        using value_type        = typename LinkedHashTable::value_type;
        using difference_type   = typename LinkedHashTable::difference_type;
        using pointer           = std::conditional_t<IsConst, const value_type *, value_type *>;
        using reference         = std::conditional_t<IsConst, const value_type &, value_type &>;

        Iterator() = default;

        template<bool OtherConst, std::enable_if_t<IsConst && !OtherConst, int> = 0>
        Iterator(const Iterator<OtherConst> &Other) noexcept : Current(Other.Current) {
        }

        reference operator*() const {
            return static_cast<NodePtr>(Current)->Value;
        }

        pointer operator->() const {
            return std::addressof(operator*());
        }

        Iterator &operator++() noexcept {
            Current = Current->Next;
            return *this;
        }

        Iterator operator++(int) noexcept {
            Iterator Tmp = *this;
            ++*this;
            return Tmp;
        }

        Iterator &operator--() noexcept {
            Current = Current->Prev;
            return *this;
        }

        Iterator operator--(int) noexcept {
            Iterator Tmp = *this;
            --*this;
            return Tmp;
        }

        // 混合 const/非 const 比较（与 std::unordered_map 互操作）。
        template<bool OtherConst>
        bool operator==(const Iterator<OtherConst> &Other) const noexcept {
            return Current == Other.Current;
        }

        template<bool OtherConst>
        bool operator!=(const Iterator<OtherConst> &Other) const noexcept {
            return !(*this == Other);
        }
    };

    using iterator               = Iterator<false>;
    using const_iterator         = Iterator<true>;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // 局部迭代器 --------------------------------------------------------
    // 迭代单个桶（home slot 为 n）的元素。开放寻址会将一个桶的元素分散到
    // 探测序列中，因此这里在全局插入顺序链表上做过滤。它是前向迭代器，与
    // std::unordered_map 对 local_iterator 的要求一致。
    template<bool IsConst>
    class LocalIterator {
        friend class LinkedHashTable;
        template<bool>
        friend class LocalIterator;

        using OwnerPtr = std::conditional_t<IsConst, const LinkedHashTable *, LinkedHashTable *>;
        using BasePtr  = std::conditional_t<IsConst, const NodeBase *, NodeBase *>;
        using NodePtr  = std::conditional_t<IsConst, const Node *, Node *>;

        OwnerPtr Owner        = nullptr;
        BasePtr Current       = nullptr;
        size_type BucketIndex = 0;

        LocalIterator(OwnerPtr InOwner, BasePtr InCurrent, size_type InBucket) noexcept
            : Owner(InOwner),
              Current(InCurrent),
              BucketIndex(InBucket) {
        }

        // 前进直到 Current 为哨兵，或指向 home 为 BucketIndex 的节点。
        void advanceToMatch() noexcept {
            while (Current != &Owner->Sentinel && Owner->bucketOfNode(static_cast<const Node *>(Current)) !=
                   BucketIndex) {
                Current = Current->Next;
            }
        }

    public:
        using iterator_category = std::forward_iterator_tag;
        using iterator_concept  = std::forward_iterator_tag;
        using value_type        = typename LinkedHashTable::value_type;
        using difference_type   = typename LinkedHashTable::difference_type;
        using pointer           = std::conditional_t<IsConst, const value_type *, value_type *>;
        using reference         = std::conditional_t<IsConst, const value_type &, value_type &>;

        LocalIterator() = default;

        template<bool OtherConst, std::enable_if_t<IsConst && !OtherConst, int> = 0>
        LocalIterator(const LocalIterator<OtherConst> &Other) noexcept
            : Owner(Other.Owner),
              Current(Other.Current),
              BucketIndex(Other.BucketIndex) {
        }

        reference operator*() const {
            return static_cast<NodePtr>(Current)->Value;
        }

        pointer operator->() const {
            return std::addressof(operator*());
        }

        LocalIterator &operator++() noexcept {
            Current = Current->Next;
            advanceToMatch();
            return *this;
        }

        LocalIterator operator++(int) noexcept {
            LocalIterator Tmp = *this;
            ++*this;
            return Tmp;
        }

        template<bool OtherConst>
        bool operator==(const LocalIterator<OtherConst> &Other) const noexcept {
            return Owner == Other.Owner && Current == Other.Current;
        }

        template<bool OtherConst>
        bool operator!=(const LocalIterator<OtherConst> &Other) const noexcept {
            return !(*this == Other);
        }
    };

    using local_iterator       = LocalIterator<false>;
    using const_local_iterator = LocalIterator<true>;

    // 构造 ---------------------------------------------------------
    LinkedHashTable()
        : LinkedHashTable(0, Hash{}, KeyEqual{}, Allocator{}) {
    }

    explicit LinkedHashTable(
        size_type BucketHint,
        const Hash &Hasher     = Hash{},
        const KeyEqual &Equal  = KeyEqual{},
        const Allocator &Alloc = Allocator{})
        : Hasher(Hasher),
          Equal(Equal),
          Alloc(Alloc) {
        Sentinel.Next = &Sentinel;
        Sentinel.Prev = &Sentinel;
        allocateBuckets(BucketHint == 0 ? 16 : BucketHint);
    }

    LinkedHashTable(const LinkedHashTable &Other)
        : Hasher(Other.Hasher),
          Equal(Other.Equal),
          Alloc(Other.Alloc) {
        Sentinel.Next = &Sentinel;
        Sentinel.Prev = &Sentinel;
        copyFrom(Other);
    }

    LinkedHashTable(LinkedHashTable &&Other) noexcept(
        std::allocator_traits<Allocator>::is_always_equal::value)
        : Hasher(std::move(Other.Hasher)),
          Equal(std::move(Other.Equal)),
          Alloc(std::move(Other.Alloc)) {
        stealFrom(std::move(Other));
    }

    ~LinkedHashTable() {
        clear();
        deallocateBuckets();
    }

    // 拷贝赋值：先拷贝目标分配器构造副本再交换进来。对 always-equal 分配器
    // （std::allocator）给出强异常保证并正确处理分配器传播；对有状态且不传播
    // 的分配器，则用自身分配器逐元素拷贝。
    LinkedHashTable &operator=(const LinkedHashTable &Other) {
        if (this == &Other) {
            return *this;
        }

        if constexpr (NodeAllocTraits::propagate_on_container_copy_assignment::value ||
                      NodeAllocTraits::is_always_equal::value) {
            LinkedHashTable Tmp(Other);
            swap(Tmp);
        }
        else {
            // 保留自身分配器；把 Other 的策略与元素拷贝进临时对象。
            LinkedHashTable Tmp(0, Other.Hasher, Other.Equal, Alloc);
            Tmp.copyFrom(Other);
            swap(Tmp);
        }
        return *this;
    }

    LinkedHashTable &operator=(LinkedHashTable &&Other)
        noexcept(std::allocator_traits<Allocator>::is_always_equal::value) {
        if (this != &Other) {
            clear();
            deallocateBuckets();
            Hasher = std::move(Other.Hasher);
            Equal  = std::move(Other.Equal);
            if constexpr (NodeAllocTraits::propagate_on_container_move_assignment::value) {
                Alloc = std::move(Other.Alloc);
            }
            stealFrom(std::move(Other));
        }
        return *this;
    }

    // 迭代器 -------------------------------------------------------------
    iterator begin() noexcept {
        return iterator(Sentinel.Next);
    }

    const_iterator begin() const noexcept {
        return const_iterator(Sentinel.Next);
    }

    const_iterator cbegin() const noexcept {
        return begin();
    }

    iterator end() noexcept {
        return iterator(&Sentinel);
    }

    const_iterator end() const noexcept {
        return const_iterator(&Sentinel);
    }

    const_iterator cend() const noexcept {
        return end();
    }

    reverse_iterator rbegin() noexcept {
        return reverse_iterator(end());
    }

    const_reverse_iterator rbegin() const noexcept {
        return const_reverse_iterator(end());
    }

    const_reverse_iterator crbegin() const noexcept {
        return rbegin();
    }

    reverse_iterator rend() noexcept {
        return reverse_iterator(begin());
    }

    const_reverse_iterator rend() const noexcept {
        return const_reverse_iterator(begin());
    }

    const_reverse_iterator crend() const noexcept {
        return rend();
    }

    // ------------------------------------------------------------- 容量
    [[nodiscard]] bool empty() const noexcept {
        return ElementCount == 0;
    }

    [[nodiscard]] size_type size() const noexcept {
        return ElementCount;
    }

    [[nodiscard]] size_type getMaxSize() const noexcept {
        return NodeAllocTraits::max_size(NodeAllocator{Alloc});
    }

    // 修改器 ------------------------------------------------------------
    void clear() noexcept {
        NodeBase *Curr = Sentinel.Next;
        while (Curr != &Sentinel) {
            NodeBase *Next = Curr->Next;
            deallocateNode(static_cast<Node *>(Curr));
            Curr = Next;
        }
        Sentinel.Next = &Sentinel;
        Sentinel.Prev = &Sentinel;
        ElementCount  = 0;
        if (Buckets) {
            resetBuckets();
        }
    }

    std::pair<iterator, bool> insert(const value_type &Value) {
        return tryEmplace(Value.first, Value.second);
    }

    std::pair<iterator, bool> insert(value_type &&Value) {
        return tryEmplace(Value.first, std::move(Value.second));
    }

    template<typename P, std::enable_if_t<std::is_constructible_v<value_type, P &&>, int> = 0>
    std::pair<iterator, bool> insert(P &&Value) {
        value_type Tmp(std::forward<P>(Value));
        return tryEmplace(Tmp.first, std::move(Tmp.second));
    }

    template<typename... Args>
    std::pair<iterator, bool> emplace(Args &&... Args_) {
        return insert(value_type(std::forward<Args>(Args_)...));
    }

    template<typename... Args>
    std::pair<iterator, bool> tryEmplace(const key_type &KeyValue, Args &&... Args_) {
        ensureCapacityForInsert();
        return tryEmplaceImpl(KeyValue, std::forward<Args>(Args_)...);
    }

    template<typename... Args>
    std::pair<iterator, bool> tryEmplace(key_type &&KeyValue, Args &&... Args_) {
        ensureCapacityForInsert();
        return tryEmplaceImpl(std::move(KeyValue), std::forward<Args>(Args_)...);
    }

    template<typename M>
    std::pair<iterator, bool> insertOrAssign(const key_type &KeyValue, M &&Obj) {
        iterator It = find(KeyValue);
        if (It != end()) {
            It->second = std::forward<M>(Obj);
            return {It, false};
        }
        return {tryEmplace(KeyValue, std::forward<M>(Obj)).first, true};
    }

    template<typename M>
    std::pair<iterator, bool> insertOrAssign(key_type &&KeyValue, M &&Obj) {
        iterator It = find(KeyValue);
        if (It != end()) {
            It->second = std::forward<M>(Obj);
            return {It, false};
        }
        return {tryEmplace(std::move(KeyValue), std::forward<M>(Obj)).first, true};
    }

    iterator erase(const_iterator Pos) {
        if (Pos == cend()) [[unlikely]] {
            return end();
        }
        Node *Target = static_cast<Node *>(const_cast<NodeBase *>(Pos.Current));
        iterator Next(Target->Next);
        eraseNode(Target);
        return Next;
    }

    size_type erase(const key_type &KeyValue) {
        Node *Found = findNode(KeyValue);
        if (!Found) [[unlikely]] {
            return 0;
        }
        eraseNode(Found);
        return 1;
    }

    iterator erase(const_iterator First, const_iterator Last) {
        while (First != Last) {
            First = erase(First);
        }
        return iterator(const_cast<NodeBase *>(Last.Current));
    }

    void swap(LinkedHashTable &Other) noexcept(
        std::allocator_traits<Allocator>::is_always_equal::value) {
        using std::swap;
        swap(Hasher, Other.Hasher);
        swap(Equal, Other.Equal);
        if constexpr (NodeAllocTraits::propagate_on_container_swap::value) {
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

    // 查找 ---------------------------------------------------------------
    iterator find(const key_type &KeyValue) {
        Node *Found = findNode(KeyValue);
        return Found ? iterator(Found) : end();
    }

    const_iterator find(const key_type &KeyValue) const {
        const Node *Found = findNode(KeyValue);
        return Found ? const_iterator(Found) : cend();
    }

    [[nodiscard]] bool contains(const key_type &KeyValue) const {
        return findNode(KeyValue) != nullptr;
    }

    mapped_type &at(const key_type &KeyValue) {
        Node *Found = findNode(KeyValue);
        if (!Found) [[unlikely]] {
            throw std::out_of_range("LinkedHashTable::at");
        }
        return Found->Value.second;
    }

    const mapped_type &at(const key_type &KeyValue) const {
        const Node *Found = findNode(KeyValue);
        if (!Found) [[unlikely]] {
            throw std::out_of_range("LinkedHashTable::at");
        }
        return Found->Value.second;
    }

    mapped_type &operator[](const key_type &KeyValue) {
        return tryEmplace(KeyValue).first->second;
    }

    mapped_type &operator[](key_type &&KeyValue) {
        return tryEmplace(std::move(KeyValue)).first->second;
    }

    // 哈希策略 ----------------------------------------------------------
    [[nodiscard]] float getLoadFactor() const noexcept {
        return BucketCount
                   ? static_cast<float>(ElementCount) / static_cast<float>(BucketCount)
                   : 0.f;
    }

    [[nodiscard]] float getMaxLoadFactor() const noexcept {
        return MaxLoadFactor;
    }

    void setMaxLoadFactor(float ML) {
        if (!(ML > 0.f && ML <= 1.f)) [[unlikely]] {
            throw std::out_of_range("LinkedHashTable::setMaxLoadFactor");
        }
        MaxLoadFactor = ML;
        updateGrowthThreshold();
    }

    void rehash(size_type Count) {
        Count = (std::max)(Count, static_cast<size_type>(static_cast<double>(ElementCount) / MaxLoadFactor) + 1);
        if (const size_type Target = roundupPowerOfTwo(Count); Target > BucketCount) {
            rehashImpl(Target);
        }
    }

    void reserve(size_type Count) {
        rehash(static_cast<size_type>(static_cast<double>(Count) / MaxLoadFactor) + 1);
    }

    [[nodiscard]] size_type getBucketCount() const noexcept {
        return BucketCount;
    }

    [[nodiscard]] size_type getMaxBucketCount() const noexcept {
        return size_type{1} << (std::numeric_limits<size_type>::digits - 1);
    }

    [[nodiscard]] size_type getBucket(const key_type &KeyValue) const {
        return hashKey(KeyValue) & (BucketCount - 1);
    }

    [[nodiscard]] size_type getBucketSize(size_type N) const {
        size_type Count = 0;
        for (const NodeBase *C = Sentinel.Next; C != &Sentinel; C = C->Next) {
            if (bucketOfNode(static_cast<const Node *>(C)) == N) {
                ++Count;
            }
        }
        return Count;
    }

    local_iterator begin(size_type N) noexcept {
        local_iterator It(this, Sentinel.Next, N);
        It.advanceToMatch();
        return It;
    }

    const_local_iterator begin(size_type N) const noexcept {
        const_local_iterator It(this, Sentinel.Next, N);
        It.advanceToMatch();
        return It;
    }

    const_local_iterator cbegin(size_type N) const noexcept {
        return begin(N);
    }

    local_iterator end(size_type N) noexcept {
        return local_iterator(this, &Sentinel, N);
    }

    const_local_iterator end(size_type N) const noexcept {
        return const_local_iterator(this, &Sentinel, N);
    }

    const_local_iterator cend(size_type N) const noexcept {
        return end(N);
    }

    [[nodiscard]] hasher getHasher() const {
        return Hasher;
    }

    [[nodiscard]] key_equal getKeyEqual() const {
        return Equal;
    }

    [[nodiscard]] allocator_type getAllocator() const noexcept {
        return Alloc;
    }

private:
    // --------------------------------------------------------------- 哈希
    // 终结器扩散熵，使弱哈希（恒等、步进等）在 2 的幂桶数下仍能良好分布。
    static constexpr size_type mixHash(size_type X) noexcept {
        if constexpr (sizeof(size_type) == 8) {
            X += 0x9e3779b97f4a7c15ULL; // splitmix64
            X = (X ^ (X >> 30)) * 0xbf58476d1ce4e5b9ULL;
            X = (X ^ (X >> 27)) * 0x94d049bb133111ebULL;
            return X ^ (X >> 31);
        }
        else {
            X ^= X >> 16; // murmur3 fmix32
            X *= 0x85ebca6bU;
            X ^= X >> 13;
            X *= 0xc2b2ae35U;
            X ^= X >> 16;
            return X;
        }
    }

    [[nodiscard]] size_type hashKey(const key_type &KeyValue) const {
        return mixHash(Hasher(KeyValue));
    }

    // 取（混合后）哈希的高 7 位：0..127，永不为 0xFF 空标记。
    static constexpr std::uint8_t fingerprintOf(size_type HashValue) noexcept {
        return static_cast<std::uint8_t>(HashValue >> (sizeof(size_type) * 8 - 7));
    }

    // 存活节点的 home slot（供 getBucketSize / 桶迭代器使用）。
    [[nodiscard]] size_type bucketOfNode(const Node *N) const {
        return hashKey(N->Value.first) & (BucketCount - 1);
    }

    // 16 槽组内位于 `From` 及其之后的控制字节槽位掩码。
    static constexpr MaskType tailMask(size_t From) noexcept {
        return static_cast<MaskType>(~((MaskType{1} << From) - 1));
    }

    static constexpr size_type roundupPowerOfTwo(size_type Value) noexcept {
        if (Value <= 16) {
            return 16;
        }
        constexpr size_type Top = size_type{1} << (std::numeric_limits<size_type>::digits - 1);
        if (Value > Top) {
            return Top;
        }
        size_type P = 16;
        while (P < Value) {
            P <<= 1;
        }
        return P;
    }

    [[nodiscard]] size_type groupIndexOf(size_type SlotIndex) const noexcept {
        return SlotIndex / GroupSize;
    }

    [[nodiscard]] size_type slotInGroup(size_type SlotIndex) const noexcept {
        return SlotIndex % GroupSize;
    }

    // ------------------------------------------------------------ 分配
    void allocateBuckets(size_type Count) {
        BucketCount                = roundupPowerOfTwo(Count);
        const size_type GroupCount = BucketCount / GroupSize;

        BucketAllocator BucketAlloc{Alloc};
        GroupAllocator GroupAlloc{Alloc};

        Node **NewBuckets          = BucketAllocTraits::allocate(BucketAlloc, BucketCount);
        SIMDControlByte *NewGroups = nullptr;
        try {
            std::fill_n(NewBuckets, BucketCount, static_cast<Node *>(nullptr));
            NewGroups = GroupAllocTraits::allocate(GroupAlloc, GroupCount);
            for (size_type I = 0; I < GroupCount; ++I) {
                GroupAllocTraits::construct(GroupAlloc, NewGroups + I); // 默认构造 => 空
            }
        }
        catch (...) {
            if (NewGroups) {
                GroupAllocTraits::deallocate(GroupAlloc, NewGroups, GroupCount);
            }
            BucketAllocTraits::deallocate(BucketAlloc, NewBuckets, BucketCount);
            throw;
        }

        Buckets = NewBuckets;
        Groups  = NewGroups;
        updateGrowthThreshold();
    }

    void deallocateBuckets() noexcept {
        if (Buckets) {
            BucketAllocator BucketAlloc{Alloc};
            BucketAllocTraits::deallocate(BucketAlloc, Buckets, BucketCount);
            Buckets = nullptr;
        }
        if (Groups) {
            GroupAllocator GroupAlloc{Alloc};
            const size_type GroupCount = BucketCount / GroupSize;
            for (size_type I = 0; I < GroupCount; ++I) {
                GroupAllocTraits::destroy(GroupAlloc, Groups + I);
            }
            GroupAllocTraits::deallocate(GroupAlloc, Groups, GroupCount);
            Groups = nullptr;
        }
    }

    void resetBuckets() noexcept {
        std::fill_n(Buckets, BucketCount, static_cast<Node *>(nullptr));
        for (size_type I = 0; I < BucketCount / GroupSize; ++I) {
            Groups[I] = SIMDControlByte{};
        }
    }

    void updateGrowthThreshold() noexcept {
        GrowthThreshold = static_cast<size_type>(static_cast<double>(BucketCount) * MaxLoadFactor);
        // 始终至少保留一个空槽，以保证探测能终止。
        if (GrowthThreshold >= BucketCount) {
            GrowthThreshold = BucketCount - 1;
        }
    }

    // -------------------------------------------------------------- 查找/插入
    Node *findNode(const key_type &KeyValue) const {
        const size_type H          = hashKey(KeyValue);
        const auto FP              = fingerprintOf(H);
        const size_type Mask       = BucketCount - 1;
        const size_type GroupCount = BucketCount / GroupSize;
        size_type Group            = (H & Mask) / GroupSize;
        size_t Start               = (H & Mask) % GroupSize;

        for (size_type Probe = 0; Probe <= GroupCount; ++Probe) {
            const SIMDControlByte &G = Groups[Group];
            const MaskType Tail      = tailMask(Start);

            MaskType Match = G.matchFingerprint(FP) & Tail;
            while (Match) {
                const auto Bit       = static_cast<size_t>(std::countr_zero(Match));
                const size_type Slot = Group * GroupSize + Bit;
                Node *Candidate      = Buckets[Slot];
                if (Candidate && Equal(Candidate->Value.first, KeyValue)) [[likely]] {
                    return Candidate;
                }
                Match &= static_cast<MaskType>(Match - 1);
            }

            if (G.matchEmpty() & Tail) [[unlikely]] {
                break;
            }

            Group = (Group + 1) & (GroupCount - 1);
            Start = 0;
        }
        return nullptr;
    }

    template<typename K, typename... Args>
    std::pair<iterator, bool> tryEmplaceImpl(K &&KeyValue, Args &&... Args_) {
        const size_type H          = hashKey(KeyValue);
        const auto FP              = fingerprintOf(H);
        const size_type Mask       = BucketCount - 1;
        const size_type GroupCount = BucketCount / GroupSize;
        size_type Group            = (H & Mask) / GroupSize;
        size_t Start               = (H & Mask) % GroupSize;

        for (size_type Probe = 0; Probe <= GroupCount; ++Probe) {
            SIMDControlByte &G  = Groups[Group];
            const MaskType Tail = tailMask(Start);

            MaskType Match = G.matchFingerprint(FP) & Tail;
            while (Match) {
                const auto Bit       = static_cast<size_t>(std::countr_zero(Match));
                const size_type Slot = Group * GroupSize + Bit;
                Node *Candidate      = Buckets[Slot];
                if (Candidate && Equal(Candidate->Value.first, KeyValue)) {
                    return {iterator{Candidate}, false};
                }
                Match &= static_cast<MaskType>(Match - 1);
            }

            if (const MaskType EmptyMask = G.matchEmpty() & Tail) {
                const auto Bit       = static_cast<size_t>(std::countr_zero(EmptyMask));
                const size_type Slot = Group * GroupSize + Bit;
                Node *NewNode        = allocateNode(std::forward<K>(KeyValue), std::forward<Args>(Args_)...);
                Buckets[Slot]        = NewNode;
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

    // --------------------------------------------------------------- 删除
    // 向后移位删除：无需墓碑即可保持探测簇连续。对每个后继元素重新计算其
    // home；仅当其在循环探测序上位于空洞处或之前时，才将其移入空洞。
    void eraseFromBuckets(Node *Target) {
        const size_type Mask = BucketCount - 1;
        const size_type H    = hashKey(Target->Value.first);
        size_type Slot       = H & Mask;
        while (Buckets[Slot] != Target) {
            Slot = (Slot + 1) & Mask;
        }

        size_type I = Slot;
        size_type J = (Slot + 1) & Mask;
        while (Buckets[J] != nullptr) {
            const size_type JH   = hashKey(Buckets[J]->Value.first);
            const size_type Home = JH & Mask;
            if (((I - Home) & Mask) < ((J - Home) & Mask)) {
                Buckets[I] = Buckets[J];
                Groups[groupIndexOf(I)].set(slotInGroup(I), fingerprintOf(JH));
                I = J;
            }
            J = (J + 1) & Mask;
        }
        Buckets[I] = nullptr;
        Groups[groupIndexOf(I)].set(slotInGroup(I), SIMDControlByte::EmptyByte);
    }

    void eraseNode(Node *Target) {
        eraseFromBuckets(Target);
        unlink(Target);
        deallocateNode(Target);
        --ElementCount;
    }

    // ---------------------------------------------------------------- 重哈希
    void rehashImpl(size_type NewBucketCount) {
        NewBucketCount                = roundupPowerOfTwo(NewBucketCount);
        const size_type NewGroupCount = NewBucketCount / GroupSize;

        BucketAllocator BucketAlloc{Alloc};
        GroupAllocator GroupAlloc{Alloc};

        Node **NewBuckets          = BucketAllocTraits::allocate(BucketAlloc, NewBucketCount);
        SIMDControlByte *NewGroups = nullptr;
        try {
            std::fill_n(NewBuckets, NewBucketCount, static_cast<Node *>(nullptr));
            NewGroups = GroupAllocTraits::allocate(GroupAlloc, NewGroupCount);
            for (size_type I = 0; I < NewGroupCount; ++I) {
                GroupAllocTraits::construct(GroupAlloc, NewGroups + I);
            }
        }
        catch (...) {
            if (NewGroups) {
                GroupAllocTraits::deallocate(GroupAlloc, NewGroups, NewGroupCount);
            }
            BucketAllocTraits::deallocate(BucketAlloc, NewBuckets, NewBucketCount);
            throw;
        }

        try {
            const size_type NewMask = NewBucketCount - 1;
            NodeBase *Curr          = Sentinel.Next;
            while (Curr != &Sentinel) {
                Node *N           = static_cast<Node *>(Curr);
                const size_type H = hashKey(N->Value.first);
                size_type Slot    = H & NewMask;
                while (NewBuckets[Slot] != nullptr) {
                    Slot = (Slot + 1) & NewMask;
                }
                NewBuckets[Slot] = N;
                NewGroups[Slot / GroupSize].set(Slot % GroupSize, fingerprintOf(H));
                Curr = Curr->Next;
            }
        }
        catch (...) {
            for (size_type I = 0; I < NewGroupCount; ++I) {
                GroupAllocTraits::destroy(GroupAlloc, NewGroups + I);
            }
            GroupAllocTraits::deallocate(GroupAlloc, NewGroups, NewGroupCount);
            BucketAllocTraits::deallocate(BucketAlloc, NewBuckets, NewBucketCount);
            throw;
        }

        // 提交。
        Node **OldBuckets             = Buckets;
        SIMDControlByte *OldGroups    = Groups;
        const size_type OldCount      = BucketCount;
        const size_type OldGroupCount = OldCount / GroupSize;

        Buckets     = NewBuckets;
        Groups      = NewGroups;
        BucketCount = NewBucketCount;
        updateGrowthThreshold();

        for (size_type I = 0; I < OldGroupCount; ++I) {
            GroupAllocTraits::destroy(GroupAlloc, OldGroups + I);
        }
        GroupAllocTraits::deallocate(GroupAlloc, OldGroups, OldGroupCount);
        BucketAllocTraits::deallocate(BucketAlloc, OldBuckets, OldCount);
    }

    void ensureCapacityForInsert() {
        if (ElementCount + 1 > GrowthThreshold) [[unlikely]] {
            rehashImpl(BucketCount >= (std::numeric_limits<size_type>::max() >> 1)
                           ? BucketCount
                           : BucketCount * 2);
        }
    }

    // ------------------------------------------------------------ 节点分配
    template<typename K, typename... Args>
    Node *allocateNode(K &&KeyValue, Args &&... Args_) {
        NodeAllocator NodeAlloc{Alloc};
        Node *P = NodeAllocTraits::allocate(NodeAlloc, 1);
        try {
            NodeAllocTraits::construct(
                                       NodeAlloc,
                                       P,
                                       std::piecewise_construct,
                                       std::forward_as_tuple(std::forward<K>(KeyValue)),
                                       std::forward_as_tuple(std::forward<Args>(Args_)...));
        }
        catch (...) {
            NodeAllocTraits::deallocate(NodeAlloc, P, 1);
            throw;
        }
        return P;
    }

    void deallocateNode(Node *P) noexcept {
        NodeAllocator NodeAlloc{Alloc};
        NodeAllocTraits::destroy(NodeAlloc, P);
        NodeAllocTraits::deallocate(NodeAlloc, P, 1);
    }

    // 链表操作 --------------------------------------------------------
    void linkTail(NodeBase *Target) noexcept {
        Target->Prev        = Sentinel.Prev;
        Target->Next        = &Sentinel;
        Sentinel.Prev->Next = Target;
        Sentinel.Prev       = Target;
    }

    void unlink(NodeBase *Target) noexcept {
        Target->Prev->Next = Target->Next;
        Target->Next->Prev = Target->Prev;
        Target->Next       = nullptr;
        Target->Prev       = nullptr;
    }

    // 所有权 -------------------------------------------------------------
    void copyFrom(const LinkedHashTable &Other) {
        allocateBuckets(Other.BucketCount == 0 ? 16 : Other.BucketCount);
        try {
            const NodeBase *Curr = Other.Sentinel.Next;
            while (Curr != &Other.Sentinel) {
                const Node *Src   = static_cast<const Node *>(Curr);
                const size_type H = hashKey(Src->Value.first);
                Node *NewNode     = allocateNode(Src->Value.first, Src->Value.second);
                size_type Slot    = H & (BucketCount - 1);
                while (Buckets[Slot] != nullptr) {
                    Slot = (Slot + 1) & (BucketCount - 1);
                }
                Buckets[Slot] = NewNode;
                Groups[groupIndexOf(Slot)].set(slotInGroup(Slot), fingerprintOf(H));
                linkTail(NewNode);
                ++ElementCount;
                Curr = Curr->Next;
            }
        }
        catch (...) {
            clear();
            deallocateBuckets();
            throw;
        }
    }

    void stealFrom(LinkedHashTable &&Other) noexcept {
        Buckets         = Other.Buckets;
        Groups          = Other.Groups;
        BucketCount     = Other.BucketCount;
        ElementCount    = Other.ElementCount;
        GrowthThreshold = Other.GrowthThreshold;
        MaxLoadFactor   = Other.MaxLoadFactor;

        if (Other.ElementCount == 0) {
            Sentinel.Next = &Sentinel;
            Sentinel.Prev = &Sentinel;
        }
        else {
            Sentinel.Next       = Other.Sentinel.Next;
            Sentinel.Prev       = Other.Sentinel.Prev;
            Sentinel.Next->Prev = &Sentinel;
            Sentinel.Prev->Next = &Sentinel;
        }

        Other.Buckets         = nullptr;
        Other.Groups          = nullptr;
        Other.BucketCount     = 0;
        Other.ElementCount    = 0;
        Other.GrowthThreshold = 0;
        Other.Sentinel.Next   = &Other.Sentinel;
        Other.Sentinel.Prev   = &Other.Sentinel;
    }

    // `swap` 交换了数组指针与计数器，但每个对象的 Sentinel 是固定子对象，
    // 因此链表必须重新链接到各自新的所有者。
    void relinkAfterSwap(LinkedHashTable &Other) noexcept {
        NodeBase *const AHead = Sentinel.Next;
        NodeBase *const ATail = Sentinel.Prev;
        NodeBase *const BHead = Other.Sentinel.Next;
        NodeBase *const BTail = Other.Sentinel.Prev;

        if (BHead == &Other.Sentinel) {
            Sentinel.Next = &Sentinel;
            Sentinel.Prev = &Sentinel;
        }
        else {
            Sentinel.Next = BHead;
            Sentinel.Prev = BTail;
            BHead->Prev   = &Sentinel;
            BTail->Next   = &Sentinel;
        }

        if (AHead == &Sentinel) {
            Other.Sentinel.Next = &Other.Sentinel;
            Other.Sentinel.Prev = &Other.Sentinel;
        }
        else {
            Other.Sentinel.Next = AHead;
            Other.Sentinel.Prev = ATail;
            AHead->Prev         = &Other.Sentinel;
            ATail->Next         = &Other.Sentinel;
        }
    }

private:
    Hash Hasher;
    KeyEqual Equal;
    Allocator Alloc;

    NodeBase Sentinel; // 末尾哨兵；仅携带链接指针，无值
    Node **Buckets            = nullptr;
    SIMDControlByte *Groups   = nullptr;
    size_type BucketCount     = 0;
    size_type ElementCount    = 0;
    size_type GrowthThreshold = 0;
    float MaxLoadFactor       = 0.875f;
};

template<typename Key, typename T, typename Hash, typename KeyEqual, typename Allocator>
void swap(LinkedHashTable<Key, T, Hash, KeyEqual, Allocator> &L,
          LinkedHashTable<Key, T, Hash, KeyEqual, Allocator> &R) noexcept(noexcept(L.swap(R))) {
    L.swap(R);
}

#endif //PICTOR_LINKEDHASHTABLE_H
