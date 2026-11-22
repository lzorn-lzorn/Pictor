// Tests for infra/containers/IntrusiveRBTree.hpp.
//
// IntrusiveRBTree is an intrusive red-black tree with std::set-like semantics:
// unique keys, ordered iteration, O(log n) insert/erase/lookup. Nodes are owned
// by the caller, not by the container.

#include "TestFramework.h"

#include <functional>
#include <list>
#include <random>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "infra/containers/IntrusiveRBTree.hpp"

namespace
{
    // A minimal value type keyed by int. It publicly derives from RBNodeBase so
    // it can live inside an IntrusiveRBTree.
    struct Item : RBNodeBase
    {
        int Key = 0;
        bool operator<(Item const& Other) const { return Key < Other.Key; }
    };

    // Heterogeneous comparison with int enables std::less<> to do
    // find/lower_bound/... with a bare int instead of a full Item.
    bool operator<(Item const& A, int B) { return A.Key < B; }
    bool operator<(int A, Item const& B) { return A < B.Key; }

    // A transparent comparator that orders items by descending key.
    struct Descending
    {
        bool operator()(Item const& A, Item const& B) const { return A.Key > B.Key; }
        bool operator()(Item const& A, int B) const { return A.Key > B; }
        bool operator()(int A, Item const& B) const { return A > B.Key; }
    };

    // 值类型不定义 operator<，用 KeyOfValue 提取键做透明键查找（boost key_of_value 模型）。
    struct KeyedItem : RBNodeBase
    {
        int Id = 0;
    };

    struct ExtractId
    {
        int operator()(KeyedItem const& V) const { return V.Id; }
    };

    // Collects keys in iteration order as a comma-separated string, for
    // readable diagnostics.
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
} // namespace

// ---- type-level contracts ----

PICTOR_TEST_CASE(IteratorTypesAndDereference)
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

// ---- insertion / iteration ----

PICTOR_TEST_CASE(InsertMaintainsSortedOrder)
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

PICTOR_TEST_CASE(ReverseIteration)
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

PICTOR_TEST_CASE(InsertRejectsDuplicateKey)
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
}

// ---- lookup ----

PICTOR_TEST_CASE(HeterogeneousLookup)
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
    PICTOR_CHECK(Tree.contains(4) == false); // 4 is absent
    PICTOR_CHECK(Tree.contains(5));
    PICTOR_CHECK_EQ(Tree.count(5), 1u);
    PICTOR_CHECK_EQ(Tree.count(4), 0u);

    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Tree.getLowerBound(4))->Key, 5);
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Tree.getUpperBound(5))->Key, 7);
    PICTOR_CHECK(Tree.getLowerBound(10) == Tree.end());
    PICTOR_CHECK(Tree.getUpperBound(9) == Tree.end());

    const auto [First, Last] = Tree.getEqualRange(5);
    PICTOR_CHECK(First != Tree.end());
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*First)->Key, 5);
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Last)->Key, 7);
}

// ---- erase ----

PICTOR_TEST_CASE(EraseByIteratorReturnsSuccessor)
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
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Next)->Key, 4);
    PICTOR_CHECK(Tree.validate());
}

PICTOR_TEST_CASE(EraseByKeyReturnsCount)
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
    PICTOR_CHECK_EQ(Tree.erase(2), 0u); // already gone
    PICTOR_CHECK_EQ(collectKeys(Tree), "1,3");
    PICTOR_CHECK(Tree.validate());
}

PICTOR_TEST_CASE(EraseRange)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    for (int Key = 1; Key <= 10; ++Key)
    {
        Items.emplace_back();
        Items.back().Key = Key;
        Tree.insert(Items.back());
    }

    // Remove [3, 7) => 3,4,5,6.
    const auto First = Tree.find(3);
    const auto Last = Tree.find(7);
    const auto Next = Tree.erase(First, Last);

    PICTOR_CHECK_EQ(collectKeys(Tree), "1,2,7,8,9,10");
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Next)->Key, 7);
    PICTOR_CHECK(Tree.validate());
}

// ---- iterator semantics ----

PICTOR_TEST_CASE(PostIncrementAndDecrement)
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
    const int Before = static_cast<Item const*>(&*It)->Key;
    auto Old = It++; // Old must alias the pre-increment position.
    PICTOR_CHECK_EQ(Before, 1);
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Old)->Key, 1);
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*It)->Key, 2);

    auto End = Tree.end();
    auto EndOld = End--;
    PICTOR_CHECK(EndOld == Tree.end());
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*End)->Key, 3);
}

// ---- capacity / clearing ----

PICTOR_TEST_CASE(ClearAndReuseNodes)
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

    // Cleared nodes have had their links reset, so they can be re-inserted.
    for (Item& Node : Items)
    {
        Tree.insert(Node);
    }
    PICTOR_CHECK_EQ(Tree.size(), 100u);
    PICTOR_CHECK(Tree.validate());
}

PICTOR_TEST_CASE(ClearAndDispose)
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
}

// ---- swap ----

PICTOR_TEST_CASE(Swap)
{
    IntrusiveRBTree<Item> A;
    IntrusiveRBTree<Item> B;
    std::list<Item> ItemsA;
    std::list<Item> ItemsB;
    for (int Key : {1, 2, 3}) { ItemsA.emplace_back(); ItemsA.back().Key = Key; A.insert(ItemsA.back()); }
    for (int Key : {7, 8})   { ItemsB.emplace_back(); ItemsB.back().Key = Key; B.insert(ItemsB.back()); }

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
}

// ---- relational operators ----

PICTOR_TEST_CASE(RelationalOperators)
{
    std::list<Item> StorageA, StorageB, StorageC;
    IntrusiveRBTree<Item> Left, Right, Subset;

    for (int Key : {1, 2, 3}) { StorageA.emplace_back(); StorageA.back().Key = Key; Left.insert(StorageA.back()); }
    for (int Key : {1, 2, 3}) { StorageB.emplace_back(); StorageB.back().Key = Key; Right.insert(StorageB.back()); }
    for (int Key : {1, 2})    { StorageC.emplace_back(); StorageC.back().Key = Key; Subset.insert(StorageC.back()); }

    PICTOR_CHECK(Left == Right);
    PICTOR_CHECK(!(Left != Right));
    PICTOR_CHECK(Subset != Left);
    PICTOR_CHECK(Subset < Left);
    PICTOR_CHECK(Left > Subset);
    PICTOR_CHECK(Subset <= Left);
    PICTOR_CHECK(Left >= Subset);
}

// ---- custom comparator ----

PICTOR_TEST_CASE(CustomComparatorDescending)
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
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*Tree.find(3))->Key, 3); // heterogeneous find
}

// ---- randomized differential vs std::set<int> ----

PICTOR_TEST_CASE(RandomizedDifferentialAgainstStdSet)
{
    std::mt19937 Rng(0xC0FFEE);
    std::set<int> Reference;
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Pool; // stable storage for every inserted node

    auto makeNode = [&](int Key) -> Item&
    {
        Pool.emplace_back();
        Pool.back().Key = Key;
        return Pool.back();
    };

    int FailStep = -1;
    for (int Step = 0; Step < 100000; ++Step)
    {
        const int Key = static_cast<int>(Rng() % 2000);
        const int Op = static_cast<int>(Rng() % 3);

        if (Op == 0)
        {
            const auto RefResult = Reference.insert(Key);
            const auto TreeResult = Tree.insert(makeNode(Key));
            if (RefResult.second != TreeResult.second) { FailStep = Step; break; }
        }
        else if (Op == 1)
        {
            if (Reference.erase(Key) != Tree.erase(Key)) { FailStep = Step; break; }
        }
        else
        {
            if ((Reference.find(Key) != Reference.end()) != (Tree.find(Key) != Tree.end())) { FailStep = Step; break; }
        }

        if (!Tree.validate()) { FailStep = Step; break; }

        if ((Step % 4096) == 0)
        {
            if (Tree.size() != Reference.size()) { FailStep = Step; break; }
            std::vector<int> TreeKeys;
            for (auto It = Tree.cbegin(); It != Tree.cend(); ++It)
            {
                TreeKeys.push_back(static_cast<Item const*>(&*It)->Key);
            }
            if (TreeKeys != std::vector<int>(Reference.begin(), Reference.end())) { FailStep = Step; break; }

            // Spot-check bounds agree with std::set.
            const int Query = static_cast<int>(Rng() % 2000);
            auto RefLo = Reference.lower_bound(Query);
            auto TreeLo = Tree.getLowerBound(Query);
            const int TreeLoKey = (TreeLo == Tree.end()) ? -1 : static_cast<Item const*>(&*TreeLo)->Key;
            const int RefLoKey = (RefLo == Reference.end()) ? -1 : *RefLo;
            if (TreeLoKey != RefLoKey) { FailStep = Step; break; }

            auto RefUp = Reference.upper_bound(Query);
            auto TreeUp = Tree.getUpperBound(Query);
            const int TreeUpKey = (TreeUp == Tree.end()) ? -1 : static_cast<Item const*>(&*TreeUp)->Key;
            const int RefUpKey = (RefUp == Reference.end()) ? -1 : *RefUp;
            if (TreeUpKey != RefUpKey) { FailStep = Step; break; }
        }
    }

    PICTOR_CHECK_EQ(FailStep, -1);
    PICTOR_CHECK_EQ(Tree.size(), Reference.size());
    PICTOR_CHECK(Tree.validate());
}

// ---- Boost 同款：hook 可拷贝，副本为未链接状态 ----

PICTOR_TEST_CASE(CopyConstructsAnUnlinkedNode)
{
    IntrusiveRBTree<Item> Tree;
    std::list<Item> Items;
    Items.emplace_back();
    Items.back().Key = 1;
    Tree.insert(Items.back());

    // 复制一个已插入的节点：副本的链接关系被复位为未链接（不共享链接）。
    Item Copy = Items.back();
    PICTOR_CHECK(Copy.Parent() == nullptr);
    PICTOR_CHECK(Copy.Left == nullptr);
    PICTOR_CHECK(Copy.Right == nullptr);
    PICTOR_CHECK_EQ(Tree.size(), 1u); // 复制不影响原树

    // 副本键相同 -> 判定为重复，不插入。
    const auto [It, Inserted] = Tree.insert(Copy);
    PICTOR_CHECK(!Inserted);
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*It)->Key, 1);

    // 改键后可独立插入。
    Copy.Key = 2;
    const auto [It2, Inserted2] = Tree.insert(Copy);
    PICTOR_CHECK(Inserted2);
    PICTOR_CHECK_EQ(static_cast<Item const*>(&*It2)->Key, 2);
    PICTOR_CHECK_EQ(Tree.size(), 2u);
    PICTOR_CHECK(Tree.validate());
}

// ---- 透明键查找（KeyOfValue）与可选 size（ConstantTimeSize） ----

PICTOR_TEST_CASE(TransparentKeyLookupViaKeyOfValue)
{
    // 值类型不定义 operator<，也无需跨类型 operator<：用 KeyOfValue 提取键，
    // 比较器只比较键（等价 boost 的 key_of_value）。
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

PICTOR_TEST_CASE(ConstantTimeSizeFalseFallsBackToLinearSize)
{
    using LinearTree = IntrusiveRBTree<Item, std::less<>, rbtree_detail::Identity, false>;
    using FixedTree  = IntrusiveRBTree<Item, std::less<>, rbtree_detail::Identity, true>;

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
}

int main()
{
    return ::PictorTest::runAll();
}
