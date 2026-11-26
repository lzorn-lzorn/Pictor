// Tests for infra/containers/LinkedHashTable.hpp.
//
// LinkedHashTable is a std::unordered_map-compatible hash table that preserves
// insertion order via an internal doubly-linked list. These tests verify the
// unordered-map contract (insert/find/erase/at/operator[], copy/move/swap) plus
// the insertion-order guarantee and a randomized differential run against
// std::unordered_map.

#include "TestFramework.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
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

#include "infra/containers/LinkedHashTable.hpp"

namespace
{
    // Collects keys in iteration order for readable diagnostics.
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
        for (const auto& [K, Val] : Map_)
        {
            (void)Val;
            V.push_back(K);
        }
        return V;
    }

    // Types without a default constructor — the container must never need one.
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

    // ---- exception-safety probes ----

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

    // ---- counting allocator (verifies metadata allocation goes through it) ----
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

// ---- type-level contracts ----

PICTOR_TEST_CASE(IteratorTypesAndDereference)
{
    using Map = LinkedHashTable<int, int>;
    static_assert(std::is_same_v<Map::iterator, Map::Iterator<false>>);
    static_assert(std::is_same_v<Map::const_iterator, Map::Iterator<true>>);
    static_assert(std::is_same_v<decltype(*std::declval<Map::iterator>()), Map::value_type&>);
    static_assert(std::is_same_v<decltype(*std::declval<Map::const_iterator>()), const Map::value_type&>);
    static_assert(std::is_convertible_v<Map::iterator, Map::const_iterator>);
    static_assert(!std::is_convertible_v<Map::const_iterator, Map::iterator>);
    PICTOR_CHECK(true);
}

// ---- insertion / lookup / iteration ----

PICTOR_TEST_CASE(InsertFindContainsAtOperatorBracket)
{
    LinkedHashTable<int, std::string> Map_;
    Map_.insert({1, "one"});
    Map_.insert(std::make_pair(2, "two"));
    Map_.emplace(3, "three");
    Map_.tryEmplace(4, "four");
    Map_[5] = "five";

    PICTOR_CHECK_EQ(Map_.size(), 5u);
    PICTOR_CHECK(!Map_.empty());
    PICTOR_CHECK(Map_.contains(1));
    PICTOR_CHECK(!Map_.contains(99));
    PICTOR_CHECK_EQ(Map_.at(3), std::string("three"));
    PICTOR_CHECK_EQ(Map_[5], std::string("five"));
    PICTOR_CHECK(Map_.find(2) != Map_.end());
    PICTOR_CHECK_EQ(Map_.find(2)->second, std::string("two"));
    PICTOR_CHECK(Map_.find(42) == Map_.end());
}

PICTOR_TEST_CASE(InsertRejectsDuplicateKey)
{
    LinkedHashTable<int, int> Map_;
    Map_[1] = 100;
    const auto [It, Inserted] = Map_.insert({1, 200});
    PICTOR_CHECK(!Inserted);
    PICTOR_CHECK_EQ(Map_.size(), 1u);
    PICTOR_CHECK_EQ(It->second, 100); // existing value untouched
}

PICTOR_TEST_CASE(InsertionOrderPreserved)
{
    LinkedHashTable<int, int> Map_;
    for (int K : {5, 3, 7, 1, 9, 6, 8, 2, 4})
    {
        Map_[K] = K;
    }
    PICTOR_CHECK_EQ(collectKeys(Map_), "5,3,7,1,9,6,8,2,4");

    // Re-inserting an existing key must not change its position.
    Map_.insert({3, 0});
    PICTOR_CHECK_EQ(collectKeys(Map_), "5,3,7,1,9,6,8,2,4");
}

PICTOR_TEST_CASE(ReverseIteration)
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

PICTOR_TEST_CASE(PostIncrementAndDecrement)
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

// ---- erase ----

PICTOR_TEST_CASE(EraseByIteratorReturnsSuccessorAndKeepsOrder)
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

    // Erasing the last element returns end().
    PICTOR_CHECK(Map_.erase(Map_.find(5)) == Map_.end());
    PICTOR_CHECK_EQ(collectKeys(Map_), "1,2,4");
}

PICTOR_TEST_CASE(EraseByKeyReturnsCount)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 1; K <= 3; ++K)
    {
        Map_[K] = K;
    }
    PICTOR_CHECK_EQ(Map_.erase(2), 1u);
    PICTOR_CHECK_EQ(Map_.erase(2), 0u);
    PICTOR_CHECK_EQ(collectKeys(Map_), "1,3");
}

PICTOR_TEST_CASE(EraseRange)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 1; K <= 10; ++K)
    {
        Map_[K] = K;
    }
    const auto First = Map_.find(3);
    const auto Last = Map_.find(7);
    const auto Next = Map_.erase(First, Last); // removes 3..6
    PICTOR_CHECK_EQ(collectKeys(Map_), "1,2,7,8,9,10");
    PICTOR_CHECK_EQ(Next->first, 7);
}

// ---- copy / move / swap ----

PICTOR_TEST_CASE(CopyIsDeepAndIndependent)
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

    // Copy assignment.
    LinkedHashTable<int, int> Assigned;
    Assigned = Map_;
    PICTOR_CHECK_EQ(collectKeys(Assigned), "1,2,3,4,5");
    PICTOR_CHECK_EQ(Assigned.size(), 5u);
}

PICTOR_TEST_CASE(MoveLeavesSourceValidAndEmpty)
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

    // The moved-from table must remain reusable and destructible.
    Map_[10] = 10;
    PICTOR_CHECK_EQ(Map_.size(), 1u);
    PICTOR_CHECK(Map_.contains(10));

    // Move assignment.
    LinkedHashTable<int, int> Assigned;
    Assigned[100] = 100;
    Assigned = std::move(Moved);
    PICTOR_CHECK_EQ(collectKeys(Assigned), "1,2,3,4,5");
    PICTOR_CHECK(Moved.empty());
}

PICTOR_TEST_CASE(SwapExchangesContents)
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

    // Swapping an empty with a non-empty table must relink sentinels correctly.
    LinkedHashTable<int, int> Empty;
    Empty.swap(A);
    PICTOR_CHECK_EQ(collectKeys(Empty), "1,2,3");
    PICTOR_CHECK(A.empty());
    PICTOR_CHECK_EQ(Empty.size(), 3u);
}

// ---- non-default-constructible key / value ----

PICTOR_TEST_CASE(NonDefaultConstructibleKeyAndValue)
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

// ---- hash policy ----

PICTOR_TEST_CASE(RehashReserveAndLoadFactor)
{
    LinkedHashTable<int, int> Map_;
    PICTOR_CHECK_EQ(Map_.getBucketCount(), 16u);

    for (int K = 0; K < 100; ++K)
    {
        Map_[K] = K;
    }
    PICTOR_CHECK(Map_.getBucketCount() >= 128u); // grew past 16 via rehashing
    PICTOR_CHECK(Map_.getLoadFactor() <= Map_.getMaxLoadFactor() + 0.0001f);

    Map_.reserve(10000);
    PICTOR_CHECK(Map_.getBucketCount() >= 10000u);

    for (int K = 0; K < 100; ++K)
    {
        PICTOR_CHECK(Map_.contains(K));
    }
}

PICTOR_TEST_CASE(AtThrowsOutOfRange)
{
    LinkedHashTable<int, int> Map_;
    bool Threw = false;
    try
    {
        (void)Map_.at(1);
    }
    catch (const std::out_of_range&)
    {
        Threw = true;
    }
    PICTOR_CHECK(Threw);
}

// ---- custom hash / equality ----

PICTOR_TEST_CASE(CustomHashAndEqual)
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
}

// ---- randomized differential vs std::unordered_map ----

PICTOR_TEST_CASE(RandomizedDifferentialAgainstStdUnorderedMap)
{
    std::mt19937 Rng(0xC0FFEE);
    std::unordered_map<int, int> Reference;
    LinkedHashTable<int, int> Table;
    std::list<int> InsertionOrder; // keys in insertion order (present keys only)

    int FailStep = -1;
    for (int Step = 0; Step < 200000; ++Step)
    {
        const int Key = static_cast<int>(Rng() % 4000);
        const int Op = static_cast<int>(Rng() % 4);

        if (Op == 0) // insert / tryEmplace
        {
            const auto RefResult = Reference.emplace(Key, Step);
            const auto TblResult = Table.tryEmplace(Key, Step);
            if (RefResult.second != TblResult.second)
            {
                FailStep = Step;
                break;
            }
            if (RefResult.second)
            {
                InsertionOrder.push_back(Key);
            }
            if (RefResult.first->second != TblResult.first->second)
            {
                FailStep = Step;
                break;
            }
        }
        else if (Op == 1) // erase
        {
            const auto RefCount = Reference.erase(Key);
            const auto TblCount = Table.erase(Key);
            if (RefCount != TblCount)
            {
                FailStep = Step;
                break;
            }
            if (RefCount != 0)
            {
                InsertionOrder.remove(Key);
            }
        }
        else if (Op == 2) // find / contains
        {
            const bool RefHas = Reference.find(Key) != Reference.end();
            if (RefHas != Table.contains(Key))
            {
                FailStep = Step;
                break;
            }
        }
        else // operator[] growth path (exercises rehash under mixed load)
        {
            ++Reference[Key];
            ++Table[Key];
            if (Reference[Key] == 1)
            {
                InsertionOrder.push_back(Key);
            }
        }

        if (Table.size() != Reference.size())
        {
            FailStep = Step;
            break;
        }

        // Periodically verify full agreement, including insertion order.
        if ((Step % 8192) == 0)
        {
            if (keyVector(Table) != std::vector<int>(InsertionOrder.begin(), InsertionOrder.end()))
            {
                FailStep = Step;
                break;
            }
            for (const auto& [K, V] : Reference)
            {
                if (!Table.contains(K) || Table.at(K) != V)
                {
                    FailStep = Step;
                    break;
                }
            }
        }
    }

    PICTOR_CHECK_EQ(FailStep, -1);
    PICTOR_CHECK_EQ(Table.size(), Reference.size());
    PICTOR_CHECK_EQ(Table.size(), InsertionOrder.size());
}

// ---- dense sequential keys (weak identity-hash stress) ----

PICTOR_TEST_CASE(DenseSequentialKeysStress)
{
    // Sequential keys defeat an unmixed identity hash; the built-in finalizer
    // must still keep the table functional and distributed.
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
}

// ---- iterator / const_iterator interoperability ----

PICTOR_TEST_CASE(IteratorConstIteratorInterop)
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

    // iterator converts to const_iterator.
    LinkedHashTable<int, int>::const_iterator Converted = It;
    PICTOR_CHECK(Converted == It);
    PICTOR_CHECK(It == Converted);
}

// ---- bucket operations ----

PICTOR_TEST_CASE(BucketOperations)
{
    LinkedHashTable<int, int> Map_;
    for (int K = 0; K < 100; ++K)
    {
        Map_[K] = K;
    }

    const auto BC = Map_.getBucketCount();
    PICTOR_CHECK(BC >= 128u);
    PICTOR_CHECK(Map_.getMaxBucketCount() >= BC);

    // bucket(k) is in range.
    for (int K = 0; K < 100; ++K)
    {
        PICTOR_CHECK(Map_.getBucket(K) < BC);
    }

    // getBucketSize sums to size().
    std::size_t Total = 0;
    for (std::size_t I = 0; I < BC; ++I)
    {
        Total += Map_.getBucketSize(I);
    }
    PICTOR_CHECK_EQ(Total, Map_.size());

    // Local iteration visits every element exactly once across all buckets.
    std::vector<int> Seen;
    for (std::size_t I = 0; I < BC; ++I)
    {
        for (auto It = Map_.beginBucket(I); It != Map_.endBucket(I); ++It)
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

    // Each key is found in its own bucket's local range.
    for (int K = 0; K < 100; ++K)
    {
        bool Found = false;
        for (auto It = Map_.cbeginBucket(Map_.getBucket(K)); It != Map_.cendBucket(Map_.getBucket(K)); ++It)
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

// ---- ranges compatibility ----

PICTOR_TEST_CASE(RangesCompatibility)
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

// ---- strong exception safety ----

PICTOR_TEST_CASE(StrongExceptionSafetyTryEmplace)
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

    // Table unchanged after the failed insert.
    PICTOR_CHECK_EQ(Map_.size(), 1u);
    PICTOR_CHECK(Map_.contains(1));
    PICTOR_CHECK(!Map_.contains(2));

    // Still fully usable.
    Map_.tryEmplace(2, 20);
    PICTOR_CHECK_EQ(Map_.size(), 2u);
    PICTOR_CHECK(Map_.contains(2));
}

PICTOR_TEST_CASE(StrongExceptionSafetyCopy)
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

PICTOR_TEST_CASE(StrongExceptionSafetyRehash)
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

    // Stop the hasher from throwing again so the invariant checks can run.
    ThrowingHash::ThrowAt = std::numeric_limits<std::size_t>::max();

    // Table unchanged: same bucket count, same size, all elements present.
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

// ---- metadata allocated through the user allocator ----

PICTOR_TEST_CASE(GroupsAllocatedViaAllocator)
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

        // 3 node allocations + 1 bucket array + 1 control-byte group array.
        PICTOR_CHECK_EQ(CountingState::Allocations, 5u);
        PICTOR_CHECK_EQ(Map_.size(), 3u);
    }

    // Destruction must balance every allocation.
    PICTOR_CHECK_EQ(CountingState::Deallocations, CountingState::Allocations);
}

int main()
{
    return ::PictorTest::runAll();
}
