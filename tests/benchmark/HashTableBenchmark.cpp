// HashTableBenchmark.cpp — FlatHashTable / LinkedHashTable 与 std::unordered_map
// 的多场景性能对比（std::unordered_map 为基准 = 1.00x）。
//
// 场景设计（参考 Google Benchmark 的多场景 + 批量折算做法）：
//   insert.grow       空表插入 N 个随机键（含动态扩容重哈希）
//   insert.reserved   reserve(N) 后插入 N 个随机键（排除扩容成本的上限）
//   insert.sequential 按升序插入 0..N-1 顺序键（最坏聚集模式）
//   findHit / findMiss 预构建表上的命中 / 未命中查找（每次调用批量 1024 次）
//   eraseAll          删除全部键；报表中净耗时 = 自身 − insert.grow（抵消建表）
//   iterate           全量遍历求和（每元素折算）
//   copy              整体拷贝预构建表（每元素折算）
//   mixed             混合读写：命中查找 / 插入 / 删除（每次调用 4096 次）
//   stringInsert / stringFindHit  24 字符字符串键（超过 SSO，贴近真实缓存行为）
//
// 公平性说明：三种实现共享同一份预生成键序列；每次计时调用都从完全相同的
// 初始状态开始，因此测量的是同一个语义工作负载的纯执行成本。

#include "benchmark/BenchmarkCommon.hpp"

#include <infra/containers/FlatHashTable.hpp>
#include <infra/containers/LinkedHashTable.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // 规模与批量（全部取 2 的幂，旋转索引用掩码代替取模，摊薄计时框架开销）。
    constexpr std::size_t HitN = 1u << 17;  // 131072, 标签 "128k"
    constexpr std::size_t GrowN = 1u << 20; // 1048576, 标签 "1m"
    constexpr std::size_t StrN = 1u << 17;
    constexpr std::size_t BatchFind = 1024;
    constexpr std::size_t BatchMixed = 4096;

    // 共享工作负载（所有实现使用同一份键序列）。
    std::vector<int> HitKeys;       // HitN 个互不相同的随机键
    std::vector<int> MissKeys;      // HitN 个与 HitKeys 不相交的随机键
    std::vector<int> GrowKeys;      // GrowN 个随机键（范围 31 bit，重复概率可忽略）
    std::vector<std::size_t> EraseOrder;   // 删除顺序（0..HitN-1 的随机排列）
    std::vector<std::size_t> MixedKeyIdx;  // 混合场景的键下标序列（BatchMixed 个）
    std::vector<std::string> StrHitKeys;   // StrN 个互不相同的 24 字符字符串
    std::vector<std::string> StrMissKeys;

    // 预构建常驻表（per-op / iterate / copy 场景）。
    FlatHashTable<int, int> GFlatHit;
    LinkedHashTable<int, int> GLinkedHit;
    std::unordered_map<int, int> GStdHit;

    FlatHashTable<std::string, std::int64_t> GFlatStr;
    LinkedHashTable<std::string, std::int64_t> GLinkedStr;
    std::unordered_map<std::string, std::int64_t> GStdStr;

    std::size_t GFindIdx = 0;
    std::size_t GStrIdx = 0;
    volatile std::int64_t GSink = 0; // 防死代码消除

    std::string makeStringKey(std::uint64_t I)
    {
        std::string S;
        S.reserve(24);
        for (std::size_t J = 0; J < 24; ++J)
        {
            const std::uint64_t V =
                (I >> (J % 6)) + J * 0x9E3779B9ULL + I * 0xC2B2AE3DULL;
            S.push_back(static_cast<char>('a' + static_cast<char>(V % 26)));
        }
        return S;
    }

    void buildWorkload()
    {
        std::mt19937_64 Rng(0x9E3779B97F4A7C15ULL);

        // 命中键：互不相同。
        {
            std::unordered_set<int> Seen;
            Seen.reserve(HitN * 2);
            HitKeys.reserve(HitN);
            while (HitKeys.size() < HitN)
            {
                const int K = static_cast<int>(Rng() & 0x7FFFFFFF);
                if (Seen.insert(K).second)
                {
                    HitKeys.push_back(K);
                }
            }
        }

        // 未命中键：与命中键不相交。
        {
            std::unordered_set<int> Seen(HitKeys.begin(), HitKeys.end());
            MissKeys.reserve(HitN);
            while (MissKeys.size() < HitN)
            {
                const int K = static_cast<int>(Rng() & 0x7FFFFFFF);
                if (Seen.insert(K).second)
                {
                    MissKeys.push_back(K);
                }
            }
        }

        // 1M 随机键（允许重复；2^31 空间下 1M 次抽样期望碰撞 ~244，可忽略）。
        GrowKeys.reserve(GrowN);
        for (std::size_t I = 0; I < GrowN; ++I)
        {
            GrowKeys.push_back(static_cast<int>(Rng() & 0x7FFFFFFF));
        }

        // 删除顺序与混合场景键下标。
        EraseOrder.resize(HitN);
        for (std::size_t I = 0; I < HitN; ++I)
        {
            EraseOrder[I] = I;
        }
        std::shuffle(EraseOrder.begin(), EraseOrder.end(), Rng);

        MixedKeyIdx.reserve(BatchMixed);
        for (std::size_t I = 0; I < BatchMixed; ++I)
        {
            MixedKeyIdx.push_back(static_cast<std::size_t>(Rng() & (HitN - 1)));
        }

        // 字符串键。
        StrHitKeys.reserve(StrN);
        StrMissKeys.reserve(StrN);
        for (std::size_t I = 0; I < StrN; ++I)
        {
            StrHitKeys.push_back(makeStringKey(I));
            StrMissKeys.push_back(makeStringKey(I + StrN));
        }

        // 预构建常驻表。
        for (std::size_t I = 0; I < HitN; ++I)
        {
            const int K = HitKeys[I];
            GFlatHit.tryEmplace(K, static_cast<int>(I));
            GLinkedHit.tryEmplace(K, static_cast<int>(I));
            GStdHit.try_emplace(K, static_cast<int>(I));
        }
        for (std::size_t I = 0; I < StrN; ++I)
        {
            GFlatStr.tryEmplace(StrHitKeys[I], static_cast<std::int64_t>(I));
            GLinkedStr.tryEmplace(StrHitKeys[I], static_cast<std::int64_t>(I));
            GStdStr.try_emplace(StrHitKeys[I], static_cast<std::int64_t>(I));
        }
        GSink += static_cast<std::int64_t>(GFlatHit.size() + GLinkedHit.size() + GStdHit.size() +
                                           GFlatStr.size() + GLinkedStr.size() + GStdStr.size());
    }

    // ------------------------------------------------------------ 插入

    void bmFlatInsertGrow128k()
    {
        FlatHashTable<int, int> Table;
        for (std::size_t I = 0; I < HitN; ++I)
        {
            Table.tryEmplace(HitKeys[I], static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }
    void bmLinkedInsertGrow128k()
    {
        LinkedHashTable<int, int> Table;
        for (std::size_t I = 0; I < HitN; ++I)
        {
            Table.tryEmplace(HitKeys[I], static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }
    void bmStdInsertGrow128k()
    {
        std::unordered_map<int, int> Table;
        for (std::size_t I = 0; I < HitN; ++I)
        {
            Table.try_emplace(HitKeys[I], static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }

    void bmFlatInsertGrow1m()
    {
        FlatHashTable<int, int> Table;
        for (std::size_t I = 0; I < GrowN; ++I)
        {
            Table.tryEmplace(GrowKeys[I], static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }
    void bmLinkedInsertGrow1m()
    {
        LinkedHashTable<int, int> Table;
        for (std::size_t I = 0; I < GrowN; ++I)
        {
            Table.tryEmplace(GrowKeys[I], static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }
    void bmStdInsertGrow1m()
    {
        std::unordered_map<int, int> Table;
        for (std::size_t I = 0; I < GrowN; ++I)
        {
            Table.try_emplace(GrowKeys[I], static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }

    void bmFlatInsertReserved1m()
    {
        FlatHashTable<int, int> Table;
        Table.reserve(GrowN);
        for (std::size_t I = 0; I < GrowN; ++I)
        {
            Table.tryEmplace(GrowKeys[I], static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }
    void bmLinkedInsertReserved1m()
    {
        LinkedHashTable<int, int> Table;
        Table.reserve(GrowN);
        for (std::size_t I = 0; I < GrowN; ++I)
        {
            Table.tryEmplace(GrowKeys[I], static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }
    void bmStdInsertReserved1m()
    {
        std::unordered_map<int, int> Table;
        Table.reserve(GrowN);
        for (std::size_t I = 0; I < GrowN; ++I)
        {
            Table.try_emplace(GrowKeys[I], static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }

    void bmFlatInsertSequential128k()
    {
        FlatHashTable<int, int> Table;
        for (std::size_t I = 0; I < HitN; ++I)
        {
            Table.tryEmplace(static_cast<int>(I), static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }
    void bmLinkedInsertSequential128k()
    {
        LinkedHashTable<int, int> Table;
        for (std::size_t I = 0; I < HitN; ++I)
        {
            Table.tryEmplace(static_cast<int>(I), static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }
    void bmStdInsertSequential128k()
    {
        std::unordered_map<int, int> Table;
        for (std::size_t I = 0; I < HitN; ++I)
        {
            Table.try_emplace(static_cast<int>(I), static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }

    // ------------------------------------------------------------ 删除（净耗时扣除建表）

    void bmFlatEraseAll()
    {
        FlatHashTable<int, int> Table;
        for (std::size_t I = 0; I < HitN; ++I)
        {
            Table.tryEmplace(HitKeys[I], static_cast<int>(I));
        }
        for (std::size_t I = 0; I < HitN; ++I)
        {
            Table.erase(HitKeys[EraseOrder[I]]);
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }
    void bmLinkedEraseAll()
    {
        LinkedHashTable<int, int> Table;
        for (std::size_t I = 0; I < HitN; ++I)
        {
            Table.tryEmplace(HitKeys[I], static_cast<int>(I));
        }
        for (std::size_t I = 0; I < HitN; ++I)
        {
            Table.erase(HitKeys[EraseOrder[I]]);
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }
    void bmStdEraseAll()
    {
        std::unordered_map<int, int> Table;
        for (std::size_t I = 0; I < HitN; ++I)
        {
            Table.try_emplace(HitKeys[I], static_cast<int>(I));
        }
        for (std::size_t I = 0; I < HitN; ++I)
        {
            Table.erase(HitKeys[EraseOrder[I]]);
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }

    // ------------------------------------------------------------ 查找

    void bmFlatFindHit()
    {
        const std::size_t Base = GFindIdx;
        GFindIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GFlatHit.find(HitKeys[(Base + J) & (HitN - 1)]) != GFlatHit.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }
    void bmLinkedFindHit()
    {
        const std::size_t Base = GFindIdx;
        GFindIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GLinkedHit.find(HitKeys[(Base + J) & (HitN - 1)]) != GLinkedHit.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }
    void bmStdFindHit()
    {
        const std::size_t Base = GFindIdx;
        GFindIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GStdHit.find(HitKeys[(Base + J) & (HitN - 1)]) != GStdHit.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }

    void bmFlatFindMiss()
    {
        const std::size_t Base = GFindIdx;
        GFindIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GFlatHit.find(MissKeys[(Base + J) & (HitN - 1)]) != GFlatHit.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }
    void bmLinkedFindMiss()
    {
        const std::size_t Base = GFindIdx;
        GFindIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GLinkedHit.find(MissKeys[(Base + J) & (HitN - 1)]) != GLinkedHit.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }
    void bmStdFindMiss()
    {
        const std::size_t Base = GFindIdx;
        GFindIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GStdHit.find(MissKeys[(Base + J) & (HitN - 1)]) != GStdHit.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }

    // ------------------------------------------------------------ 遍历 / 拷贝

    void bmFlatIterate()
    {
        std::int64_t Sum = 0;
        for (const auto& Entry : GFlatHit)
        {
            Sum += Entry.second;
        }
        GSink += Sum;
    }
    void bmLinkedIterate()
    {
        std::int64_t Sum = 0;
        for (const auto& Entry : GLinkedHit)
        {
            Sum += Entry.second;
        }
        GSink += Sum;
    }
    void bmStdIterate()
    {
        std::int64_t Sum = 0;
        for (const auto& Entry : GStdHit)
        {
            Sum += Entry.second;
        }
        GSink += Sum;
    }

    void bmFlatCopy()
    {
        FlatHashTable<int, int> Copy(GFlatHit);
        GSink += static_cast<std::int64_t>(Copy.size());
    }
    void bmLinkedCopy()
    {
        LinkedHashTable<int, int> Copy(GLinkedHit);
        GSink += static_cast<std::int64_t>(Copy.size());
    }
    void bmStdCopy()
    {
        std::unordered_map<int, int> Copy(GStdHit);
        GSink += static_cast<std::int64_t>(Copy.size());
    }

    // ------------------------------------------------------------ 混合读写

    void bmFlatMixed()
    {
        FlatHashTable<int, int> Table;
        std::int64_t Work = 0;
        for (std::size_t I = 0; I < BatchMixed; ++I)
        {
            const int K = HitKeys[MixedKeyIdx[I]];
            if (Table.find(K) != Table.end())
            {
                Table.erase(K);
                ++Work;
            }
            else
            {
                Table.tryEmplace(K, 1);
                Work += 2;
            }
        }
        GSink += Work;
    }
    void bmLinkedMixed()
    {
        LinkedHashTable<int, int> Table;
        std::int64_t Work = 0;
        for (std::size_t I = 0; I < BatchMixed; ++I)
        {
            const int K = HitKeys[MixedKeyIdx[I]];
            if (Table.find(K) != Table.end())
            {
                Table.erase(K);
                ++Work;
            }
            else
            {
                Table.tryEmplace(K, 1);
                Work += 2;
            }
        }
        GSink += Work;
    }
    void bmStdMixed()
    {
        std::unordered_map<int, int> Table;
        std::int64_t Work = 0;
        for (std::size_t I = 0; I < BatchMixed; ++I)
        {
            const int K = HitKeys[MixedKeyIdx[I]];
            if (Table.find(K) != Table.end())
            {
                Table.erase(K);
                ++Work;
            }
            else
            {
                Table.try_emplace(K, 1);
                Work += 2;
            }
        }
        GSink += Work;
    }

    // ------------------------------------------------------------ 字符串键

    void bmFlatStringInsert()
    {
        FlatHashTable<std::string, std::int64_t> Table;
        for (std::size_t I = 0; I < StrN; ++I)
        {
            Table.tryEmplace(StrHitKeys[I], static_cast<std::int64_t>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }
    void bmLinkedStringInsert()
    {
        LinkedHashTable<std::string, std::int64_t> Table;
        for (std::size_t I = 0; I < StrN; ++I)
        {
            Table.tryEmplace(StrHitKeys[I], static_cast<std::int64_t>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }
    void bmStdStringInsert()
    {
        std::unordered_map<std::string, std::int64_t> Table;
        for (std::size_t I = 0; I < StrN; ++I)
        {
            Table.try_emplace(StrHitKeys[I], static_cast<std::int64_t>(I));
        }
        GSink += static_cast<std::int64_t>(Table.size());
    }

    void bmFlatStringFindHit()
    {
        const std::size_t Base = GStrIdx;
        GStrIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GFlatStr.find(StrHitKeys[(Base + J) & (StrN - 1)]) != GFlatStr.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }
    void bmLinkedStringFindHit()
    {
        const std::size_t Base = GStrIdx;
        GStrIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GLinkedStr.find(StrHitKeys[(Base + J) & (StrN - 1)]) != GLinkedStr.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }
    void bmStdStringFindHit()
    {
        const std::size_t Base = GStrIdx;
        GStrIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GStdStr.find(StrHitKeys[(Base + J) & (StrN - 1)]) != GStdStr.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }

    // ------------------------------------------------------------ 预热钩子
    struct WorkloadBuilder
    {
        WorkloadBuilder()
        {
            buildWorkload();
        }
    };
    static const WorkloadBuilder GWorkloadBuilder;
} // namespace

PICTOR_BENCHMARK("hashtable", "flat", "insert.grow", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "空表插入 128k 个随机键（含动态扩容重哈希）", bmFlatInsertGrow128k);
PICTOR_BENCHMARK("hashtable", "linked", "insert.grow", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "空表插入 128k 个随机键（含动态扩容重哈希）", bmLinkedInsertGrow128k);
PICTOR_BENCHMARK("hashtable", "std", "insert.grow", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "空表插入 128k 个随机键（含动态扩容重哈希）", bmStdInsertGrow128k);

PICTOR_BENCHMARK("hashtable", "flat", "insert.grow", "1m", GrowN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "空表插入 1m 个随机键（含动态扩容重哈希）", bmFlatInsertGrow1m);
PICTOR_BENCHMARK("hashtable", "linked", "insert.grow", "1m", GrowN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "空表插入 1m 个随机键（含动态扩容重哈希）", bmLinkedInsertGrow1m);
PICTOR_BENCHMARK("hashtable", "std", "insert.grow", "1m", GrowN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "空表插入 1m 个随机键（含动态扩容重哈希）", bmStdInsertGrow1m);

PICTOR_BENCHMARK("hashtable", "flat", "insert.reserved", "1m", GrowN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "reserve(1m) 后插入 1m 个随机键", bmFlatInsertReserved1m);
PICTOR_BENCHMARK("hashtable", "linked", "insert.reserved", "1m", GrowN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "reserve(1m) 后插入 1m 个随机键", bmLinkedInsertReserved1m);
PICTOR_BENCHMARK("hashtable", "std", "insert.reserved", "1m", GrowN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "reserve(1m) 后插入 1m 个随机键", bmStdInsertReserved1m);

PICTOR_BENCHMARK("hashtable", "flat", "insert.sequential", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "按升序插入 0..127999 顺序键（最坏聚集模式）", bmFlatInsertSequential128k);
PICTOR_BENCHMARK("hashtable", "linked", "insert.sequential", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "按升序插入 0..127999 顺序键（最坏聚集模式）", bmLinkedInsertSequential128k);
PICTOR_BENCHMARK("hashtable", "std", "insert.sequential", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "按升序插入 0..127999 顺序键（最坏聚集模式）", bmStdInsertSequential128k);

PICTOR_BENCHMARK("hashtable", "flat", "eraseAll", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "insert.grow",
                 "删除全部键（净耗时：已扣除建表成本）", bmFlatEraseAll);
PICTOR_BENCHMARK("hashtable", "linked", "eraseAll", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "insert.grow",
                 "删除全部键（净耗时：已扣除建表成本）", bmLinkedEraseAll);
PICTOR_BENCHMARK("hashtable", "std", "eraseAll", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "insert.grow",
                 "删除全部键（净耗时：已扣除建表成本）", bmStdEraseAll);

PICTOR_BENCHMARK("hashtable", "flat", "findHit", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "命中查找（预构建 128k 表，每调用批量 1024 次）", bmFlatFindHit);
PICTOR_BENCHMARK("hashtable", "linked", "findHit", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "命中查找（预构建 128k 表，每调用批量 1024 次）", bmLinkedFindHit);
PICTOR_BENCHMARK("hashtable", "std", "findHit", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "命中查找（预构建 128k 表，每调用批量 1024 次）", bmStdFindHit);

PICTOR_BENCHMARK("hashtable", "flat", "findMiss", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "未命中查找（每调用批量 1024 次）", bmFlatFindMiss);
PICTOR_BENCHMARK("hashtable", "linked", "findMiss", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "未命中查找（每调用批量 1024 次）", bmLinkedFindMiss);
PICTOR_BENCHMARK("hashtable", "std", "findMiss", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "未命中查找（每调用批量 1024 次）", bmStdFindMiss);

PICTOR_BENCHMARK("hashtable", "flat", "iterate", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "全量遍历求和（按每元素折算）", bmFlatIterate);
PICTOR_BENCHMARK("hashtable", "linked", "iterate", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "全量遍历求和（按每元素折算）", bmLinkedIterate);
PICTOR_BENCHMARK("hashtable", "std", "iterate", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "全量遍历求和（按每元素折算）", bmStdIterate);

PICTOR_BENCHMARK("hashtable", "flat", "copy", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "整体拷贝预构建 128k 表（按每元素折算）", bmFlatCopy);
PICTOR_BENCHMARK("hashtable", "linked", "copy", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "整体拷贝预构建 128k 表（按每元素折算）", bmLinkedCopy);
PICTOR_BENCHMARK("hashtable", "std", "copy", "128k", HitN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "整体拷贝预构建 128k 表（按每元素折算）", bmStdCopy);

PICTOR_BENCHMARK("hashtable", "flat", "mixed", "128k", BatchMixed,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "混合读写：命中/插入/删除（每调用批量 4096 次）", bmFlatMixed);
PICTOR_BENCHMARK("hashtable", "linked", "mixed", "128k", BatchMixed,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "混合读写：命中/插入/删除（每调用批量 4096 次）", bmLinkedMixed);
PICTOR_BENCHMARK("hashtable", "std", "mixed", "128k", BatchMixed,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "混合读写：命中/插入/删除（每调用批量 4096 次）", bmStdMixed);

PICTOR_BENCHMARK("hashtable", "flat", "stringInsert", "128k", StrN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "插入 128k 个 24 字符字符串键", bmFlatStringInsert);
PICTOR_BENCHMARK("hashtable", "linked", "stringInsert", "128k", StrN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "插入 128k 个 24 字符字符串键", bmLinkedStringInsert);
PICTOR_BENCHMARK("hashtable", "std", "stringInsert", "128k", StrN,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "插入 128k 个 24 字符字符串键", bmStdStringInsert);

PICTOR_BENCHMARK("hashtable", "flat", "stringFindHit", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "24 字符字符串键命中查找（每调用批量 1024 次）", bmFlatStringFindHit);
PICTOR_BENCHMARK("hashtable", "linked", "stringFindHit", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "24 字符字符串键命中查找（每调用批量 1024 次）", bmLinkedStringFindHit);
PICTOR_BENCHMARK("hashtable", "std", "stringFindHit", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "24 字符字符串键命中查找（每调用批量 1024 次）", bmStdStringFindHit);
