// TestMain.cpp — InfraTests 统一入口。
//
// 单一构建目标（tests/CMakeLists.txt 只定义一个可执行文件），通过子命令在
// 单元测试与性能基准之间切换：
//
//   InfraTests                    运行全部单元测试（等价于 `InfraTests test`）
//   InfraTests test  [测试选项]   单元测试
//   InfraTests bench [基准选项]   性能基准
//   InfraTests all               先单元测试，后性能基准
//   InfraTests --help            用法说明
//
// 单元测试选项（对齐 GoogleTest 常用开关）：
//   --filter=<substr>  只运行名称包含子串的用例
//   --seed=<n>         按种子打乱用例顺序（0 表示不打乱，默认 0）
//   --repeat=<n>       整体重复运行 n 次
//   --list-tests       列出用例名并退出
//   --out-dir=<path>   报告输出目录（默认：二进制所在目录）
//   测试结果：控制台 + test_results.txt（镜像）+ test_results.xml（JUnit）
//
// 基准选项见 `InfraTests bench --help`（BenchmarkMain.cpp）。

#include "TestFramework.h"

#include "benchmark/BenchmarkCommon.hpp"

#include <cstdio>
#include <string>

namespace
{
    void printUsage()
    {
        std::printf(
            "usage: InfraTests [test|bench|all] [options]\n"
            "  (no subcommand)     run all unit tests (same as `test`)\n"
            "  test  [options]     unit tests: --filter=<substr> --seed=<n> --repeat=<n>\n"
            "                      --list-tests --out-dir=<path>\n"
            "  bench [options]     benchmarks: --filter=<substr> --min-time-ms=<n>\n"
            "                      --iterations=<n> --list --out-dir=<path>\n"
            "  all [test options]  run unit tests, then benchmarks (bench uses defaults)\n");
    }

    int runTests(int Argc, char** Argv)
    {
        const auto Options = PictorTest::parseOptions(Argc, Argv);
        if (!Options)
        {
            printUsage();
            return 2;
        }
        return PictorTest::runAll(*Options);
    }
} // namespace

int main(int Argc, char** Argv)
{
    std::string Mode = "test";
    int FirstArgument = 1;

    if (Argc > 1)
    {
        const std::string First = Argv[1];
        if (First == "test")
        {
            Mode = "test";
            FirstArgument = 2;
        }
        else if (First == "bench")
        {
            Mode = "bench";
            FirstArgument = 2;
        }
        else if (First == "all")
        {
            Mode = "all";
            FirstArgument = 2;
        }
        else if (First == "-h" || First == "--help" || First == "help")
        {
            printUsage();
            return 0;
        }
    }

    if (Mode == "bench")
    {
        return PictorBench::runBenchmarks(Argc - FirstArgument, Argv + FirstArgument);
    }

    const int TestResult = runTests(Argc - FirstArgument, Argv + FirstArgument);
    if (Mode == "all")
    {
        if (TestResult != 0)
        {
            std::printf("[all] unit tests failed (exit %d); running benchmarks anyway\n",
                        TestResult);
        }
        // `all` 只接受测试选项；基准按默认配置运行（需要定制基准参数请单独跑 `bench`）。
        const int BenchResult = PictorBench::runBenchmarks(0, nullptr);
        return TestResult != 0 ? TestResult : BenchResult;
    }
    return TestResult;
}
