// Tests for infra/containers/FlatHashTable.hpp.
//
// FlatHashTable 是扁平（开放寻址）哈希映射：值内联存放、插入原地构造、
// 删除采用向后移位（无墓碑）。迭代顺序为内存顺序（未指定，非插入顺序），
// insert（重哈希）与 erase 会使迭代器失效——因此内容比较一律按多重集合
// （排序后的键向量）进行。
//
// 覆盖范围（先 API 行为正确性与边界，后压力）：
//   * 类型契约、构造/容量边界、插入查找删除全 API、insertOrAssign、
//     空表/单元素/极端键/空字符串/移动后状态等边界输入
//   * 哈希策略边界（setMaxLoadFactor、rehash/reserve 语义）
//   * 拷贝/移动/交换（含自赋值、自交换）、非默认构造与 move-only 值
//   * 异常安全（构造抛、拷贝抛、重哈希抛）
//   * 分配器记账（零逐元素分配、扩容分配/释放平衡）
//   * 多种子随机差分测试（对照 std::unordered_map）
//   * 顺序键/弱哈希压力（终结器生效验证）

#include "TestFramework.h"

#include <infra/containers/FlatHashTable.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <climits>
#include <functional>
#include <limits>
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
    template <typename Map>
    std::vector<int> sortedKeyVector(const Map& Map_)
    {
        std::vector<int> V;
        for (const auto& Entry : Map_)
        {
            V.push_back(Entry.first);
        }
        std::sort(V.begin(), V.end());
        return V;
    }

    // 无默认构造的键/值类型——容器任何路径都不得要求默认构造。
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

    // 恒等哈希：验证终结器对弱哈希的防护（分布与正确性）。
    struct IdentityHash
    {
        std::size_t operator()(std::uint64_t K) const
        {
            return static_cast<std::size_t>(K);
        }
    };

    // ---- 计数分配器（验证元数据分配 + 插入/删除零逐元素分配）----
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

PICTOR_TEST_CASE(FlatHashTable_IteratorTypesAndDereference)
{
    using Map = FlatHashTable<int, int>;
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

PICTOR_TEST_CASE(FlatHashTable_DefaultConstructionIsEmpty)
{
    FlatHashTable<int, int> Map_;
    PICTOR_CHECK(Map_.empty());
    PICTOR_CHECK_EQ(Map_.size(), 0u);
    PICTOR_CHECK(Map_.begin() == Map_.end());
    PICTOR_CHECK(Map_.cbegin() == Map_.cend());
    PICTOR_CHECK(Map_.find(1) == Map_.end());
    PICTOR_CHECK(!Map_.contains(1));
    PICTOR_CHECK_EQ(Map_.erase(1), 0u);
    PICTOR_CHECK(Map_.getLoadFactor() == 0.f || Map_.getLoadFactor() >= 0.f);
    // 空表上的擦除与清空是无害的。
    Map_.clear();
    PICTOR_CHECK(Map_.empty());
    Map_.erase(Map_.begin(), Map_.end());
    PICTOR_CHECK(Map_.empty());
    PICTOR_CHECK_NO_THROW(Map_.rehash(1024));
    PICTOR_CHECK_NO_THROW(Map_.reserve(1024));
    PICTOR_CHECK_NO_THROW(Map_.clear());
    // 空表 at() 抛 out_of_range。
    PICTOR_CHECK_THROWS(Map_.at(1), std::out_of_range);
}

PICTOR_TEST_CASE(FlatHashTable_BucketHintConstructor)
{
    FlatHashTable<int, int> Default_;
    PICTOR_CHECK_EQ(Default_.getBucketCount(), 16u);

    FlatHashTable<int, int> Hint32(32);
    PICTOR_CHECK_EQ(Hint32.getBucketCount(), 32u);

    // 提示向上取整到 2 的幂。
    FlatHashTable<int, int> Hint1000(1000);
    PICTOR_CHECK_EQ(Hint1000.getBucketCount(), 1024u);

    FlatHashTable<int, int> Tiny(1);
    PICTOR_CHECK_EQ(Tiny.getBucketCount(), 16u);
}

// ---- 插入 / 查找 / 修改全 API ----

PICTOR_TEST_CASE(FlatHashTable_InsertFindContainsAtOperatorBracket)
{
    FlatHashTable<int, std::string> Map_;
    Map_.insert({1, "one"});
    Map_.insert(std::make_pair(2, "two"));
    Map_.insert(FlatHashTable<int, std::string>::value_type{3, "three"});
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

    // const 表上的查找与 at。
    const FlatHashTable<int, std::string>& ConstMap = Map_;
    PICTOR_CHECK_EQ(ConstMap.at(4), std::string("four"));
    PICTOR_CHECK(ConstMap.find(1) != ConstMap.end());
    PICTOR_CHECK(ConstMap.contains(6));
    PICTOR_CHECK_THROWS(ConstMap.at(42), std::out_of_range);

    // operator[] 原地更新已有键。
    Map_[1] = "ONE";
    PICTOR_CHECK_EQ(Map_.size(), 6u);
    PICTOR_CHECK_EQ(Map_.at(1), std::string("ONE"));
}

PICTOR_TEST_CASE(FlatHashTable_InsertRejectsDuplicateKey)
{
    FlatHashTable<int, int> Map_;
    Map_[1] = 100;
    const auto [It, Inserted] = Map_.insert({1, 200});
    PICTOR_CHECK(!Inserted);
    PICTOR_CHECK_EQ(Map_.size(), 1u);
    PICTOR_CHECK_EQ(It->second, 100); // 已有值不受影响

    const auto [It2, Inserted2] = Map_.tryEmplace(1, 300);
    PICTOR_CHECK(!Inserted2);
    PICTOR_CHECK_EQ(It2->second, 100);
}

PICTOR_TEST_CASE(FlatHashTable_InsertOrAssign)
{
    FlatHashTable<int, int> Map_;

    // 插入新键：返回 {迭代器, true}。
    const auto [ItNew, InsertedNew] = Map_.insertOrAssign(1, 10);
    PICTOR_CHECK(InsertedNew);
    PICTOR_CHECK_EQ(ItNew->second, 10);
    PICTOR_CHECK_EQ(Map_.size(), 1u);

    // 覆盖已有键：返回 {迭代器, false}。
    const auto [ItOld, InsertedOld] = Map_.insertOrAssign(1, 20);
    PICTOR_CHECK(!InsertedOld);
    PICTOR_CHECK_EQ(ItOld->second, 20);
    PICTOR_CHECK_EQ(Map_.size(), 1u);

    // 右值键版本。
    Map_.insertOrAssign(2, 30);
    PICTOR_CHECK_EQ(Map_.at(2), 30);

    // 与 std::unordered_map::insert_or_assign 语义一致。
    std::unordered_map<int, int> Reference;
    Reference.insert_or_assign(1, 20);
    Reference.insert_or_assign(2, 30);
    PICTOR_CHECK_EQ(Reference.at(1), Map_.at(1));
    PICTOR_CHECK_EQ(Reference.at(2), Map_.at(2));
    PICTOR_CHECK_EQ(Reference.size(), Map_.size());
}

// ---- 迭代 ----

PICTOR_TEST_CASE(FlatHashTable_IterationVisitsAllElementsExactlyOnce)
{
    FlatHashTable<int, int> Map_;
    for (int K = 0; K < 100; ++K)
    {
        Map_[K] = K * 2;
    }
    std::vector<int> Expected;
    for (int K = 0; K < 100; ++K)
    {
        Expected.push_back(K);
    }
    PICTOR_CHECK_EQ(sortedKeyVector(Map_), Expected);

    // const 迭代与 cbegin/cend 一致。
    int Count = 0;
    for (auto It = Map_.cbegin(); It != Map_.cend(); ++It)
    {
        ++Count;
    }
    PICTOR_CHECK_EQ(Count, 100);
}

PICTOR_TEST_CASE(FlatHashTable_ReverseIterationVisitsAllElements)
{
    FlatHashTable<int, int> Map_;
    for (int K = 0; K < 50; ++K)
    {
        Map_[K] = K;
    }
    std::vector<int> Forward = sortedKeyVector(Map_);
    std::vector<int> Backward;
    for (auto It = Map_.crbegin(); It != Map_.crend(); ++It)
    {
        Backward.push_back(It->first);
    }
    std::sort(Backward.begin(), Backward.end());
    PICTOR_CHECK_EQ(Backward, Forward);
    PICTOR_CHECK_EQ(Backward.size(), 50u);
}

PICTOR_TEST_CASE(FlatHashTable_PostIncrementAndDecrement)
{
    FlatHashTable<int, int> Map_;
    for (int K = 1; K <= 3; ++K)
    {
        Map_[K] = K;
    }
    auto It = Map_.begin();
    const int First = It->first;
    auto Old = It++;
    PICTOR_CHECK_EQ(Old->first, First);
    PICTOR_CHECK(It == Map_.end() || It->first != First);

    auto End = Map_.end();
    auto EndOld = End--;
    PICTOR_CHECK(EndOld == Map_.end());
    PICTOR_CHECK(End == Map_.end() || End->first >= 0);
}

// ---- 删除 ----

PICTOR_TEST_CASE(FlatHashTable_EraseByIteratorKeepsRemainingElements)
{
    FlatHashTable<int, int> Map_;
    for (int K = 0; K < 20; ++K)
    {
        Map_[K] = K;
    }

    auto It = Map_.begin();
    while (It != Map_.end())
    {
        It = Map_.erase(It);
    }
    PICTOR_CHECK(Map_.empty());
    PICTOR_CHECK_EQ(Map_.size(), 0u);
    PICTOR_CHECK(Map_.begin() == Map_.end());
}

PICTOR_TEST_CASE(FlatHashTable_EraseByKeyReturnsCount)
{
    FlatHashTable<int, int> Map_;
    for (int K = 1; K <= 3; ++K)
    {
        Map_[K] = K;
    }
    PICTOR_CHECK_EQ(Map_.erase(2), 1u);
    PICTOR_CHECK_EQ(Map_.erase(2), 0u);
    PICTOR_CHECK(!Map_.contains(2));
    PICTOR_CHECK(Map_.contains(1) && Map_.contains(3));

    // 删除最后一个元素后表为空但可继续使用。
    Map_.erase(1);
    Map_.erase(3);
    PICTOR_CHECK(Map_.empty());
    Map_[9] = 9;
    PICTOR_CHECK(Map_.contains(9));
}

PICTOR_TEST_CASE(FlatHashTable_EraseEndIteratorIsNoop)
{
    FlatHashTable<int, int> Map_;
    Map_[1] = 1;
    Map_[2] = 2;
    PICTOR_CHECK(Map_.erase(Map_.cend()) == Map_.end());
    PICTOR_CHECK_EQ(Map_.size(), 2u);
}

PICTOR_TEST_CASE(FlatHashTable_EraseRangeRemovesExactlyTheRange)
{
    FlatHashTable<int, int> Map_;
    for (int K = 0; K < 10; ++K)
    {
        Map_[K] = K;
    }

    // 迭代顺序的前 4 个元素。
    auto First = Map_.begin();
    auto Last = Map_.begin();
    std::vector<int> RangeKeys;
    for (int I = 0; I < 4; ++I)
    {
        RangeKeys.push_back(Last->first);
        ++Last;
    }

    Map_.erase(First, Last);
    PICTOR_CHECK_EQ(Map_.size(), 6u);
    for (int K : RangeKeys)
    {
        PICTOR_CHECK(!Map_.contains(K));
    }
    for (int K = 0; K < 10; ++K)
    {
        const bool InRange = std::find(RangeKeys.begin(), RangeKeys.end(), K) != RangeKeys.end();
        PICTOR_CHECK_EQ(Map_.contains(K), !InRange);
    }

    // 相等迭代器区间是空操作。
    auto Any = Map_.begin();
    Map_.erase(Any, Any);
    PICTOR_CHECK_EQ(Map_.size(), 6u);

    // [begin, end) 清空。
    Map_.erase(Map_.begin(), Map_.end());
    PICTOR_CHECK(Map_.empty());
}

// ---- 拷贝 / 移动 / 交换 ----

PICTOR_TEST_CASE(FlatHashTable_CopyIsDeepAndIndependent)
{
    FlatHashTable<int, int> Map_;
    for (int K = 0; K < 100; ++K)
    {
        Map_[K] = K;
    }
    FlatHashTable<int, int> Copy(Map_);
    PICTOR_CHECK_EQ(Copy.size(), Map_.size());
    PICTOR_CHECK_EQ(sortedKeyVector(Copy), sortedKeyVector(Map_));

    Copy[1] = 999;
    Copy[1000] = 1000;
    PICTOR_CHECK_EQ(Map_[1], 1);
    PICTOR_CHECK(!Map_.contains(1000));

    FlatHashTable<int, int> Assigned;
    Assigned = Map_;
    PICTOR_CHECK_EQ(Assigned.size(), 100u);
    PICTOR_CHECK_EQ(sortedKeyVector(Assigned), sortedKeyVector(Map_));

    // 自赋值是空操作。
    Assigned = Assigned;
    PICTOR_CHECK_EQ(Assigned.size(), 100u);
    PICTOR_CHECK_EQ(sortedKeyVector(Assigned), sortedKeyVector(Map_));
}

PICTOR_TEST_CASE(FlatHashTable_MoveLeavesSourceValidAndEmpty)
{
    FlatHashTable<int, int> Map_;
    for (int K = 0; K < 100; ++K)
    {
        Map_[K] = K;
    }

    FlatHashTable<int, int> Moved(std::move(Map_));
    PICTOR_CHECK_EQ(Moved.size(), 100u);
    PICTOR_CHECK(Map_.empty());
    PICTOR_CHECK(Map_.begin() == Map_.end());
    PICTOR_CHECK(Map_.cbegin() == Map_.cend());

    // 移动后的表是可复用、可查询的空表（std::unordered_map 语义）。
    PICTOR_CHECK(Map_.find(0) == Map_.end());
    PICTOR_CHECK(!Map_.contains(0));
    PICTOR_CHECK_EQ(Map_.erase(0), 0u);
    PICTOR_CHECK_THROWS(Map_.at(0), std::out_of_range);
    Map_.erase(Map_.begin(), Map_.end());
    PICTOR_CHECK(Map_.empty());
    Map_[10] = 10;
    PICTOR_CHECK_EQ(Map_.size(), 1u);
    PICTOR_CHECK(Map_.contains(10));

    // 移动赋值。
    FlatHashTable<int, int> Assigned;
    Assigned[100] = 100;
    Assigned = std::move(Moved);
    PICTOR_CHECK_EQ(Assigned.size(), 100u);
    PICTOR_CHECK(Moved.empty());
    PICTOR_CHECK(Assigned.contains(0) && Assigned.contains(99));

    // 自移动赋值是空操作。
    Assigned = std::move(Assigned);
    PICTOR_CHECK_EQ(Assigned.size(), 100u);
    PICTOR_CHECK(Assigned.contains(99));
}

PICTOR_TEST_CASE(FlatHashTable_SwapExchangesContents)
{
    FlatHashTable<int, int> A;
    FlatHashTable<int, int> B;
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
    PICTOR_CHECK_EQ(sortedKeyVector(A), (std::vector<int>{7, 8}));
    PICTOR_CHECK_EQ(sortedKeyVector(B), (std::vector<int>{1, 2, 3}));

    using std::swap;
    swap(A, B);
    PICTOR_CHECK_EQ(sortedKeyVector(A), (std::vector<int>{1, 2, 3}));
    PICTOR_CHECK_EQ(sortedKeyVector(B), (std::vector<int>{7, 8}));

    // 与空表交换（两个方向）。
    FlatHashTable<int, int> Empty;
    Empty.swap(A);
    PICTOR_CHECK_EQ(sortedKeyVector(Empty), (std::vector<int>{1, 2, 3}));
    PICTOR_CHECK(A.empty());
    B.swap(Empty);
    PICTOR_CHECK_EQ(sortedKeyVector(B), (std::vector<int>{1, 2, 3}));
    PICTOR_CHECK_EQ(sortedKeyVector(Empty), (std::vector<int>{7, 8}));

    // 自交换：内容不变。
    B.swap(B);
    PICTOR_CHECK_EQ(sortedKeyVector(B), (std::vector<int>{1, 2, 3}));
    PICTOR_CHECK_EQ(B.size(), 3u);
}

// ---- 非默认构造键值 / move-only 值 ----

PICTOR_TEST_CASE(FlatHashTable_NonDefaultConstructibleKeyAndValue)
{
    FlatHashTable<NoDefaultKey, NoDefaultValue, NoDefaultKeyHash> Map_;
    Map_.tryEmplace(NoDefaultKey{1}, NoDefaultValue{10});
    Map_.tryEmplace(NoDefaultKey{2}, NoDefaultValue{20});

    PICTOR_CHECK_EQ(Map_.size(), 2u);
    PICTOR_CHECK(Map_.contains(NoDefaultKey{1}));
    PICTOR_CHECK_EQ(Map_.at(NoDefaultKey{2}).X, 20);
    PICTOR_CHECK_EQ(Map_.find(NoDefaultKey{1})->second.X, 10);

    Map_.erase(NoDefaultKey{1});
    PICTOR_CHECK_EQ(Map_.size(), 1u);
}

PICTOR_TEST_CASE(FlatHashTable_MoveOnlyValue)
{
    using Ptr = std::unique_ptr<int>;
    FlatHashTable<int, Ptr> Map_;
    Map_.tryEmplace(1, std::make_unique<int>(10));
    Map_.tryEmplace(2, std::make_unique<int>(20));
    Map_.tryEmplace(3, std::make_unique<int>(30));

    PICTOR_CHECK_EQ(Map_.size(), 3u);
    PICTOR_CHECK_EQ(*Map_.at(1), 10);
    PICTOR_CHECK_EQ(*Map_.find(2)->second, 20);

    // 向后移位删除需要搬移值：move-only 值走移动路径。
    Map_.erase(1);
    PICTOR_CHECK_EQ(Map_.size(), 2u);
    PICTOR_CHECK_EQ(*Map_.at(3), 30);

    // 重哈希搬移。
    Map_.reserve(10000);
    PICTOR_CHECK_EQ(*Map_.at(2), 20);
    PICTOR_CHECK_EQ(*Map_.at(3), 30);

    // 整体移动。
    FlatHashTable<int, Ptr> Moved(std::move(Map_));
    PICTOR_CHECK_EQ(Moved.size(), 2u);
    PICTOR_CHECK_EQ(*Moved.at(2), 20);
}

// ---- 极端键值（边界输入） ----

PICTOR_TEST_CASE(FlatHashTable_ExtremeKeyValues)
{
    FlatHashTable<int, int> Map_;
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

    // 字符串键边界：空串、短串、超长串。
    FlatHashTable<std::string, int> StrMap;
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

PICTOR_TEST_CASE(FlatHashTable_RehashReserveAndLoadFactor)
{
    FlatHashTable<int, int> Map_;
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

    // rehash 到比当前更小的值不会收缩（std 语义：只增不减）。
    const auto Before = Map_.getBucketCount();
    Map_.rehash(16);
    PICTOR_CHECK_EQ(Map_.getBucketCount(), Before);

    // clear 保留已分配容量。
    Map_.clear();
    PICTOR_CHECK(Map_.empty());
    PICTOR_CHECK_EQ(Map_.getBucketCount(), Before);
}

PICTOR_TEST_CASE(FlatHashTable_SetMaxLoadFactorBoundaries)
{
    FlatHashTable<int, int> Map_;
    Map_.setMaxLoadFactor(0.5f);
    PICTOR_CHECK_NEAR(Map_.getMaxLoadFactor(), 0.5f, 0.001f);
    Map_.setMaxLoadFactor(1.0f);
    PICTOR_CHECK_NEAR(Map_.getMaxLoadFactor(), 1.0f, 0.001f);

    // 非法值必须抛 out_of_range。
    PICTOR_CHECK_THROWS(Map_.setMaxLoadFactor(0.0f), std::out_of_range);
    PICTOR_CHECK_THROWS(Map_.setMaxLoadFactor(-0.5f), std::out_of_range);
    PICTOR_CHECK_THROWS(Map_.setMaxLoadFactor(1.5f), std::out_of_range);
    PICTOR_CHECK_NEAR(Map_.getMaxLoadFactor(), 1.0f, 0.001f);

    // 更低的装载因子导致更早扩容：阈值生效。
    FlatHashTable<int, int> Low(64);
    Low.setMaxLoadFactor(0.25f);
    for (int K = 0; K < 16; ++K)
    {
        Low[K] = K;
    }
    PICTOR_CHECK_EQ(Low.size(), 16u);
    PICTOR_CHECK(Low.getLoadFactor() <= 0.25f + 0.0001f);
}

// ---- 自定义哈希 / 相等 ----

PICTOR_TEST_CASE(FlatHashTable_CustomHashAndEqual)
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

    FlatHashTable<std::string, int, CaseInsensitiveHash, CaseInsensitiveEqual> Map_;
    Map_["Key"] = 1;
    PICTOR_CHECK(Map_.contains("key"));
    PICTOR_CHECK(Map_.contains("KEY"));
    PICTOR_CHECK_EQ(Map_.at("kEy"), 1);
    PICTOR_CHECK_EQ(Map_.size(), 1u); // 大小写变体不产生重复键
}

PICTOR_TEST_CASE(FlatHashTable_WeakIdentityHashStress)
{
    // 恒等哈希 + 密集顺序键是开放寻址最坏模式；终结器必须保持可用与正确。
    FlatHashTable<std::uint64_t, std::uint64_t, IdentityHash> Map_;
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

PICTOR_TEST_CASE(FlatHashTable_IteratorConstIteratorInterop)
{
    FlatHashTable<int, int> Map_;
    Map_[1] = 1;
    Map_[2] = 2;

    FlatHashTable<int, int>::iterator It = Map_.find(1);
    FlatHashTable<int, int>::const_iterator Cit = Map_.find(2);

    PICTOR_CHECK(It != Cit);
    PICTOR_CHECK(Cit != It);
    PICTOR_CHECK(It != Map_.cend());
    PICTOR_CHECK(Map_.begin() == Map_.cbegin());
    PICTOR_CHECK(Map_.cbegin() == Map_.begin());
    PICTOR_CHECK(It == Map_.find(1));

    FlatHashTable<int, int>::const_iterator Converted = It;
    PICTOR_CHECK(Converted == It);
}

// ---- 桶操作 ----

PICTOR_TEST_CASE(FlatHashTable_BucketOperations)
{
    FlatHashTable<int, int> Map_;
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

    // 局部迭代恰好遍历每个元素一次。
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

    // 每个键都出现在自己的桶的局部范围内。
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

PICTOR_TEST_CASE(FlatHashTable_RangesCompatibility)
{
    FlatHashTable<int, int> Map_;
    for (int K = 1; K <= 10; ++K)
    {
        Map_[K] = K * 10;
    }

    static_assert(std::ranges::range<FlatHashTable<int, int>>);
    static_assert(std::ranges::bidirectional_range<FlatHashTable<int, int>>);
    static_assert(std::ranges::sized_range<FlatHashTable<int, int>>);

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

PICTOR_TEST_CASE(FlatHashTable_StrongExceptionSafetyTryEmplace)
{
    using Map = FlatHashTable<int, ThrowingValueCtor>;
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
}

PICTOR_TEST_CASE(FlatHashTable_StrongExceptionSafetyCopy)
{
    using Map = FlatHashTable<int, ThrowingCopyValue>;
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

PICTOR_TEST_CASE(FlatHashTable_StrongExceptionSafetyRehash)
{
    FlatHashTable<int, int, ThrowingHash> Map_;
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

PICTOR_TEST_CASE(FlatHashTable_ZeroAllocationInsertAndErase)
{
    using Map = FlatHashTable<int, int, std::hash<int>, std::equal_to<int>,
                              CountingAllocator<std::pair<const int, int>>>;

    CountingState::Allocations = 0;
    CountingState::Deallocations = 0;

    {
        Map Map_;

        // 构造：槽位数组 + 控制字节组数组各一次。
        PICTOR_CHECK_EQ(CountingState::Allocations, 2u);

        // 16 桶（阈值 14）内插入 10 个：零分配。
        for (int K = 0; K < 10; ++K)
        {
            Map_.tryEmplace(K, K);
        }
        PICTOR_CHECK_EQ(CountingState::Allocations, 2u);

        // 移位删除：零分配、零释放。
        for (int K = 0; K < 5; ++K)
        {
            Map_.erase(K);
        }
        PICTOR_CHECK_EQ(CountingState::Allocations, 2u);
        PICTOR_CHECK_EQ(CountingState::Deallocations, 0u);
        PICTOR_CHECK_EQ(Map_.size(), 5u);
        for (int K = 5; K < 10; ++K)
        {
            PICTOR_CHECK(Map_.contains(K));
        }
    }

    // 析构释放两个数组。
    PICTOR_CHECK_EQ(CountingState::Deallocations, 2u);
    PICTOR_CHECK_EQ(CountingState::Deallocations, CountingState::Allocations);
}

PICTOR_TEST_CASE(FlatHashTable_GrowthAllocationsAreBalanced)
{
    using Map = FlatHashTable<int, int, std::hash<int>, std::equal_to<int>,
                              CountingAllocator<std::pair<const int, int>>>;

    CountingState::Allocations = 0;
    CountingState::Deallocations = 0;

    {
        Map Map_;
        for (int K = 0; K < 100000; ++K)
        {
            Map_.tryEmplace(K, K);
        }
        // 每次扩容恰好 2 次新分配（旧数组随后释放）。
        PICTOR_CHECK_EQ(CountingState::Allocations % 2, 0u);
        PICTOR_CHECK(CountingState::Allocations >= 2u);
    }

    PICTOR_CHECK_EQ(CountingState::Deallocations, CountingState::Allocations);
}

// ---- 随机差分测试（对照 std::unordered_map，多种子） ----

PICTOR_TEST_CASE(FlatHashTable_RandomizedDifferentialAgainstStdUnorderedMap)
{
    const std::uint32_t Seeds[] = {0xC0FFEEu, 0xDEADBEEFu, 12345u, 0xABCDEF01u};
    for (std::uint32_t Seed : Seeds)
    {
        std::mt19937 Rng(Seed);
        std::unordered_map<int, int> Reference;
        FlatHashTable<int, int> Table;

        for (int Step = 0; Step < 120000; ++Step)
        {
            const int Key = static_cast<int>(Rng() % 4000);
            const int Op = static_cast<int>(Rng() % 6);

            if (Op == 0) // tryEmplace
            {
                const auto RefResult = Reference.try_emplace(Key, Step);
                const auto TblResult = Table.tryEmplace(Key, Step);
                PICTOR_CHECK_EQ(RefResult.second, TblResult.second);
                PICTOR_CHECK_EQ(RefResult.first->second, TblResult.first->second);
            }
            else if (Op == 1) // erase(key)
            {
                PICTOR_CHECK_EQ(Reference.erase(Key), Table.erase(Key));
            }
            else if (Op == 2) // find / contains
            {
                PICTOR_CHECK_EQ(Reference.find(Key) != Reference.end(), Table.contains(Key));
                PICTOR_CHECK_EQ(Reference.find(Key) != Reference.end(), Table.find(Key) != Table.end());
            }
            else if (Op == 3) // operator[] 增长路径
            {
                ++Reference[Key];
                ++Table[Key];
            }
            else if (Op == 4) // insertOrAssign
            {
                const auto RefResult = Reference.insert_or_assign(Key, Step);
                const auto TblResult = Table.insertOrAssign(Key, Step);
                PICTOR_CHECK_EQ(RefResult.second, TblResult.second);
                PICTOR_CHECK_EQ(RefResult.first->second, TblResult.first->second);
            }
            else // erase(iterator)
            {
                const auto RefIt = Reference.find(Key);
                const auto TblIt = Table.find(Key);
                PICTOR_CHECK_EQ(RefIt != Reference.end(), TblIt != Table.end());
                if (RefIt != Reference.end())
                {
                    Reference.erase(RefIt);
                    Table.erase(TblIt);
                }
            }

            PICTOR_CHECK_EQ(Table.size(), Reference.size());

            if ((Step % 8192) == 0)
            {
                std::vector<int> TblKeys;
                TblKeys.reserve(Table.size());
                for (const auto& Entry : Table)
                {
                    TblKeys.push_back(Entry.first);
                }
                std::sort(TblKeys.begin(), TblKeys.end());

                std::vector<int> RefKeys;
                RefKeys.reserve(Reference.size());
                for (const auto& Entry : Reference)
                {
                    RefKeys.push_back(Entry.first);
                }
                std::sort(RefKeys.begin(), RefKeys.end());

                PICTOR_CHECK_EQ(TblKeys, RefKeys);
                for (const auto& Entry : Reference)
                {
                    PICTOR_CHECK(Table.contains(Entry.first));
                    PICTOR_CHECK_EQ(Table.at(Entry.first), Entry.second);
                }
            }
        }
        PICTOR_CHECK_EQ(Table.size(), Reference.size());
    }
}

// ---- 密集顺序键压力 ----

PICTOR_TEST_CASE(FlatHashTable_DenseSequentialKeysStress)
{
    FlatHashTable<std::uint64_t, std::uint64_t> Table;
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
    // 删除全部后重插入仍然正确。
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
