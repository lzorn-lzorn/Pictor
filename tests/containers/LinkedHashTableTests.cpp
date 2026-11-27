// Tests for infra/containers/LinkedHashTable.hpp.
//
// LinkedHashTable 是与 std::unordered_map 接口兼容的哈希表，通过侵入式双向链表
// 保持插入顺序：迭代顺序稳定、erase 不影响其它迭代器、重复插入/覆盖不改变位置。
// 本文件验证：
//   * unordered_map 契约（insert/find/erase/at/operator[]/insertOrAssign/拷贝/移动/交换）
//   * 插入顺序保证（含 rehash/reserve/拷贝/交换/清空重填后的顺序）
//   * 迭代器稳定性（插入（强制扩容）与删除他人不影响既有迭代器）
//   * 空表/单元素/极端键/移动后状态等边界输入
//   * 哈希策略边界（setMaxLoadFactor、rehash/reserve 语义）
//   * 异常安全、分配器记账
//   * 多种子随机差分测试（对照 std::unordered_map，并核对插入顺序）

#include "TestFramework.h"

#include <infra/containers/LinkedHashTable.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <climits>
#include <functional>
#include <limits>
#include <list>
#include <memory>
#include <random>
#include <ranges>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    // 按迭代顺序收集键（可读诊断用）。
    template <typename Map>
    std::string collectKeys(const Map& Map_)
    {
        std::string Result;
        for (auto It = Map_.cbegin(); It != Map_.cend(); ++It)
        {
            if (!Result.empty())
            {
                Result += ',';
            }
            Result += std::to_string(It->first);
        }
        return Result;
    }

    template <typename Map>
    std::vector<int> keyVector(const Map& Map_)
    {
        std::vector<int> V;
        for (const auto& Entry : Map_)
        {
            V.push_back(Entry.first);
        }
        return V;
    }

    // 无默认构造的键/值类型。
    struct NoDefaultKey
    {
        int X;
        explicit NoDefaultKey(int V) : X(V) {}
        bool operator==(const NoDefaultKey& O) const { return X == O.X; }
    };
    struct NoDefaultKeyHash
    {
        std::size_t operator()(const NoDefaultKey& K) const
        {
            return std::hash<int>{}(K.X);
        }
    };
    struct NoDefaultValue
    {
        int X;
        explicit NoDefaultValue(int V) : X(V) {}
        bool operator==(const NoDefaultValue& O) const { return X == O.X; }
    };

    // ---- 异常安全探针 ----

    struct ThrowingValueCtor
    {
        int X;
        static inline bool Throw = false;
        explicit ThrowingValueCtor(int V) : X(V)
        {
            if (Throw)
            {
                throw std::runtime_error("ctor");
            }
        }
    };

    struct ThrowingCopyValue
    {
        int X;
        static inline bool ThrowOnCopy = false;
        explicit ThrowingCopyValue(int V) : X(V) {}
        ThrowingCopyValue(const ThrowingCopyValue& O) : X(O.X)
        {
            if (ThrowOnCopy)
            {
                throw std::runtime_error("copy");
            }
        }
        ThrowingCopyValue(ThrowingCopyValue&&) = default;
        ThrowingCopyValue& operator=(const ThrowingCopyValue&) = default;
        ThrowingCopyValue& operator=(ThrowingCopyValue&&) = default;
    };

    struct ThrowingHash
    {
        static inline std::size_t Calls = 0;
        static inline std::size_t ThrowAt = std::numeric_limits<std::size_t>::max();
        std::size_t operator()(int K) const
        {
            if (++Calls > ThrowAt)
            {
                throw std::runtime_error("hash");
            }
            return std::hash<int>{}(K);
        }
    };

    struct IdentityHash
    {
        std::size_t operator()(std::uint64_t K) const
        {
            return static_cast<std::size_t>(K);
        }
    };

    // ---- 计数分配器 ----
    struct CountingState
    {
        static inline std::size_t Allocations = 0;
        static inline std::size_t Deallocations = 0;
    };

    template <typename T>
    struct CountingAllocator : CountingState
    {
        using value_type = T;

        CountingAllocator() = default;
        template <typename U>
        CountingAllocator(const CountingAllocator<U>&) noexcept {}

        T* allocate(std::size_t N)
        {
            ++Allocations;
            return std::allocator<T>{}.allocate(N);
        }
        void deallocate(T* P, std::size_t N) noexcept
        {
            ++Deallocations;
            std::allocator<T>{}.deallocate(P, N);
        }

        template <typename U>
        bool operator==(const CountingAllocator<U>&) const noexcept { return true; }
    };
} // namespace

// ---- 类型契约 ----

PICTOR_TEST_CASE(LinkedHashTable_IteratorTypesAndDereference)
{
    using Map = LinkedHashTable<int, int>;
    static_assert(std::is_same_v<Map::iterator, Map::Iterator<false>>);
    static_assert(std::is_same_v<Map::const_iterator, Map::Iterator<true>>);
    static_assert(std::is_same_v<decltype(*std::declval<Map::iterator>()), Map::value_type&>);
    static_assert(std::is_same_v<decltype(*std::declval<Map::const_iterator>()), const Map::value_type&>);
    static_assert(std::is_convertible_v<Map::iterator, Map::const_iterator>);
    static_assert(!std::is_convertible_v<Map::const_iterator, Map::iterator>);
    static_assert(std::is_nothrow_move_constructible_v<Map>);
    static_assert(std::is_nothrow_swappable_v<Map>);
    PICTOR_CHECK(true);
}

// ---- 构造与容量边界 ----

PICTOR_TEST_CASE(LinkedHashTable_DefaultConstructionIsEmpty)
{
    LinkedHashTable<int, int> Map_;
    PICTOR_CHECK(Map_.empty());
    PICTOR_CHECK_EQ(Map_.size(), 0u);
    PICTOR_CHECK(Map_.begin() == Map_.end());
    PICTOR_CHECK(Map_.cbegin() == Map_.cend());
    PICTOR_CHECK(Map_.find(1) == Map_.end());
    PICTOR_CHECK(!Map_.contains(1));
    PICTOR_CHECK_EQ(Map_.erase(1), 0u);
    Map_.clear();
    PICTOR_CHECK(Map_.empty());
    Map_.erase(Map_.begin(), Map_.end());
    PICTOR_CHECK(Map_.empty());
    PICTOR_CHECK_NO_THROW(Map_.rehash(1024));
    PICTOR_CHECK_NO_THROW(Map_.reserve(1024));
    PICTOR_CHECK_NO_THROW(Map_.clear());
    PICTOR_CHECK_THROWS(Map_.at(1), std::out_of_range);
}

PICTOR_TEST_CASE(LinkedHashTable_BucketHintConstructor)
{
    LinkedHashTable<int, int> Default_;
    PICTOR_CHECK_EQ(Default_.getBucketCount(), 16u);

    LinkedHashTable<int, int> Hint32(32);
    PICTOR_CHECK_EQ(Hint32.getBucketCount(), 32u);

    LinkedHashTable<int, int> Hint1000(1000);
    PICTOR_CHECK_EQ(Hint1000.getBucketCount(), 1024u);

    LinkedHashTable<int, int> Tiny(1);
    PICTOR_CHECK_EQ(Tiny.getBucketCount(), 16u);
}

// ---- 插入 / 查找 / 修改全 API ----

PICTOR_TEST_CASE(LinkedHashTable_InsertFindContainsAtOperatorBracket)
{
    LinkedHashTable<int, std::string> Map_;
    Map_.insert({1, "one"});
    Map_.insert(std::make_pair(2, "two"));
    Map_.insert(LinkedHashTable<int, std::string>::value_type{3, "three"});
    Map_.emplace(4, "four");
    Map_.tryEmplace(5, "five");
    Map_[6] = "six";

    PICTOR_CHECK_EQ(Map_.size(), 6u);
    PICTOR_CHECK(!Map_.empty());
    PICTOR_CHECK(Map_.contains(1));
    PICTOR_CHECK(!Map_.contains(99));
    PICTOR_CHECK_EQ(Map_.at(3), std::string("three"));
    PICTOR_CHECK_EQ(Map_[6], std::string("six"));
    PICTOR_CHECK(Map_.find(2) != Map_.end());
    PICTOR_CHECK_EQ(Map_.find(2)->second, std::string("two"));
    PICTOR_CHECK(Map_.find(42) == Map_.end());

    const LinkedHashTable<int, std::string>& ConstMap = Map_;
    PICTOR_CHECK_EQ(ConstMap.at(4), std::string("four"));
    PICTOR_CHECK(ConstMap.find(1) != ConstMap.end());
    PICTOR_CHECK(ConstMap.contains(6));
    PICTOR_CHECK_THROWS(ConstMap.at(42), std::out_of_range);
}

PICTOR_TEST_CASE(LinkedHashTable_InsertRejectsDuplicateKey)
{
    LinkedHashTable<int, int> Map_;
    Map_[1] = 100;
    const auto [It, Inserted] = Map_.insert({1, 200});
    PICTOR_CHECK(!Inserted);
    PICTOR_CHECK_EQ(Map_.size(), 1u);
    PICTOR_CHECK_EQ(It->second, 100);

    const auto [It2, Inserted2] = Map_.tryEmplace(1, 300);
    PICTOR_CHECK(!Inserted2);
    PICTOR_CHECK_EQ(It2->second, 100);
}

PICTOR_TEST_CASE(LinkedHashTable_InsertOrAssign)
{
    LinkedHashTable<int, int> Map_;

    const auto [ItNew, InsertedNew] = Map_.insertOrAssign(1, 10);
    PICTOR_CHECK(InsertedNew);
    PICTOR_CHECK_EQ(ItNew->second, 10);
    PICTOR_CHECK_EQ(Map_.size(), 1u);

    // 覆盖已有键：不改变插入顺序位置。
    const auto [ItOld, InsertedOld] = Map_.insertOrAssign(1, 20);
    PICTOR_CHECK(!InsertedOld);
    PICTOR_CHECK_EQ(ItOld->second, 20);
    PICTOR_CHECK_EQ(Map_.size(), 1u);

    Map_.insertOrAssign(2, 30);
    Map_.insertOrAssign(3, 40);
    PICTOR_CHECK_EQ(collectKeys(Map_), "1,2,3");

    std::unordered_map<int, int> Reference;
    Reference.insert_or_assign(1, 20);
    Reference.insert_or_assign(2, 30);
    Reference.insert_or_assign(3, 40);
    PICTOR_CHECK_EQ(Reference.at(1), Map_.at(1));
    PICTOR_CHECK_EQ(Reference.at(2), Map_.at(2));
    PICTOR_CHECK_EQ(Reference.at(3), Map_.at(3));
}

// ---- 插入顺序保证 ----

PICTOR_TEST_CASE(LinkedHashTable_InsertionOrderPreserved)
{
    LinkedHashTable<int, int> Map_;
    for (int K : {5, 3, 7, 1, 9, 6, 8, 2, 4})
    {
        Map_[K] = K;
    }
    PICTOR_CHECK_EQ(collectKeys(Map_), "5,3,7,1,9,6,8,2,4");

    // 重复插入不改变位置。
    Map_.insert({3, 0});
    PICTOR_CHECK_EQ(collectKeys(Map_), "5,3,7,1,9,6,8,2,4");

    // operator[] 覆盖也不改变位置。
    Map_[3] = 99;
    PICTOR_CHECK_EQ(collectKeys(Map_), "5,3,7,1,9,6,8,2,4");
    PICTOR_CHECK_EQ(Map_.at(3), 99);
}

PICTOR_TEST_CASE(LinkedHashTable_OrderSurvivesRehashReserveAndCopy)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 0; K < 100; ++K)
    {
        Map_[K] = K;
    }
    const std::string Expected = collectKeys(Map_);

    // reserve 触发重哈希：顺序不变。
    Map_.reserve(100000);
    PICTOR_CHECK_EQ(collectKeys(Map_), Expected);
    PICTOR_CHECK(Map_.getBucketCount() >= 100000u);

    // 显式 rehash：顺序不变。
    Map_.rehash(200000);
    PICTOR_CHECK_EQ(collectKeys(Map_), Expected);

    // 拷贝：顺序与源一致。
    LinkedHashTable<int, int> Copy(Map_);
    PICTOR_CHECK_EQ(collectKeys(Copy), Expected);

    // 交换后再交换回来：顺序恢复。
    LinkedHashTable<int, int> Empty;
    Copy.swap(Empty);
    PICTOR_CHECK_EQ(collectKeys(Empty), Expected);
    Copy.swap(Empty);
    PICTOR_CHECK_EQ(collectKeys(Copy), Expected);
}

PICTOR_TEST_CASE(LinkedHashTable_IteratorStabilityAcrossInsertAndErase)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 0; K < 10; ++K)
    {
        Map_[K] = K;
    }

    // 保存指向中间元素的迭代器。
    auto Anchor = Map_.find(5);
    PICTOR_CHECK(Anchor != Map_.end());
    const int* AnchorAddress = &Anchor->first;

    // 删除其它元素：锚点迭代器依然有效且地址稳定。
    Map_.erase(1);
    Map_.erase(8);
    Map_.erase(Map_.find(0));
    PICTOR_CHECK(Anchor != Map_.end());
    PICTOR_CHECK_EQ(Anchor->first, 5);
    PICTOR_CHECK_EQ(&Anchor->first, AnchorAddress);

    // 大量插入触发扩容重哈希：节点本身不重新分配，迭代器依旧有效。
    for (int K = 100; K < 5000; ++K)
    {
        Map_[K] = K;
    }
    PICTOR_CHECK(Anchor != Map_.end());
    PICTOR_CHECK_EQ(Anchor->first, 5);
    PICTOR_CHECK_EQ(&Anchor->first, AnchorAddress);

    // 删除锚点之后的元素不影响锚点。
    Map_.erase(4999);
    PICTOR_CHECK(Anchor != Map_.end());
    PICTOR_CHECK_EQ(Anchor->first, 5);

    // 全表遍历核对插入顺序。
    std::vector<int> Order;
    for (const auto& Entry : Map_)
    {
        Order.push_back(Entry.first);
    }
    std::vector<int> ExpectedOrder;
    ExpectedOrder.reserve(Map_.size());
    for (int K = 0; K < 10; ++K)
    {
        if (K != 0 && K != 1 && K != 8)
        {
            ExpectedOrder.push_back(K);
        }
    }
    for (int K = 100; K < 4999; ++K)
    {
        ExpectedOrder.push_back(K);
    }
    PICTOR_CHECK_EQ(Order, ExpectedOrder);
}

PICTOR_TEST_CASE(LinkedHashTable_ReverseIteration)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 1; K <= 5; ++K)
    {
        Map_[K] = K;
    }

    std::string Descending;
    for (auto It = Map_.crbegin(); It != Map_.crend(); ++It)
    {
        if (!Descending.empty())
        {
            Descending += ',';
        }
        Descending += std::to_string(It->first);
    }
    PICTOR_CHECK_EQ(Descending, "5,4,3,2,1");
}

PICTOR_TEST_CASE(LinkedHashTable_PostIncrementAndDecrement)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 1; K <= 3; ++K)
    {
        Map_[K] = K;
    }

    auto It = Map_.begin();
    auto Old = It++;
    PICTOR_CHECK_EQ(Old->first, 1);
    PICTOR_CHECK_EQ(It->first, 2);

    auto End = Map_.end();
    auto EndOld = End--;
    PICTOR_CHECK(EndOld == Map_.end());
    PICTOR_CHECK_EQ(End->first, 3);
}

// ---- 删除 ----

PICTOR_TEST_CASE(LinkedHashTable_EraseByIteratorReturnsSuccessorAndKeepsOrder)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 1; K <= 5; ++K)
    {
        Map_[K] = K;
    }

    const auto Next = Map_.erase(Map_.find(3));
    PICTOR_CHECK_EQ(Map_.size(), 4u);
    PICTOR_CHECK(!Map_.contains(3));
    PICTOR_CHECK_EQ(Next->first, 4);
    PICTOR_CHECK_EQ(collectKeys(Map_), "1,2,4,5");

    // 删除最后一个元素返回 end()。
    PICTOR_CHECK(Map_.erase(Map_.find(5)) == Map_.end());
    PICTOR_CHECK_EQ(collectKeys(Map_), "1,2,4");
}

PICTOR_TEST_CASE(LinkedHashTable_EraseByKeyReturnsCount)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 1; K <= 3; ++K)
    {
        Map_[K] = K;
    }
    PICTOR_CHECK_EQ(Map_.erase(2), 1u);
    PICTOR_CHECK_EQ(Map_.erase(2), 0u);
    PICTOR_CHECK_EQ(collectKeys(Map_), "1,3");

    // 删空后继续可用。
    Map_.erase(1);
    Map_.erase(3);
    PICTOR_CHECK(Map_.empty());
    Map_[9] = 9;
    PICTOR_CHECK_EQ(collectKeys(Map_), "9");
}

PICTOR_TEST_CASE(LinkedHashTable_EraseEndIteratorIsNoop)
{
    LinkedHashTable<int, int> Map_;
    Map_[1] = 1;
    Map_[2] = 2;
    PICTOR_CHECK(Map_.erase(Map_.cend()) == Map_.end());
    PICTOR_CHECK_EQ(Map_.size(), 2u);
}

PICTOR_TEST_CASE(LinkedHashTable_EraseRange)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 1; K <= 10; ++K)
    {
        Map_[K] = K;
    }
    const auto First = Map_.find(3);
    const auto Last = Map_.find(7);
    const auto Next = Map_.erase(First, Last); // 删除 3..6
    PICTOR_CHECK_EQ(collectKeys(Map_), "1,2,7,8,9,10");
    PICTOR_CHECK_EQ(Next->first, 7);

    // 相等迭代器区间是空操作。
    auto Any = Map_.find(7);
    Map_.erase(Any, Any);
    PICTOR_CHECK_EQ(Map_.size(), 6u);

    // [begin, end) 清空。
    Map_.erase(Map_.begin(), Map_.end());
    PICTOR_CHECK(Map_.empty());
}

// ---- 拷贝 / 移动 / 交换 ----

PICTOR_TEST_CASE(LinkedHashTable_CopyIsDeepAndIndependent)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 1; K <= 5; ++K)
    {
        Map_[K] = K;
    }

    LinkedHashTable<int, int> Copy(Map_);
    PICTOR_CHECK_EQ(Copy.size(), Map_.size());
    PICTOR_CHECK_EQ(collectKeys(Copy), collectKeys(Map_));

    Copy[1] = 999;
    Copy[6] = 6;
    PICTOR_CHECK_EQ(Map_[1], 1);
    PICTOR_CHECK(!Map_.contains(6));
    PICTOR_CHECK_EQ(Map_.size(), 5u);

    LinkedHashTable<int, int> Assigned;
    Assigned = Map_;
    PICTOR_CHECK_EQ(collectKeys(Assigned), "1,2,3,4,5");
    PICTOR_CHECK_EQ(Assigned.size(), 5u);

    // 自赋值。
    Assigned = Assigned;
    PICTOR_CHECK_EQ(collectKeys(Assigned), "1,2,3,4,5");
}

PICTOR_TEST_CASE(LinkedHashTable_MoveLeavesSourceValidAndEmpty)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 1; K <= 5; ++K)
    {
        Map_[K] = K;
    }

    LinkedHashTable<int, int> Moved(std::move(Map_));
    PICTOR_CHECK_EQ(Moved.size(), 5u);
    PICTOR_CHECK_EQ(collectKeys(Moved), "1,2,3,4,5");
    PICTOR_CHECK(Map_.empty());
    PICTOR_CHECK_EQ(Map_.begin(), Map_.end());

    // 移动后的表可查询、可复用（std::unordered_map 语义）。
    PICTOR_CHECK(Map_.find(1) == Map_.end());
    PICTOR_CHECK(!Map_.contains(1));
    PICTOR_CHECK_EQ(Map_.erase(1), 0u);
    PICTOR_CHECK_THROWS(Map_.at(1), std::out_of_range);
    Map_[10] = 10;
    PICTOR_CHECK_EQ(Map_.size(), 1u);
    PICTOR_CHECK(Map_.contains(10));
    PICTOR_CHECK_EQ(collectKeys(Map_), "10");

    // 移动赋值。
    LinkedHashTable<int, int> Assigned;
    Assigned[100] = 100;
    Assigned = std::move(Moved);
    PICTOR_CHECK_EQ(collectKeys(Assigned), "1,2,3,4,5");
    PICTOR_CHECK(Moved.empty());

    // 自移动赋值。
    Assigned = std::move(Assigned);
    PICTOR_CHECK_EQ(collectKeys(Assigned), "1,2,3,4,5");
}

PICTOR_TEST_CASE(LinkedHashTable_SwapExchangesContents)
{
    LinkedHashTable<int, int> A;
    LinkedHashTable<int, int> B;
    for (int K : {1, 2, 3})
    {
        A[K] = K;
    }
    for (int K : {7, 8})
    {
        B[K] = K;
    }

    A.swap(B);
    PICTOR_CHECK_EQ(A.size(), 2u);
    PICTOR_CHECK_EQ(B.size(), 3u);
    PICTOR_CHECK_EQ(collectKeys(A), "7,8");
    PICTOR_CHECK_EQ(collectKeys(B), "1,2,3");

    using std::swap;
    swap(A, B);
    PICTOR_CHECK_EQ(collectKeys(A), "1,2,3");
    PICTOR_CHECK_EQ(collectKeys(B), "7,8");

    // 空表 ↔ 非空表（两个方向）。
    LinkedHashTable<int, int> Empty;
    Empty.swap(A);
    PICTOR_CHECK_EQ(collectKeys(Empty), "1,2,3");
    PICTOR_CHECK(A.empty());
    B.swap(Empty);
    PICTOR_CHECK_EQ(collectKeys(B), "1,2,3");
    PICTOR_CHECK_EQ(collectKeys(Empty), "7,8");

    // 自交换。
    B.swap(B);
    PICTOR_CHECK_EQ(collectKeys(B), "1,2,3");
    PICTOR_CHECK_EQ(B.size(), 3u);
}

// ---- 非默认构造键值 / move-only 值 ----

PICTOR_TEST_CASE(LinkedHashTable_NonDefaultConstructibleKeyAndValue)
{
    LinkedHashTable<NoDefaultKey, NoDefaultValue, NoDefaultKeyHash> Map_;
    Map_.tryEmplace(NoDefaultKey{1}, NoDefaultValue{10});
    Map_.tryEmplace(NoDefaultKey{2}, NoDefaultValue{20});

    PICTOR_CHECK_EQ(Map_.size(), 2u);
    PICTOR_CHECK(Map_.contains(NoDefaultKey{1}));
    PICTOR_CHECK_EQ(Map_.at(NoDefaultKey{2}).X, 20);
    PICTOR_CHECK_EQ(Map_.find(NoDefaultKey{1})->second.X, 10);

    Map_.erase(NoDefaultKey{1});
    PICTOR_CHECK_EQ(Map_.size(), 1u);
}

PICTOR_TEST_CASE(LinkedHashTable_MoveOnlyValue)
{
    using Ptr = std::unique_ptr<int>;
    LinkedHashTable<int, Ptr> Map_;
    Map_.tryEmplace(1, std::make_unique<int>(10));
    Map_.tryEmplace(2, std::make_unique<int>(20));
    Map_.tryEmplace(3, std::make_unique<int>(30));

    PICTOR_CHECK_EQ(Map_.size(), 3u);
    PICTOR_CHECK_EQ(*Map_.at(1), 10);
    PICTOR_CHECK_EQ(*Map_.find(2)->second, 20);

    Map_.erase(1);
    PICTOR_CHECK_EQ(Map_.size(), 2u);
    PICTOR_CHECK_EQ(*Map_.at(3), 30);

    Map_.reserve(10000);
    PICTOR_CHECK_EQ(*Map_.at(2), 20);

    LinkedHashTable<int, Ptr> Moved(std::move(Map_));
    PICTOR_CHECK_EQ(Moved.size(), 2u);
    PICTOR_CHECK_EQ(*Moved.at(3), 30);
}

// ---- 极端键值（边界输入） ----

PICTOR_TEST_CASE(LinkedHashTable_ExtremeKeyValues)
{
    LinkedHashTable<int, int> Map_;
    const int Keys[] = {0, 1, -1, INT_MIN, INT_MAX, 1 << 30, -1000000};
    for (int K : Keys)
    {
        Map_[K] = K;
    }
    PICTOR_CHECK_EQ(Map_.size(), 7u);
    for (int K : Keys)
    {
        PICTOR_CHECK(Map_.contains(K));
        PICTOR_CHECK_EQ(Map_.at(K), K);
    }
    for (int K : Keys)
    {
        PICTOR_CHECK_EQ(Map_.erase(K), 1u);
    }
    PICTOR_CHECK(Map_.empty());

    LinkedHashTable<std::string, int> StrMap;
    StrMap[""] = 1;
    StrMap["a"] = 2;
    const std::string LongKey(10000, 'x');
    StrMap[LongKey] = 3;
    PICTOR_CHECK_EQ(StrMap.at(""), 1);
    PICTOR_CHECK_EQ(StrMap.at("a"), 2);
    PICTOR_CHECK_EQ(StrMap.at(LongKey), 3);
    PICTOR_CHECK_EQ(StrMap.size(), 3u);
}

// ---- 哈希策略 ----

PICTOR_TEST_CASE(LinkedHashTable_RehashReserveAndLoadFactor)
{
    LinkedHashTable<int, int> Map_;
    PICTOR_CHECK_EQ(Map_.getBucketCount(), 16u);
    PICTOR_CHECK_NEAR(Map_.getMaxLoadFactor(), 0.875f, 0.001f);

    for (int K = 0; K < 100; ++K)
    {
        Map_[K] = K;
    }
    PICTOR_CHECK(Map_.getBucketCount() >= 128u);
    PICTOR_CHECK(Map_.getLoadFactor() <= Map_.getMaxLoadFactor() + 0.0001f);

    Map_.reserve(10000);
    PICTOR_CHECK(Map_.getBucketCount() >= 10000u);
    for (int K = 0; K < 100; ++K)
    {
        PICTOR_CHECK(Map_.contains(K));
    }

    const auto Before = Map_.getBucketCount();
    Map_.rehash(16);
    PICTOR_CHECK_EQ(Map_.getBucketCount(), Before);

    Map_.clear();
    PICTOR_CHECK(Map_.empty());
    PICTOR_CHECK_EQ(Map_.getBucketCount(), Before);
}

PICTOR_TEST_CASE(LinkedHashTable_SetMaxLoadFactorBoundaries)
{
    LinkedHashTable<int, int> Map_;
    Map_.setMaxLoadFactor(0.5f);
    PICTOR_CHECK_NEAR(Map_.getMaxLoadFactor(), 0.5f, 0.001f);
    Map_.setMaxLoadFactor(1.0f);
    PICTOR_CHECK_NEAR(Map_.getMaxLoadFactor(), 1.0f, 0.001f);

    PICTOR_CHECK_THROWS(Map_.setMaxLoadFactor(0.0f), std::out_of_range);
    PICTOR_CHECK_THROWS(Map_.setMaxLoadFactor(-0.5f), std::out_of_range);
    PICTOR_CHECK_THROWS(Map_.setMaxLoadFactor(1.5f), std::out_of_range);
    PICTOR_CHECK_NEAR(Map_.getMaxLoadFactor(), 1.0f, 0.001f);

    LinkedHashTable<int, int> Low(64);
    Low.setMaxLoadFactor(0.25f);
    for (int K = 0; K < 16; ++K)
    {
        Low[K] = K;
    }
    PICTOR_CHECK_EQ(Low.size(), 16u);
    PICTOR_CHECK(Low.getLoadFactor() <= 0.25f + 0.0001f);
}

// ---- 自定义哈希 / 相等 ----

PICTOR_TEST_CASE(LinkedHashTable_CustomHashAndEqual)
{
    struct CaseInsensitiveHash
    {
        std::size_t operator()(const std::string& S) const
        {
            std::size_t H = 0;
            for (char C : S)
            {
                H = H * 131 + static_cast<unsigned char>(std::tolower(static_cast<unsigned char>(C)));
            }
            return H;
        }
    };
    struct CaseInsensitiveEqual
    {
        bool operator()(const std::string& A, const std::string& B) const
        {
            if (A.size() != B.size())
            {
                return false;
            }
            for (std::size_t I = 0; I < A.size(); ++I)
            {
                if (std::tolower(static_cast<unsigned char>(A[I])) !=
                    std::tolower(static_cast<unsigned char>(B[I])))
                {
                    return false;
                }
            }
            return true;
        }
    };

    LinkedHashTable<std::string, int, CaseInsensitiveHash, CaseInsensitiveEqual> Map_;
    Map_["Key"] = 1;
    PICTOR_CHECK(Map_.contains("key"));
    PICTOR_CHECK(Map_.contains("KEY"));
    PICTOR_CHECK_EQ(Map_.at("kEy"), 1);
    PICTOR_CHECK_EQ(Map_.size(), 1u);
}

PICTOR_TEST_CASE(LinkedHashTable_WeakIdentityHashStress)
{
    LinkedHashTable<std::uint64_t, std::uint64_t, IdentityHash> Map_;
    for (std::uint64_t K = 0; K < 50000; ++K)
    {
        Map_[K] = K * 3;
    }
    PICTOR_CHECK_EQ(Map_.size(), 50000u);
    for (std::uint64_t K = 0; K < 50000; K += 101)
    {
        PICTOR_CHECK_EQ(Map_.at(K), K * 3);
    }
    for (std::uint64_t K = 0; K < 50000; K += 2)
    {
        Map_.erase(K);
    }
    PICTOR_CHECK_EQ(Map_.size(), 25000u);
    for (std::uint64_t K = 1; K < 50000; K += 2)
    {
        PICTOR_CHECK(Map_.contains(K));
    }
}

// ---- 迭代器 / const_iterator 互操作 ----

PICTOR_TEST_CASE(LinkedHashTable_IteratorConstIteratorInterop)
{
    LinkedHashTable<int, int> Map_;
    Map_[1] = 1;
    Map_[2] = 2;

    LinkedHashTable<int, int>::iterator It = Map_.find(1);
    LinkedHashTable<int, int>::const_iterator Cit = Map_.find(2);

    PICTOR_CHECK(It != Cit);
    PICTOR_CHECK(Cit != It);
    PICTOR_CHECK(It != Map_.cend());
    PICTOR_CHECK(Map_.begin() == Map_.cbegin());
    PICTOR_CHECK(Map_.cbegin() == Map_.begin());
    PICTOR_CHECK(It == Map_.cbegin());

    LinkedHashTable<int, int>::const_iterator Converted = It;
    PICTOR_CHECK(Converted == It);
    PICTOR_CHECK(It == Converted);
}

// ---- 桶操作 ----

PICTOR_TEST_CASE(LinkedHashTable_BucketOperations)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 0; K < 100; ++K)
    {
        Map_[K] = K;
    }

    const auto BC = Map_.getBucketCount();
    PICTOR_CHECK(BC >= 128u);
    PICTOR_CHECK(Map_.getMaxBucketCount() >= BC);

    for (int K = 0; K < 100; ++K)
    {
        PICTOR_CHECK(Map_.getBucket(K) < BC);
    }

    std::size_t Total = 0;
    for (std::size_t I = 0; I < BC; ++I)
    {
        Total += Map_.getBucketSize(I);
    }
    PICTOR_CHECK_EQ(Total, Map_.size());

    std::vector<int> Seen;
    for (std::size_t I = 0; I < BC; ++I)
    {
        for (auto It = Map_.begin(I); It != Map_.end(I); ++It)
        {
            Seen.push_back(It->first);
        }
    }
    std::sort(Seen.begin(), Seen.end());
    PICTOR_CHECK_EQ(Seen.size(), 100u);
    for (int I = 0; I < 100; ++I)
    {
        PICTOR_CHECK_EQ(Seen[I], I);
    }

    for (int K = 0; K < 100; ++K)
    {
        bool Found = false;
        for (auto It = Map_.cbegin(Map_.getBucket(K)); It != Map_.cend(Map_.getBucket(K)); ++It)
        {
            if (It->first == K)
            {
                Found = true;
                break;
            }
        }
        PICTOR_CHECK(Found);
    }
}

// ---- ranges 互操作 ----

PICTOR_TEST_CASE(LinkedHashTable_RangesCompatibility)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 1; K <= 10; ++K)
    {
        Map_[K] = K * 10;
    }

    static_assert(std::ranges::range<LinkedHashTable<int, int>>);
    static_assert(std::ranges::bidirectional_range<LinkedHashTable<int, int>>);
    static_assert(std::ranges::sized_range<LinkedHashTable<int, int>>);

    const auto Count = std::ranges::count_if(Map_, [](const auto& P) { return P.second > 30; });
    PICTOR_CHECK_EQ(Count, 7);

    auto It = std::ranges::find_if(Map_, [](const auto& P) { return P.first == 4; });
    PICTOR_CHECK(It != Map_.end());
    PICTOR_CHECK_EQ(It->second, 40);

    int Sum = 0;
    std::ranges::for_each(Map_, [&](const auto& P) { Sum += P.first; });
    PICTOR_CHECK_EQ(Sum, 55);

    PICTOR_CHECK_EQ(std::ranges::size(Map_), 10u);
    PICTOR_CHECK(!std::ranges::empty(Map_));
}

// ---- 强异常安全 ----

PICTOR_TEST_CASE(LinkedHashTable_StrongExceptionSafetyTryEmplace)
{
    using Map = LinkedHashTable<int, ThrowingValueCtor>;
    Map Map_;
    Map_.tryEmplace(1, 10);
    PICTOR_CHECK_EQ(Map_.size(), 1u);

    ThrowingValueCtor::Throw = true;
    bool Threw = false;
    try
    {
        Map_.tryEmplace(2, 20);
    }
    catch (const std::runtime_error&)
    {
        Threw = true;
    }
    PICTOR_CHECK(Threw);
    ThrowingValueCtor::Throw = false;

    PICTOR_CHECK_EQ(Map_.size(), 1u);
    PICTOR_CHECK(Map_.contains(1));
    PICTOR_CHECK(!Map_.contains(2));

    Map_.tryEmplace(2, 20);
    PICTOR_CHECK_EQ(Map_.size(), 2u);
    PICTOR_CHECK(Map_.contains(2));
}

PICTOR_TEST_CASE(LinkedHashTable_StrongExceptionSafetyCopy)
{
    using Map = LinkedHashTable<int, ThrowingCopyValue>;
    Map Map_;
    for (int K = 1; K <= 5; ++K)
    {
        Map_.tryEmplace(K, K * 10);
    }

    ThrowingCopyValue::ThrowOnCopy = true;
    bool Threw = false;
    try
    {
        Map Copy(Map_);
    }
    catch (const std::runtime_error&)
    {
        Threw = true;
    }
    PICTOR_CHECK(Threw);
    ThrowingCopyValue::ThrowOnCopy = false;

    PICTOR_CHECK_EQ(Map_.size(), 5u);
    for (int K = 1; K <= 5; ++K)
    {
        PICTOR_CHECK(Map_.contains(K));
        PICTOR_CHECK_EQ(Map_.at(K).X, K * 10);
    }
}

PICTOR_TEST_CASE(LinkedHashTable_StrongExceptionSafetyRehash)
{
    LinkedHashTable<int, int, ThrowingHash> Map_;
    for (int K = 0; K < 100; ++K)
    {
        Map_[K] = K;
    }

    const auto OldBC = Map_.getBucketCount();
    ThrowingHash::Calls = 0;
    ThrowingHash::ThrowAt = 50;

    bool Threw = false;
    try
    {
        Map_.rehash(10000);
    }
    catch (const std::runtime_error&)
    {
        Threw = true;
    }
    PICTOR_CHECK(Threw);

    ThrowingHash::ThrowAt = std::numeric_limits<std::size_t>::max();

    PICTOR_CHECK_EQ(Map_.getBucketCount(), OldBC);
    PICTOR_CHECK_EQ(Map_.size(), 100u);
    bool AllPresent = true;
    for (int K = 0; K < 100; ++K)
    {
        if (!Map_.contains(K))
        {
            AllPresent = false;
            break;
        }
    }
    PICTOR_CHECK(AllPresent);
}

// ---- 分配器记账 ----

PICTOR_TEST_CASE(LinkedHashTable_GroupsAllocatedViaAllocator)
{
    using Map = LinkedHashTable<int, int, std::hash<int>, std::equal_to<int>,
                                CountingAllocator<std::pair<const int, int>>>;

    CountingState::Allocations = 0;
    CountingState::Deallocations = 0;

    {
        Map Map_;
        Map_.tryEmplace(1, 10);
        Map_.tryEmplace(2, 20);
        Map_.tryEmplace(3, 30);

        // 3 个节点 + 1 个桶数组 + 1 个控制字节组数组。
        PICTOR_CHECK_EQ(CountingState::Allocations, 5u);
        PICTOR_CHECK_EQ(Map_.size(), 3u);

        // 删除节点：逐元素释放。
        Map_.erase(1);
        PICTOR_CHECK_EQ(CountingState::Deallocations, 1u);
    }

    PICTOR_CHECK_EQ(CountingState::Deallocations, CountingState::Allocations);
}

// ---- 随机差分测试（对照 std::unordered_map + 插入顺序，多种子） ----

PICTOR_TEST_CASE(LinkedHashTable_RandomizedDifferentialAgainstStdUnorderedMap)
{
    const std::uint32_t Seeds[] = {0xC0FFEEu, 0xDEADBEEFu, 12345u, 0xABCDEF01u};
    for (std::uint32_t Seed : Seeds)
    {
        std::mt19937 Rng(Seed);
        std::unordered_map<int, int> Reference;
        LinkedHashTable<int, int> Table;
        std::list<int> InsertionOrder; // 现存键的插入顺序

        for (int Step = 0; Step < 120000; ++Step)
        {
            const int Key = static_cast<int>(Rng() % 4000);
            const int Op = static_cast<int>(Rng() % 6);

            if (Op == 0) // tryEmplace
            {
                const auto RefResult = Reference.try_emplace(Key, Step);
                const auto TblResult = Table.tryEmplace(Key, Step);
                PICTOR_CHECK_EQ(RefResult.second, TblResult.second);
                if (RefResult.second)
                {
                    InsertionOrder.push_back(Key);
                }
                PICTOR_CHECK_EQ(RefResult.first->second, TblResult.first->second);
            }
            else if (Op == 1) // erase(key)
            {
                const auto RefCount = Reference.erase(Key);
                const auto TblCount = Table.erase(Key);
                PICTOR_CHECK_EQ(RefCount, TblCount);
                if (RefCount != 0)
                {
                    InsertionOrder.remove(Key);
                }
            }
            else if (Op == 2) // find / contains
            {
                PICTOR_CHECK_EQ(Reference.find(Key) != Reference.end(), Table.contains(Key));
                PICTOR_CHECK_EQ(Reference.find(Key) != Reference.end(), Table.find(Key) != Table.end());
            }
            else if (Op == 3) // operator[] 增长路径
            {
                const bool WasAbsent = Reference.find(Key) == Reference.end();
                ++Reference[Key];
                ++Table[Key];
                if (WasAbsent)
                {
                    InsertionOrder.push_back(Key);
                }
            }
            else if (Op == 4) // insertOrAssign（覆盖不改变顺序）
            {
                const auto RefResult = Reference.insert_or_assign(Key, Step);
                const auto TblResult = Table.insertOrAssign(Key, Step);
                PICTOR_CHECK_EQ(RefResult.second, TblResult.second);
                if (RefResult.second)
                {
                    InsertionOrder.push_back(Key);
                }
                PICTOR_CHECK_EQ(RefResult.first->second, TblResult.first->second);
            }
            else // erase(iterator)（链表保证其它迭代器不受影响）
            {
                const auto RefIt = Reference.find(Key);
                const auto TblIt = Table.find(Key);
                PICTOR_CHECK_EQ(RefIt != Reference.end(), TblIt != Table.end());
                if (RefIt != Reference.end())
                {
                    Reference.erase(RefIt);
                    Table.erase(TblIt);
                    InsertionOrder.remove(Key);
                }
            }

            PICTOR_CHECK_EQ(Table.size(), Reference.size());

            if ((Step % 8192) == 0)
            {
                // 全量核对：内容、值与插入顺序。
                PICTOR_CHECK_EQ(keyVector(Table),
                                std::vector<int>(InsertionOrder.begin(), InsertionOrder.end()));
                for (const auto& Entry : Reference)
                {
                    PICTOR_CHECK(Table.contains(Entry.first));
                    PICTOR_CHECK_EQ(Table.at(Entry.first), Entry.second);
                }
            }
        }

        PICTOR_CHECK_EQ(Table.size(), Reference.size());
        PICTOR_CHECK_EQ(Table.size(), InsertionOrder.size());
        PICTOR_CHECK_EQ(keyVector(Table),
                        std::vector<int>(InsertionOrder.begin(), InsertionOrder.end()));
    }
}

// ---- 密集顺序键压力 ----

PICTOR_TEST_CASE(LinkedHashTable_DenseSequentialKeysStress)
{
    LinkedHashTable<std::uint64_t, std::uint64_t> Table;
    for (std::uint64_t K = 0; K < 100000; ++K)
    {
        Table[K] = K * 3;
    }
    PICTOR_CHECK_EQ(Table.size(), 100000u);
    for (std::uint64_t K = 0; K < 100000; K += 997)
    {
        PICTOR_CHECK_EQ(Table.at(K), K * 3);
    }
    for (std::uint64_t K = 0; K < 100000; K += 2)
    {
        Table.erase(K);
    }
    PICTOR_CHECK_EQ(Table.size(), 50000u);
    for (std::uint64_t K = 1; K < 100000; K += 2)
    {
        PICTOR_CHECK(Table.contains(K));
    }
    // 插入顺序必须是 1,3,5,...（删除偶数不改变奇数相对顺序）。
    std::uint64_t Expected = 1;
    for (const auto& Entry : Table)
    {
        PICTOR_CHECK_EQ(Entry.first, Expected);
        Expected += 2;
    }
    for (std::uint64_t K = 1; K < 100000; K += 2)
    {
        Table.erase(K);
    }
    PICTOR_CHECK(Table.empty());
    for (std::uint64_t K = 0; K < 1000; ++K)
    {
        Table[K] = K;
    }
    PICTOR_CHECK_EQ(Table.size(), 1000u);
    PICTOR_CHECK_EQ(Table.at(999), 999u);
}
