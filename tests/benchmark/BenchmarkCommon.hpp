// BenchmarkCommon.hpp — 性能基准共享设施：元数据注册、分组、对比报表。
//
// 设计参考 Google Benchmark / quick-bench 的常见做法：
//   * 每个场景以「基本操作」为单位报告 ns/op（一次插入/查找/删除/遍历一个元素），
//     批量场景在单次计时调用内完成 OpsPerCall 次操作后再折算，摊薄计时框架开销。
//   * 每个实现（flat / linked / intrusive）都配有标准库基线（std::unordered_map /
//     std::set），报表输出相对基线的比值（<1.00x 表示更快）。
//   * 原始测量走项目自带的 Bm 框架（infra/tracer/Benchmark.hpp，自动校准迭代次数），
//     本文件负责场景元数据与三种输出：控制台表格、JSON、Markdown。
//   * "eraseAll" 等派生场景：净耗时 = 自身测量 − 相同构建成本的基准测量
//     （DerivedFrom），避免把建表时间计入删除耗时。

#ifndef PICTOR_BENCHMARK_COMMON_H
#define PICTOR_BENCHMARK_COMMON_H

#include <infra/tracer/Benchmark.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace PictorBench
{
    /** @brief 测量模式：Bulk 用固定迭代次数，PerOp 用自动校准（适合单次调用很短的操作）。 */
    enum class EMeasureMode
    {
        Bulk,
        PerOp,
    };

    /** @brief 单个基准的场景元数据。 */
    struct BenchMeta
    {
        std::string Name;          ///< Bm 注册名，如 hashtable.flat.insert.grow.128k
        std::string Suite;         ///< 套件：hashtable | rbtree
        std::string Impl;          ///< 实现：flat | linked | std | intrusive | stdset
        std::string Scenario;      ///< 场景：insert.grow、findHit、eraseAll、mixed ...
        std::string SizeLabel;     ///< 规模标签：128k、1m ...
        std::uint64_t OpsPerCall = 1; ///< 单次计时调用内的基本操作数（折算 ns/op 用）
        EMeasureMode Mode = EMeasureMode::Bulk;
        std::string DerivedFrom;   ///< 非空时：净耗时 = 自身 − 该基础场景（同 Suite/Impl/Size）
        std::string Note;          ///< 场景说明（写进报表与 JSON）
    };

    inline std::vector<BenchMeta>& getMetaRegistry()
    {
        static std::vector<BenchMeta> Registry;
        return Registry;
    }

    /** @brief 静态初始化时注册基准函数（Bm）与场景元数据（本模块）。 */
    struct MetaRegistrar
    {
        MetaRegistrar(const BenchMeta& Meta, void (*Function)())
        {
            Bm::registerBenchmark(Meta.Name, Function);
            getMetaRegistry().push_back(Meta);
        }
    };

    // ------------------------------------------------------------ 报表数据

    struct ImplResult
    {
        std::string Impl;
        double MeanNsPerOp = 0.0;              ///< 每基本操作平均耗时
        double ThroughputOpsPerSecond = 0.0;   ///< 每秒基本操作数
        double RatioVsBaseline = 0.0;          ///< 相对基线的比值（基线=1.00）
        bool IsBaseline = false;
    };

    struct ScenarioGroup
    {
        BenchMeta Meta;
        bool IsDerived = false;                ///< 是否做过净耗时扣除
        bool HasBaseline = false;              ///< 本组是否包含基线实现
        std::vector<ImplResult> Impls;
        std::string BaselineImpl;              ///< 基线实现名（std / stdset）
    };

    /** @brief 环境信息（写入报表头部）。 */
    inline std::string environmentDescription()
    {
        std::ostringstream Stream;
#ifdef PICTOR_TEST_COMPILER
        Stream << PICTOR_TEST_COMPILER;
#else
        Stream << "unknown compiler";
#endif
        Stream << ", build=";
#ifdef PICTOR_TEST_BUILD_TYPE
        Stream << PICTOR_TEST_BUILD_TYPE;
#else
        Stream << "unknown";
#endif
        Stream << ", C++23, " << std::thread::hardware_concurrency() << " logical CPU(s)";
#if defined(__APPLE__)
        Stream << ", macOS";
#elif defined(_WIN32)
        Stream << ", Windows";
#elif defined(__linux__)
        Stream << ", Linux";
#endif
        return Stream.str();
    }

    /**
     * @brief 把原始 Bm 结果按 (Suite, Scenario, SizeLabel) 分组，处理派生净耗时。
     */
    inline std::vector<ScenarioGroup> buildGroups(const std::vector<Bm::BenchmarkResult>& Results)
    {
        std::map<std::string, const Bm::BenchmarkResult*> ByName;
        for (const Bm::BenchmarkResult& Result : Results)
        {
            ByName[Result.Name] = &Result;
        }

        std::vector<ScenarioGroup> Groups;
        for (const BenchMeta& Meta : getMetaRegistry())
        {
            const auto ResultIt = ByName.find(Meta.Name);
            if (ResultIt == ByName.end())
            {
                continue; // 本轮未运行（被 --filter 过滤）
            }

            auto GroupIt = std::find_if(Groups.begin(), Groups.end(),
                                        [&](const ScenarioGroup& G) {
                                            return G.Meta.Suite == Meta.Suite &&
                                                   G.Meta.Scenario == Meta.Scenario &&
                                                   G.Meta.SizeLabel == Meta.SizeLabel;
                                        });
            if (GroupIt == Groups.end())
            {
                Groups.push_back(ScenarioGroup{Meta, !Meta.DerivedFrom.empty(), false, {}, ""});
                GroupIt = Groups.end() - 1;
            }

            ImplResult Impl;
            Impl.Impl = Meta.Impl;
            Impl.MeanNsPerOp = ResultIt->second->MeanTimeNanoseconds;

            if (!Meta.DerivedFrom.empty())
            {
                // 净耗时 = 自身 − 基础场景（抵消建表成本）。负数（噪声）截断为 0。
                const std::string BaseName =
                    Meta.Suite + "." + Meta.Impl + "." + Meta.DerivedFrom + "." + Meta.SizeLabel;
                const auto BaseIt = ByName.find(BaseName);
                if (BaseIt != ByName.end())
                {
                    Impl.MeanNsPerOp =
                        (std::max)(0.0, ResultIt->second->MeanTimeNanoseconds -
                                            BaseIt->second->MeanTimeNanoseconds);
                }
            }
            Impl.MeanNsPerOp /= static_cast<double>(Meta.OpsPerCall);
            Impl.ThroughputOpsPerSecond =
                Impl.MeanNsPerOp > 0.0 ? 1e9 / Impl.MeanNsPerOp : 0.0;
            Impl.IsBaseline = Meta.Impl.rfind("std", 0) == 0;
            GroupIt->Impls.push_back(Impl);
        }

        for (ScenarioGroup& Group : Groups)
        {
            double BaselineMean = 0.0;
            for (const ImplResult& Impl : Group.Impls)
            {
                if (Impl.IsBaseline)
                {
                    BaselineMean = Impl.MeanNsPerOp;
                    Group.BaselineImpl = Impl.Impl;
                    Group.HasBaseline = true;
                }
            }
            for (ImplResult& Impl : Group.Impls)
            {
                Impl.RatioVsBaseline =
                    BaselineMean > 0.0 ? Impl.MeanNsPerOp / BaselineMean : 0.0;
            }
        }
        return Groups;
    }

    /** @brief 人类可读的速率格式化（1.23 M/s、45.6 G/s）。 */
    inline std::string formatRate(double OpsPerSecond)
    {
        std::ostringstream Stream;
        Stream.setf(std::ios::fixed);
        if (OpsPerSecond >= 1e9)
        {
            Stream.precision(2);
            Stream << OpsPerSecond / 1e9 << " G/s";
        }
        else if (OpsPerSecond >= 1e6)
        {
            Stream.precision(2);
            Stream << OpsPerSecond / 1e6 << " M/s";
        }
        else if (OpsPerSecond >= 1e3)
        {
            Stream.precision(2);
            Stream << OpsPerSecond / 1e3 << " K/s";
        }
        else
        {
            Stream.precision(0);
            Stream << OpsPerSecond << "/s";
        }
        return Stream.str();
    }

    /** @brief 控制台对比报表。 */
    inline void printConsoleReport(const std::vector<ScenarioGroup>& Groups)
    {
        std::printf("[bench] environment: %s\n", environmentDescription().c_str());
        std::printf("[bench] unit: ns/op (lower is better); baseline ratio = 1.00x\n\n");

        for (const ScenarioGroup& Group : Groups)
        {
            std::printf("[bench] %s/%s @ %s%s\n", Group.Meta.Suite.c_str(),
                        Group.Meta.Scenario.c_str(), Group.Meta.SizeLabel.c_str(),
                        Group.IsDerived ? "  (net: build cost subtracted)" : "");
            if (!Group.Meta.Note.empty())
            {
                std::printf("[bench]   note: %s\n", Group.Meta.Note.c_str());
            }
            std::printf("[bench]   %-10s %14s %14s %10s\n", "impl", "mean ns/op", "throughput",
                        "vs base");
            for (const ImplResult& Impl : Group.Impls)
            {
                if (Group.HasBaseline)
                {
                    std::printf("[bench]   %-10s %14.2f %14s %9.2fx%s\n", Impl.Impl.c_str(),
                                Impl.MeanNsPerOp, formatRate(Impl.ThroughputOpsPerSecond).c_str(),
                                Impl.RatioVsBaseline, Impl.IsBaseline ? "  (baseline)" : "");
                }
                else
                {
                    std::printf("[bench]   %-10s %14.2f %14s %10s\n", Impl.Impl.c_str(),
                                Impl.MeanNsPerOp, formatRate(Impl.ThroughputOpsPerSecond).c_str(),
                                "-");
                }
            }
            std::printf("\n");
        }
    }

    /** @brief 对比结果 JSON（bench_report.json）。 */
    inline bool writeJsonReport(const std::string& Path, const std::vector<ScenarioGroup>& Groups)
    {
        std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
        if (!Output)
        {
            return false;
        }

        Bm::Detail::JsonWriter Writer(true, 2);
        Writer.beginObject();

        Writer.writeKey("environment");
        Writer.writeString(environmentDescription());

        Writer.writeKey("unit");
        Writer.writeString("ns/op");

        Writer.writeKey("scenarios");
        Writer.beginArray();
        for (const ScenarioGroup& Group : Groups)
        {
            Writer.beginObject();
            Writer.writeKey("suite");
            Writer.writeString(Group.Meta.Suite);
            Writer.writeKey("scenario");
            Writer.writeString(Group.Meta.Scenario);
            Writer.writeKey("size");
            Writer.writeString(Group.Meta.SizeLabel);
            Writer.writeKey("note");
            Writer.writeString(Group.Meta.Note);
            Writer.writeKey("net_cost");
            Writer.writeBool(Group.IsDerived);
            Writer.writeKey("has_baseline");
            Writer.writeBool(Group.HasBaseline);
            Writer.writeKey("baseline");
            Writer.writeString(Group.BaselineImpl);

            Writer.writeKey("impls");
            Writer.beginArray();
            for (const ImplResult& Impl : Group.Impls)
            {
                Writer.beginObject();
                Writer.writeKey("impl");
                Writer.writeString(Impl.Impl);
                Writer.writeKey("mean_ns_per_op");
                Writer.writeDouble(Impl.MeanNsPerOp);
                Writer.writeKey("throughput_ops_per_second");
                Writer.writeDouble(Impl.ThroughputOpsPerSecond);
                if (Group.HasBaseline)
                {
                    Writer.writeKey("ratio_vs_baseline");
                    Writer.writeDouble(Impl.RatioVsBaseline);
                }
                Writer.endObject();
            }
            Writer.endArray();

            Writer.endObject();
        }
        Writer.endArray();

        Writer.endObject();

        const std::string Text = Writer.toString();
        Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
        return static_cast<bool>(Output);
    }

    namespace Detail
    {
        // 实现显示名（Markdown 表头用）。
        inline std::string implDisplayName(const std::string& Impl)
        {
            if (Impl == "flat")
            {
                return "FlatHashTable";
            }
            if (Impl == "linked")
            {
                return "LinkedHashTable";
            }
            if (Impl == "std")
            {
                return "std::unordered_map";
            }
            if (Impl == "intrusive")
            {
                return "IntrusiveRBTree";
            }
            if (Impl == "stdset")
            {
                return "std::set";
            }
            return Impl;
        }

        inline void writeMarkdownSuite(std::ofstream& Output, const std::string& Title,
                                       const std::string& Suite,
                                       const std::vector<ScenarioGroup>& Groups)
        {
            std::vector<const ScenarioGroup*> SuiteGroups;
            for (const ScenarioGroup& Group : Groups)
            {
                if (Group.Meta.Suite == Suite)
                {
                    SuiteGroups.push_back(&Group);
                }
            }
            if (SuiteGroups.empty())
            {
                return;
            }

            Output << "## " << Title << "\n\n";

            Output << "| 场景 | 规模 |";
            for (const ImplResult& Impl : SuiteGroups.front()->Impls)
            {
                Output << " " << Detail::implDisplayName(Impl.Impl) << " (ns/op) |";
            }
            Output << " 相对基线 |\n";

            Output << "|---|--|";
            for (std::size_t I = 0; I < SuiteGroups.front()->Impls.size(); ++I)
            {
                (void)I;
                Output << "--|";
            }
            Output << "--|\n";

            for (const ScenarioGroup* Group : SuiteGroups)
            {
                Output << "| " << Group->Meta.Scenario
                       << (Group->IsDerived ? " (净)" : "") << " | " << Group->Meta.SizeLabel
                       << " |";
                for (const ImplResult& Impl : Group->Impls)
                {
                    std::ostringstream Cell;
                    Cell.setf(std::ios::fixed);
                    Cell.precision(2);
                    Cell << Impl.MeanNsPerOp;
                    Output << " " << Cell.str() << " |";
                }

                std::string Ratios;
                if (!Group->HasBaseline)
                {
                    Ratios = "-";
                }
                else
                {
                    for (const ImplResult& Impl : Group->Impls)
                    {
                        if (Impl.IsBaseline)
                        {
                            continue;
                        }
                        std::ostringstream Cell;
                        Cell.setf(std::ios::fixed);
                        Cell.precision(2);
                        Cell << Impl.RatioVsBaseline << "x";
                        if (!Ratios.empty())
                        {
                            Ratios += " · ";
                        }
                        Ratios += Detail::implDisplayName(Impl.Impl) + " " + Cell.str();
                    }
                }
                Output << " " << Ratios << " |\n";
            }
            Output << "\n";
        }
    } // namespace Detail

    /** @brief 对比结果 Markdown（bench_report.md）。 */
    inline bool writeMarkdownReport(const std::string& Path,
                                    const std::vector<ScenarioGroup>& Groups)
    {
        std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
        if (!Output)
        {
            return false;
        }

        Output << "# Pictor 容器性能基准报告\n\n";
        Output << "- 环境：" << environmentDescription() << "\n";
        Output << "- 单位：ns/op（每个基本操作的平均纳秒数，数值越小越好）\n";
        Output << "- 基线：std::unordered_map / std::set = 1.00x；比值 < 1.00x 表示快于标准库\n";
        Output << "- 方法：每个场景自动校准到约 80 ms 总测量时长（批量场景固定迭代次数），"
                  "预热 1–2 次后取均值\n\n";

        Detail::writeMarkdownSuite(Output, "哈希表：FlatHashTable / LinkedHashTable vs std::unordered_map",
                                   "hashtable", Groups);
        Detail::writeMarkdownSuite(Output, "红黑树：IntrusiveRBTree vs std::set", "rbtree", Groups);

        return static_cast<bool>(Output);
    }

    /** @brief 基准 CLI 解析结果。 */
    struct BenchCliOptions
    {
        std::string Filter;
        std::string OutputDir;
        std::int64_t MinTimeMilliseconds = 80;
        std::int64_t BulkIterations = 5;
        bool ListOnly = false;
    };

    inline std::optional<BenchCliOptions> parseBenchCli(int Argc, char** Argv)
    {
        BenchCliOptions Cli;
        for (int I = 0; I < Argc; ++I)
        {
            const std::string Arg = Argv[I];
            auto ReadValue = [&](const char* Key) -> std::optional<std::string> {
                const std::string Prefix = std::string(Key) + "=";
                if (Arg == Key)
                {
                    if (I + 1 >= Argc)
                    {
                        return std::nullopt;
                    }
                    return std::string(Argv[++I]);
                }
                if (Arg.rfind(Prefix, 0) == 0)
                {
                    return Arg.substr(Prefix.size());
                }
                return std::nullopt;
            };

            if (Arg == "--list" || Arg == "-l")
            {
                Cli.ListOnly = true;
            }
            else if (auto V = ReadValue("--filter"))
            {
                Cli.Filter = *V;
            }
            else if (auto V = ReadValue("--min-time-ms"))
            {
                Cli.MinTimeMilliseconds = std::atoll(V->c_str());
                if (Cli.MinTimeMilliseconds < 1)
                {
                    return std::nullopt;
                }
            }
            else if (auto V = ReadValue("--iterations"))
            {
                Cli.BulkIterations = std::atoll(V->c_str());
                if (Cli.BulkIterations < 1)
                {
                    return std::nullopt;
                }
            }
            else if (auto V = ReadValue("--out-dir"))
            {
                Cli.OutputDir = *V;
            }
            else
            {
                return std::nullopt;
            }
        }

        if (Cli.OutputDir.empty())
        {
#ifdef TEST_OUTPUT_DIR
            Cli.OutputDir = TEST_OUTPUT_DIR;
#else
            Cli.OutputDir = ".";
#endif
        }
        return Cli;
    }

    /** @brief 基准统一入口（TestMain 调用；声明供跨 TU 使用）。 */
    int runBenchmarks(int Argc, char** Argv);

} // namespace PictorBench

// 注册宏：一次调用同时完成 Bm 注册（BENCHMARK_NAME）与场景元数据注册。
#define PICTOR_BENCHMARK(Suite, Impl, Scenario, SizeLabel, OpsPerCall, Mode, DerivedFrom, Note, \
                         Function)                                                              \
    BENCHMARK_NAME(Suite "." Impl "." Scenario "." SizeLabel, Function);                        \
    static const ::PictorBench::MetaRegistrar PictorBenchMetaRegistrar_##Function##_##__LINE__{ \
        ::PictorBench::BenchMeta{Suite "." Impl "." Scenario "." SizeLabel, Suite, Impl,        \
                                 Scenario, SizeLabel, static_cast<std::uint64_t>(OpsPerCall),   \
                                 Mode, DerivedFrom, Note},                                      \
        &Function};

#endif // PICTOR_BENCHMARK_COMMON_H
