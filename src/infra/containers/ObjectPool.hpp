#ifndef OBJECTPOOL_OBJECTPOOL_HPP
#define OBJECTPOOL_OBJECTPOOL_HPP

/**
 * @brief 分配器感知的对象池，具备标准库容器的语义，header-only 实现。
 *
 * @notice 设计要点
 * - 元素存放在"槽位(slot)"中。槽位要么被占用（持有有效值），要么空闲（可复用）。
 *   删除元素仅标记其槽位为空闲，不会搬移其余元素，因此 erase 为 O(1)，
 *   且其它元素的引用/指针保持有效。重新插入（emplaceBack/pushBack/append）
 *   会优先复用空闲槽位，再增长底层存储。
 * - 迭代器按物理槽位顺序遍历被占用的槽位，因此是双向迭代器（非随机访问）：
 *   空洞使 O(1) 随机访问在无额外索引表的前提下无法实现。
 * - 对于平凡可拷贝类型（int/double/bool/POD 等），槽位退化为纯对齐存储，
 *   占用标志移入独立的位图（每槽 1 bit），避免小对象的 bool 标志与对齐填充
 *   带来的翻倍开销。
 * - 最低要求 C++11；当 C++17/C++20 库设施可用时（std::launder、std::construct_at、
 *   std::destroy_at、[[no_unique_address]]）会自动启用。
 *
 * @notice 命名约定（LLVM 风格）
 * - 修改器方法采用动词开头小驼峰：pushBack / emplaceBack / popBack / shrinkToFit /
 *   append / clear / reserve / resize / assign / erase / getFreeCount 等。
 * - 标准算法与泛型代码依赖的"容器概念接口"保留标准名：begin/end/cbegin/cend/
 *   rbegin/rend/crbegin/crend、size、empty、swap、get_allocator、max_size、
 *   operator[]、比较运算符，以及 value_type/allocator_type 等 typedef。
 *
 * @notice 底层容器要求
 * - 主模板要求「可增长 + 随机访问」的序列容器（std::vector、std::deque）。
 *   std::list 由偏特化提供（节点型、迭代器稳定）。
 *   std::array 为定长容器，不受支持（编译期 static_assert 给出提示）。
 * - 可通过模板模板参数 Container 传入满足上述要求的其它容器。
 *
 * @notice 分配器要求
 * - 分配器需满足标准 Allocator 要求：value_type、allocate(n)、deallocate(p, n)、
 *   operator==、operator!=、rebind<U>（可重绑定到 SlotType/size_type 等内部类型）。
 * - 可选提供传播 traits（propagate_on_container_{copy,move}_assignment、
 *   propagate_on_container_swap、is_always_equal），本实现经 std::allocator_traits
 *   统一处理，有状态与无状态分配器均可使用。
 * - 注：紧凑存储的占用位图使用默认 std::allocator<bool>（内部元数据）。
 *
 * @notice Key-Value 用法
 * 本池是值容器，不内建映射。推荐以组合方式实现 Key -> SlotId -> Value：
 *   ObjectPool<V> Pool;
 *   std::unordered_map<K, size_type> Index;
 *   Index[key] = Pool.getSlotIndex(Pool.append(value)); // 写入并记录槽位下标
 *   Pool.erase(Pool.getIterator(Index[key]));           // 按下标删除
 *   详情见 main.cpp 中的 testKeyValuePool 示例。
 *
 * @notice 分阶段刷新用法（预申请 -> 批量刷入 -> 刷掉 -> 再刷入，均不触发池内存分配）
 *   ObjectPool<LargeObject> Pool;
 *   Pool.reserve(N);                    // 预申请 N 个槽位，不执行 LargeObject 构造
 *   Pool.fill(first, last);             // 一次性批量刷入（复用空闲槽位，不分配）
 *   Pool.clear();                       // 刷掉旧数据（销毁对象，但不回收内存）
 *   Pool.fill(first2, last2);           // 刷入新数据（复用同一批槽位，不分配）
 *   稳定句柄：连续存储用 getSlotIndex 得到物理下标（重分配后下标不变）；
 *   std::list 特化直接用迭代器（节点型、天然稳定）。详情见 testFillRefresh 示例。
 *
 * @notice 复杂度（主模板 / std::list 特化）
 * - size / empty / getCapacity                 : O(1)
 * - emplaceBack / pushBack / append（复用）    : 均摊 O(1)
 * - erase（单个或区间）                         : O(1) / O(区间)，不搬移元素
 * - getSlotIndex / getIterator                 : O(1)（仅主模板）
 * - getFront / getBack / operator[] / at       : O(k)，k 为前导/尾随空洞数
 * - clear                                      : O(capacity)
 * - shrinkToFit                                : O(capacity) 压缩
 */

#include <algorithm>
#include <array>
#include <atomic>
#include <bitset>
#include <cstddef>
#include <deque>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

/**
 * @brief 特性探测。MSVC 提供 std::construct_at / std::destroy_at 却不定义其
 *        __cpp_lib_* 特性宏，因此同时探测生效的语言标准。
 * @notice _MSVC_LANG 在 MSVC 上始终正确，与 /Zc:__cplusplus 设置无关。
 */
#if defined(_MSVC_LANG)
#  define OBJECTPOOL_LANGUAGE_LEVEL _MSVC_LANG
#else
#  define OBJECTPOOL_LANGUAGE_LEVEL __cplusplus
#endif

#if OBJECTPOOL_LANGUAGE_LEVEL >= 201703L
#  define OBJECTPOOL_HAS_CPP17 1
#endif

#if OBJECTPOOL_LANGUAGE_LEVEL >= 202002L
#  define OBJECTPOOL_HAS_CPP20 1
#endif

namespace ObjectPoolDetail {

/**
 * @brief 单值的对齐原始存储。
 * @notice 避免使用 C++23 中已废弃的 std::aligned_storage，同时保持 C++11 兼容。
 */
template <typename Type>
struct AlignedStorage {
    alignas(Type) unsigned char Bytes[sizeof(Type)];
};

// @brief 取得存储的原始指针（未 launder），用于 placement 构造。
template <typename Type>
Type* getRawStoragePointer(AlignedStorage<Type>& Storage) {
    return reinterpret_cast<Type*>(Storage.Bytes);
}

// @brief 取得存储的原始指针（未 launder）的 const 重载。
template <typename Type>
Type const* getRawStoragePointer(AlignedStorage<Type> const& Storage) {
    return reinterpret_cast<Type const*>(Storage.Bytes);
}

// @brief 取得存活对象的指针。
// @notice 可用时（C++17）应用 std::launder。
template <typename Type>
Type* getObjectPointer(AlignedStorage<Type>& Storage) {
#if defined(__cpp_lib_launder) || defined(OBJECTPOOL_HAS_CPP17)
    return std::launder(getRawStoragePointer(Storage));
#else
    return getRawStoragePointer(Storage);
#endif
}

// @brief 取得存活对象的指针的 const 重载。
template <typename Type>
Type const* getObjectPointer(AlignedStorage<Type> const& Storage) {
#if defined(__cpp_lib_launder) || defined(OBJECTPOOL_HAS_CPP17)
    return std::launder(getRawStoragePointer(Storage));
#else
    return getRawStoragePointer(Storage);
#endif
}

// @brief 在存储中原地构造值，转发参数。
// @param Storage 目标存储
// @param Arguments 转发给 Type 构造函数的参数
// @return 构造出的对象指针
template <typename Type, typename... Args>
Type* constructInStorage(AlignedStorage<Type>& Storage, Args&&... Arguments) {
#if defined(__cpp_lib_construct_at) || defined(OBJECTPOOL_HAS_CPP20)
    return std::construct_at(getRawStoragePointer(Storage),
                             std::forward<Args>(Arguments)...);
#else
    return ::new (static_cast<void*>(getRawStoragePointer(Storage)))
        Type(std::forward<Args>(Arguments)...);
#endif
}

// @brief 销毁存储中的值。
template <typename Type>
void destroyInStorage(AlignedStorage<Type>& Storage) {
#if defined(__cpp_lib_destroy_at) || defined(OBJECTPOOL_HAS_CPP17)
    std::destroy_at(getObjectPointer(Storage));
#else
    getObjectPointer(Storage)->~Type();
#endif
}

/**
 * @brief 非平凡类型的槽位：对齐存储 + 占用标志。
 * @notice 特殊成员函数拥有所存值的生命周期，使底层容器可以安全地
 *         重分配/移动/删除槽位，而无需我们管理其原始内存。
 */
template <typename Type>
struct Slot {
    AlignedStorage<Type> Storage;
    bool bOccupied;

    Slot() : Storage(), bOccupied(false) {}

    Slot(Slot const& Other) : Storage(), bOccupied(false) {
        if (Other.bOccupied) {
            constructInStorage(Storage, *getObjectPointer(Other.Storage));
            bOccupied = true;
        }
    }

    Slot(Slot&& Other)
        noexcept(std::is_nothrow_move_constructible<Type>::value)
        : Storage(), bOccupied(false) {
        if (Other.bOccupied) {
            constructInStorage(Storage,
                               std::move(*getObjectPointer(Other.Storage)));
            bOccupied = true;
            // @notice 源即将被遗弃（如 vector 重分配后释放旧缓冲），
            //         因此销毁其移动后的值。
            destroyInStorage(Other.Storage);
            Other.bOccupied = false;
        }
    }

    ~Slot() {
        if (bOccupied)
            destroyInStorage(Storage);
    }

    Slot& operator=(Slot const& Other) {
        if (this != &Other) {
            if (bOccupied) {
                destroyInStorage(Storage);
                bOccupied = false;
            }
            if (Other.bOccupied) {
                constructInStorage(Storage, *getObjectPointer(Other.Storage));
                bOccupied = true;
            }
        }
        return *this;
    }

    Slot& operator=(Slot&& Other)
        noexcept(std::is_nothrow_move_constructible<Type>::value) {
        if (this != &Other) {
            if (bOccupied) {
                destroyInStorage(Storage);
                bOccupied = false;
            }
            if (Other.bOccupied) {
                constructInStorage(Storage,
                                   std::move(*getObjectPointer(Other.Storage)));
                bOccupied = true;
                destroyInStorage(Other.Storage);
                Other.bOccupied = false;
            }
        }
        return *this;
    }
};

/**
 * @brief 平凡可拷贝类型的紧凑槽位：仅纯对齐存储，占用标志外置于位图。
 * @notice 该类型平凡可拷贝，容器重分配时的按位复制天然安全。
 */
template <typename Type>
struct CompactSlot {
    AlignedStorage<Type> Storage;
};

/**
 * @brief 依据 Type 是否为平凡可拷贝类型选择槽位表示。
 */
template <typename Type>
struct SlotSelector {
    using type = typename std::conditional<
        std::is_trivially_copyable<Type>::value,
        CompactSlot<Type>,
        Slot<Type>>::type;
};

/**
 * @brief 检测容器是否提供 shrink_to_fit。
 * @notice std::vector 提供，std::deque 不提供。
 */
template <typename Container, typename = void>
struct HasShrinkToFit : std::false_type {};

template <typename Container>
struct HasShrinkToFit<
    Container,
    decltype(void(std::declval<Container&>().shrink_to_fit()))>
    : std::true_type {};

/**
 * @brief 检测容器是否可增长（提供 emplace_back）。
 * @notice 用于在编译期识别 std::array 等定长容器并给出明确提示。
 */
template <typename Container, typename = void>
struct HasEmplaceBack : std::false_type {};

template <typename Container>
struct HasEmplaceBack<
    Container,
    decltype(void(std::declval<Container&>().emplace_back()))>
    : std::true_type {};

} // namespace ObjectPoolDetail

/**
 * @brief 主模板：使用随机访问容器（std::vector、std::deque、std::array 等）
 *        作为底层存储，槽位以下标寻址。
 */
template <typename Type,
          typename Allocator = std::allocator<Type>,
          template <typename, typename> class Container = std::vector>
class ObjectPool {
private:
    using SlotType = typename ObjectPoolDetail::SlotSelector<Type>::type;
    using SlotAllocator =
        typename std::allocator_traits<Allocator>::template rebind_alloc<SlotType>;

    using PoolTraits = std::allocator_traits<Allocator>;
    using IsTrivialType = std::is_trivially_copyable<Type>;

public:
    using allocator_type = Allocator;
    using value_type = Type;
    using container_type = Container<SlotType, SlotAllocator>;
    using size_type = typename container_type::size_type;
    using difference_type = typename container_type::difference_type;
    using reference = value_type&;
    using const_reference = value_type const&;
    using pointer = value_type*;
    using const_pointer = value_type const*;

    class const_iterator;
    class iterator;

    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // @notice 主模板要求随机访问序列容器。
    static_assert(
        std::is_same<
            typename container_type::iterator::iterator_category,
            std::random_access_iterator_tag>::value,
        "ObjectPool 主模板要求随机访问序列容器（std::vector、std::deque）。"
        "节点型容器请使用 std::list 偏特化。");
    // @notice 主模板要求可增长容器（std::array 为定长容器，不受支持）。
    static_assert(
        ObjectPoolDetail::HasEmplaceBack<container_type>::value,
        "ObjectPool 主模板要求可增长的容器；std::array 为定长容器，不受支持。"
        "请改用 std::vector/std::deque，或使用专用的定长对象池。");

private:
    using FreeListAllocator =
        typename PoolTraits::template rebind_alloc<size_type>;
    using FreeListType = std::vector<size_type, FreeListAllocator>;

    using PropagateOnCopyAssignment =
        typename PoolTraits::propagate_on_container_copy_assignment;
    using PropagateOnMoveAssignment =
        typename PoolTraits::propagate_on_container_move_assignment;

    // ------------------------------------------------------------------ //
    // 迭代器（双向；跳过空闲槽位）。                                      //
    // ------------------------------------------------------------------ //
public:
    // @brief 可变元素的双向迭代器。
    class iterator {
        friend class ObjectPool;
        friend class const_iterator;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = ObjectPool::value_type;
        using difference_type = ObjectPool::difference_type;
        using pointer = ObjectPool::pointer;
        using reference = ObjectPool::reference;

        iterator() : Owner(nullptr), Index(0) {}

        reference operator*() const {
            return *ObjectPoolDetail::getObjectPointer(Owner->Slots[Index].Storage);
        }

        pointer operator->() const {
            return ObjectPoolDetail::getObjectPointer(Owner->Slots[Index].Storage);
        }

        iterator& operator++() {
            advanceToNextOccupied();
            return *this;
        }

        iterator operator++(int) {
            iterator Temp(*this);
            ++*this;
            return Temp;
        }

        iterator& operator--() {
            retreatToPreviousOccupied();
            return *this;
        }

        iterator operator--(int) {
            iterator Temp(*this);
            --*this;
            return Temp;
        }

        bool operator==(iterator const& Rhs) const {
            return Owner == Rhs.Owner && Index == Rhs.Index;
        }
        bool operator!=(iterator const& Rhs) const { return !(*this == Rhs); }

        // @brief 与 const_iterator 的交叉比较（const_iterator 完整后于类外定义）。
        bool operator==(const_iterator const& Rhs) const;
        bool operator!=(const_iterator const& Rhs) const;

    private:
        iterator(ObjectPool* InOwner, size_type InIndex)
            : Owner(InOwner), Index(InIndex) {}

        void advanceToNextOccupied() {
            do {
                ++Index;
            } while (Index < Owner->Slots.size() && !Owner->isOccupied(Index));
        }

        void retreatToPreviousOccupied() {
            do {
                --Index;
            } while (Index != static_cast<size_type>(-1) &&
                     !Owner->isOccupied(Index));
        }

        ObjectPool* Owner;
        size_type Index;
    };

    // @brief 只读元素的双向迭代器。
    class const_iterator {
        friend class ObjectPool;
        friend class iterator;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = ObjectPool::value_type;
        using difference_type = ObjectPool::difference_type;
        using pointer = ObjectPool::const_pointer;
        using reference = ObjectPool::const_reference;

        const_iterator() : Owner(nullptr), Index(0) {}

        // @brief 由可变迭代器隐式转换（iterator 完整后于类外定义）。
        const_iterator(iterator const& Other);

        reference operator*() const {
            return *ObjectPoolDetail::getObjectPointer(Owner->Slots[Index].Storage);
        }

        pointer operator->() const {
            return ObjectPoolDetail::getObjectPointer(Owner->Slots[Index].Storage);
        }

        const_iterator& operator++() {
            advanceToNextOccupied();
            return *this;
        }

        const_iterator operator++(int) {
            const_iterator Temp(*this);
            ++*this;
            return Temp;
        }

        const_iterator& operator--() {
            retreatToPreviousOccupied();
            return *this;
        }

        const_iterator operator--(int) {
            const_iterator Temp(*this);
            --*this;
            return Temp;
        }

        bool operator==(const_iterator const& Rhs) const {
            return Owner == Rhs.Owner && Index == Rhs.Index;
        }
        bool operator!=(const_iterator const& Rhs) const { return !(*this == Rhs); }
        bool operator==(iterator const& Rhs) const {
            return Owner == Rhs.Owner && Index == Rhs.Index;
        }
        bool operator!=(iterator const& Rhs) const { return !(*this == Rhs); }

    private:
        const_iterator(ObjectPool const* InOwner, size_type InIndex)
            : Owner(InOwner), Index(InIndex) {}

        void advanceToNextOccupied() {
            do {
                ++Index;
            } while (Index < Owner->Slots.size() && !Owner->isOccupied(Index));
        }

        void retreatToPreviousOccupied() {
            do {
                --Index;
            } while (Index != static_cast<size_type>(-1) &&
                     !Owner->isOccupied(Index));
        }

        ObjectPool const* Owner;
        size_type Index;
    };

    // ------------------------------------------------------------------ //
    // 构造 / 析构 / 赋值。                                                 //
    // ------------------------------------------------------------------ //
public:
    ObjectPool()
        : PoolAllocator(Allocator()),
          Slots(SlotAllocator(PoolAllocator)),
          FreeSlots(FreeListAllocator(PoolAllocator)),
          LiveSize(0) {}

    explicit ObjectPool(Allocator const& Alloc)
        : PoolAllocator(Alloc),
          Slots(SlotAllocator(PoolAllocator)),
          FreeSlots(FreeListAllocator(PoolAllocator)),
          LiveSize(0) {}

    // @brief 构造 Count 个默认插入的元素。
    explicit ObjectPool(size_type Count)
        : ObjectPool(Allocator()) {
        for (size_type I = 0; I < Count; ++I)
            emplaceBack();
    }

    // @brief 构造 Count 个 Value 的副本。
    ObjectPool(size_type Count, const_reference Value)
        : ObjectPool(Allocator()) {
        for (size_type I = 0; I < Count; ++I)
            emplaceBack(Value);
    }

    ObjectPool(size_type Count, const_reference Value, Allocator const& Alloc)
        : ObjectPool(Alloc) {
        for (size_type I = 0; I < Count; ++I)
            emplaceBack(Value);
    }

    // @brief 由区间 [First, Last) 构造。
    template <typename InputIterator,
              typename = typename std::enable_if<
                  !std::is_integral<InputIterator>::value>::type>
    ObjectPool(InputIterator First, InputIterator Last)
        : ObjectPool(Allocator()) {
        appendRange(First, Last);
    }

    template <typename InputIterator,
              typename = typename std::enable_if<
                  !std::is_integral<InputIterator>::value>::type>
    ObjectPool(InputIterator First, InputIterator Last, Allocator const& Alloc)
        : ObjectPool(Alloc) {
        appendRange(First, Last);
    }

    // @brief 由初始化列表构造。
    ObjectPool(std::initializer_list<value_type> Init)
        : ObjectPool(Allocator()) {
        appendRange(Init.begin(), Init.end());
    }

    ObjectPool(std::initializer_list<value_type> Init, Allocator const& Alloc)
        : ObjectPool(Alloc) {
        appendRange(Init.begin(), Init.end());
    }

    // @brief 拷贝构造。
    // @notice 分配器经 select_on_container_copy_construction 获取。
    ObjectPool(ObjectPool const& Other)
        : PoolAllocator(PoolTraits::select_on_container_copy_construction(
              Other.PoolAllocator)),
          Slots(SlotAllocator(PoolAllocator)),
          FreeSlots(FreeListAllocator(PoolAllocator)),
          LiveSize(0) {
        copyElementsFrom(Other);
    }

    // @brief 移动构造（分配器可无异常移动时为 noexcept）。
    ObjectPool(ObjectPool&& Other)
        noexcept(std::is_nothrow_move_constructible<Allocator>::value)
        : PoolAllocator(std::move(Other.PoolAllocator)),
          Slots(std::move(Other.Slots)),
          Occupied(std::move(Other.Occupied)),
          FreeSlots(std::move(Other.FreeSlots)),
          LiveSize(Other.LiveSize) {
        Other.LiveSize = 0;
    }

    ~ObjectPool() = default;

    // @brief 拷贝赋值，遵循 propagate_on_container_copy_assignment。
    ObjectPool& operator=(ObjectPool const& Other) {
        if (this != &Other)
            copyAssignImpl(Other, PropagateOnCopyAssignment());
        return *this;
    }

    // @brief 移动赋值，遵循 propagate_on_container_move_assignment。
    ObjectPool& operator=(ObjectPool&& Other)
        noexcept(PropagateOnMoveAssignment::value ||
                 std::is_empty<Allocator>::value) {
        moveAssignImpl(std::move(Other), PropagateOnMoveAssignment());
        return *this;
    }

    ObjectPool& operator=(std::initializer_list<value_type> Init) {
        assign(Init);
        return *this;
    }

    // @brief 返回分配器副本。
    allocator_type get_allocator() const { return PoolAllocator; }

    // ------------------------------------------------------------------ //
    // 元素访问。                                                          //
    // ------------------------------------------------------------------ //
    // @brief 带边界检查的元素访问，越界抛 std::out_of_range。
    // @param Pos 逻辑下标（第 Pos 个有效元素）
    reference at(size_type Pos) {
        checkIndex(Pos);
        return (*this)[Pos];
    }

    const_reference at(size_type Pos) const {
        checkIndex(Pos);
        return (*this)[Pos];
    }

    // @brief 无边界检查的元素访问（第 Pos 个有效元素）。
    reference operator[](size_type Pos) {
        return const_cast<reference>(
            static_cast<ObjectPool const&>(*this)[Pos]);
    }

    const_reference operator[](size_type Pos) const {
        return *ObjectPoolDetail::getObjectPointer(
            Slots[findNthOccupied(Pos)].Storage);
    }

    reference front() {
        return *ObjectPoolDetail::getObjectPointer(
            Slots[findFirstOccupied()].Storage);
    }

    const_reference front() const {
        return *ObjectPoolDetail::getObjectPointer(
            Slots[findFirstOccupied()].Storage);
    }

    reference back() {
        return *ObjectPoolDetail::getObjectPointer(
            Slots[findLastOccupied()].Storage);
    }

    const_reference back() const {
        return *ObjectPoolDetail::getObjectPointer(
            Slots[findLastOccupied()].Storage);
    }

    // ------------------------------------------------------------------ //
    // 迭代器。                                                            //
    // ------------------------------------------------------------------ //
    iterator begin() { return iterator(this, findFirstOccupied()); }
    const_iterator begin() const {
        return const_iterator(this, findFirstOccupied());
    }
    const_iterator cbegin() const {
        return const_iterator(this, findFirstOccupied());
    }

    iterator end() { return iterator(this, Slots.size()); }
    const_iterator end() const { return const_iterator(this, Slots.size()); }
    const_iterator cend() const { return const_iterator(this, Slots.size()); }

    reverse_iterator rbegin() { return reverse_iterator(end()); }
    const_reverse_iterator rbegin() const {
        return const_reverse_iterator(end());
    }
    const_reverse_iterator crbegin() const {
        return const_reverse_iterator(cend());
    }

    reverse_iterator rend() { return reverse_iterator(begin()); }
    const_reverse_iterator rend() const {
        return const_reverse_iterator(begin());
    }
    const_reverse_iterator crend() const {
        return const_reverse_iterator(cbegin());
    }

    // ------------------------------------------------------------------ //
    // 容量。                                                              //
    // ------------------------------------------------------------------ //
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(nodiscard) >= 201907L
    [[nodiscard]]
#endif
#endif
    bool empty() const { return LiveSize == 0; }

    // @brief 有效元素个数。
    size_type size() const { return LiveSize; }

    size_type max_size() const {
        return std::min(Slots.max_size(), FreeSlots.max_size());
    }

    // @brief 槽位总数（占用 + 空闲），即底层存储大小。
    size_type capacity() const { return Slots.size(); }

    // @brief 可复用的空闲槽位数。
    size_type getFreeCount() const { return Slots.size() - LiveSize; }

    // @brief 确保底层存储至少容纳 NewCapacity 个槽位（不构造元素）。
    // @param NewCapacity 目标槽位总数
    void reserve(size_type NewCapacity) {
        size_type OldSize = Slots.size();
        while (Slots.size() < NewCapacity)
            Slots.emplace_back();
        // @notice 新增槽位逆序入栈，使 LIFO 弹出为正向（旧槽位 -> 新槽位）。
        for (size_type I = Slots.size(); I > OldSize; --I)
            FreeSlots.push_back(I - 1);
        resizeOccupied();
    }

    // @brief 将被占用的槽位压实到前部并丢弃空闲槽位。
    void shrinkToFit() {
        if (LiveSize == 0) {
            Slots.clear();
            FreeSlots.clear();
            resizeOccupied();
            callShrinkToFit(Slots, ObjectPoolDetail::HasShrinkToFit<container_type>());
            return;
        }

        if (FreeSlots.empty()) {
            callShrinkToFit(Slots, ObjectPoolDetail::HasShrinkToFit<container_type>());
            return;
        }

        size_type Write = 0;
        for (size_type Read = 0; Read < Slots.size(); ++Read) {
            if (!isOccupied(Read))
                continue;
            if (Read != Write) {
                ObjectPoolDetail::constructInStorage(
                    Slots[Write].Storage,
                    std::move(*ObjectPoolDetail::getObjectPointer(
                        Slots[Read].Storage)));
                setOccupied(Write, true);
                ObjectPoolDetail::destroyInStorage(Slots[Read].Storage);
                setOccupied(Read, false);
            }
            ++Write;
        }
        Slots.resize(Write);
        FreeSlots.clear();
        resizeOccupied();
        callShrinkToFit(Slots, ObjectPoolDetail::HasShrinkToFit<container_type>());
    }

    // ------------------------------------------------------------------ //
    // 修改器。                                                            //
    // ------------------------------------------------------------------ //
    // @brief 清空所有元素（保留容量，不回收内存）。
    void clear() {
        for (size_type I = 0; I < Slots.size(); ++I) {
            if (isOccupied(I)) {
                ObjectPoolDetail::destroyInStorage(Slots[I].Storage);
                setOccupied(I, false);
            }
        }
        LiveSize = 0;
        FreeSlots.clear();
        FreeSlots.reserve(Slots.size());
        // @notice 逆序入栈，使后续 fill/append 为正向（槽位 0,1,2,...）。
        for (size_type I = Slots.size(); I > 0; --I)
            FreeSlots.push_back(I - 1);
    }

    void pushBack(const_reference Value) { emplaceBack(Value); }
    void pushBack(value_type&& Value) { emplaceBack(std::move(Value)); }

    // @brief 原地构造新元素，优先复用空闲槽位。
    // @param Arguments 转发给 value_type 构造函数的参数
    // @return 新元素的引用
    // @notice 构造抛异常时回滚（槽位保持空闲），保证强异常安全。
    template <typename... Args>
    reference emplaceBack(Args&&... Arguments) {
        return *ObjectPoolDetail::getObjectPointer(
            Slots[emplaceIntoFreeSlot(std::forward<Args>(Arguments)...)].Storage);
    }

    // @brief 追加元素并返回指向它的迭代器（稳定，可作为外部映射句柄）。
    // @param Value 追加的值
    // @return 指向新元素的迭代器
    // @notice 结合 getSlotIndex 可实现 Key -> SlotId -> Value 映射。
    iterator append(const_reference Value) {
        return iterator(this, emplaceIntoFreeSlot(Value));
    }
    iterator append(value_type&& Value) {
        return iterator(this, emplaceIntoFreeSlot(std::move(Value)));
    }

    // @brief 删除最后一个有效元素。
    void popBack() {
        size_type Last = findLastOccupied();
        erase(const_iterator(this, Last));
    }

    // @brief 删除单个元素（墓碑式，O(1)，不搬移）。
    // @param Pos 指向待删元素的迭代器
    // @return 指向被删元素之后下一个有效元素的迭代器
    iterator erase(const_iterator Pos) {
        size_type Index = Pos.Index;
        SlotType& SlotRef = Slots[Index];
        ObjectPoolDetail::destroyInStorage(SlotRef.Storage);
        setOccupied(Index, false);
        FreeSlots.push_back(Index);
        --LiveSize;
        return iterator(this, findNextOccupied(Index));
    }

    // @brief 删除区间 [First, Last)（O(区间)，不搬移）。
    iterator erase(const_iterator First, const_iterator Last) {
        size_type Stop = Last.Index;
        for (size_type Index = First.Index; Index < Stop; ++Index) {
            if (isOccupied(Index)) {
                ObjectPoolDetail::destroyInStorage(Slots[Index].Storage);
                setOccupied(Index, false);
                FreeSlots.push_back(Index);
                --LiveSize;
            }
        }
        return iterator(this, Stop);
    }

    // @brief 调整有效元素个数；增长时默认插入。
    void resize(size_type Count) {
        while (LiveSize < Count)
            emplaceBack();
        while (LiveSize > Count)
            popBack();
    }

    // @brief 调整有效元素个数；增长时插入 Value 的副本。
    void resize(size_type Count, const_reference Value) {
        while (LiveSize < Count)
            emplaceBack(Value);
        while (LiveSize > Count)
            popBack();
    }

    // @brief 以 Count 个 Value 副本重新赋值。
    void assign(size_type Count, const_reference Value) {
        Slots.clear();
        FreeSlots.clear();
        resizeOccupied();
        LiveSize = 0;
        for (size_type I = 0; I < Count; ++I)
            emplaceBack(Value);
    }

    // @brief 以区间 [First, Last) 重新赋值。
    template <typename InputIterator>
    typename std::enable_if<!std::is_integral<InputIterator>::value>::type
    assign(InputIterator First, InputIterator Last) {
        Slots.clear();
        FreeSlots.clear();
        resizeOccupied();
        LiveSize = 0;
        appendRange(First, Last);
    }

    void assign(std::initializer_list<value_type> Init) {
        assign(Init.begin(), Init.end());
    }

    // @brief 从区间 [First, Last) 批量刷入元素，复用空闲槽位（不触发池内存分配）。
    // @param First, Last 输入区间
    // @return 刷入的元素个数
    // @notice 空闲槽位不足时抛 std::length_error（已刷入的部分保留）。
    template <typename InputIterator>
    typename std::enable_if<!std::is_integral<InputIterator>::value, size_type>::type
    fill(InputIterator First, InputIterator Last) {
        size_type Count = 0;
        for (; First != Last; ++First) {
            fillOne(*First);
            ++Count;
        }
        return Count;
    }

    // @brief 批量刷入 Count 个 Value 副本，复用空闲槽位（不触发池内存分配）。
    // @param Count 刷入个数
    // @param Value 刷入的值
    // @return 刷入的元素个数
    size_type fill(size_type Count, const_reference Value) {
        for (size_type I = 0; I < Count; ++I)
            fillOne(Value);
        return Count;
    }

    // @brief 交换内容与分配器（遵循 propagate_on_container_swap）。
    void swap(ObjectPool& Other) {
        using std::swap;
        swap(PoolAllocator, Other.PoolAllocator);
        swap(Slots, Other.Slots);
        swap(Occupied, Other.Occupied);
        swap(FreeSlots, Other.FreeSlots);
        swap(LiveSize, Other.LiveSize);
    }

    // @brief 返回迭代器对应的物理槽位下标（稳定，可作外部映射的紧凑句柄）。
    // @param Pos 指向某元素的迭代器
    // @return 该元素的物理槽位下标
    size_type getSlotIndex(const_iterator Pos) const { return Pos.Index; }

    // @brief 由物理槽位下标构造迭代器（用于按下标删除）。
    // @param Index 物理槽位下标
    // @return 指向该槽位的迭代器
    // @notice Index 必须是有效的占用槽位下标（由 getSlotIndex/append 获得）。
    iterator getIterator(size_type Index) { return iterator(this, Index); }

private:
    // ------------------------------------------------------------------ //
    // 内部辅助。                                                          //
    // ------------------------------------------------------------------ //
    // @brief 占用标志读取（按类型分派：平凡类型读位图，非平凡类型读槽位内标志）。
    bool isOccupied(size_type Index) const {
        return isOccupiedImpl(Index, IsTrivialType());
    }
    bool isOccupiedImpl(size_type Index, std::true_type) const {
        return Occupied[Index];
    }
    bool isOccupiedImpl(size_type Index, std::false_type) const {
        return Slots[Index].bOccupied;
    }

    // @brief 占用标志写入（按类型分派）。
    void setOccupied(size_type Index, bool Value) {
        setOccupiedImpl(Index, Value, IsTrivialType());
    }
    void setOccupiedImpl(size_type Index, bool Value, std::true_type) {
        Occupied[Index] = Value;
    }
    void setOccupiedImpl(size_type Index, bool Value, std::false_type) {
        Slots[Index].bOccupied = Value;
    }

    // @brief 使占用位图大小与槽位存储对齐（平凡类型专用；非平凡类型为空操作）。
    void resizeOccupied() { resizeOccupiedImpl(IsTrivialType()); }
    void resizeOccupiedImpl(std::true_type) {
        Occupied.resize(Slots.size(), false);
    }
    void resizeOccupiedImpl(std::false_type) {}

    template <typename InputIterator>
    void appendRange(InputIterator First, InputIterator Last) {
        for (; First != Last; ++First)
            emplaceBack(*First);
    }

    // @brief 在空闲槽位中构造元素，返回其物理下标。
    // @return 新元素所在的物理槽位下标
    template <typename... Args>
    size_type emplaceIntoFreeSlot(Args&&... Arguments) {
        size_type Index = acquireFreeIndex();
        SlotType& SlotRef = Slots[Index];
        try {
            ObjectPoolDetail::constructInStorage(SlotRef.Storage,
                                                 std::forward<Args>(Arguments)...);
        } catch (...) {
            markSlotFree(Index);
            throw;
        }
        setOccupied(Index, true);
        ++LiveSize;
        return Index;
    }

    // @brief 不增长的单个刷入：从空闲链表取一个槽位并构造 Value。
    void fillOne(const_reference Value) {
        size_type Index = acquireFreeIndexNoGrow();
        SlotType& SlotRef = Slots[Index];
        try {
            ObjectPoolDetail::constructInStorage(SlotRef.Storage, Value);
        } catch (...) {
            markSlotFree(Index);
            throw;
        }
        setOccupied(Index, true);
        ++LiveSize;
    }

    // @brief 不增长的槽位获取：无空闲槽位时抛 std::length_error。
    size_type acquireFreeIndexNoGrow() {
        if (FreeSlots.empty())
            throw std::length_error("ObjectPool::fill: 空闲槽位不足");
        size_type Index = FreeSlots.back();
        FreeSlots.pop_back();
        return Index;
    }
    // @notice 若直接复用 LIFO 空闲链表，会反转迭代顺序。
    void copyElementsFrom(ObjectPool const& Other) {
        Slots.clear();
        FreeSlots.clear();
        resizeOccupied();
        LiveSize = 0;
        for (const_iterator It = Other.cbegin(); It != Other.cend(); ++It)
            emplaceBack(*It);
    }

    void copyAssignImpl(ObjectPool const& Other, std::true_type) {
        if (PoolAllocator != Other.PoolAllocator) {
            clear();
            Slots = container_type(SlotAllocator(Other.PoolAllocator));
            FreeSlots = FreeListType(FreeListAllocator(Other.PoolAllocator));
            PoolAllocator = Other.PoolAllocator;
            LiveSize = 0;
            resizeOccupied();
        }
        copyElementsFrom(Other);
    }

    void copyAssignImpl(ObjectPool const& Other, std::false_type) {
        clear();
        copyElementsFrom(Other);
    }

    void moveAssignImpl(ObjectPool&& Other, std::true_type) {
        PoolAllocator = std::move(Other.PoolAllocator);
        Slots = std::move(Other.Slots);
        Occupied = std::move(Other.Occupied);
        FreeSlots = std::move(Other.FreeSlots);
        LiveSize = Other.LiveSize;
        Other.LiveSize = 0;
    }

    void moveAssignImpl(ObjectPool&& Other, std::false_type) {
        if (PoolAllocator == Other.PoolAllocator) {
            Slots = std::move(Other.Slots);
            Occupied = std::move(Other.Occupied);
            FreeSlots = std::move(Other.FreeSlots);
            LiveSize = Other.LiveSize;
            Other.LiveSize = 0;
        } else {
            Slots.clear();
            FreeSlots.clear();
            resizeOccupied();
            LiveSize = 0;
            for (iterator It = Other.begin(); It != Other.end(); ++It)
                emplaceBack(std::move(*It));
            Other.clear();
        }
    }

    // @brief 取得一个空闲槽位下标；无空闲时增长一个槽位。
    size_type acquireFreeIndex() {
        if (!FreeSlots.empty()) {
            size_type Index = FreeSlots.back();
            FreeSlots.pop_back();
            return Index;
        }
        Slots.emplace_back();
        resizeOccupied();
        return Slots.size() - 1;
    }

    // @brief 将槽位标记为空闲并放回空闲链表。
    void markSlotFree(size_type Index) {
        setOccupied(Index, false);
        FreeSlots.push_back(Index);
    }

    size_type findFirstOccupied() const {
        size_type Index = 0;
        while (Index < Slots.size() && !isOccupied(Index))
            ++Index;
        return Index;
    }

    size_type findLastOccupied() const {
        size_type Index = Slots.size();
        while (Index != 0 && !isOccupied(Index - 1))
            --Index;
        return Index - 1;
    }

    size_type findNextOccupied(size_type From) const {
        size_type Index = From + 1;
        while (Index < Slots.size() && !isOccupied(Index))
            ++Index;
        return Index;
    }

    size_type findNthOccupied(size_type Pos) const {
        size_type Index = 0;
        size_type Seen = 0;
        while (Index < Slots.size()) {
            if (isOccupied(Index)) {
                if (Seen == Pos)
                    break;
                ++Seen;
            }
            ++Index;
        }
        return Index;
    }

    void checkIndex(size_type Pos) const {
        if (Pos >= LiveSize)
            throw std::out_of_range("ObjectPool::at: index out of range");
    }

    template <typename C>
    static void callShrinkToFit(C& Cont, std::true_type) {
        Cont.shrink_to_fit();
    }

    template <typename C>
    static void callShrinkToFit(C&, std::false_type) {}

    // ------------------------------------------------------------------ //
    // 成员。                                                              //
    // ------------------------------------------------------------------ //
#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(no_unique_address) >= 201803L
    [[no_unique_address]]
#endif
#endif
    Allocator PoolAllocator;
    container_type Slots;
    // @notice 占用位图：仅平凡可拷贝类型使用；非平凡类型为空（读取走槽位内标志）。
    std::vector<bool> Occupied;
    FreeListType FreeSlots;
    size_type LiveSize;
};

// @brief 类外定义：iterator -> const_iterator 隐式转换。
template <typename Type, typename Allocator,
          template <typename, typename> class Container>
inline ObjectPool<Type, Allocator, Container>::const_iterator::const_iterator(
    ObjectPool<Type, Allocator, Container>::iterator const& Other)
    : Owner(Other.Owner), Index(Other.Index) {}

// @brief 类外定义：iterator 与 const_iterator 的交叉比较（委托给 const_iterator）。
template <typename Type, typename Allocator,
          template <typename, typename> class Container>
inline bool ObjectPool<Type, Allocator, Container>::iterator::operator==(
    typename ObjectPool<Type, Allocator, Container>::const_iterator const& Rhs) const {
    return Rhs == *this;
}

template <typename Type, typename Allocator,
          template <typename, typename> class Container>
inline bool ObjectPool<Type, Allocator, Container>::iterator::operator!=(
    typename ObjectPool<Type, Allocator, Container>::const_iterator const& Rhs) const {
    return !(Rhs == *this);
}

/**
 * @brief 偏特化：以 std::list 作为节点型底层存储，槽位以稳定的链表迭代器寻址。
 * @notice 节点型存储的槽位内 bool 相对节点指针开销可忽略，故不采用紧凑位图；
 *         其稳定句柄是链表迭代器（非下标）。
 */
template <typename Type, typename Allocator>
class ObjectPool<Type, Allocator, std::list> {
private:
    using SlotType = ObjectPoolDetail::Slot<Type>;
    using SlotAllocator =
        typename std::allocator_traits<Allocator>::template rebind_alloc<SlotType>;

    using PoolTraits = std::allocator_traits<Allocator>;

public:
    using allocator_type = Allocator;
    using value_type = Type;
    using container_type = std::list<SlotType, SlotAllocator>;
    using size_type = typename container_type::size_type;
    using difference_type = typename container_type::difference_type;
    using reference = value_type&;
    using const_reference = value_type const&;
    using pointer = value_type*;
    using const_pointer = value_type const*;

    class const_iterator;
    class iterator;

    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

private:
    using SlotIterator = typename container_type::iterator;
    using ConstSlotIterator = typename container_type::const_iterator;

    using FreeListAllocator =
        typename PoolTraits::template rebind_alloc<SlotIterator>;
    using FreeListType = std::vector<SlotIterator, FreeListAllocator>;

    using PropagateOnCopyAssignment =
        typename PoolTraits::propagate_on_container_copy_assignment;
    using PropagateOnMoveAssignment =
        typename PoolTraits::propagate_on_container_move_assignment;

public:
    // @notice 两种迭代器都持有可变链表迭代器：const 语义由 Owner 指针保证，
    //         const_iterator 仅暴露只读访问。
    class iterator {
        friend class ObjectPool;
        friend class const_iterator;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = ObjectPool::value_type;
        using difference_type = ObjectPool::difference_type;
        using pointer = ObjectPool::pointer;
        using reference = ObjectPool::reference;

        iterator() : Owner(nullptr) {}

        reference operator*() const {
            return *ObjectPoolDetail::getObjectPointer(It->Storage);
        }
        pointer operator->() const {
            return ObjectPoolDetail::getObjectPointer(It->Storage);
        }

        iterator& operator++() {
            advanceToNextOccupied();
            return *this;
        }
        iterator operator++(int) {
            iterator Temp(*this);
            ++*this;
            return Temp;
        }
        iterator& operator--() {
            retreatToPreviousOccupied();
            return *this;
        }
        iterator operator--(int) {
            iterator Temp(*this);
            --*this;
            return Temp;
        }

        bool operator==(iterator const& Rhs) const {
            return Owner == Rhs.Owner && It == Rhs.It;
        }
        bool operator!=(iterator const& Rhs) const { return !(*this == Rhs); }

        bool operator==(const_iterator const& Rhs) const;
        bool operator!=(const_iterator const& Rhs) const;

    private:
        iterator(ObjectPool* InOwner, SlotIterator InIt)
            : Owner(InOwner), It(InIt) {}

        void advanceToNextOccupied() {
            do {
                ++It;
            } while (It != Owner->Slots.end() && !It->bOccupied);
        }
        void retreatToPreviousOccupied() {
            do {
                --It;
            } while (It != Owner->Slots.begin() && !It->bOccupied);
        }

        ObjectPool* Owner;
        SlotIterator It;
    };

    class const_iterator {
        friend class ObjectPool;
        friend class iterator;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = ObjectPool::value_type;
        using difference_type = ObjectPool::difference_type;
        using pointer = ObjectPool::const_pointer;
        using reference = ObjectPool::const_reference;

        const_iterator() : Owner(nullptr) {}

        const_iterator(iterator const& Other);

        reference operator*() const {
            return *ObjectPoolDetail::getObjectPointer(It->Storage);
        }
        pointer operator->() const {
            return ObjectPoolDetail::getObjectPointer(It->Storage);
        }

        const_iterator& operator++() {
            advanceToNextOccupied();
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator Temp(*this);
            ++*this;
            return Temp;
        }
        const_iterator& operator--() {
            retreatToPreviousOccupied();
            return *this;
        }
        const_iterator operator--(int) {
            const_iterator Temp(*this);
            --*this;
            return Temp;
        }

        bool operator==(const_iterator const& Rhs) const {
            return Owner == Rhs.Owner && It == Rhs.It;
        }
        bool operator!=(const_iterator const& Rhs) const { return !(*this == Rhs); }
        bool operator==(iterator const& Rhs) const {
            return Owner == Rhs.Owner && It == Rhs.It;
        }
        bool operator!=(iterator const& Rhs) const { return !(*this == Rhs); }

    private:
        const_iterator(ObjectPool const* InOwner, SlotIterator InIt)
            : Owner(InOwner), It(InIt) {}

        void advanceToNextOccupied() {
            do {
                ++It;
            } while (It != Owner->Slots.end() && !It->bOccupied);
        }
        void retreatToPreviousOccupied() {
            do {
                --It;
            } while (It != Owner->Slots.begin() && !It->bOccupied);
        }

        ObjectPool const* Owner;
        SlotIterator It;
    };

public:
    ObjectPool()
        : PoolAllocator(Allocator()),
          Slots(SlotAllocator(PoolAllocator)),
          FreeSlots(FreeListAllocator(PoolAllocator)),
          LiveSize(0) {}

    explicit ObjectPool(Allocator const& Alloc)
        : PoolAllocator(Alloc),
          Slots(SlotAllocator(PoolAllocator)),
          FreeSlots(FreeListAllocator(PoolAllocator)),
          LiveSize(0) {}

    explicit ObjectPool(size_type Count)
        : ObjectPool(Allocator()) {
        for (size_type I = 0; I < Count; ++I)
            emplaceBack();
    }

    ObjectPool(size_type Count, const_reference Value)
        : ObjectPool(Allocator()) {
        for (size_type I = 0; I < Count; ++I)
            emplaceBack(Value);
    }

    ObjectPool(size_type Count, const_reference Value, Allocator const& Alloc)
        : ObjectPool(Alloc) {
        for (size_type I = 0; I < Count; ++I)
            emplaceBack(Value);
    }

    template <typename InputIterator,
              typename = typename std::enable_if<
                  !std::is_integral<InputIterator>::value>::type>
    ObjectPool(InputIterator First, InputIterator Last)
        : ObjectPool(Allocator()) {
        appendRange(First, Last);
    }

    template <typename InputIterator,
              typename = typename std::enable_if<
                  !std::is_integral<InputIterator>::value>::type>
    ObjectPool(InputIterator First, InputIterator Last, Allocator const& Alloc)
        : ObjectPool(Alloc) {
        appendRange(First, Last);
    }

    ObjectPool(std::initializer_list<value_type> Init)
        : ObjectPool(Allocator()) {
        appendRange(Init.begin(), Init.end());
    }

    ObjectPool(std::initializer_list<value_type> Init, Allocator const& Alloc)
        : ObjectPool(Alloc) {
        appendRange(Init.begin(), Init.end());
    }

    ObjectPool(ObjectPool const& Other)
        : PoolAllocator(PoolTraits::select_on_container_copy_construction(
              Other.PoolAllocator)),
          Slots(SlotAllocator(PoolAllocator)),
          FreeSlots(FreeListAllocator(PoolAllocator)),
          LiveSize(0) {
        copyElementsFrom(Other);
    }

    ObjectPool(ObjectPool&& Other)
        noexcept(std::is_nothrow_move_constructible<Allocator>::value)
        : PoolAllocator(std::move(Other.PoolAllocator)),
          Slots(std::move(Other.Slots)),
          FreeSlots(std::move(Other.FreeSlots)),
          LiveSize(Other.LiveSize) {
        Other.LiveSize = 0;
    }

    ~ObjectPool() = default;

    ObjectPool& operator=(ObjectPool const& Other) {
        if (this != &Other)
            copyAssignImpl(Other, PropagateOnCopyAssignment());
        return *this;
    }

    ObjectPool& operator=(ObjectPool&& Other)
        noexcept(PropagateOnMoveAssignment::value ||
                 std::is_empty<Allocator>::value) {
        moveAssignImpl(std::move(Other), PropagateOnMoveAssignment());
        return *this;
    }

    ObjectPool& operator=(std::initializer_list<value_type> Init) {
        assign(Init);
        return *this;
    }

    allocator_type get_allocator() const { return PoolAllocator; }

    reference front() {
        return *ObjectPoolDetail::getObjectPointer(findFirstOccupied()->Storage);
    }
    const_reference front() const {
        return *ObjectPoolDetail::getObjectPointer(findFirstOccupied()->Storage);
    }
    reference back() {
        return *ObjectPoolDetail::getObjectPointer(findLastOccupied()->Storage);
    }
    const_reference back() const {
        return *ObjectPoolDetail::getObjectPointer(findLastOccupied()->Storage);
    }

    iterator begin() { return iterator(this, findFirstOccupied()); }
    const_iterator begin() const {
        return const_iterator(this, findFirstOccupied());
    }
    const_iterator cbegin() const {
        return const_iterator(this, findFirstOccupied());
    }

    iterator end() { return iterator(this, endSlot()); }
    const_iterator end() const { return const_iterator(this, endSlot()); }
    const_iterator cend() const { return const_iterator(this, endSlot()); }

    reverse_iterator rbegin() { return reverse_iterator(end()); }
    const_reverse_iterator rbegin() const {
        return const_reverse_iterator(end());
    }
    const_reverse_iterator crbegin() const {
        return const_reverse_iterator(cend());
    }

    reverse_iterator rend() { return reverse_iterator(begin()); }
    const_reverse_iterator rend() const {
        return const_reverse_iterator(begin());
    }
    const_reverse_iterator crend() const {
        return const_reverse_iterator(cbegin());
    }

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(nodiscard) >= 201907L
    [[nodiscard]]
#endif
#endif
    bool empty() const { return LiveSize == 0; }

    size_type size() const { return LiveSize; }

    size_type max_size() const {
        return std::min(Slots.max_size(), FreeSlots.max_size());
    }

    size_type capacity() const { return Slots.size(); }

    size_type getFreeCount() const { return Slots.size() - LiveSize; }

    void reserve(size_type NewCapacity) {
        std::vector<SlotIterator> NewSlots;
        while (Slots.size() < NewCapacity) {
            Slots.emplace_back();
            NewSlots.push_back(std::prev(Slots.end()));
        }
        // @notice 新增节点逆序入栈，使 LIFO 弹出为正向。
        for (typename std::vector<SlotIterator>::const_reverse_iterator It =
                 NewSlots.rbegin();
             It != NewSlots.rend(); ++It)
            FreeSlots.push_back(*It);
    }

    // @brief 删除所有空闲节点，将其内存归还分配器。
    void shrinkToFit() {
        for (typename FreeListType::const_iterator It = FreeSlots.begin();
             It != FreeSlots.end(); ++It)
            Slots.erase(*It);
        FreeSlots.clear();
    }

    void clear() {
        for (SlotIterator It = Slots.begin(); It != Slots.end(); ++It) {
            if (It->bOccupied) {
                ObjectPoolDetail::destroyInStorage(It->Storage);
                It->bOccupied = false;
            }
        }
        LiveSize = 0;
        FreeSlots.clear();
        FreeSlots.reserve(Slots.size());
        for (SlotIterator It = Slots.begin(); It != Slots.end(); ++It)
            FreeSlots.push_back(It);
        // @notice 逆序，使后续 fill/append 为正向（节点顺序）。
        std::reverse(FreeSlots.begin(), FreeSlots.end());
    }

    void pushBack(const_reference Value) { emplaceBack(Value); }
    void pushBack(value_type&& Value) { emplaceBack(std::move(Value)); }

    template <typename... Args>
    reference emplaceBack(Args&&... Arguments) {
        return *ObjectPoolDetail::getObjectPointer(
            emplaceIntoFreeSlot(std::forward<Args>(Arguments)...)->Storage);
    }

    // @brief 追加元素并返回指向它的迭代器（节点型存储中迭代器稳定）。
    iterator append(const_reference Value) {
        return iterator(this, emplaceIntoFreeSlot(Value));
    }
    iterator append(value_type&& Value) {
        return iterator(this, emplaceIntoFreeSlot(std::move(Value)));
    }

    void popBack() {
        erase(const_iterator(this, findLastOccupied()));
    }

    iterator erase(const_iterator Pos) {
        SlotIterator It = Pos.It;
        ObjectPoolDetail::destroyInStorage(It->Storage);
        It->bOccupied = false;
        FreeSlots.push_back(It);
        --LiveSize;
        return iterator(this, findNextOccupied(It));
    }

    iterator erase(const_iterator First, const_iterator Last) {
        SlotIterator It = First.It;
        SlotIterator Stop = Last.It;
        while (It != Stop) {
            SlotIterator Next = It;
            ++Next;
            if (It->bOccupied) {
                ObjectPoolDetail::destroyInStorage(It->Storage);
                It->bOccupied = false;
                FreeSlots.push_back(It);
                --LiveSize;
            }
            It = Next;
        }
        return iterator(this, Stop);
    }

    void resize(size_type Count) {
        while (LiveSize < Count)
            emplaceBack();
        while (LiveSize > Count)
            popBack();
    }

    void resize(size_type Count, const_reference Value) {
        while (LiveSize < Count)
            emplaceBack(Value);
        while (LiveSize > Count)
            popBack();
    }

    void assign(size_type Count, const_reference Value) {
        Slots.clear();
        FreeSlots.clear();
        LiveSize = 0;
        for (size_type I = 0; I < Count; ++I)
            emplaceBack(Value);
    }

    template <typename InputIterator>
    typename std::enable_if<!std::is_integral<InputIterator>::value>::type
    assign(InputIterator First, InputIterator Last) {
        Slots.clear();
        FreeSlots.clear();
        LiveSize = 0;
        appendRange(First, Last);
    }

    void assign(std::initializer_list<value_type> Init) {
        assign(Init.begin(), Init.end());
    }

    // @brief 从区间 [First, Last) 批量刷入元素，复用空闲槽位（不触发池内存分配）。
    template <typename InputIterator>
    typename std::enable_if<!std::is_integral<InputIterator>::value, size_type>::type
    fill(InputIterator First, InputIterator Last) {
        size_type Count = 0;
        for (; First != Last; ++First) {
            fillOne(*First);
            ++Count;
        }
        return Count;
    }

    // @brief 批量刷入 Count 个 Value 副本（不触发池内存分配）。
    size_type fill(size_type Count, const_reference Value) {
        for (size_type I = 0; I < Count; ++I)
            fillOne(Value);
        return Count;
    }

    void swap(ObjectPool& Other) {
        using std::swap;
        swap(PoolAllocator, Other.PoolAllocator);
        swap(Slots, Other.Slots);
        swap(FreeSlots, Other.FreeSlots);
        swap(LiveSize, Other.LiveSize);
    }

private:
    template <typename InputIterator>
    void appendRange(InputIterator First, InputIterator Last) {
        for (; First != Last; ++First)
            emplaceBack(*First);
    }

    // @brief 在空闲节点中构造元素，返回其内部链表迭代器。
    template <typename... Args>
    SlotIterator emplaceIntoFreeSlot(Args&&... Arguments) {
        SlotIterator It = acquireFreeSlot();
        SlotType& SlotRef = *It;
        try {
            ObjectPoolDetail::constructInStorage(SlotRef.Storage,
                                                 std::forward<Args>(Arguments)...);
        } catch (...) {
            markSlotFree(It);
            throw;
        }
        SlotRef.bOccupied = true;
        ++LiveSize;
        return It;
    }

    // @brief 不增长的单个刷入：从空闲链表取一个节点并构造 Value。
    void fillOne(const_reference Value) {
        SlotIterator It = acquireFreeSlotNoGrow();
        SlotType& SlotRef = *It;
        try {
            ObjectPoolDetail::constructInStorage(SlotRef.Storage, Value);
        } catch (...) {
            markSlotFree(It);
            throw;
        }
        SlotRef.bOccupied = true;
        ++LiveSize;
    }

    // @brief 不增长的节点获取：无空闲节点时抛 std::length_error。
    SlotIterator acquireFreeSlotNoGrow() {
        if (FreeSlots.empty())
            throw std::length_error("ObjectPool::fill: 空闲槽位不足");
        SlotIterator It = FreeSlots.back();
        FreeSlots.pop_back();
        return It;
    }

    void copyElementsFrom(ObjectPool const& Other) {
        // @notice 若直接复用 LIFO 空闲链表，会反转迭代顺序。
        Slots.clear();
        FreeSlots.clear();
        LiveSize = 0;
        for (const_iterator It = Other.cbegin(); It != Other.cend(); ++It)
            emplaceBack(*It);
    }

    void copyAssignImpl(ObjectPool const& Other, std::true_type) {
        if (PoolAllocator != Other.PoolAllocator) {
            clear();
            Slots = container_type(SlotAllocator(Other.PoolAllocator));
            FreeSlots = FreeListType(FreeListAllocator(Other.PoolAllocator));
            PoolAllocator = Other.PoolAllocator;
            LiveSize = 0;
        }
        copyElementsFrom(Other);
    }

    void copyAssignImpl(ObjectPool const& Other, std::false_type) {
        clear();
        copyElementsFrom(Other);
    }

    void moveAssignImpl(ObjectPool&& Other, std::true_type) {
        PoolAllocator = std::move(Other.PoolAllocator);
        Slots = std::move(Other.Slots);
        FreeSlots = std::move(Other.FreeSlots);
        LiveSize = Other.LiveSize;
        Other.LiveSize = 0;
    }

    void moveAssignImpl(ObjectPool&& Other, std::false_type) {
        if (PoolAllocator == Other.PoolAllocator) {
            Slots = std::move(Other.Slots);
            FreeSlots = std::move(Other.FreeSlots);
            LiveSize = Other.LiveSize;
            Other.LiveSize = 0;
        } else {
            Slots.clear();
            FreeSlots.clear();
            LiveSize = 0;
            for (iterator It = Other.begin(); It != Other.end(); ++It)
                emplaceBack(std::move(*It));
            Other.clear();
        }
    }

    // @brief 取得一个空闲槽位；无空闲时新增一个节点。
    SlotIterator acquireFreeSlot() {
        if (!FreeSlots.empty()) {
            SlotIterator It = FreeSlots.back();
            FreeSlots.pop_back();
            return It;
        }
        Slots.emplace_back();
        return std::prev(Slots.end());
    }

    void markSlotFree(SlotIterator It) {
        It->bOccupied = false;
        FreeSlots.push_back(It);
    }

    // @notice 下面的 const 成员仅为了取得可变迭代器而对底层容器做 const_cast；
    //         节点始终由本池以非 const 持有，因此这是安全的，且此处无任何修改。
    SlotIterator endSlot() const {
        return const_cast<container_type&>(Slots).end();
    }

    SlotIterator findFirstOccupied() const {
        container_type& MutableSlots = const_cast<container_type&>(Slots);
        for (SlotIterator It = MutableSlots.begin();
             It != MutableSlots.end(); ++It)
            if (It->bOccupied)
                return It;
        return MutableSlots.end();
    }

    SlotIterator findLastOccupied() const {
        container_type& MutableSlots = const_cast<container_type&>(Slots);
        SlotIterator It = MutableSlots.end();
        while (It != MutableSlots.begin()) {
            --It;
            if (It->bOccupied)
                return It;
        }
        return MutableSlots.end();
    }

    SlotIterator findNextOccupied(SlotIterator From) const {
        SlotIterator End = endSlot();
        do {
            ++From;
        } while (From != End && !From->bOccupied);
        return From;
    }

#if defined(__has_cpp_attribute)
#if __has_cpp_attribute(no_unique_address) >= 201803L
    [[no_unique_address]]
#endif
#endif
    Allocator PoolAllocator;
    container_type Slots;
    FreeListType FreeSlots;
    size_type LiveSize;
};

// @brief 类外定义：iterator -> const_iterator 隐式转换。
template <typename Type, typename Allocator>
inline ObjectPool<Type, Allocator, std::list>::const_iterator::const_iterator(
    ObjectPool<Type, Allocator, std::list>::iterator const& Other)
    : Owner(Other.Owner), It(Other.It) {}

// @brief 类外定义：iterator 与 const_iterator 的交叉比较（委托给 const_iterator）。
template <typename Type, typename Allocator>
inline bool ObjectPool<Type, Allocator, std::list>::iterator::operator==(
    typename ObjectPool<Type, Allocator, std::list>::const_iterator const& Rhs) const {
    return Rhs == *this;
}

template <typename Type, typename Allocator>
inline bool ObjectPool<Type, Allocator, std::list>::iterator::operator!=(
    typename ObjectPool<Type, Allocator, std::list>::const_iterator const& Rhs) const {
    return !(Rhs == *this);
}

/**
 * @brief 定容对象池：以 std::array 作为底层存储，容量编译期固定、不支持增长。
 * @notice 适用于容量上界已知、需避免动态分配的场景。满时 emplaceBack/pushBack/append
 *         抛出 std::length_error。占用标志：平凡类型用 std::bitset 位图，
 *         非平凡类型用槽位内 bool。不提供 reserve/shrinkToFit/resize/assign。
 */
template <typename Type, std::size_t Capacity>
class FixedObjectPool {
private:
    using SlotType = typename ObjectPoolDetail::SlotSelector<Type>::type;
    using IsTrivialType = std::is_trivially_copyable<Type>;

public:
    using value_type = Type;
    using size_type = std::size_t;
    using difference_type = std::ptrdiff_t;
    using reference = value_type&;
    using const_reference = value_type const&;
    using pointer = value_type*;
    using const_pointer = value_type const*;

    class const_iterator;
    class iterator;

    using reverse_iterator = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

public:
    // @brief 可变元素的双向迭代器。
    class iterator {
        friend class FixedObjectPool;
        friend class const_iterator;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = FixedObjectPool::value_type;
        using difference_type = FixedObjectPool::difference_type;
        using pointer = FixedObjectPool::pointer;
        using reference = FixedObjectPool::reference;

        iterator() : Owner(nullptr), Index(0) {}

        reference operator*() const {
            return *ObjectPoolDetail::getObjectPointer(Owner->Slots[Index].Storage);
        }
        pointer operator->() const {
            return ObjectPoolDetail::getObjectPointer(Owner->Slots[Index].Storage);
        }
        iterator& operator++() {
            advanceToNextOccupied();
            return *this;
        }
        iterator operator++(int) {
            iterator Temp(*this);
            ++*this;
            return Temp;
        }
        iterator& operator--() {
            retreatToPreviousOccupied();
            return *this;
        }
        iterator operator--(int) {
            iterator Temp(*this);
            --*this;
            return Temp;
        }

        bool operator==(iterator const& Rhs) const {
            return Owner == Rhs.Owner && Index == Rhs.Index;
        }
        bool operator!=(iterator const& Rhs) const { return !(*this == Rhs); }
        bool operator==(const_iterator const& Rhs) const;
        bool operator!=(const_iterator const& Rhs) const;

    private:
        iterator(FixedObjectPool* InOwner, size_type InIndex)
            : Owner(InOwner), Index(InIndex) {}

        void advanceToNextOccupied() {
            do {
                ++Index;
            } while (Index < Capacity && !Owner->isOccupied(Index));
        }
        void retreatToPreviousOccupied() {
            do {
                --Index;
            } while (Index != static_cast<size_type>(-1) &&
                     !Owner->isOccupied(Index));
        }

        FixedObjectPool* Owner;
        size_type Index;
    };

    // @brief 只读元素的双向迭代器。
    class const_iterator {
        friend class FixedObjectPool;
        friend class iterator;

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type = FixedObjectPool::value_type;
        using difference_type = FixedObjectPool::difference_type;
        using pointer = FixedObjectPool::const_pointer;
        using reference = FixedObjectPool::const_reference;

        const_iterator() : Owner(nullptr), Index(0) {}

        const_iterator(iterator const& Other);

        reference operator*() const {
            return *ObjectPoolDetail::getObjectPointer(Owner->Slots[Index].Storage);
        }
        pointer operator->() const {
            return ObjectPoolDetail::getObjectPointer(Owner->Slots[Index].Storage);
        }
        const_iterator& operator++() {
            advanceToNextOccupied();
            return *this;
        }
        const_iterator operator++(int) {
            const_iterator Temp(*this);
            ++*this;
            return Temp;
        }
        const_iterator& operator--() {
            retreatToPreviousOccupied();
            return *this;
        }
        const_iterator operator--(int) {
            const_iterator Temp(*this);
            --*this;
            return Temp;
        }

        bool operator==(const_iterator const& Rhs) const {
            return Owner == Rhs.Owner && Index == Rhs.Index;
        }
        bool operator!=(const_iterator const& Rhs) const { return !(*this == Rhs); }
        bool operator==(iterator const& Rhs) const {
            return Owner == Rhs.Owner && Index == Rhs.Index;
        }
        bool operator!=(iterator const& Rhs) const { return !(*this == Rhs); }

    private:
        const_iterator(FixedObjectPool const* InOwner, size_type InIndex)
            : Owner(InOwner), Index(InIndex) {}

        void advanceToNextOccupied() {
            do {
                ++Index;
            } while (Index < Capacity && !Owner->isOccupied(Index));
        }
        void retreatToPreviousOccupied() {
            do {
                --Index;
            } while (Index != static_cast<size_type>(-1) &&
                     !Owner->isOccupied(Index));
        }

        FixedObjectPool const* Owner;
        size_type Index;
    };

public:
    // @brief 默认构造：所有槽位初始为空闲。
    FixedObjectPool()
        : Slots(), Occupied(), FreeSlots(), FreeCount(0), LiveSize(0) {
        // @notice 逆序填充，使 LIFO 弹出顺序为 0,1,2,...,Capacity-1（正向插入）。
        for (size_type I = 0; I < Capacity; ++I)
            FreeSlots[I] = Capacity - 1 - I;
        FreeCount = Capacity;
    }

    FixedObjectPool(FixedObjectPool const&) = default;
    FixedObjectPool(FixedObjectPool&&) = default;
    FixedObjectPool& operator=(FixedObjectPool const&) = default;
    FixedObjectPool& operator=(FixedObjectPool&&) = default;

    // @brief 带边界检查的元素访问，越界抛 std::out_of_range。
    reference at(size_type Pos) {
        checkIndex(Pos);
        return (*this)[Pos];
    }
    const_reference at(size_type Pos) const {
        checkIndex(Pos);
        return (*this)[Pos];
    }

    reference operator[](size_type Pos) {
        return const_cast<reference>(
            static_cast<FixedObjectPool const&>(*this)[Pos]);
    }
    const_reference operator[](size_type Pos) const {
        return *ObjectPoolDetail::getObjectPointer(
            Slots[findNthOccupied(Pos)].Storage);
    }

    reference front() {
        return *ObjectPoolDetail::getObjectPointer(
            Slots[findFirstOccupied()].Storage);
    }
    const_reference front() const {
        return *ObjectPoolDetail::getObjectPointer(
            Slots[findFirstOccupied()].Storage);
    }
    reference back() {
        return *ObjectPoolDetail::getObjectPointer(
            Slots[findLastOccupied()].Storage);
    }
    const_reference back() const {
        return *ObjectPoolDetail::getObjectPointer(
            Slots[findLastOccupied()].Storage);
    }

    iterator begin() { return iterator(this, findFirstOccupied()); }
    const_iterator begin() const {
        return const_iterator(this, findFirstOccupied());
    }
    const_iterator cbegin() const {
        return const_iterator(this, findFirstOccupied());
    }

    iterator end() { return iterator(this, Capacity); }
    const_iterator end() const { return const_iterator(this, Capacity); }
    const_iterator cend() const { return const_iterator(this, Capacity); }

    reverse_iterator rbegin() { return reverse_iterator(end()); }
    const_reverse_iterator rbegin() const {
        return const_reverse_iterator(end());
    }
    const_reverse_iterator crbegin() const {
        return const_reverse_iterator(cend());
    }
    reverse_iterator rend() { return reverse_iterator(begin()); }
    const_reverse_iterator rend() const {
        return const_reverse_iterator(begin());
    }
    const_reverse_iterator crend() const {
        return const_reverse_iterator(cbegin());
    }

    bool empty() const { return LiveSize == 0; }
    size_type size() const { return LiveSize; }
    size_type capacity() const { return Capacity; }
    size_type getFreeCount() const { return Capacity - LiveSize; }
    bool full() const { return LiveSize == Capacity; }

    // @brief 清空所有元素（容量不变）。
    void clear() {
        for (size_type I = 0; I < Capacity; ++I) {
            if (isOccupied(I)) {
                ObjectPoolDetail::destroyInStorage(Slots[I].Storage);
                setOccupied(I, false);
            }
        }
        LiveSize = 0;
        for (size_type I = 0; I < Capacity; ++I)
            FreeSlots[I] = Capacity - 1 - I;
        FreeCount = Capacity;
    }

    void pushBack(const_reference Value) { emplaceBack(Value); }
    void pushBack(value_type&& Value) { emplaceBack(std::move(Value)); }

    // @brief 原地构造新元素；满时抛 std::length_error。
    // @return 新元素的引用
    template <typename... Args>
    reference emplaceBack(Args&&... Arguments) {
        return *ObjectPoolDetail::getObjectPointer(
            Slots[emplaceIntoFreeSlot(std::forward<Args>(Arguments)...)].Storage);
    }

    // @brief 追加元素并返回指向它的迭代器；满时抛 std::length_error。
    iterator append(const_reference Value) {
        return iterator(this, emplaceIntoFreeSlot(Value));
    }
    iterator append(value_type&& Value) {
        return iterator(this, emplaceIntoFreeSlot(std::move(Value)));
    }

    // @brief 从区间 [First, Last) 批量刷入元素，复用空闲槽位（不触发分配）。
    template <typename InputIterator>
    typename std::enable_if<!std::is_integral<InputIterator>::value, size_type>::type
    fill(InputIterator First, InputIterator Last) {
        size_type Count = 0;
        for (; First != Last; ++First) {
            fillOne(*First);
            ++Count;
        }
        return Count;
    }

    // @brief 批量刷入 Count 个 Value 副本（不触发分配）。
    size_type fill(size_type Count, const_reference Value) {
        for (size_type I = 0; I < Count; ++I)
            fillOne(Value);
        return Count;
    }

    void popBack() {
        erase(const_iterator(this, findLastOccupied()));
    }

    // @brief 删除单个元素（墓碑式，O(1)，不搬移）。
    iterator erase(const_iterator Pos) {
        size_type Index = Pos.Index;
        ObjectPoolDetail::destroyInStorage(Slots[Index].Storage);
        setOccupied(Index, false);
        FreeSlots[FreeCount++] = Index;
        --LiveSize;
        return iterator(this, findNextOccupied(Index));
    }

    // @brief 删除区间 [First, Last)（O(区间)，不搬移）。
    iterator erase(const_iterator First, const_iterator Last) {
        size_type Stop = Last.Index;
        for (size_type Index = First.Index; Index < Stop; ++Index) {
            if (isOccupied(Index)) {
                ObjectPoolDetail::destroyInStorage(Slots[Index].Storage);
                setOccupied(Index, false);
                FreeSlots[FreeCount++] = Index;
                --LiveSize;
            }
        }
        return iterator(this, Stop);
    }

    void swap(FixedObjectPool& Other) {
        using std::swap;
        swap(Slots, Other.Slots);
        swap(Occupied, Other.Occupied);
        swap(FreeSlots, Other.FreeSlots);
        swap(FreeCount, Other.FreeCount);
        swap(LiveSize, Other.LiveSize);
    }

    // @brief 返回迭代器对应的物理槽位下标（稳定句柄）。
    size_type getSlotIndex(const_iterator Pos) const { return Pos.Index; }

    // @brief 由物理槽位下标构造迭代器。
    iterator getIterator(size_type Index) { return iterator(this, Index); }

private:
    bool isOccupied(size_type Index) const {
        return isOccupiedImpl(Index, IsTrivialType());
    }
    bool isOccupiedImpl(size_type Index, std::true_type) const {
        return Occupied[Index];
    }
    bool isOccupiedImpl(size_type Index, std::false_type) const {
        return Slots[Index].bOccupied;
    }

    void setOccupied(size_type Index, bool Value) {
        setOccupiedImpl(Index, Value, IsTrivialType());
    }
    void setOccupiedImpl(size_type Index, bool Value, std::true_type) {
        Occupied[Index] = Value;
    }
    void setOccupiedImpl(size_type Index, bool Value, std::false_type) {
        Slots[Index].bOccupied = Value;
    }

    // @brief 取得一个空闲槽位下标；满时抛 std::length_error。
    size_type acquireFreeIndex() {
        if (FreeCount == 0)
            throw std::length_error("FixedObjectPool: 已满，无空闲槽位");
        return FreeSlots[--FreeCount];
    }

    template <typename... Args>
    size_type emplaceIntoFreeSlot(Args&&... Arguments) {
        size_type Index = acquireFreeIndex();
        SlotType& SlotRef = Slots[Index];
        try {
            ObjectPoolDetail::constructInStorage(SlotRef.Storage,
                                                 std::forward<Args>(Arguments)...);
        } catch (...) {
            FreeSlots[FreeCount++] = Index;
            throw;
        }
        setOccupied(Index, true);
        ++LiveSize;
        return Index;
    }

    // @brief 不增长的单个刷入：复用现有空闲槽位构造 Value（满时抛 length_error）。
    void fillOne(const_reference Value) {
        size_type Index = acquireFreeIndex();
        SlotType& SlotRef = Slots[Index];
        try {
            ObjectPoolDetail::constructInStorage(SlotRef.Storage, Value);
        } catch (...) {
            FreeSlots[FreeCount++] = Index;
            throw;
        }
        setOccupied(Index, true);
        ++LiveSize;
    }

    size_type findFirstOccupied() const {
        size_type Index = 0;
        while (Index < Capacity && !isOccupied(Index))
            ++Index;
        return Index;
    }
    size_type findLastOccupied() const {
        size_type Index = Capacity;
        while (Index != 0 && !isOccupied(Index - 1))
            --Index;
        return Index - 1;
    }
    size_type findNextOccupied(size_type From) const {
        size_type Index = From + 1;
        while (Index < Capacity && !isOccupied(Index))
            ++Index;
        return Index;
    }
    size_type findNthOccupied(size_type Pos) const {
        size_type Index = 0;
        size_type Seen = 0;
        while (Index < Capacity) {
            if (isOccupied(Index)) {
                if (Seen == Pos)
                    break;
                ++Seen;
            }
            ++Index;
        }
        return Index;
    }
    void checkIndex(size_type Pos) const {
        if (Pos >= LiveSize)
            throw std::out_of_range("FixedObjectPool::at: index out of range");
    }

    std::array<SlotType, Capacity> Slots;
    std::bitset<Capacity> Occupied;   // @notice 仅平凡类型使用；非平凡类型读槽位内标志。
    std::array<size_type, Capacity> FreeSlots;
    size_type FreeCount;
    size_type LiveSize;
};

// @brief 类外定义：iterator -> const_iterator 隐式转换。
template <typename Type, std::size_t Capacity>
inline FixedObjectPool<Type, Capacity>::const_iterator::const_iterator(
    FixedObjectPool<Type, Capacity>::iterator const& Other)
    : Owner(Other.Owner), Index(Other.Index) {}

// @brief 类外定义：iterator 与 const_iterator 的交叉比较（委托给 const_iterator）。
template <typename Type, std::size_t Capacity>
inline bool FixedObjectPool<Type, Capacity>::iterator::operator==(
    typename FixedObjectPool<Type, Capacity>::const_iterator const& Rhs) const {
    return Rhs == *this;
}

template <typename Type, std::size_t Capacity>
inline bool FixedObjectPool<Type, Capacity>::iterator::operator!=(
    typename FixedObjectPool<Type, Capacity>::const_iterator const& Rhs) const {
    return !(Rhs == *this);
}

// @brief FixedObjectPool 相等比较（对有效元素按字典序）。
template <typename Type, std::size_t Capacity>
bool operator==(FixedObjectPool<Type, Capacity> const& Lhs,
                FixedObjectPool<Type, Capacity> const& Rhs) {
    if (Lhs.size() != Rhs.size())
        return false;
    return std::equal(Lhs.begin(), Lhs.end(), Rhs.begin());
}

template <typename Type, std::size_t Capacity>
bool operator!=(FixedObjectPool<Type, Capacity> const& Lhs,
                FixedObjectPool<Type, Capacity> const& Rhs) {
    return !(Lhs == Rhs);
}

/**
 * @brief 线程安全对象池（互斥锁保护；非无锁）。
 * @notice 提供 acquire/release 语义（无迭代器），定容、句柄稳定。
 *         真正无锁版本（Treiber 栈 + ABA 规避）作为更高级的后续可选。
 */
template <typename Type, std::size_t Capacity>
class ConcurrentObjectPool {
public:
    using value_type = Type;
    using size_type = std::size_t;
    using pointer = Type*;

    // @brief 满时 emplace 返回的哨兵值。
    static constexpr size_type NoSlot = Capacity;

private:
    mutable std::mutex Mutex;
    FixedObjectPool<Type, Capacity> Pool;

public:
    ConcurrentObjectPool() = default;

    // @brief 线程安全地构造并取得一个对象，返回其稳定槽位下标；满时返回 NoSlot。
    template <typename... Args>
    size_type emplace(Args&&... Arguments) {
        std::lock_guard<std::mutex> Lock(Mutex);
        if (Pool.full())
            return NoSlot;
        return Pool.getSlotIndex(Pool.append(std::forward<Args>(Arguments)...));
    }

    // @brief 取得下标对应的对象指针。
    // @notice 调用方须保证在 release 之前持有该下标；期间无其它线程修改该槽位。
    pointer get(size_type Index) {
        return &*Pool.getIterator(Index);
    }

    // @brief 线程安全地释放对象。
    void release(size_type Index) {
        std::lock_guard<std::mutex> Lock(Mutex);
        Pool.erase(Pool.getIterator(Index));
    }

    size_type size() const {
        std::lock_guard<std::mutex> Lock(Mutex);
        return Pool.size();
    }
    size_type capacity() const { return Capacity; }
    size_type getFreeCount() const {
        std::lock_guard<std::mutex> Lock(Mutex);
        return Pool.getFreeCount();
    }
    bool empty() const {
        std::lock_guard<std::mutex> Lock(Mutex);
        return Pool.empty();
    }
    bool full() const {
        std::lock_guard<std::mutex> Lock(Mutex);
        return Pool.full();
    }
};

/**
 * @brief 非成员比较运算符（对有效元素按字典序比较）。
 */
template <typename Type, typename Allocator,
          template <typename, typename> class Container>
bool operator==(ObjectPool<Type, Allocator, Container> const& Lhs,
                ObjectPool<Type, Allocator, Container> const& Rhs) {
    if (Lhs.size() != Rhs.size())
        return false;
    return std::equal(Lhs.begin(), Lhs.end(), Rhs.begin());
}

template <typename Type, typename Allocator,
          template <typename, typename> class Container>
bool operator!=(ObjectPool<Type, Allocator, Container> const& Lhs,
                ObjectPool<Type, Allocator, Container> const& Rhs) {
    return !(Lhs == Rhs);
}

template <typename Type, typename Allocator,
          template <typename, typename> class Container>
bool operator<(ObjectPool<Type, Allocator, Container> const& Lhs,
               ObjectPool<Type, Allocator, Container> const& Rhs) {
    return std::lexicographical_compare(Lhs.begin(), Lhs.end(), Rhs.begin(),
                                        Rhs.end());
}

template <typename Type, typename Allocator,
          template <typename, typename> class Container>
bool operator>(ObjectPool<Type, Allocator, Container> const& Lhs,
               ObjectPool<Type, Allocator, Container> const& Rhs) {
    return Rhs < Lhs;
}

template <typename Type, typename Allocator,
          template <typename, typename> class Container>
bool operator<=(ObjectPool<Type, Allocator, Container> const& Lhs,
                ObjectPool<Type, Allocator, Container> const& Rhs) {
    return !(Rhs < Lhs);
}

template <typename Type, typename Allocator,
          template <typename, typename> class Container>
bool operator>=(ObjectPool<Type, Allocator, Container> const& Lhs,
                ObjectPool<Type, Allocator, Container> const& Rhs) {
    return !(Lhs < Rhs);
}

/**
 * @brief std::swap 支持。
 */
template <typename Type, typename Allocator,
          template <typename, typename> class Container>
void swap(ObjectPool<Type, Allocator, Container>& Lhs,
          ObjectPool<Type, Allocator, Container>& Rhs) noexcept(noexcept(Lhs.swap(Rhs))) {
    Lhs.swap(Rhs);
}

#endif // OBJECTPOOL_OBJECTPOOL_HPP
