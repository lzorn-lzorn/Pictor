// Tests for infra/containers/IntrusiveRBTree.hpp.
//
// IntrusiveRBTree 是侵入式红黑树（语义对齐 std::set：键唯一、有序迭代、
// O(log n) 增删查）。节点内存与生命周期由调用方管理；容器从不拷贝/移动元素。
// 本文件验证：
//   * 全 API（insert/带提示 insert/erase(iterator/const_iterator/key/range)/
//     find/count/contains/lowerBound/upperBound/equalRange/clear/clear_and_dispose/swap）
//   * 空树/单元素/极端键/移动不可用节点（不可拷贝不可移动）等边界输入
//   * 上下界与等值区间的精确语义（键存在/不存在两种情况）
//   * 节点地址稳定性（侵入式容器核心承诺）
//   * 红黑不变量校验（validate）+ 多种子随机差分测试（对照 std::set，含每步
//     不变量校验与每 4096 步全序列比对）
//   * 升序/降序插入压力、透明键查找（KeyOfValue）、自定义比较器、
//     关系运算符、swap 边界、ConstantTimeSize=false 的线性 size

#include "TestFramework.h"

#include <infra/containers/IntrusiveRBTree.hpp>

#include <algorithm>
#include <climits>
#include <functional>
#include <list>
#include <random>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

namespace
{
    // 以 int 为键的最小值类型。
    struct Item : RBNodeBase
    {
        int Key = 0;
        bool operator<(Item const& Other) const { return Key < Other.Key; }
    };

    bool operator<(Item const& A, int B)
    {
        return A.Key < B;
    }
    bool operator<(int A, Item const& B)
    {
        return A < B.Key;
    }

    // 不可拷贝、不可移动的节点：侵入式容器不需要元素可复制/可移动。
    struct NonCopyItem : RBNodeBase
    {
        int Key = 0;
        explicit NonCopyItem(int K = 0) : Key(K) {}
        NonCopyItem(const NonCopyItem&) = delete;
        NonCopyItem(NonCopyItem&&) = delete;
        NonCopyItem& operator=(const NonCopyItem&) = delete;
        NonCopyItem& operator=(NonCopyItem&&) = delete;
        bool operator<(const NonCopyItem& Other) const { return Key < Other.Key; }
    };

    bool operator<(int A, const NonCopyItem& B)
    {
        return A < B.Key;
    }
    bool operator<(const NonCopyItem& A, int B)
    {
        return A.Key < B;
    }

    // 降序比较器（透明，支持异构查找）。
    struct Descending
    {
        bool operator()(Item const& A, Item const& B) const { return A.Key > B.Key; }
        bool operator()(Item const& A, int B) const { return A.Key > B; }
        bool operator()(int A, Item const& B) const { return A > B.Key; }
    };

    // 值类型不定义 operator<，用 KeyOfValue 提取键（boost key_of_value 模型）。
    struct KeyedItem : RBNodeBase
    {
        int Id = 0;
    };

    struct ExtractId
    {
        int operator()(KeyedItem const& V) const { return V.Id; }
    };

    // 字符串键 + KeyOfValue。
    struct StrItem : RBNodeBase
    {
        std::string Name;
        int Value = 0;
    };

    struct ExtractName
    {
        const std::string& operator()(const StrItem& V) const { return V.Name; }
    };

    std::string collectKeys(IntrusiveRBTree<Item> const& Tree)
    {
        std::string Result;
        for (auto It = Tree.cbegin(); It != Tree.cend(); ++It)
        {
            if (!Result.empty())
            {
                Result += ',';
            }
            Result += std::to_string(static_cast<Item const*>(&*It)->Key);
        }
        return Result;
    }

    template <typename Tree>
    std::string collectKeysOf(Tree const& InTree)
    {
        std::string Result;
        for (auto It = InTree.cbegin(); It != InTree.cend(); ++It)
        {
            if (!Result.empty())
            {
                Result += ',';
            }
            Result += std::to_string(static_cast<Item const*>(&*It)->Key);
        }
        return Result;
    }

    int keyOf(const IntrusiveRBTree<Item>::const_iterator& It)
    {
        return static_cast<Item const*>(&*It)->Key;
    }
} // namespace

// ---- 类型契约 ----

PICTOR_TEST_CASE(IntrusiveRBTree_IteratorTypesAndDereference)
{
    using Tree = IntrusiveRBTree<Item>;
    static_assert(std::is_same_v<Tree::iterator, Tree::Iterator<false>>);
    static_assert(std::is_same_v<Tree::const_iterator, Tree::Iterator<true>>);
    static_assert(std::is_same_v<decltype(*std::declval<Tree::iterator>()), Item&>);
    static_assert(std::is_same_v<decltype(*std::declval<Tree::const_iterator>()), Item const&>);
    static_assert(std::is_same_v<decltype(std::declval<Tree::iterator>().operator->()), Item*>);
    static_assert(std::is_convertible_v<Tree::iterator, Tree::const_iterator>);
    static_assert(!std::is_convertible_v<Tree::const_iterator, Tree::iterator>);
    PICTOR_CHECK(true);
}

// ---- 空树边界 ----

PICTOR_TEST_CASE(IntrusiveRBTree_EmptyTreeEdges)
{
    IntrusiveRBTree<Item> Tree;

    PICTOR_CHECK(Tree.empty());
    PICTOR_CHECK_EQ(Tree.size(), 0u);
    PICTOR_CHECK(Tree.begin() == Tree.end());
    PICTOR_CHECK(Tree.cbegin() == Tree.cend());
    PICTOR_CHECK(Tree.crbegin() == Tree.crend());
    PICTOR_CHECK(Tree.validate());

    // 全部查询在空树上返回「未找到」语义。
    PICTOR_CHECK(Tree.find(1) == Tree.end());
    PICTOR_CHECK(!Tree.contains(1));
    PICTOR_CHECK_EQ(Tree.count(1), 0u);
    PICTOR_CHECK(Tree.getLowerBound(1) == Tree.end());
    PICTOR_CHECK(Tree.getUpperBound(1) == Tree.end());
    const auto [First, Last] = Tree.getEqualRange(1);
    PICTOR_CHECK(First == Tree.end());
    PICTOR_CHECK(Last == Tree.end());
    PICTOR_CHECK_EQ(Tree.erase(1), 0u);

    // 空树上的 erase(end()) 是空操作；clear 无害。
    PICTOR_CHECK(Tree.erase(Tree.end()) == Tree.end());
    Tree.clear();
    PICTOR_CHECK(Tree.empty());
    PICTOR_CHECK_EQ(Tree.size(), 0u);
    PICTOR_CHECK(Tree.validate());

    // 空树比较。
    IntrusiveRBTree<Item> OtherEmpty;
    PICTOR_CHECK(Tree == OtherEmpty);
    PICTOR_CHECK(!(Tree < OtherEmpty));
}

// ---- 插入 / 迭代 ----

PICTOR_TEST_CASE(IntrusiveRBTree_InsertMaintainsSortedOrder)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key : {5, 3, 7, 1, 9, 6, 8, 2, 4})
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    PICTOR_CHECK_EQ(Tree.size(), 9u);
    PICTOR_CHECK(!Tree.empty());
    PICTOR_CHECK_EQ(collectKeys(Tree), "1,2,3,4,5,6,7,8,9");
    PICTOR_CHECK(Tree.validate());
}

PICTOR_TEST_CASE(IntrusiveRBTree_InsertWithHint)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key : {1, 2, 4, 5})
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    // 提示插入（当前实现为尽力而为）：结果必须正确。
    Items.emplace_back();
    Items.back().Key = 3;
    const auto It = Tree.insert(Tree.find(4), Items.back());
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*It)->Key, 3);
    PICTOR_CHECK_EQ(collectKeys(Tree), "1,2,3,4,5");
    PICTOR_CHECK(Tree.validate());

    // 重复键的提示插入返回既有元素。
    Items.emplace_back();
    Items.back().Key = 3;
    const auto Dup = Tree.insert(Tree.begin(), Items.back());
    PICTOR_CHECK_EQ(Tree.size(), 5u);
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Dup)->Key, 3);
}

PICTOR_TEST_CASE(IntrusiveRBTree_ReverseIteration)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key : {1, 2, 3, 4, 5})
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    std::string Descending;
    for (auto It = Tree.crbegin(); It != Tree.crend(); ++It)
    {
        if (!Descending.empty())
        {
            Descending += ',';
        }
        Descending += std::to_string(static_cast<Item const*>(&*It)->Key);
    }
    PICTOR_CHECK_EQ(Descending, "5,4,3,2,1");
}

PICTOR_TEST_CASE(IntrusiveRBTree_InsertRejectsDuplicateKey)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    Items.emplace_back();
    Items.back().Key = 7;
    Tree.insert(Items.back());

    Items.emplace_back();
    Items.back().Key = 7;
    const auto [It, Inserted] = Tree.insert(Items.back());

    PICTOR_CHECK(!Inserted);
    PICTOR_CHECK_EQ(Tree.size(), 1u);
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*It)->Key, 7);
    PICTOR_CHECK(Tree.validate());
}

// ---- 查找与边界语义 ----

PICTOR_TEST_CASE(IntrusiveRBTree_HeterogeneousLookup)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key : {5, 3, 7, 1, 9})
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    PICTOR_CHECK(Tree.find(9) != Tree.end());
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Tree.find(9))->Key, 9);
    PICTOR_CHECK(Tree.find(0) == Tree.end());
    PICTOR_CHECK(!Tree.contains(4));
    PICTOR_CHECK(Tree.contains(5));
    PICTOR_CHECK_EQ(Tree.count(5), 1u);
    PICTOR_CHECK_EQ(Tree.count(4), 0u);

    const IntrusiveRBTree<Item>& ConstTree = Tree;
    PICTOR_CHECK(ConstTree.find(7) != ConstTree.end());
    PICTOR_CHECK_EQ(ConstTree.count(9), 1u);
}

PICTOR_TEST_CASE(IntrusiveRBTree_LowerUpperBoundExactness)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key : {2, 5, 9})
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    // 键存在：lowerBound == find，upperBound == 后继。
    PICTOR_CHECK(Tree.getLowerBound(5) == Tree.find(5));
    PICTOR_CHECK_EQ(keyOf(Tree.getUpperBound(5)), 9);

    // 键不存在：lowerBound == upperBound == 第一个更大元素。
    PICTOR_CHECK_EQ(keyOf(Tree.getLowerBound(4)), 5);
    PICTOR_CHECK_EQ(keyOf(Tree.getUpperBound(4)), 5);
    PICTOR_CHECK_EQ(keyOf(Tree.getLowerBound(0)), 2);
    PICTOR_CHECK_EQ(keyOf(Tree.getUpperBound(0)), 2);

    // 越界：无下界/上界返回 end()。
    PICTOR_CHECK(Tree.getLowerBound(10) == Tree.end());
    PICTOR_CHECK(Tree.getUpperBound(9) == Tree.end());

    // 等值区间。
    {
        const auto [First, Last] = Tree.getEqualRange(5);
        PICTOR_CHECK(First != Tree.end());
        PICTOR_CHECK_EQ(keyOf(First), 5);
        PICTOR_CHECK_EQ(keyOf(Last), 9);
    }
    {
        const auto [First, Last] = Tree.getEqualRange(4); // 不存在 → 空区间
        PICTOR_CHECK(First == Last);
        PICTOR_CHECK_EQ(keyOf(First), 5);
    }
    {
        const auto [First, Last] = Tree.getEqualRange(10);
        PICTOR_CHECK(First == Tree.end());
        PICTOR_CHECK(Last == Tree.end());
    }
}

// ---- 删除 ----

PICTOR_TEST_CASE(IntrusiveRBTree_EraseByIteratorReturnsSuccessor)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key : {1, 2, 3, 4, 5})
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    const auto Next = Tree.erase(Tree.find(3));
    PICTOR_CHECK_EQ(Tree.size(), 4u);
    PICTOR_CHECK(!Tree.contains(3));
    PICTOR_CHECK_EQ(keyOf(Next), 4);
    PICTOR_CHECK(Tree.validate());
}

PICTOR_TEST_CASE(IntrusiveRBTree_EraseConstIterator)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key : {1, 2, 3})
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    const IntrusiveRBTree<Item>& ConstTree = Tree;
    const auto CIt = ConstTree.find(2);
    PICTOR_CHECK(CIt != ConstTree.end());
    const auto Next = Tree.erase(CIt);
    PICTOR_CHECK_EQ(Tree.size(), 2u);
    PICTOR_CHECK_EQ(keyOf(Next), 3);
    PICTOR_CHECK_EQ(collectKeys(Tree), "1,3");
    PICTOR_CHECK(Tree.validate());
}

PICTOR_TEST_CASE(IntrusiveRBTree_EraseByKeyReturnsCount)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key : {1, 2, 3})
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    PICTOR_CHECK_EQ(Tree.erase(2), 1u);
    PICTOR_CHECK_EQ(Tree.erase(2), 0u);
    PICTOR_CHECK_EQ(collectKeys(Tree), "1,3");
    PICTOR_CHECK(Tree.validate());
}

PICTOR_TEST_CASE(IntrusiveRBTree_EraseRange)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key = 1; Key <= 10; ++Key)
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    const auto First = Tree.find(3);
    const auto Last = Tree.find(7);
    const auto Next = Tree.erase(First, Last); // 删除 3..6

    PICTOR_CHECK_EQ(collectKeys(Tree), "1,2,7,8,9,10");
    PICTOR_CHECK_EQ(keyOf(Next), 7);
    PICTOR_CHECK(Tree.validate());

    // 空区间是空操作。
    auto Any = Tree.find(7);
    Tree.erase(Any, Any);
    PICTOR_CHECK_EQ(Tree.size(), 6u);

    // [begin, end) 清空。
    Tree.erase(Tree.begin(), Tree.end());
    PICTOR_CHECK(Tree.empty());
    PICTOR_CHECK(Tree.validate());
}

// ---- 迭代器语义 ----

PICTOR_TEST_CASE(IntrusiveRBTree_PostIncrementAndDecrement)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key : {1, 2, 3})
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    auto It = Tree.begin();
    auto Old = It++;
    PICTOR_CHECK_EQ(keyOf(Old), 1);
    PICTOR_CHECK_EQ(keyOf(It), 2);

    auto End = Tree.end();
    auto EndOld = End--;
    PICTOR_CHECK(EndOld == Tree.end());
    PICTOR_CHECK_EQ(keyOf(End), 3);
}

// ---- 指针稳定性（侵入式容器核心承诺） ----

PICTOR_TEST_CASE(IntrusiveRBTree_PointerStability)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key = 0; Key < 1000; ++Key)
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    Item* Anchored = &*Tree.find(500);
    PICTOR_CHECK(Anchored != nullptr);
    PICTOR_CHECK_EQ(Anchored->Key, 500);

    // 删除他人、插入新元素、旋转/重着色都不会移动节点。
    for (int Key = 1; Key < 1000; Key += 3)
    {
        Tree.erase(Key);
    }
    PICTOR_CHECK_EQ(&*Tree.find(500), Anchored);
    PICTOR_CHECK_EQ(Anchored->Key, 500);

    for (int Key = 1000; Key < 1500; ++Key)
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }
    PICTOR_CHECK_EQ(&*Tree.find(500), Anchored);
    PICTOR_CHECK_EQ(Anchored->Key, 500);
    PICTOR_CHECK(Tree.validate());

    // 删除锚点自身：erase 只使指向被删节点的迭代器失效。
    // 当前规模：1000 − 333(键 1,4,...,997) + 500(新增 1000..1499) − 1 = 1166。
    const auto Next = Tree.erase(Tree.find(500));
    PICTOR_CHECK_EQ(Tree.size(), 1166u);
    PICTOR_CHECK(keyOf(Next) >= 500);
    PICTOR_CHECK(Tree.validate());
}

// ---- 容量 / 清空 ----

PICTOR_TEST_CASE(IntrusiveRBTree_ClearAndReuseNodes)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key = 0; Key < 100; ++Key)
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    Tree.clear();
    PICTOR_CHECK(Tree.empty());
    PICTOR_CHECK_EQ(Tree.size(), 0u);
    PICTOR_CHECK(Tree.begin() == Tree.end());
    PICTOR_CHECK(Tree.validate());

    // 被清空的节点链接已复位，可再次插入。
    for (Item& Node : Items)
    {
        Tree.insert(Node);
    }
    PICTOR_CHECK_EQ(Tree.size(), 100u);
    PICTOR_CHECK(Tree.validate());
}

PICTOR_TEST_CASE(IntrusiveRBTree_ClearAndDispose)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key = 0; Key < 50; ++Key)
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    int Disposed = 0;
    Tree.clear_and_dispose([&](Item*) { ++Disposed; });
    PICTOR_CHECK(Tree.empty());
    PICTOR_CHECK_EQ(Disposed, 50);
    PICTOR_CHECK(Tree.validate());
}

// ---- 交换 ----

PICTOR_TEST_CASE(IntrusiveRBTree_Swap)
{
    IntrusiveRBTree<Item> A;
    IntrusiveRBTree<Item> B;
    std::list<Item> ItemsA;
    std::list<Item> ItemsB;
    for (int Key : {1, 2, 3})
    {
        ItemsA.emplace_back();
        ItemsA.back().Key = Key;
        A.insert(ItemsA.back());
    }
    for (int Key : {7, 8})
    {
        ItemsB.emplace_back();
        ItemsB.back().Key = Key;
        B.insert(ItemsB.back());
    }

    A.swap(B);
    PICTOR_CHECK_EQ(A.size(), 2u);
    PICTOR_CHECK_EQ(B.size(), 3u);
    PICTOR_CHECK_EQ(collectKeys(A), "7,8");
    PICTOR_CHECK_EQ(collectKeys(B), "1,2,3");
    PICTOR_CHECK(A.validate() && B.validate());

    using std::swap;
    swap(A, B);
    PICTOR_CHECK_EQ(collectKeys(A), "1,2,3");
    PICTOR_CHECK_EQ(collectKeys(B), "7,8");
    PICTOR_CHECK(A.validate() && B.validate());

    // 空树交换（三个组合）。
    IntrusiveRBTree<Item> Empty;
    Empty.swap(A);
    PICTOR_CHECK_EQ(collectKeys(Empty), "1,2,3");
    PICTOR_CHECK(A.empty());
    PICTOR_CHECK(Empty.validate() && A.validate());
    B.swap(Empty);
    PICTOR_CHECK_EQ(collectKeys(B), "1,2,3");
    PICTOR_CHECK_EQ(collectKeys(Empty), "7,8");
    Empty.swap(Empty); // 自交换
    PICTOR_CHECK_EQ(collectKeys(Empty), "7,8");
    PICTOR_CHECK(Empty.validate());
}

// ---- 关系运算符 ----

PICTOR_TEST_CASE(IntrusiveRBTree_RelationalOperators)
{
    std::list<Item> StorageA, StorageB, StorageC;
    IntrusiveRBTree<Item> Left, Right, Subset;

    for (int Key : {1, 2, 3})
    {
        StorageA.emplace_back();
        StorageA.back().Key = Key;
        Left.insert(StorageA.back());
    }
    for (int Key : {1, 2, 3})
    {
        StorageB.emplace_back();
        StorageB.back().Key = Key;
        Right.insert(StorageB.back());
    }
    for (int Key : {1, 2})
    {
        StorageC.emplace_back();
        StorageC.back().Key = Key;
        Subset.insert(StorageC.back());
    }

    PICTOR_CHECK(Left == Right);
    PICTOR_CHECK(!(Left != Right));
    PICTOR_CHECK(Subset != Left);
    PICTOR_CHECK(Subset < Left);
    PICTOR_CHECK(Left > Subset);
    PICTOR_CHECK(Subset <= Left);
    PICTOR_CHECK(Left >= Subset);
    PICTOR_CHECK(!(Left < Right));
    PICTOR_CHECK(Left <= Right);

    // 与空树比较。
    IntrusiveRBTree<Item> Empty;
    PICTOR_CHECK(Empty < Left);
    PICTOR_CHECK(Left > Empty);
    PICTOR_CHECK(Empty != Left);
}

// ---- 自定义比较器 ----

PICTOR_TEST_CASE(IntrusiveRBTree_CustomComparatorDescending)
{
    IntrusiveRBTree<Item, Descending> Tree;
    std::list<Item> Items;
    for (int Key : {1, 5, 3, 2, 4})
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    PICTOR_CHECK_EQ(collectKeysOf(Tree), "5,4,3,2,1");
    PICTOR_CHECK(Tree.validate());
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Tree.find(3))->Key, 3); // 异构查找
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Tree.getLowerBound(4))->Key, 4);
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Tree.getUpperBound(2))->Key, 1);
}

// ---- 不可拷贝/不可移动节点 ----

PICTOR_TEST_CASE(IntrusiveRBTree_NonCopyableNonMovableNode)
{
    IntrusiveRBTree<NonCopyItem> Tree;
    std::list<NonCopyItem> Items;
    for (int Key : {4, 2, 6, 1, 3, 5})
    {
        Items.emplace_back(Key);
        Tree.insert(Items.back());
    }

    PICTOR_CHECK_EQ(Tree.size(), 6u);
    PICTOR_CHECK(Tree.find(3) != Tree.end());
    PICTOR_CHECK_EQ(Tree.find(3)->Key, 3);
    PICTOR_CHECK_EQ(Tree.erase(2), 1u);
    PICTOR_CHECK(Tree.getLowerBound(3) != Tree.end());
    PICTOR_CHECK(Tree.getLowerBound(3)->Key == 3);

    std::vector<int> Order;
    for (const NonCopyItem& Node : Tree)
    {
        Order.push_back(Node.Key);
    }
    PICTOR_CHECK_EQ(Order, (std::vector<int>{1, 3, 4, 5, 6}));
    PICTOR_CHECK(Tree.validate());

    Tree.clear();
    PICTOR_CHECK(Tree.empty());
    PICTOR_CHECK(Tree.validate());
}

// ---- 极端键值 ----

PICTOR_TEST_CASE(IntrusiveRBTree_ExtremeKeyValues)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    const int Keys[] = {0, 1, -1, INT_MIN, INT_MAX, 1 << 30};
    for (int Key : Keys)
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }
    PICTOR_CHECK_EQ(Tree.size(), 6u);
    PICTOR_CHECK(Tree.validate());
    PICTOR_CHECK_EQ(keyOf(Tree.begin()), INT_MIN);
    for (int Key : Keys)
    {
        PICTOR_CHECK(Tree.contains(Key));
        PICTOR_CHECK_EQ(keyOf(Tree.find(Key)), Key);
        PICTOR_CHECK_EQ(Tree.erase(Key), 1u);
    }
    PICTOR_CHECK(Tree.empty());
    PICTOR_CHECK(Tree.validate());
}

// ---- 升序/降序插入压力（旋转最密集的模式） ----

PICTOR_TEST_CASE(IntrusiveRBTree_AscendingDescendingInsertStress)
{
    {
        IntrusiveRBTree<Item> Tree;
        std::list<Item> Items;
        for (int Key = 0; Key < 50000; ++Key)
        {
            Items.emplace_back();
            Items.back().Key = Key;
            Tree.insert(Items.back());
        }
        PICTOR_CHECK_EQ(Tree.size(), 50000u);
        PICTOR_CHECK(Tree.validate());
        int Expected = 0;
        for (auto It = Tree.cbegin(); It != Tree.cend(); ++It)
        {
            PICTOR_CHECK_EQ(keyOf(It), Expected);
            ++Expected;
        }
        PICTOR_CHECK_EQ(Expected, 50000);
        // 抽查查找。
        for (int Key = 0; Key < 50000; Key += 997)
        {
            PICTOR_CHECK(Tree.contains(Key));
        }
    }
    {
        IntrusiveRBTree<Item> Tree;
        std::list<Item> Items;
        for (int Key = 49999; Key >= 0; --Key)
        {
            Items.emplace_back();
            Items.back().Key = Key;
            Tree.insert(Items.back());
        }
        PICTOR_CHECK_EQ(Tree.size(), 50000u);
        PICTOR_CHECK(Tree.validate());
        int Expected = 0;
        for (auto It = Tree.cbegin(); It != Tree.cend(); ++It)
        {
            PICTOR_CHECK_EQ(keyOf(It), Expected);
            ++Expected;
        }
    }
}

// ---- 随机差分 vs std::set<int>（多种子，逐步校验不变量） ----

PICTOR_TEST_CASE(IntrusiveRBTree_RandomizedDifferentialAgainstStdSet)
{
    const std::uint32_t Seeds[] = {0xC0FFEEu, 0xDEADBEEFu, 12345u};
    for (std::uint32_t Seed : Seeds)
    {
        std::mt19937 Rng(Seed);
        std::set<int> Reference;
        IntrusiveRBTree<Item> Tree;
        std::list<Item> Pool; // 所有已插入节点的稳定存储

        auto makeNode = [&](int Key) -> Item&
        {
            Pool.emplace_back();
            Pool.back().Key = Key;
            return Pool.back();
        };

        for (int Step = 0; Step < 60000; ++Step)
        {
            const int Key = static_cast<int>(Rng() % 2000);
            const int Op = static_cast<int>(Rng() % 5);

            if (Op == 0) // insert
            {
                const auto RefResult = Reference.insert(Key);
                const auto TreeResult = Tree.insert(makeNode(Key));
                PICTOR_CHECK_EQ(RefResult.second, TreeResult.second);
            }
            else if (Op == 1) // erase(key)
            {
                PICTOR_CHECK_EQ(Reference.erase(Key), Tree.erase(Key));
            }
            else if (Op == 2) // find / contains / count
            {
                const bool RefHas = Reference.find(Key) != Reference.end();
                PICTOR_CHECK_EQ(RefHas, Tree.find(Key) != Tree.end());
                PICTOR_CHECK_EQ(RefHas, Tree.contains(Key));
                PICTOR_CHECK_EQ(RefHas ? 1u : 0u, Tree.count(Key));
            }
            else if (Op == 3) // erase(iterator)
            {
                const auto TreeIt = Tree.find(Key);
                const bool RefHas = Reference.find(Key) != Reference.end();
                PICTOR_CHECK_EQ(RefHas, TreeIt != Tree.end());
                if (RefHas)
                {
                    Reference.erase(Key);
                    Tree.erase(TreeIt);
                }
            }
            else // lowerBound / upperBound 抽查
            {
                const int Query = static_cast<int>(Rng() % 2000);
                const auto RefLo = Reference.lower_bound(Query);
                const auto TreeLo = Tree.getLowerBound(Query);
                const int TreeLoKey = (TreeLo == Tree.end()) ? -1 : keyOf(TreeLo);
                const int RefLoKey = (RefLo == Reference.end()) ? -1 : *RefLo;
                PICTOR_CHECK_EQ(TreeLoKey, RefLoKey);

                const auto RefUp = Reference.upper_bound(Query);
                const auto TreeUp = Tree.getUpperBound(Query);
                const int TreeUpKey = (TreeUp == Tree.end()) ? -1 : keyOf(TreeUp);
                const int RefUpKey = (RefUp == Reference.end()) ? -1 : *RefUp;
                PICTOR_CHECK_EQ(TreeUpKey, RefUpKey);
            }

            PICTOR_CHECK_EQ(Tree.size(), Reference.size());
            PICTOR_CHECK(Tree.validate());

            if ((Step % 4096) == 0)
            {
                std::vector<int> TreeKeys;
                for (auto It = Tree.cbegin(); It != Tree.cend(); ++It)
                {
                    TreeKeys.push_back(keyOf(It));
                }
                PICTOR_CHECK_EQ(TreeKeys, std::vector<int>(Reference.begin(), Reference.end()));
            }
        }

        PICTOR_CHECK_EQ(Tree.size(), Reference.size());
        PICTOR_CHECK(Tree.validate());
    }
}

// ---- boost 同款：hook 可拷贝，副本为未链接状态 ----

PICTOR_TEST_CASE(IntrusiveRBTree_CopyConstructsAnUnlinkedNode)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    Items.emplace_back();
    Items.back().Key = 1;
    Tree.insert(Items.back());

    // 复制一个已插入的节点：副本链接复位为未链接（不共享链接）。
    Item Copy = Items.back();
    PICTOR_CHECK(Copy.Parent() == nullptr);
    PICTOR_CHECK(Copy.Left == nullptr);
    PICTOR_CHECK(Copy.Right == nullptr);
    PICTOR_CHECK_EQ(Tree.size(), 1u);

    // 副本键相同 → 判定为重复，不插入。
    const auto [It, Inserted] = Tree.insert(Copy);
    PICTOR_CHECK(!Inserted);
    PICTOR_CHECK_EQ(keyOf(It), 1);

    // 改键后可独立插入。
    Copy.Key = 2;
    const auto [It2, Inserted2] = Tree.insert(Copy);
    PICTOR_CHECK(Inserted2);
    PICTOR_CHECK_EQ(keyOf(It2), 2);
    PICTOR_CHECK_EQ(Tree.size(), 2u);
    PICTOR_CHECK(Tree.validate());
}

// ---- 透明键查找（KeyOfValue）与可选 size（ConstantTimeSize） ----

PICTOR_TEST_CASE(IntrusiveRBTree_TransparentKeyLookupViaKeyOfValue)
{
    using Tree = IntrusiveRBTree<KeyedItem, std::less<int>, ExtractId>;
    Tree Tree_;
    std::list<KeyedItem> Items;
    for (int Id : {5, 3, 7, 1, 9})
    {
        Items.emplace_back();
        Items.back().Id = Id;
        Tree_.insert(Items.back());
    }

    PICTOR_CHECK_EQ(Tree_.size(), 5u);
    PICTOR_CHECK(Tree_.find(9) != Tree_.end());
    PICTOR_CHECK_EQ(Tree_.find(9)->Id, 9);
    PICTOR_CHECK(Tree_.find(0) == Tree_.end());
    PICTOR_CHECK_EQ(Tree_.count(3), 1u);
    PICTOR_CHECK(Tree_.contains(3));
    PICTOR_CHECK_EQ(Tree_.getLowerBound(4)->Id, 5);
    PICTOR_CHECK_EQ(Tree_.getUpperBound(5)->Id, 7);
    PICTOR_CHECK_EQ(Tree_.erase(7), 1u);
    PICTOR_CHECK_EQ(Tree_.size(), 4u);

    std::vector<int> Ids;
    for (auto It = Tree_.cbegin(); It != Tree_.cend(); ++It)
    {
        Ids.push_back(It->Id);
    }
    PICTOR_CHECK_EQ(Ids, (std::vector<int>{1, 3, 5, 9}));
    PICTOR_CHECK(Tree_.validate());
}

PICTOR_TEST_CASE(IntrusiveRBTree_StringKeysViaKeyOfValue)
{
    using Tree = IntrusiveRBTree<StrItem, std::less<std::string>, ExtractName>;
    Tree Tree_;
    std::list<StrItem> Items;
    for (const char* Name : {"delta", "alpha", "charlie", "bravo"})
    {
        Items.emplace_back();
        Items.back().Name = Name;
        Items.back().Value = static_cast<int>(Items.size());
        Tree_.insert(Items.back());
    }

    PICTOR_CHECK_EQ(Tree_.size(), 4u);
    PICTOR_CHECK(Tree_.contains("alpha"));
    PICTOR_CHECK(!Tree_.contains("zulu"));
    PICTOR_CHECK_EQ(Tree_.find("bravo")->Value, 4);
    PICTOR_CHECK_EQ(Tree_.getLowerBound("b")->Name, "bravo");
    PICTOR_CHECK(Tree_.getUpperBound("charlie")->Name == "delta");

    std::vector<std::string> Order;
    for (const StrItem& Node : Tree_)
    {
        Order.push_back(Node.Name);
    }
    PICTOR_CHECK_EQ(Order, (std::vector<std::string>{"alpha", "bravo", "charlie", "delta"}));
    PICTOR_CHECK_EQ(Tree_.erase("charlie"), 1u);
    PICTOR_CHECK_EQ(Tree_.size(), 3u);
    PICTOR_CHECK(Tree_.validate());
}

PICTOR_TEST_CASE(IntrusiveRBTree_ConstantTimeSizeFalseFallsBackToLinearSize)
{
    using LinearTree = IntrusiveRBTree<Item, std::less<>, rbtree_detail::Identity, false>;
    using FixedTree = IntrusiveRBTree<Item, std::less<>, rbtree_detail::Identity, true>;

    // size 成员被优化掉：对象更小。
    PICTOR_CHECK(sizeof(LinearTree) < sizeof(FixedTree));

    LinearTree Tree;
    std::list<Item> Items;
    for (int Key = 0; Key < 100; ++Key)
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    PICTOR_CHECK_EQ(Tree.size(), 100u); // O(n)，但结果正确
    PICTOR_CHECK(!Tree.empty());
    PICTOR_CHECK(Tree.validate());

    Tree.erase(0);
    PICTOR_CHECK_EQ(Tree.size(), 99u);
    PICTOR_CHECK(Tree.validate());
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Tree.begin())->Key, 1);

    Tree.clear();
    PICTOR_CHECK_EQ(Tree.size(), 0u);
    PICTOR_CHECK(Tree.empty());
}
