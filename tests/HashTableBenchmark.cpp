// HashTableBenchmark — measures FlatHashTable vs LinkedHashTable vs
// std::unordered_map using the project's Bm benchmark framework
// (infra/tracer/Benchmark.hpp).
//
// Usage:
//   HashTableBenchmark.exe [output.json]
// Results are written as JSON (Bm::writeJson) and a compact summary is printed
// to stdout.
//
// Benchmark list (auto-calibrated to ~100 ms each):
//   *.insertAll — build a fresh 200k-entry table (includes growth/rehash).
//   *.eraseAll  — build a fresh table, then erase every key (shift-delete for
//                 the flat table); pure erase cost ≈ eraseAll − insertAll.
//   *.findHit   — per-op find of a present key (rotating).
//   *.findMiss  — per-op find of an absent key (rotating).
//   *.iterate   — full traversal summing all values.

#include "infra/tracer/Benchmark.hpp"

#include "infra/containers/FlatHashTable.hpp"
#include "infra/containers/LinkedHashTable.hpp"

#include <cstddef>
#include <cstdio>
#include <random>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace
{
    constexpr int KeyCount = 200000;

    // Shared workload state, built once before benchmarking.
    std::vector<int> Keys;      // keys present in every table (0 .. 4N)
    std::vector<int> MissKeys;  // keys absent from every table (4N .. 8N)

    FlatHashTable<int, int> GFlat;
    LinkedHashTable<int, int> GLinked;
    std::unordered_map<int, int> GStd;

    std::size_t GIndex = 0;
    volatile long GSink = 0; // prevents dead-code elimination

    void buildWorkload()
    {
        std::mt19937 Rng(20261126);
        Keys.reserve(KeyCount);
        for (int I = 0; I < KeyCount; ++I)
        {
            Keys.push_back(static_cast<int>(Rng() % (KeyCount * 4)));
        }
        MissKeys.reserve(KeyCount);
        for (int I = 0; I < KeyCount; ++I)
        {
            MissKeys.push_back(KeyCount * 4 + static_cast<int>(Rng() % (KeyCount * 4)));
        }

        for (int K : Keys)
        {
            GFlat.tryEmplace(K, K);
            GLinked.tryEmplace(K, K);
            GStd.try_emplace(K, K);
        }
        GSink += static_cast<long>(GFlat.size() + GLinked.size() + GStd.size());
    }

    // ---- whole-table workload benchmarks ----

    void bmFlatInsertAll()
    {
        FlatHashTable<int, int> Table;
        for (int K : Keys)
        {
            Table.tryEmplace(K, K);
        }
        GSink += static_cast<long>(Table.size());
    }
    void bmLinkedInsertAll()
    {
        LinkedHashTable<int, int> Table;
        for (int K : Keys)
        {
            Table.tryEmplace(K, K);
        }
        GSink += static_cast<long>(Table.size());
    }
    void bmStdInsertAll()
    {
        std::unordered_map<int, int> Table;
        for (int K : Keys)
        {
            Table.try_emplace(K, K);
        }
        GSink += static_cast<long>(Table.size());
    }

    void bmFlatEraseAll()
    {
        FlatHashTable<int, int> Table;
        for (int K : Keys)
        {
            Table.tryEmplace(K, K);
        }
        for (int K : Keys)
        {
            Table.erase(K);
        }
        GSink += static_cast<long>(Table.size());
    }
    void bmLinkedEraseAll()
    {
        LinkedHashTable<int, int> Table;
        for (int K : Keys)
        {
            Table.tryEmplace(K, K);
        }
        for (int K : Keys)
        {
            Table.erase(K);
        }
        GSink += static_cast<long>(Table.size());
    }
    void bmStdEraseAll()
    {
        std::unordered_map<int, int> Table;
        for (int K : Keys)
        {
            Table.try_emplace(K, K);
        }
        for (int K : Keys)
        {
            Table.erase(K);
        }
        GSink += static_cast<long>(Table.size());
    }

    // ---- per-op benchmarks (rotating key, pre-built tables) ----

    void bmFlatFindHit()
    {
        const int K = Keys[GIndex++ % Keys.size()];
        if (GFlat.find(K) != GFlat.end())
        {
            GSink += 1;
        }
    }
    void bmLinkedFindHit()
    {
        const int K = Keys[GIndex++ % Keys.size()];
        if (GLinked.find(K) != GLinked.end())
        {
            GSink += 1;
        }
    }
    void bmStdFindHit()
    {
        const int K = Keys[GIndex++ % Keys.size()];
        if (GStd.find(K) != GStd.end())
        {
            GSink += 1;
        }
    }

    void bmFlatFindMiss()
    {
        const int K = MissKeys[GIndex++ % MissKeys.size()];
        if (GFlat.find(K) != GFlat.end())
        {
            GSink += 1;
        }
    }
    void bmLinkedFindMiss()
    {
        const int K = MissKeys[GIndex++ % MissKeys.size()];
        if (GLinked.find(K) != GLinked.end())
        {
            GSink += 1;
        }
    }
    void bmStdFindMiss()
    {
        const int K = MissKeys[GIndex++ % MissKeys.size()];
        if (GStd.find(K) != GStd.end())
        {
            GSink += 1;
        }
    }

    // ---- full-traversal benchmarks ----

    void bmFlatIterate()
    {
        long Sum = 0;
        for (const auto& Entry : GFlat)
        {
            Sum += Entry.second;
        }
        GSink += Sum;
    }
    void bmLinkedIterate()
    {
        long Sum = 0;
        for (const auto& Entry : GLinked)
        {
            Sum += Entry.second;
        }
        GSink += Sum;
    }
    void bmStdIterate()
    {
        long Sum = 0;
        for (const auto& Entry : GStd)
        {
            Sum += Entry.second;
        }
        GSink += Sum;
    }

    const Bm::BenchmarkResult& findResult(const std::vector<Bm::BenchmarkResult>& Results,
                                          const std::string& Name)
    {
        for (const Bm::BenchmarkResult& Result : Results)
        {
            if (Result.Name == Name)
            {
                return Result;
            }
        }
        throw std::runtime_error("missing benchmark result: " + Name);
    }

    void printSummary(const std::vector<Bm::BenchmarkResult>& Results)
    {
        const auto& FlatIns = findResult(Results, "flat.insertAll");
        const auto& LinkedIns = findResult(Results, "linked.insertAll");
        const auto& StdIns = findResult(Results, "std.insertAll");

        const auto& FlatErase = findResult(Results, "flat.eraseAll");
        const auto& LinkedErase = findResult(Results, "linked.eraseAll");
        const auto& StdErase = findResult(Results, "std.eraseAll");

        const auto& FlatHit = findResult(Results, "flat.findHit");
        const auto& LinkedHit = findResult(Results, "linked.findHit");
        const auto& StdHit = findResult(Results, "std.findHit");

        const auto& FlatMiss = findResult(Results, "flat.findMiss");
        const auto& LinkedMiss = findResult(Results, "linked.findMiss");
        const auto& StdMiss = findResult(Results, "std.findMiss");

        const auto& FlatIter = findResult(Results, "flat.iterate");
        const auto& LinkedIter = findResult(Results, "linked.iterate");
        const auto& StdIter = findResult(Results, "std.iterate");

        std::printf("== HashTableBenchmark: FlatHashTable vs LinkedHashTable vs std::unordered_map ==\n");
        std::printf("workload: %d random int keys\n", KeyCount);
        std::printf("%-13s %14s %14s %14s\n", "", "flat", "linked", "std");

        const auto printMs = [](const char* Label, const Bm::BenchmarkResult& A,
                                const Bm::BenchmarkResult& B, const Bm::BenchmarkResult& C)
        {
            std::printf("%-13s %14.2f %14.2f %14.2f ms\n", Label,
                        A.MeanTimeNanoseconds / 1e6,
                        B.MeanTimeNanoseconds / 1e6,
                        C.MeanTimeNanoseconds / 1e6);
        };
        const auto printNs = [](const char* Label, const Bm::BenchmarkResult& A,
                                const Bm::BenchmarkResult& B, const Bm::BenchmarkResult& C)
        {
            std::printf("%-13s %14.1f %14.1f %14.1f ns/op\n", Label,
                        A.MeanTimeNanoseconds,
                        B.MeanTimeNanoseconds,
                        C.MeanTimeNanoseconds);
        };

        printMs("insertAll", FlatIns, LinkedIns, StdIns);

        // Pure erase cost = (insert + erase) workload minus insert-only workload.
        std::printf("%-13s %14.2f %14.2f %14.2f ms  (eraseAll - insertAll)\n", "eraseAll*",
                    (FlatErase.MeanTimeNanoseconds - FlatIns.MeanTimeNanoseconds) / 1e6,
                    (LinkedErase.MeanTimeNanoseconds - LinkedIns.MeanTimeNanoseconds) / 1e6,
                    (StdErase.MeanTimeNanoseconds - StdIns.MeanTimeNanoseconds) / 1e6);

        printNs("findHit", FlatHit, LinkedHit, StdHit);
        printNs("findMiss", FlatMiss, LinkedMiss, StdMiss);
        printMs("iterate", FlatIter, LinkedIter, StdIter);

        std::printf("(sink=%ld)\n", static_cast<long>(GSink));
    }
} // namespace

// ---- benchmark registration (project Bm framework) ----

BENCHMARK_NAME("flat.insertAll", bmFlatInsertAll);
BENCHMARK_NAME("linked.insertAll", bmLinkedInsertAll);
BENCHMARK_NAME("std.insertAll", bmStdInsertAll);
BENCHMARK_NAME("flat.eraseAll", bmFlatEraseAll);
BENCHMARK_NAME("linked.eraseAll", bmLinkedEraseAll);
BENCHMARK_NAME("std.eraseAll", bmStdEraseAll);
BENCHMARK_NAME("flat.findHit", bmFlatFindHit);
BENCHMARK_NAME("linked.findHit", bmLinkedFindHit);
BENCHMARK_NAME("std.findHit", bmStdFindHit);
BENCHMARK_NAME("flat.findMiss", bmFlatFindMiss);
BENCHMARK_NAME("linked.findMiss", bmLinkedFindMiss);
BENCHMARK_NAME("std.findMiss", bmStdFindMiss);
BENCHMARK_NAME("flat.iterate", bmFlatIterate);
BENCHMARK_NAME("linked.iterate", bmLinkedIterate);
BENCHMARK_NAME("std.iterate", bmStdIterate);

int main(int Argc, char** Argv)
{
    buildWorkload();

    Bm::BenchmarkOptions Options;
    Options.TargetTotalNanoseconds = 100000000LL; // ~100 ms per benchmark for stable means
    Options.WarmupIterations = 2;
    Options.TimeUnit = Bm::ETimeUnit::Millisecond;

    const std::vector<Bm::BenchmarkResult> Results = Bm::runAll(Options);

    const std::string JsonPath = Argc > 1 ? Argv[1] : "hash_table_benchmark.json";
    try
    {
        Bm::writeJson(JsonPath, Results, Options);
        std::printf("json written to: %s\n", JsonPath.c_str());
    }
    catch (const std::exception& Exception)
    {
        std::fprintf(stderr, "benchmark error: %s\n", Exception.what());
        return 1;
    }

    printSummary(Results);
    return 0;
}
