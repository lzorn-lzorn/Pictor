//
// IntrusiveRBTree.hpp
//
// 侵入式红黑树, 语义对齐 std::set（键唯一、有序迭代、O(log n) 增删查）。
// 与 std::set 的关键区别：容器不拥有节点, 节点内存与生命周期由调用方管理。
//
// 对 T 的要求：
//   * T 必须公开继承 RBNodeBase；
//   * T 无需可复制/可移动（容器从不复制/移动元素; 即使 T 可复制, 副本也是未链接状态, 需自行 insert）。元素在容器内期间地址必须稳定、且生命周期必须长于容器;
//   * 节点生命周期必须长于容器：节点须先于容器销毁前被移除或由调用方释放；
//   * T 需可被 CompareCallable 排序；默认 std::less<> 为透明比较器, 支持异构查找
//     （find/getLowerBound/...）。
//
// 线程安全：容器非线程安全。并发修改, 或在另一线程读取时修改, 均为未定义行为。
//

#ifndef PICTOR_INTRUSIVERBTREE_H
#define PICTOR_INTRUSIVERBTREE_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iterator>
#include <type_traits>
#include <utility>

/**
 * @brief 侵入式红黑树节点基类。
 * @notice 父指针与红/黑颜色打包进同一个指针大小的整数, 低 bit 存颜色
 *         （对齐 boost::intrusive 的 compact 红黑节点表示）；因此要求
 *         指针至少 2 字节对齐（见文件尾 static_assert）。
 */
class RBNodeBase
{
public:
    static constexpr bool Red   = false;   ///< 红色（颜色位 = 0）
    static constexpr bool Black = true;    ///< 黑色（颜色位 = 1）

    RBNodeBase() = default;

    /**
     * @brief 拷贝构造：副本为未链接状态（不复制任何链接关系，与 boost hook 同款）。
     * @param Other 被忽略（副本不会继承源的链接关系）。
     * @notice 副本需自行 insert；这正是 Boost.Intrusive hook 的拷贝语义。
     */
    RBNodeBase(RBNodeBase const& /*Other*/) noexcept { reset(); }

    /**
     * @brief 拷贝赋值：空操作（与 boost hook 同款，参数被忽略）。
     * @param Other 被忽略。
     * @return *this。
     */
    RBNodeBase& operator=(RBNodeBase const& /*Other*/) noexcept { return *this; }

    /**
     * @brief 返回父节点指针（已剥离颜色位）。
     * @return 父节点指针；空树中的 header 或独立节点可能为 nullptr。
     */
    [[nodiscard]] RBNodeBase* Parent() const noexcept
    {
        return reinterpret_cast<RBNodeBase*>(ParentColor & ~static_cast<std::uintptr_t>(1));
    }

    /** @brief 是否为黑色节点。 @return true 表示黑色。 */
    [[nodiscard]] bool isBlack() const noexcept { return (ParentColor & 1) != 0; }
    /** @brief 是否为红色节点。 @return true 表示红色。 */
    [[nodiscard]] bool isRed()   const noexcept { return !isBlack(); }

    /**
     * @brief 设置父节点, 同时保留颜色位不变。
     * @param Parent 新的父节点指针。
     * @notice 只改写父指针高位, 颜色位（低 bit）原样保留。
     */
    void setParent(RBNodeBase* Parent) noexcept
    {
        ParentColor = (ParentColor & static_cast<std::uintptr_t>(1)) |
                      reinterpret_cast<std::uintptr_t>(Parent);
    }

    /**
     * @brief 设置节点颜色, 同时保留父指针不变。
     * @param IsBlack true 表示黑色, false 表示红色。
     */
    void setColor(bool IsBlack) noexcept
    {
        ParentColor = (ParentColor & ~static_cast<std::uintptr_t>(1)) |
                      (IsBlack ? static_cast<std::uintptr_t>(1) : static_cast<std::uintptr_t>(0));
    }

    /**
     * @brief 将节点复位为干净、未链接、红色的可复用状态。
     * @notice 清空 left/right 并把 parent+颜色归零, 供 erase/clear 后再次插入。
     */
    void reset() noexcept
    {
        Left        = nullptr;
        Right       = nullptr;
        ParentColor = 0;
    }

    RBNodeBase* Left  = { nullptr };   ///< 左孩子
    RBNodeBase* Right = { nullptr };   ///< 右孩子

private:
    std::uintptr_t ParentColor = 0;
};

static_assert(alignof(RBNodeBase) >= 2, "pointer alignment must provide a free low bit for color");

namespace rbtree_detail
{
    /**
     * @brief 判断节点是否为 header 哨兵。
     * @param Node 待判断的节点。
     * @return true 表示是 header 节点。
     * @notice header 是唯一满足「红色且 parent(parent)==自身」的节点（树根恒为黑）。
     */
    inline bool isHeader(RBNodeBase const* Node) noexcept
    {
        RBNodeBase const* Parent = Node->Parent();
        return Node->isRed() && (!Parent || Parent->Parent() == Node);
    }

    /**
     * @brief 返回中序后继。
     * @param X 当前节点（可为 header 哨兵）。
     * @return 后继节点指针；在 header 上调用返回 header 自身（即 end()）。
     */
    template <typename NodePtr>
    NodePtr increase(NodePtr X) noexcept
    {
        if (isHeader(X))
        {
            return X;
        }
        if (X->Right)
        {
            X = X->Right;
            while (X->Left)
            {
                X = X->Left;
            }
            return X;
        }
        RBNodeBase* Y = X->Parent();
        while (X == Y->Right)
        {
            X = Y;
            Y = Y->Parent();
        }
        if (X->Right != Y)
        {
            X = Y;
        }
        return X;
    }

    /**
     * @brief 返回中序前驱。
     * @param X 当前节点（可为 header 哨兵）。
     * @return 前驱节点指针；在 header 上调用返回最大元素。
     */
    template <typename NodePtr>
    NodePtr decrease(NodePtr X) noexcept
    {
        if (isHeader(X))
        {
            return X->Right;
        }
        if (X->Left)
        {
            X = X->Left;
            while (X->Right)
            {
                X = X->Right;
            }
            return X;
        }
        RBNodeBase* Y = X->Parent();
        while (X == Y->Left)
        {
            X = Y;
            Y = Y->Parent();
        }
        return Y;
    }
}

/**
 * @brief 侵入式红黑树：键唯一、有序迭代, 语义对齐 std::set。
 * @notice 容器不拥有节点；插入会重置节点链接, 被 erase/clear 过的节点可再次插入；
 *         容器非线程安全。
 */
template <typename T, typename CompareCallable = std::less<>>
class IntrusiveRBTree
{
    static_assert(std::is_base_of_v<RBNodeBase, T>, "T must publicly derive from RBNodeBase");

public:
    // ---- 类型别名 ----
    using value_type      = T;
    using key_type        = T;
    using size_type       = std::size_t;
    using difference_type = std::ptrdiff_t;
    using key_compare     = CompareCallable;
    using value_compare   = CompareCallable;
    using reference       = T&;
    using const_reference = T const&;
    using pointer         = T*;
    using const_pointer   = T const*;

    template <bool IsConst> class Iterator;
    using iterator               = Iterator<false>;
    using const_iterator         = Iterator<true>;
    using reverse_iterator       = std::reverse_iterator<iterator>;
    using const_reverse_iterator = std::reverse_iterator<const_iterator>;

    // ---- 构造 ----

    /** @brief 默认构造：创建一棵空树。 */
    IntrusiveRBTree() noexcept { initHeader(); }

    /**
     * @brief 用指定比较器构造空树。
     * @param Comp 比较器（决定排序方向与等价判定）。
     */
    explicit IntrusiveRBTree(CompareCallable Comp) noexcept : Compare(std::move(Comp)) { initHeader(); }

    IntrusiveRBTree(IntrusiveRBTree const&) = delete;
    IntrusiveRBTree(IntrusiveRBTree&&) = delete;
    IntrusiveRBTree& operator=(IntrusiveRBTree const&) = delete;
    IntrusiveRBTree& operator=(IntrusiveRBTree&&) = delete;

    // ---- 容量 ----

    /** @brief 是否为空。 @return true 表示不含任何元素。 */
    [[nodiscard]] bool empty() const noexcept { return root() == nullptr; }
    /** @brief 元素个数。 @return 当前树中元素数量。 */
    [[nodiscard]] size_type size() const noexcept { return Size; }

    // ---- 观察器 ----

    /** @brief 返回键比较器（副本）。 @return 用于比较键的可调用对象。 */
    key_compare   getKeyCompare()   const { return Compare; }
    /** @brief 返回值比较器（副本）。 @return 用于比较值的可调用对象。 */
    value_compare getValueCompare() const { return Compare; }

    // ---- 迭代器 ----

    /** @brief 指向最小元素。 */
    iterator       begin()       noexcept { return iterator(Header.Left); }
    /** @brief 指向最小元素（const）。 */
    const_iterator begin() const noexcept { return const_iterator(Header.Left); }
    /** @brief 指向最小元素（const）。 */
    const_iterator cbegin() const noexcept { return const_iterator(Header.Left); }
    /** @brief 尾后迭代器（header 哨兵）。 */
    iterator       end()         noexcept { return iterator(&Header); }
    /** @brief 尾后迭代器（const）。 */
    const_iterator end()   const noexcept { return const_iterator(&Header); }
    /** @brief 尾后迭代器（const）。 */
    const_iterator cend()   const noexcept { return const_iterator(&Header); }

    /** @brief 反向起始（指向最大元素）。 */
    reverse_iterator       rbegin()       noexcept { return reverse_iterator(end()); }
    /** @brief 反向起始（const）。 */
    const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
    /** @brief 反向起始（const）。 */
    const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
    /** @brief 反向尾后（指向最小元素之前）。 */
    reverse_iterator       rend()         noexcept { return reverse_iterator(begin()); }
    /** @brief 反向尾后（const）。 */
    const_reverse_iterator rend()   const noexcept { return const_reverse_iterator(begin()); }
    /** @brief 反向尾后（const）。 */
    const_reverse_iterator crend()   const noexcept { return const_reverse_iterator(cbegin()); }

    // ---- 修改 ----

    /**
     * @brief 插入元素（键唯一）。
     * @param Value 待插入的节点（T 派生自 RBNodeBase）。
     * @return 插入成功返回 {迭代器, true}；键已存在返回 {指向已有节点的迭代器, false}。
     * @notice 插入会重置节点链接, 被 erase/clear 过的节点可直接再次插入。
     */
    std::pair<iterator, bool> insert(T& Value)
    {
        RBNodeBase* Parent  = &Header;
        RBNodeBase* Current = root();
        bool GoLeft = false;

        while (Current)
        {
            Parent = Current;
            T const& CurrentValue = *static_cast<T const*>(Current);
            if (isLess(Value, CurrentValue))
            {
                GoLeft = true;
                Current = Current->Left;
            }
            else if (isLess(CurrentValue, Value))
            {
                GoLeft = false;
                Current = Current->Right;
            }
            else
            {
                return { iterator(Current), false };
            }
        }

        RBNodeBase& Node = Value;
        Node.Left = nullptr;
        Node.Right = nullptr;
        Node.setParent(Parent);
        Node.setColor(RBNodeBase::Red);

        if (Parent == &Header)
        {
            Header.setParent(&Node);
            Header.Left = &Node;
            Header.Right = &Node;
        }
        else if (GoLeft)
        {
            Parent->Left = &Node;
            if (Header.Left == Parent)
            {
                Header.Left = &Node;
            }
        }
        else
        {
            Parent->Right = &Node;
            if (Header.Right == Parent)
            {
                Header.Right = &Node;
            }
        }

        insertFixup(&Node);
        ++Size;
        return { iterator(&Node), true };
    }

    /**
     * @brief 带提示位置的插入。
     * @param Hint 插入位置提示（最佳努力, 当前忽略）。
     * @param Value 待插入的节点。
     * @return 指向新插入元素（或键已存在时的既有元素）的迭代器。
     */
    iterator insert(const_iterator /*Hint*/, T& Value)
    {
        return insert(Value).first;
    }

    /**
     * @brief 删除迭代器所指元素。
     * @param Pos 指向待删元素的迭代器（对 end() 为无效操作）。
     * @return 指向被删元素之后元素的迭代器。
     * @notice 只使指向被删节点的迭代器失效, 其余迭代器保持有效。
     */
    iterator erase(iterator Pos) noexcept
    {
        if (Pos.Node == &Header)
        {
            return Pos;
        }
        iterator Next = Pos;
        ++Next;
        RBNodeBase* Removed = eraseNode(Pos.Node);
        Removed->reset();
        --Size;
        return Next;
    }

    /**
     * @brief 删除 const 迭代器所指元素（容器非 const 时可安全删除）。
     * @param Pos 指向待删元素的 const 迭代器。
     * @return 指向被删元素之后元素的迭代器。
     */
    iterator erase(const_iterator Pos) noexcept
    {
        return erase(iterator(const_cast<RBNodeBase*>(Pos.Node)));
    }

    /**
     * @brief 删除 [First, Last) 区间内的所有元素。
     * @param First 区间起始迭代器。
     * @param Last 区间尾后迭代器。
     * @return 指向 Last 的迭代器。
     */
    iterator erase(const_iterator First, const_iterator Last) noexcept
    {
        while (First != Last)
        {
            First = erase(First);
        }
        return iterator(const_cast<RBNodeBase*>(Last.Node));
    }

    /**
     * @brief 按键删除元素。
     * @param Key 待删除的键（异构：可用裸键类型, 需透明比较器支持）。
     * @return 实际删除的元素个数（0 或 1）。
     */
    template <typename KeyType>
    size_type erase(KeyType const& Key)
    {
        iterator It = find(Key);
        if (It == end())
        {
            return 0;
        }
        erase(It);
        return 1;
    }
    
    /**
     * @brief 清空容器, 并将每个被移除节点复位为干净可复用状态。
     * @notice 不调用析构, 节点所有权仍归调用方；复杂度 O(n log n)。
     */
    void clear() noexcept
    {
        while (!empty())
        {
            erase(begin());
        }
    }
    
    /**
     * @brief 清空容器, 并对每个元素依次调用 Fn(&node)。
     * @param Fn 释放器, 签名为 void(T*)。
     * @notice 回调顺序未指定；用于需要由容器代为释放节点的场景。
     */
    template <typename Disposer>
    void clear_and_dispose(Disposer Fn)
    {
        while (!empty())
        {
            iterator It = begin();
            T* Value = &*It;
            erase(It);
            Fn(Value);
        }
    }

    /**
     * @brief 与另一棵树交换全部内容（含比较器与 size）, O(1)。
     * @param Other 要交换的另一棵树。
     * @notice 节点地址不变, 仅改挂到另一棵树的 header 上。
     */
    void swap(IntrusiveRBTree& Other) noexcept
    {
        RBNodeBase* ThisRoot  = root();
        RBNodeBase* OtherRoot = Other.root();

        std::swap(Header.Left, Other.Header.Left);
        std::swap(Header.Right, Other.Header.Right);
       
        Header.setParent(OtherRoot);
        Other.Header.setParent(ThisRoot);
        std::swap(Size, Other.Size);
        std::swap(Compare, Other.Compare);
        
        if (ThisRoot)
        {
            ThisRoot->setParent(&Other.Header);
        }
        if (OtherRoot)
        {
            OtherRoot->setParent(&Header);
        }
    }

    // ---- 查找（CompareCallable 透明时支持异构） ----

    /**
     * @brief 查找键等于 Key 的元素。
     * @param Key 待查找的键（异构：可用裸键类型）。
     * @return 指向匹配元素的迭代器；未找到返回 end()。
     */
    template <typename KeyType>
    iterator find(KeyType const& Key) noexcept
    {
        return iterator(const_cast<RBNodeBase*>(findNode(Key)));
    }

    /**
     * @brief 查找键等于 Key 的元素（const 版本）。
     * @param Key 待查找的键。
     * @return 指向匹配元素的 const 迭代器；未找到返回 end()。
     */
    template <typename KeyType>
    const_iterator find(KeyType const& Key) const noexcept
    {
        return const_iterator(findNode(Key));
    }

    /**
     * @brief 统计键等于 Key 的元素个数（键唯一, 故为 0 或 1）。
     * @param Key 待统计的键。
     * @return 0 或 1。
     */
    template <typename KeyType>
    size_type count(KeyType const& Key) const noexcept
    {
        return findNode(Key) == &Header ? 0 : 1;
    }

    /**
     * @brief 判断是否包含键等于 Key 的元素。
     * @param Key 待判断的键。
     * @return true 表示存在。
     */
    template <typename KeyType>
    bool contains(KeyType const& Key) const noexcept
    {
        return findNode(Key) != &Header;
    }

    /**
     * @brief 返回第一个「不小于 Key」的元素（下界）。
     * @param Key 待查找的键。
     * @return 指向下界的迭代器；无下界返回 end()。
     */
    template <typename KeyType>
    iterator getLowerBound(KeyType const& Key) noexcept
    {
        return iterator(const_cast<RBNodeBase*>(getLowerNode(Key)));
    }

    /**
     * @brief 下界（const 版本）。
     * @param Key 待查找的键。
     * @return 指向下界的 const 迭代器；无下界返回 end()。
     */
    template <typename KeyType>
    const_iterator getLowerBound(KeyType const& Key) const noexcept
    {
        return const_iterator(getLowerNode(Key));
    }

    /**
     * @brief 返回第一个「大于 Key」的元素（上界）。
     * @param Key 待查找的键。
     * @return 指向上界的迭代器；无上界返回 end()。
     */
    template <typename KeyType>
    iterator getUpperBound(KeyType const& Key) noexcept
    {
        return iterator(const_cast<RBNodeBase*>(getUpperNode(Key)));
    }

    /**
     * @brief 上界（const 版本）。
     * @param Key 待查找的键。
     * @return 指向上界的 const 迭代器；无上界返回 end()。
     */
    template <typename KeyType>
    const_iterator getUpperBound(KeyType const& Key) const noexcept
    {
        return const_iterator(getUpperNode(Key));
    }

    /**
     * @brief 返回键等于 Key 的区间 [下界, 上界)。
     * @param Key 待查找的键。
     * @return 一对迭代器；键唯一时区间最多含一个元素。
     */
    template <typename KeyType>
    std::pair<iterator, iterator> getEqualRange(KeyType const& Key) noexcept
    {
        return {
            iterator(const_cast<RBNodeBase*>(getLowerNode(Key))),
            iterator(const_cast<RBNodeBase*>(getUpperNode(Key)))
        };
    }

    /**
     * @brief 等值区间（const 版本）。
     * @param Key 待查找的键。
     * @return 一对 const 迭代器。
     */
    template <typename KeyType>
    std::pair<const_iterator, const_iterator> getEqualRange(KeyType const& Key) const noexcept
    {
        return { const_iterator(getLowerNode(Key)), const_iterator(getUpperNode(Key)) };
    }

    // ---- 非成员比较（hidden friend） ----

    /**
     * @brief 判等：两树大小相同且按序元素两两等价。
     * @param L 左树。
     * @param R 右树。
     * @return true 表示相等。
     * @notice 等价判定用排序关系 !(a<b)&&!(b<a), 仅要求 operator<。
     */
    friend bool operator==(IntrusiveRBTree const& L, IntrusiveRBTree const& R)
    {
        if (L.Size != R.Size)
        {
            return false;
        }
        auto Li = L.begin();
        auto Ri = R.begin();
        for (auto Le = L.end(); Li != Le; ++Li, ++Ri)
        {
            // Equivalence under the ordering relation: !(a<b) && !(b<a).
            if (L.isLess(*Li, *Ri) || L.isLess(*Ri, *Li))
            {
                return false;
            }
        }
        return true;
    }
    /** @brief 不等比较。 */
    friend bool operator!=(IntrusiveRBTree const& L, IntrusiveRBTree const& R) { return !(L == R); }
    /** @brief 字典序小于比较。 */
    friend bool operator<(IntrusiveRBTree const& L, IntrusiveRBTree const& R)
    {
        return std::lexicographical_compare(L.begin(), L.end(), R.begin(), R.end(), L.Compare);
    }
    /** @brief 大于比较。 */
    friend bool operator>(IntrusiveRBTree const& L, IntrusiveRBTree const& R) { return R < L; }
    /** @brief 小于等于比较。 */
    friend bool operator<=(IntrusiveRBTree const& L, IntrusiveRBTree const& R) { return !(R < L); }
    /** @brief 大于等于比较。 */
    friend bool operator>=(IntrusiveRBTree const& L, IntrusiveRBTree const& R) { return !(L < R); }

    // ---- 迭代器 ----

    /**
     * @brief 双向迭代器；IsConst=true 为 const_iterator, 否则为 iterator。
     * @notice iterator 可隐式转换为 const_iterator, 反之不可。
     */
    template <bool IsConst>
    class Iterator
    {
        friend class IntrusiveRBTree;

        using NodePtrType = std::conditional_t<IsConst, RBNodeBase const*, RBNodeBase*>;

        NodePtrType Node = nullptr;

        explicit Iterator(NodePtrType Ptr) noexcept : Node(Ptr) {}

    public:
        using iterator_category = std::bidirectional_iterator_tag;
        using value_type        = T;
        using difference_type   = std::ptrdiff_t;
        using pointer           = std::conditional_t<IsConst, T const*, T*>;
        using reference         = std::conditional_t<IsConst, T const&, T&>;

        /** @brief 默认构造：产生未关联任何节点的迭代器。 */
        Iterator() = default;

        /** @brief 从 iterator 到 const_iterator 的隐式转换（反向禁止）。 */
        template <bool OtherConst, std::enable_if_t<IsConst && !OtherConst, int> = 0>
        Iterator(Iterator<OtherConst> const& Other) noexcept : Node(Other.Node) {}

        /** @brief 解引用, 返回 T&（const 迭代器返回 T const&）。 */
        reference operator*()  const noexcept { return *static_cast<pointer>(Node); }
        /** @brief 成员访问, 返回 T*（const 迭代器返回 T const*）。 */
        pointer   operator->() const noexcept { return  static_cast<pointer>(Node); }

        /** @brief 前置自增：移动到中序后继。 @return 自身引用。 */
        Iterator& operator++()    noexcept { Node = rbtree_detail::increase(Node); return *this; }
        /** @brief 后置自增：返回旧值副本。 */
        Iterator  operator++(int) noexcept { Iterator Tmp = *this; ++(*this); return Tmp; }
        /** @brief 前置自减：移动到中序前驱。 @return 自身引用。 */
        Iterator& operator--()    noexcept { Node = rbtree_detail::decrease(Node); return *this; }
        /** @brief 后置自减：返回旧值副本。 */
        Iterator  operator--(int) noexcept { Iterator Tmp = *this; --(*this); return Tmp; }

        /** @brief 相等比较（支持 iterator 与 const_iterator 互比）。 */
        template <bool OtherConst>
        bool operator==(Iterator<OtherConst> const& Other) const noexcept { return Node == Other.Node; }
        /** @brief 不等比较。 */
        template <bool OtherConst>
        bool operator!=(Iterator<OtherConst> const& Other) const noexcept { return Node != Other.Node; }
    };
    
    /**
     * @brief 校验全部红黑不变量, O(n), 不抛异常。
     * @return true 表示结构合法。
     * @notice 校验根黑、无红红、黑高相等、父子指针一致、size 与实际节点数一致。
     */
    [[nodiscard]] bool validate() const noexcept
    {
        RBNodeBase const* R = root();
        if (R && R->isRed())
        {
            return false;
        }
        bool Ok = true;
        std::size_t Count = 0;
        blackHeight(R, Count, Ok);
        return Ok && Count == Size;
    }

private:
    /** @brief 将 header 初始化为空树状态。 */
    void initHeader() noexcept
    {
        Header.reset();
        Header.Left = &Header;
        Header.Right = &Header;
        Size = 0;
    }

    /** @brief 返回树根（header 的父指针）, 空树为 nullptr。 */
    RBNodeBase*       root()       noexcept { return Header.Parent(); }
    /** @brief 返回树根（const 版本）。 */
    RBNodeBase const* root() const noexcept { return Header.Parent(); }

    /** @brief 用比较器比较 L 与 R（异构）, 等价于 Compare(L, R)。 */
    template <typename A, typename B>
    bool isLess(A const& L, B const& R) const { return Compare(L, R); }

    /** @brief 查找节点：返回匹配节点, 未找到返回 &Header（即 end()）。 */
    template <typename KeyType>
    RBNodeBase const* findNode(KeyType const& Key) const noexcept
    {
        RBNodeBase const* Current = root();
        while (Current)
        {
            T const& Value = *static_cast<T const*>(Current);
            if (isLess(Key, Value))
            {
                Current = Current->Left;
            }
            else if (isLess(Value, Key))
            {
                Current = Current->Right;
            }
            else
            {
                return Current;
            }
        }
        return &Header;
    }

    /** @brief 返回第一个「不小于 Key」的节点, 无则返回 &Header。 */
    template <typename KeyType>
    RBNodeBase const* getLowerNode(KeyType const& Key) const noexcept
    {
        RBNodeBase const* Current = root();
        RBNodeBase const* Result = &Header;
        while (Current)
        {
            T const& Value = *static_cast<T const*>(Current);
            if (!isLess(Value, Key))   // Value >= Key
            {
                Result = Current;
                Current = Current->Left;
            }
            else
            {
                Current = Current->Right;
            }
        }
        return Result;
    }

    /** @brief 返回第一个「大于 Key」的节点, 无则返回 &Header。 */
    template <typename KeyType>
    RBNodeBase const* getUpperNode(KeyType const& Key) const noexcept
    {
        RBNodeBase const* Current = root();
        RBNodeBase const* Result = &Header;
        while (Current)
        {
            T const& Value = *static_cast<T const*>(Current);
            if (isLess(Key, Value))   // Key < Value  =>  Value > Key
            {
                Result = Current;
                Current = Current->Left;
            }
            else
            {
                Current = Current->Right;
            }
        }
        return Result;
    }

    /** @brief 以 X 为根的子树最小值。 */
    static RBNodeBase* minimum(RBNodeBase* X) noexcept
    {
        while (X->Left)
        {
            X = X->Left;
        }
        return X;
    }

    /** @brief 以 X 为根的子树最大值。 */
    static RBNodeBase* maximum(RBNodeBase* X) noexcept
    {
        while (X->Right)
        {
            X = X->Right;
        }
        return X;
    }

    /**
     * @brief 递归校验红黑性质并返回黑高；违规时置 Ok=false。
     * @param N 当前子树根。
     * @param Count 累计节点数（用于核对 size）。
     * @param Ok 校验结果（输出）。
     * @return 子树黑高（空节点按 1 计）。
     */
    static std::ptrdiff_t blackHeight(RBNodeBase const* N, std::size_t& Count, bool& Ok) noexcept
    {
        if (!N)
        {
            return 1;
        }
        ++Count;
        if (N->Left  && N->Left->Parent()  != N) Ok = false;
        if (N->Right && N->Right->Parent() != N) Ok = false;

        std::ptrdiff_t L = blackHeight(N->Left, Count, Ok);
        std::ptrdiff_t R = blackHeight(N->Right, Count, Ok);
        if (L != R)
        {
            Ok = false;
        }
        if (N->isRed() && ((N->Left && N->Left->isRed()) || (N->Right && N->Right->isRed())))
        {
            Ok = false;
        }
        return L + (N->isBlack() ? 1 : 0);
    }

    /** @brief 左旋 X（要求 X 有右孩子）。 */
    void rotateLeft(RBNodeBase* X) noexcept
    {
        RBNodeBase* R = X->Right;
        RBNodeBase* P = X->Parent();

        X->Right = R->Left;
        if (R->Left)
        {
            R->Left->setParent(X);
        }

        R->setParent(P);
        if (X == root())
        {
            Header.setParent(R);
        }
        else if (P->Left == X)
        {
            P->Left = R;
        }
        else
        {
            P->Right = R;
        }

        R->Left = X;
        X->setParent(R);
    }

    /** @brief 右旋 X（要求 X 有左孩子）。 */
    void rotateRight(RBNodeBase* X) noexcept
    {
        RBNodeBase* L = X->Left;
        RBNodeBase* P = X->Parent();

        X->Left = L->Right;
        if (L->Right)
        {
            L->Right->setParent(X);
        }

        L->setParent(P);
        if (X == root())
        {
            Header.setParent(L);
        }
        else if (P->Right == X)
        {
            P->Right = L;
        }
        else
        {
            P->Left = L;
        }

        L->Right = X;
        X->setParent(L);
    }

    /** @brief 插入后修复红黑性质（CLRS 的 RB-INSERT-FIXUP）。 */
    void insertFixup(RBNodeBase* X) noexcept
    {
        while (X != root() && X->Parent()->isRed())
        {
            RBNodeBase* P = X->Parent();
            RBNodeBase* G = P->Parent();
            if (P == G->Left)
            {
                RBNodeBase* U = G->Right;
                if (U && U->isRed())
                {
                    P->setColor(RBNodeBase::Black);
                    U->setColor(RBNodeBase::Black);
                    G->setColor(RBNodeBase::Red);
                    X = G;
                }
                else
                {
                    if (X == P->Right)
                    {
                        X = P;
                        rotateLeft(X);
                        P = X->Parent();
                        G = P->Parent();
                    }
                    P->setColor(RBNodeBase::Black);
                    G->setColor(RBNodeBase::Red);
                    rotateRight(G);
                    break;
                }
            }
            else
            {
                RBNodeBase* U = G->Left;
                if (U && U->isRed())
                {
                    P->setColor(RBNodeBase::Black);
                    U->setColor(RBNodeBase::Black);
                    G->setColor(RBNodeBase::Red);
                    X = G;
                }
                else
                {
                    if (X == P->Left)
                    {
                        X = P;
                        rotateRight(X);
                        P = X->Parent();
                        G = P->Parent();
                    }
                    P->setColor(RBNodeBase::Black);
                    G->setColor(RBNodeBase::Red);
                    rotateLeft(G);
                    break;
                }
            }
        }
        root()->setColor(RBNodeBase::Black);
    }

    /**
     * @brief 从树中摘除 Z 并恢复红黑平衡, 返回 Z（已摘除的节点）。
     * @param Z 待摘除节点。
     * @return 被摘除的节点（即 Z 本身）。
     * @notice 采用 CLRS「RB-DELETE」：后继 relink 而非拷贝, 与 boost::intrusive 一致。
     */
    RBNodeBase* eraseNode(RBNodeBase* Z) noexcept
    {
        RBNodeBase* Y = Z;
        RBNodeBase* X = nullptr;
        RBNodeBase* ParentX = nullptr;

        if (Z->Left == nullptr)
        {
            X = Z->Right;
        }
        else if (Z->Right == nullptr)
        {
            X = Z->Left;
        }
        else
        {
            Y = Z->Right;
            while (Y->Left)
            {
                Y = Y->Left;
            }
            X = Y->Right;
        }

        if (Y != Z)
        {
            // Relink successor Y into Z's position; Z is the node that leaves.
            Z->Left->setParent(Y);
            Y->Left = Z->Left;

            if (Y != Z->Right)
            {
                ParentX = Y->Parent();
                if (X)
                {
                    X->setParent(Y->Parent());
                }
                Y->Parent()->Left = X;
                Y->Right = Z->Right;
                Z->Right->setParent(Y);
            }
            else
            {
                ParentX = Y;
            }

            if (root() == Z)
            {
                Header.setParent(Y);
            }
            else if (Z->Parent()->Left == Z)
            {
                Z->Parent()->Left = Y;
            }
            else
            {
                Z->Parent()->Right = Y;
            }
            Y->setParent(Z->Parent());

            bool BlackY = Y->isBlack();
            Y->setColor(Z->isBlack());
            Z->setColor(BlackY);
            Y = Z;
        }
        else
        {
            ParentX = Y->Parent();
            if (X)
            {
                X->setParent(Y->Parent());
            }
            if (root() == Z)
            {
                Header.setParent(X);
            }
            else if (Z->Parent()->Left == Z)
            {
                Z->Parent()->Left = X;
            }
            else
            {
                Z->Parent()->Right = X;
            }

            if (Header.Left == Z)
            {
                Header.Left = (Z->Right == nullptr) ? Z->Parent() : minimum(X);
            }
            if (Header.Right == Z)
            {
                Header.Right = (Z->Left == nullptr) ? Z->Parent() : maximum(X);
            }
        }

        if (Y->isBlack())
        {
            while (X != root() && (X == nullptr || X->isBlack()))
            {
                if (X == ParentX->Left)
                {
                    RBNodeBase* W = ParentX->Right;
                    if (W->isRed())
                    {
                        W->setColor(RBNodeBase::Black);
                        ParentX->setColor(RBNodeBase::Red);
                        rotateLeft(ParentX);
                        W = ParentX->Right;
                    }
                    if ((W->Left == nullptr || W->Left->isBlack()) &&
                        (W->Right == nullptr || W->Right->isBlack()))
                    {
                        W->setColor(RBNodeBase::Red);
                        X = ParentX;
                        ParentX = ParentX->Parent();
                    }
                    else
                    {
                        if (W->Right == nullptr || W->Right->isBlack())
                        {
                            W->Left->setColor(RBNodeBase::Black);
                            W->setColor(RBNodeBase::Red);
                            rotateRight(W);
                            W = ParentX->Right;
                        }
                        W->setColor(ParentX->isBlack());
                        ParentX->setColor(RBNodeBase::Black);
                        if (W->Right)
                        {
                            W->Right->setColor(RBNodeBase::Black);
                        }
                        rotateLeft(ParentX);
                        break;
                    }
                }
                else
                {
                    RBNodeBase* W = ParentX->Left;
                    if (W->isRed())
                    {
                        W->setColor(RBNodeBase::Black);
                        ParentX->setColor(RBNodeBase::Red);
                        rotateRight(ParentX);
                        W = ParentX->Left;
                    }
                    if ((W->Right == nullptr || W->Right->isBlack()) &&
                        (W->Left == nullptr || W->Left->isBlack()))
                    {
                        W->setColor(RBNodeBase::Red);
                        X = ParentX;
                        ParentX = ParentX->Parent();
                    }
                    else
                    {
                        if (W->Left == nullptr || W->Left->isBlack())
                        {
                            W->Right->setColor(RBNodeBase::Black);
                            W->setColor(RBNodeBase::Red);
                            rotateLeft(W);
                            W = ParentX->Left;
                        }
                        W->setColor(ParentX->isBlack());
                        ParentX->setColor(RBNodeBase::Black);
                        if (W->Left)
                        {
                            W->Left->setColor(RBNodeBase::Black);
                        }
                        rotateRight(ParentX);
                        break;
                    }
                }
            }
            if (X)
            {
                X->setColor(RBNodeBase::Black);
            }
        }
        return Y;
    }

private:
    RBNodeBase Header;
    size_type Size = { 0 };
    [[no_unique_address]] CompareCallable Compare;
};

/**
 * @brief ADL 自由函数 swap。
 * @param L 左树。
 * @param R 右树。
 */
template <typename T, typename CompareCallable>
void swap(IntrusiveRBTree<T, CompareCallable>& L, IntrusiveRBTree<T, CompareCallable>& R) noexcept
{
    L.swap(R);
}

#endif // PICTOR_INTRUSIVERBTREE_H
