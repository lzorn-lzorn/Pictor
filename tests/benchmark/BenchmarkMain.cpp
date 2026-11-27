// BenchmarkMain.cpp — 性能基准统一入口（InfraTests bench）。
//
// 流程：
//   1. 解析 CLI（--filter / --min-time-ms / --iterations / --list / --out-dir）。
//   2. 选中匹配 --filter 的基准（派生场景自动补齐其基础场景，保证净耗时可计算）。
//   3. 分别用 Bulk（固定迭代）与 PerOp（自动校准）两类配置运行 Bm 测量。
//   4. 输出四份结果：控制台对比表、bench_report.json（对比数据）、
//      bench_report.md（Markdown 报表）、bench_raw.json（Bm 原始测量）。

#include "benchmark/BenchmarkCommon.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <string>
#include <vector>

namespace PictorBench
{
    namespace
    {
        void printUsage()
        {
            std::printf(
                "usage: InfraTests bench [--filter=<substr>] [--min-time-ms=<n>] "
                "[--iterations=<n>] [--list] [--out-dir=<path>]\n"
                "  --filter=<substr>   only run benchmarks whose name contains <substr>\n"
                "  --min-time-ms=<n>   per-op auto-calibration target, ms (default 80)\n"
                "  --iterations=<n>    fixed iterations for bulk scenarios (default 5)\n"
                "  --list              list benchmark names and exit\n"
                "  --out-dir=<path>    report output directory (default: binary dir)\n");
        }
    } // namespace

    int runBenchmarks(int Argc, char** Argv)
    {
        const auto Cli = parseBenchCli(Argc, Argv);
        if (!Cli)
        {
            printUsage();
            return 2;
        }

        // 1. 选择基准；派生场景（eraseAll 等）自动带上其基础场景。
        std::vector<BenchMeta> Selected;
        for (const BenchMeta& Meta : getMetaRegistry())
        {
            if (Cli->Filter.empty() || Meta.Name.find(Cli->Filter) != std::string::npos)
            {
                Selected.push_back(Meta);
            }
        }
        for (const BenchMeta& Meta : Selected)
        {
            if (Meta.DerivedFrom.empty())
            {
                continue;
            }
            const std::string BaseName =
                Meta.Suite + "." + Meta.Impl + "." + Meta.DerivedFrom + "." + Meta.SizeLabel;
            const bool HasBase = std::any_of(Selected.begin(), Selected.end(),
                                             [&](const BenchMeta& M) { return M.Name == BaseName; });
            if (!HasBase)
            {
                for (const BenchMeta& Candidate : getMetaRegistry())
                {
                    if (Candidate.Name == BaseName)
                    {
                        Selected.push_back(Candidate);
                        break;
                    }
                }
            }
        }

        if (Selected.empty())
        {
            std::fprintf(stderr, "benchmark error: no benchmark matches filter \"%s\"\n",
                         Cli->Filter.c_str());
            return 1;
        }

        if (Cli->ListOnly)
        {
            for (const BenchMeta& Meta : Selected)
            {
                std::printf("%-40s %s\n", Meta.Name.c_str(), Meta.Note.c_str());
            }
            return 0;
        }

        // 2. 两套测量配置。
        Bm::BenchmarkOptions BulkOptions;
        BulkOptions.Iterations = Cli->BulkIterations;
        BulkOptions.WarmupIterations = 2; // 消化冷启动（页错误/分配器预热）
        BulkOptions.TimeUnit = Bm::ETimeUnit::Nanosecond;

        Bm::BenchmarkOptions PerOpOptions;
        PerOpOptions.Iterations = 0; // 自动校准
        PerOpOptions.WarmupIterations = 2;
        PerOpOptions.TargetTotalNanoseconds = Cli->MinTimeMilliseconds * 1000000LL;
        PerOpOptions.TimeUnit = Bm::ETimeUnit::Nanosecond;

        // 派生净耗时（eraseAll − insert.grow 等）是两个大测量的差值，对噪声
        // 敏感：基础场景与派生场景都用更长的采样（10 次 + 2 次预热）。
        Bm::BenchmarkOptions DerivedOptions = BulkOptions;
        DerivedOptions.Iterations = (std::max)(Cli->BulkIterations, static_cast<std::int64_t>(10));

        std::vector<Bm::BenchmarkResult> Results;
        Results.reserve(Selected.size());
        for (const BenchMeta& Meta : Selected)
        {
            Bm::BenchmarkOptions Options;
            if (Meta.Mode == EMeasureMode::PerOp)
            {
                Options = PerOpOptions;
            }
            else if (Meta.Scenario == "eraseAll" || !Meta.DerivedFrom.empty() ||
                     Meta.Scenario.rfind("insert.grow", 0) == 0)
            {
                Options = DerivedOptions;
            }
            else
            {
                Options = BulkOptions;
            }
            try
            {
                Results.push_back(Bm::runOne(Meta.Name, Options));
            }
            catch (const std::exception& Exception)
            {
                std::fprintf(stderr, "benchmark error: %s\n", Exception.what());
                return 1;
            }
        }

        // 3. 报表。
        const std::vector<ScenarioGroup> Groups = buildGroups(Results);
        printConsoleReport(Groups);

        std::error_code Ec;
        std::filesystem::create_directories(Cli->OutputDir, Ec);
        if (Ec)
        {
            std::fprintf(stderr, "benchmark error: cannot create output dir %s: %s\n",
                         Cli->OutputDir.c_str(), Ec.message().c_str());
            return 1;
        }

        const std::filesystem::path OutDir(Cli->OutputDir);
        const std::string RawPath = (OutDir / "bench_raw.json").string();
        const std::string JsonPath = (OutDir / "bench_report.json").string();
        const std::string MarkdownPath = (OutDir / "bench_report.md").string();

        bool AllWritten = true;
        try
        {
            Bm::writeJson(RawPath, Results, BulkOptions);
        }
        catch (const std::exception& Exception)
        {
            AllWritten = false;
            std::fprintf(stderr, "benchmark error: %s\n", Exception.what());
        }
        if (!writeJsonReport(JsonPath, Groups))
        {
            AllWritten = false;
            std::fprintf(stderr, "benchmark error: cannot write %s\n", JsonPath.c_str());
        }
        if (!writeMarkdownReport(MarkdownPath, Groups))
        {
            AllWritten = false;
            std::fprintf(stderr, "benchmark error: cannot write %s\n", MarkdownPath.c_str());
        }

        std::printf("[bench] raw measurements written to: %s\n", RawPath.c_str());
        std::printf("[bench] comparison JSON written to:  %s\n", JsonPath.c_str());
        std::printf("[bench] Markdown report written to:  %s\n", MarkdownPath.c_str());

        return AllWritten ? 0 : 1;
    }
} // namespace PictorBench
