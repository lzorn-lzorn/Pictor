// Tests for infra/containers/FlatHashTable.hpp.
//
// FlatHashTable is a flat (open-addressing) hash map with inline values,
// in-place insertion and shift-delete erasure. Iteration order is memory order
// (unspecified, NOT insertion order) and iterators are invalidated by insert
// and erase, so these tests compare contents as multisets instead of checking
// ordering, and additionally verify the zero-allocation insert/erase property.

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

#include "infra/containers/FlatHashTable.hpp"

namespace
{
    template <typename Map>
    std::vector<int> sortedKeyVector(const Map& Map_)
    {
        std::vector<int> V;
        for (const auto& [K, Val] : Map_)
        {
            (void)Val;
            V.push_back(K);
        }
        std::sort(V.begin(), V.end());
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

    // ---- counting allocator (verifies metadata allocation + zero per-insert allocs) ----
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
    using Map = FlatHashTable<int, int>;
    static_assert(std::is_same_v<Map::iterator, Map::Iterator<false>>);
    static_assert(std::is_same_v<Map::const_iterator, Map::Iterator<true>>);
    static_assert(std::is_same_v<decltype(*std::declval<Map::iterator>()), Map::value_type&>);
    static_assert(std::is_same_v<decltype(*std::declval<Map::const_iterator>()), const Map::value_type&>);
    static_assert(std::is_convertible_v<Map::iterator, Map::const_iterator>);
    static_assert(!std::is_convertible_v<Map::const_iterator, Map::iterator>);
    PICTOR_CHECK(true);
}

// ---- insertion / lookup ----

PICTOR_TEST_CASE(InsertFindContainsAtOperatorBracket)
{
    FlatHashTable<int, std::string> Map_;
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

    // operator[] updates an existing key in place.
    Map_[1] = "ONE";
    PICTOR_CHECK_EQ(Map_.size(), 5u);
    PICTOR_CHECK_EQ(Map_.at(1), std::string("ONE"));
}

PICTOR_TEST_CASE(InsertRejectsDuplicateKey)
{
    FlatHashTable<int, int> Map_;
    Map_[1] = 100;
    const auto [It, Inserted] = Map_.insert({1, 200});
    PICTOR_CHECK(!Inserted);
    PICTOR_CHECK_EQ(Map_.size(), 1u);
    PICTOR_CHECK_EQ(It->second, 100); // existing value untouched
}

PICTOR_TEST_CASE(IterationVisitsAllElementsExactlyOnce)
{
    FlatHashTable<int, int> Map_;
    for (int K = 0; K < 100; ++K)
    {
        Map_[K] = K * 2;
    }
    PICTOR_CHECK_EQ(sortedKeyVector(Map_), [&] {
        std::vector<int> V;
        for (int K = 0; K < 100; ++K) V.push_back(K);
        return V;
    }());

    // Const iteration agrees.
    int Count = 0;
    for (auto It = Map_.cbegin(); It != Map_.cend(); ++It)
    {
        ++Count;
    }
    PICTOR_CHECK_EQ(Count, 100);
}

PICTOR_TEST_CASE(ReverseIterationVisitsAllElements)
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

PICTOR_TEST_CASE(PostIncrementAndDecrement)
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

// ---- erase ----

PICTOR_TEST_CASE(EraseByIteratorKeepsRemainingElements)
{
    FlatHashTable<int, int> Map_;
    for (int K = 0; K < 20; ++K)
    {
        Map_[K] = K;
    }

    // Erase every element one at a time through the returned successor.
    auto It = Map_.begin();
    while (It != Map_.end())
    {
        It = Map_.erase(It);
    }
    PICTOR_CHECK(Map_.empty());
    PICTOR_CHECK_EQ(Map_.size(), 0u);
}

PICTOR_TEST_CASE(EraseByKeyReturnsCount)
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
}

PICTOR_TEST_CASE(EraseRangeRemovesExactlyTheRange)
{
    FlatHashTable<int, int> Map_;
    for (int K = 0; K < 10; ++K)
    {
        Map_[K] = K;
    }

    // Take the first 4 elements in iteration order as the range.
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
}

// ---- copy / move / swap ----

PICTOR_TEST_CASE(CopyIsDeepAndIndependent)
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
}

PICTOR_TEST_CASE(MoveLeavesSourceValidAndEmpty)
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

    // Moved-from table is reusable (re-initializes on demand).
    Map_[10] = 10;
    PICTOR_CHECK_EQ(Map_.size(), 1u);

    FlatHashTable<int, int> Assigned;
    Assigned[100] = 100;
    Assigned = std::move(Moved);
    PICTOR_CHECK_EQ(Assigned.size(), 100u);
    PICTOR_CHECK(Moved.empty());
    PICTOR_CHECK(Assigned.contains(0) && Assigned.contains(99));
}

PICTOR_TEST_CASE(SwapExchangesContents)
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

    FlatHashTable<int, int> Empty;
    Empty.swap(A);
    PICTOR_CHECK_EQ(sortedKeyVector(Empty), (std::vector<int>{1, 2, 3}));
    PICTOR_CHECK(A.empty());
}

// ---- non-default-constructible key / value ----

PICTOR_TEST_CASE(NonDefaultConstructibleKeyAndValue)
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

// ---- hash policy ----

PICTOR_TEST_CASE(RehashReserveAndLoadFactor)
{
    FlatHashTable<int, int> Map_;
    PICTOR_CHECK_EQ(Map_.getBucketCount(), 16u);

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
}

PICTOR_TEST_CASE(AtThrowsOutOfRange)
{
    FlatHashTable<int, int> Map_;
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

    FlatHashTable<std::string, int, CaseInsensitiveHash, CaseInsensitiveEqual> Map_;
    Map_["Key"] = 1;
    PICTOR_CHECK(Map_.contains("key"));
    PICTOR_CHECK(Map_.contains("KEY"));
    PICTOR_CHECK_EQ(Map_.at("kEy"), 1);
}

// ---- iterator / const_iterator interoperability ----

PICTOR_TEST_CASE(IteratorConstIteratorInterop)
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

// ---- bucket operations ----

PICTOR_TEST_CASE(BucketOperations)
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

    // Local iteration visits every element exactly once across all buckets.
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

    // Each key is found in its own bucket's local range.
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

// ---- ranges compatibility ----

PICTOR_TEST_CASE(RangesCompatibility)
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

// ---- strong exception safety ----

PICTOR_TEST_CASE(StrongExceptionSafetyTryEmplace)
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

PICTOR_TEST_CASE(StrongExceptionSafetyCopy)
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

PICTOR_TEST_CASE(StrongExceptionSafetyRehash)
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

// ---- zero allocation overhead ----

PICTOR_TEST_CASE(ZeroAllocationInsertAndErase)
{
    using Map = FlatHashTable<int, int, std::hash<int>, std::equal_to<int>,
                              CountingAllocator<std::pair<const int, int>>>;

    CountingState::Allocations = 0;
    CountingState::Deallocations = 0;

    {
        Map Map_;

        // Construction: one slot array + one control-byte group array.
        PICTOR_CHECK_EQ(CountingState::Allocations, 2u);

        // 10 inserts into 16 buckets (growth threshold 14): zero allocations.
        for (int K = 0; K < 10; ++K)
        {
            Map_.tryEmplace(K, K);
        }
        PICTOR_CHECK_EQ(CountingState::Allocations, 2u);

        // Shift-delete erasure: no allocation, no deallocation.
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

    // Destruction frees both arrays.
    PICTOR_CHECK_EQ(CountingState::Deallocations, 2u);
    PICTOR_CHECK_EQ(CountingState::Deallocations, CountingState::Allocations);
}

// ---- randomized differential vs std::unordered_map ----

PICTOR_TEST_CASE(RandomizedDifferentialAgainstStdUnorderedMap)
{
    std::mt19937 Rng(0xC0FFEE);
    std::unordered_map<int, int> Reference;
    FlatHashTable<int, int> Table;

    int FailStep = -1;
    for (int Step = 0; Step < 200000; ++Step)
    {
        const int Key = static_cast<int>(Rng() % 4000);
        const int Op = static_cast<int>(Rng() % 4);

        if (Op == 0)
        {
            const auto RefResult = Reference.emplace(Key, Step);
            const auto TblResult = Table.tryEmplace(Key, Step);
            if (RefResult.second != TblResult.second ||
                RefResult.first->second != TblResult.first->second)
            {
                FailStep = Step;
                break;
            }
        }
        else if (Op == 1)
        {
            if (Reference.erase(Key) != Table.erase(Key))
            {
                FailStep = Step;
                break;
            }
        }
        else if (Op == 2)
        {
            if ((Reference.find(Key) != Reference.end()) != Table.contains(Key))
            {
                FailStep = Step;
                break;
            }
        }
        else
        {
            ++Reference[Key];
            ++Table[Key];
        }

        if (Table.size() != Reference.size())
        {
            FailStep = Step;
            break;
        }

        if ((Step % 8192) == 0)
        {
            // Contents (as a multiset) and values must agree.
            std::vector<int> TblKeys;
            TblKeys.reserve(Table.size());
            for (const auto& [K, V] : Table)
            {
                (void)V;
                TblKeys.push_back(K);
            }
            std::sort(TblKeys.begin(), TblKeys.end());

            std::vector<int> RefKeys;
            RefKeys.reserve(Reference.size());
            for (const auto& [K, V] : Reference)
            {
                (void)V;
                RefKeys.push_back(K);
            }
            std::sort(RefKeys.begin(), RefKeys.end());

            if (TblKeys != RefKeys)
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
}

// ---- dense sequential keys (weak identity-hash stress) ----

PICTOR_TEST_CASE(DenseSequentialKeysStress)
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
}

int main()
{
    return ::PictorTest::runAll();
}
