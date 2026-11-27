// RBTreeBenchmark.cpp — IntrusiveRBTree 与 std::set<int> 的多场景性能对比
// （std::set 为基线 = 1.00x）。
//
// 场景设计：
//   insert.random / insert.sorted / insert.reverse  三种插入顺序（树结构差异大）
//   findHit / findMiss        预构建满树上的命中 / 未命中查找（批量 1024 次/调用）
//   lowerBound                下界查询（批量 1024 次/调用，键在 [0, 2N) 混合命中）
//   iterate                   全量中序遍历求和（每元素折算）
//   eraseAll                  删除全部键（净耗时 = 自身 − insert.random，抵消建树）
//   mixed                     混合读写：命中查找/插入/删除（批量 4096 次/调用）
//
// 侵入式树的测量方式说明：常驻满树 GTree 使用独立的 GTreePool（只读场景
// 依赖它，基准期间绝不动用这批节点）；插入/删除/混合基准复用另一份节点池
// Pool——每个计时调用都从同一个「空树」状态开始，IntrusiveRBTree 不拥有节点、
// 析构不做任何逐节点工作，且 insert 会重置节点链接，因此同一批节点可以在
// 各次调用间安全复用，与 std::set 在完全相同的工作负载上公平对比。

#include "benchmark/BenchmarkCommon.hpp"

#include <infra/containers/IntrusiveRBTree.hpp>

#include <cstddef>
#include <cstdint>
#include <random>
#include <set>
#include <vector>

namespace
{
    constexpr std::size_t N = 1u << 17; // 131072, 标签 "128k"
    constexpr std::size_t QueryN = 1u << 16;
    constexpr std::size_t BatchFind = 1024;
    constexpr std::size_t BatchMixed = 4096;

    struct RBNode : RBNodeBase
    {
        int Key = 0;
        explicit RBNode(int K = 0) : Key(K) {}
    };
    bool operator<(const RBNode& A, const RBNode& B)
    {
        return A.Key < B.Key;
    }
    bool operator<(const RBNode& A, int B)
    {
        return A.Key < B;
    }
    bool operator<(int A, const RBNode& B)
    {
        return A < B.Key;
    }

    std::vector<RBNode> GTreePool;     // 常驻满树专用节点：基准期间绝不被复用
    std::vector<RBNode> Pool;          // 插入/删除/混合基准复用（每轮建新树, 弃树无主）
    std::vector<std::size_t> RandomOrder; // 随机插入顺序（0..N-1 排列）
    std::vector<std::size_t> EraseOrder;  // 随机删除顺序
    std::vector<std::size_t> MixedKeyIdx; // 混合场景键下标
    std::vector<int> QueryKeys;      // lowerBound 查询键（[0, 2N) 随机）

    IntrusiveRBTree<RBNode> GTree;   // 常驻满树（键 0..N-1），仅用于只读场景
    std::set<int> GSet;

    std::size_t GFindIdx = 0;
    std::size_t GQueryIdx = 0;
    volatile std::int64_t GSink = 0;

    void buildWorkload()
    {
        GTreePool.reserve(N);
        Pool.reserve(N);
        for (std::size_t I = 0; I < N; ++I)
        {
            GTreePool.emplace_back(static_cast<int>(I));
            Pool.emplace_back(static_cast<int>(I));
        }

        std::mt19937_64 Rng(0xD1B54A32D192ED03ULL);

        RandomOrder.resize(N);
        for (std::size_t I = 0; I < N; ++I)
        {
            RandomOrder[I] = I;
        }
        std::shuffle(RandomOrder.begin(), RandomOrder.end(), Rng);

        EraseOrder.resize(N);
        for (std::size_t I = 0; I < N; ++I)
        {
            EraseOrder[I] = I;
        }
        std::shuffle(EraseOrder.begin(), EraseOrder.end(), Rng);

        MixedKeyIdx.reserve(BatchMixed);
        for (std::size_t I = 0; I < BatchMixed; ++I)
        {
            MixedKeyIdx.push_back(static_cast<std::size_t>(Rng() & (N - 1)));
        }

        QueryKeys.reserve(QueryN);
        for (std::size_t I = 0; I < QueryN; ++I)
        {
            QueryKeys.push_back(static_cast<int>(Rng() & ((N << 1) - 1)));
        }

        for (std::size_t I = 0; I < N; ++I)
        {
            GTree.insert(GTreePool[I]);
            GSet.insert(static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(GTree.size() + GSet.size());
    }

    // ------------------------------------------------------------ 插入

    void bmTreeInsertRandom()
    {
        IntrusiveRBTree<RBNode> Tree;
        for (std::size_t I = 0; I < N; ++I)
        {
            Tree.insert(Pool[RandomOrder[I]]);
        }
        GSink += static_cast<std::int64_t>(Tree.size());
    }
    void bmSetInsertRandom()
    {
        std::set<int> Set;
        for (std::size_t I = 0; I < N; ++I)
        {
            Set.insert(static_cast<int>(RandomOrder[I]));
        }
        GSink += static_cast<std::int64_t>(Set.size());
    }

    void bmTreeInsertSorted()
    {
        IntrusiveRBTree<RBNode> Tree;
        for (std::size_t I = 0; I < N; ++I)
        {
            Tree.insert(Pool[I]);
        }
        GSink += static_cast<std::int64_t>(Tree.size());
    }
    void bmSetInsertSorted()
    {
        std::set<int> Set;
        for (std::size_t I = 0; I < N; ++I)
        {
            Set.insert(static_cast<int>(I));
        }
        GSink += static_cast<std::int64_t>(Set.size());
    }

    void bmTreeInsertReverse()
    {
        IntrusiveRBTree<RBNode> Tree;
        for (std::size_t I = N; I > 0; --I)
        {
            Tree.insert(Pool[I - 1]);
        }
        GSink += static_cast<std::int64_t>(Tree.size());
    }
    void bmSetInsertReverse()
    {
        std::set<int> Set;
        for (std::size_t I = N; I > 0; --I)
        {
            Set.insert(static_cast<int>(I - 1));
        }
        GSink += static_cast<std::int64_t>(Set.size());
    }

    // ------------------------------------------------------------ 删除（净耗时扣除建树）

    void bmTreeEraseAll()
    {
        IntrusiveRBTree<RBNode> Tree;
        for (std::size_t I = 0; I < N; ++I)
        {
            Tree.insert(Pool[RandomOrder[I]]);
        }
        for (std::size_t I = 0; I < N; ++I)
        {
            Tree.erase(static_cast<int>(EraseOrder[I]));
        }
        GSink += static_cast<std::int64_t>(Tree.size());
    }
    void bmSetEraseAll()
    {
        std::set<int> Set;
        for (std::size_t I = 0; I < N; ++I)
        {
            Set.insert(static_cast<int>(RandomOrder[I]));
        }
        for (std::size_t I = 0; I < N; ++I)
        {
            Set.erase(static_cast<int>(EraseOrder[I]));
        }
        GSink += static_cast<std::int64_t>(Set.size());
    }

    // ------------------------------------------------------------ 查找

    void bmTreeFindHit()
    {
        const std::size_t Base = GFindIdx;
        GFindIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GTree.find(static_cast<int>((Base + J) & (N - 1))) != GTree.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }
    void bmSetFindHit()
    {
        const std::size_t Base = GFindIdx;
        GFindIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GSet.find(static_cast<int>((Base + J) & (N - 1))) != GSet.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }

    void bmTreeFindMiss()
    {
        const std::size_t Base = GFindIdx;
        GFindIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GTree.find(static_cast<int>(N + ((Base + J) & (N - 1)))) != GTree.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }
    void bmSetFindMiss()
    {
        const std::size_t Base = GFindIdx;
        GFindIdx += BatchFind;
        std::int64_t Hits = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            if (GSet.find(static_cast<int>(N + ((Base + J) & (N - 1)))) != GSet.end())
            {
                ++Hits;
            }
        }
        GSink += Hits;
    }

    void bmTreeLowerBound()
    {
        const std::size_t Base = GQueryIdx;
        GQueryIdx += BatchFind;
        std::int64_t Work = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            const auto It = GTree.getLowerBound(QueryKeys[(Base + J) & (QueryN - 1)]);
            Work += (It == GTree.end()) ? 1 : 2;
        }
        GSink += Work;
    }
    void bmSetLowerBound()
    {
        const std::size_t Base = GQueryIdx;
        GQueryIdx += BatchFind;
        std::int64_t Work = 0;
        for (std::size_t J = 0; J < BatchFind; ++J)
        {
            const auto It = GSet.lower_bound(QueryKeys[(Base + J) & (QueryN - 1)]);
            Work += (It == GSet.end()) ? 1 : 2;
        }
        GSink += Work;
    }

    // ------------------------------------------------------------ 遍历 / 混合

    void bmTreeIterate()
    {
        std::int64_t Sum = 0;
        for (const RBNode& Node : GTree)
        {
            Sum += Node.Key;
        }
        GSink += Sum;
    }
    void bmSetIterate()
    {
        std::int64_t Sum = 0;
        for (int Value : GSet)
        {
            Sum += Value;
        }
        GSink += Sum;
    }

    void bmTreeMixed()
    {
        IntrusiveRBTree<RBNode> Tree;
        std::int64_t Work = 0;
        for (std::size_t I = 0; I < BatchMixed; ++I)
        {
            const std::size_t K = MixedKeyIdx[I];
            if (Tree.find(static_cast<int>(K)) != Tree.end())
            {
                Tree.erase(static_cast<int>(K));
                ++Work;
            }
            else
            {
                Tree.insert(Pool[K]);
                Work += 2;
            }
        }
        GSink += Work;
    }
    void bmSetMixed()
    {
        std::set<int> Set;
        std::int64_t Work = 0;
        for (std::size_t I = 0; I < BatchMixed; ++I)
        {
            const int K = static_cast<int>(MixedKeyIdx[I]);
            if (Set.find(K) != Set.end())
            {
                Set.erase(K);
                ++Work;
            }
            else
            {
                Set.insert(K);
                Work += 2;
            }
        }
        GSink += Work;
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

PICTOR_BENCHMARK("rbtree", "intrusive", "insert.random", "128k", N,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "随机顺序插入 128k 个节点", bmTreeInsertRandom);
PICTOR_BENCHMARK("rbtree", "stdset", "insert.random", "128k", N,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "随机顺序插入 128k 个键", bmSetInsertRandom);

PICTOR_BENCHMARK("rbtree", "intrusive", "insert.sorted", "128k", N,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "升序插入 0..127999（顺序键）", bmTreeInsertSorted);
PICTOR_BENCHMARK("rbtree", "stdset", "insert.sorted", "128k", N,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "升序插入 0..127999（顺序键）", bmSetInsertSorted);

PICTOR_BENCHMARK("rbtree", "intrusive", "insert.reverse", "128k", N,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "降序插入 127999..0（逆序键）", bmTreeInsertReverse);
PICTOR_BENCHMARK("rbtree", "stdset", "insert.reverse", "128k", N,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "降序插入 127999..0（逆序键）", bmSetInsertReverse);

PICTOR_BENCHMARK("rbtree", "intrusive", "eraseAll", "128k", N,
                 ::PictorBench::EMeasureMode::Bulk, "insert.random",
                 "删除全部键（净耗时：已扣除建树成本）", bmTreeEraseAll);
PICTOR_BENCHMARK("rbtree", "stdset", "eraseAll", "128k", N,
                 ::PictorBench::EMeasureMode::Bulk, "insert.random",
                 "删除全部键（净耗时：已扣除建树成本）", bmSetEraseAll);

PICTOR_BENCHMARK("rbtree", "intrusive", "findHit", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "命中查找（预构建满树，每调用批量 1024 次）", bmTreeFindHit);
PICTOR_BENCHMARK("rbtree", "stdset", "findHit", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "命中查找（预构建满树，每调用批量 1024 次）", bmSetFindHit);

PICTOR_BENCHMARK("rbtree", "intrusive", "findMiss", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "未命中查找（键在 [N, 2N)，每调用批量 1024 次）", bmTreeFindMiss);
PICTOR_BENCHMARK("rbtree", "stdset", "findMiss", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "未命中查找（键在 [N, 2N)，每调用批量 1024 次）", bmSetFindMiss);

PICTOR_BENCHMARK("rbtree", "intrusive", "lowerBound", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "下界查询（键在 [0, 2N) 随机，每调用批量 1024 次）", bmTreeLowerBound);
PICTOR_BENCHMARK("rbtree", "stdset", "lowerBound", "128k", BatchFind,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "下界查询（键在 [0, 2N) 随机，每调用批量 1024 次）", bmSetLowerBound);

PICTOR_BENCHMARK("rbtree", "intrusive", "iterate", "128k", N,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "全量中序遍历求和（按每元素折算）", bmTreeIterate);
PICTOR_BENCHMARK("rbtree", "stdset", "iterate", "128k", N,
                 ::PictorBench::EMeasureMode::Bulk, "",
                 "全量中序遍历求和（按每元素折算）", bmSetIterate);

PICTOR_BENCHMARK("rbtree", "intrusive", "mixed", "128k", BatchMixed,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "混合读写：命中查找/插入/删除（每调用批量 4096 次）", bmTreeMixed);
PICTOR_BENCHMARK("rbtree", "stdset", "mixed", "128k", BatchMixed,
                 ::PictorBench::EMeasureMode::PerOp, "",
                 "混合读写：命中查找/插入/删除（每调用批量 4096 次）", bmSetMixed);
