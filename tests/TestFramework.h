// PictorTest — 轻量、零外部依赖的测试框架（风格对齐 GoogleTest / Catch2 的
// 最小可用子集），供 tests/ 目录中的全部测试使用。
//
// 特性：
//   * PICTOR_TEST_CASE(Name) 注册用例；静态注册表 + 单一 main()（见 TestMain.cpp）。
//   * 断言宏：PICTOR_CHECK / CHECK_EQ / CHECK_NE / CHECK_LT / CHECK_LE /
//     CHECK_GT / CHECK_GE / CHECK_NEAR / CHECK_THROWS / CHECK_ANY_THROW /
//     CHECK_NO_THROW（均为非致命断言：失败记录后继续执行，类似 gtest 的 EXPECT_*）。
//   * 用例抛出的未捕获异常会被框架捕获并记为失败，不会中断整个运行。
//   * 运行选项：--filter=、--seed=、--repeat=、--list-tests、--out-dir=（见
//     PictorTest::parseOptions）。
//   * 结果双通道输出：控制台（gtest 风格 [ RUN ]/[ OK ]/[ FAILED ] + 汇总），
//     同时写入文件：test_results.txt（控制台镜像）与 test_results.xml（JUnit
//     格式，可直接被 CI 系统解析）。
//
// 用法：
//   #include "TestFramework.h"
//   PICTOR_TEST_CASE(MyTest) { PICTOR_CHECK(1 + 1 == 2); }
//   // main() 位于 TestMain.cpp：
//   //   PictorTest::RunOptions Opt = PictorTest::parseOptions(Argc, Argv);
//   //   return PictorTest::runAll(Opt);

#ifndef PICTOR_TEST_FRAMEWORK_H
#define PICTOR_TEST_FRAMEWORK_H

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace PictorTest
{
    struct TestCase
    {
        const char* Name;
        void (*Function)();
    };

    /** @brief 单条失败信息。 */
    struct Failure
    {
        std::string Expression;
        std::string Detail;
        std::string File;
        int Line = 0;
    };

    /** @brief 单个用例的一次运行结果（供 JUnit 报告使用）。 */
    struct TestReport
    {
        std::string Name;         ///< 展示名（repeat>1 时带 /i 后缀）
        bool Passed = false;
        double ElapsedMilliseconds = 0.0;
        std::vector<Failure> Failures;
    };

    /** @brief 运行选项（对齐 gtest 常用开关）。 */
    struct RunOptions
    {
        std::string Filter;   ///< 仅运行名称包含该子串的用例（区分大小写）
        std::uint32_t Seed = 0; ///< 非 0 时按该种子打乱用例顺序（可复现）
        int Repeat = 1;       ///< 整体重复运行次数（gtest --gtest_repeat）
        std::string OutputDir; ///< 报告输出目录；为空时用 TEST_OUTPUT_DIR
        bool ListOnly = false; ///< 只列出用例名后退出
    };

    inline std::vector<TestCase>& getRegistry()
    {
        static std::vector<TestCase> Registry;
        return Registry;
    }

    inline int& getFailureCount()
    {
        static int Failures = 0;
        return Failures;
    }

    inline int& getCheckCount()
    {
        static int Checks = 0;
        return Checks;
    }

    inline std::vector<Failure>& getFailures()
    {
        static std::vector<Failure> Failures;
        return Failures;
    }

    inline const char*& getCurrentTestName()
    {
        static const char* Name = nullptr;
        return Name;
    }

    /** @brief 静态初始化时向注册表登记一个测试用例。 */
    struct Registrar
    {
        Registrar(const char* Name, void (*Function)())
        {
            getRegistry().push_back(TestCase{Name, Function});
        }
    };

    template <typename T>
    concept Streamable = requires(std::ostringstream& Stream, const T& Value) { Stream << Value; };

    /**
     * @brief 把值格式化为诊断字符串；不可流式输出的类型打印占位符。
     * @notice 是模板，因此 if constexpr 会丢弃不匹配的分支而不实例化。
     */
    template <typename T>
    std::string describe(const T& Value)
    {
        std::ostringstream Stream;
        Stream << std::boolalpha;
        if constexpr (Streamable<T>)
        {
            Stream << Value;
        }
        else
        {
            Stream << "<unprintable>";
        }
        return Stream.str();
    }

    // ------------------------------------------------------------ 报告器
    // 同时写控制台并累积一份文本镜像，供写入 test_results.txt。

    class Reporter
    {
    public:
        void println(const std::string& Line)
        {
            std::printf("%s\n", Line.c_str());
            std::fflush(stdout);
            Mirror << Line << '\n';
        }

        void printlnErr(const std::string& Line)
        {
            std::fprintf(stderr, "%s\n", Line.c_str());
            Mirror << Line << '\n';
        }

        std::string mirror() const
        {
            return Mirror.str();
        }

    private:
        std::ostringstream Mirror;
    };

    inline Reporter& getReporter()
    {
        static Reporter TheReporter;
        return TheReporter;
    }

    inline void reportSuccess()
    {
        ++getCheckCount();
    }

    inline void reportFailure(const char* Expression, const std::string& Detail,
                              const char* File, int Line)
    {
        ++getCheckCount();
        ++getFailureCount();
        getFailures().push_back(Failure{Expression, Detail, File, Line});
        getReporter().printlnErr("  FAILED: " + std::string(Expression) + "\n           " +
                                 Detail + " (" + File + ":" + std::to_string(Line) + ")");
    }

    // ------------------------------------------------------------ 内部工具

    inline std::string xmlEscape(const std::string& Text)
    {
        std::string Result;
        Result.reserve(Text.size());
        for (char C : Text)
        {
            switch (C)
            {
                case '&':
                    Result += "&amp;";
                    break;
                case '<':
                    Result += "&lt;";
                    break;
                case '>':
                    Result += "&gt;";
                    break;
                case '"':
                    Result += "&quot;";
                    break;
                case '\'':
                    Result += "&apos;";
                    break;
                default:
                    Result += C;
            }
        }
        return Result;
    }

    inline std::string formatDouble(double Value)
    {
        std::ostringstream Stream;
        Stream << Value;
        return Stream.str();
    }

    /** @brief 默认报告目录：优先 TEST_OUTPUT_DIR（测试二进制所在构建目录）。 */
    inline std::string defaultOutputDir()
    {
#ifdef TEST_OUTPUT_DIR
        return TEST_OUTPUT_DIR;
#else
        return ".";
#endif
    }

    /**
     * @brief 解析命令行选项（TestMain 使用）。
     * @return RunOptions；无法识别或参数非法时打印用法并返回 std::nullopt。
     */
    inline std::optional<RunOptions> parseOptions(int Argc, char** Argv)
    {
        RunOptions Options;
        for (int I = 0; I < Argc; ++I)
        {
            const std::string Arg = Argv[I];
            auto ReadValue = [&](const char* Key) -> std::optional<std::string> {
                const std::string Prefix = std::string(Key) + "=";
                if (Arg == Key)
                {
                    if (I + 1 >= Argc)
                    {
                        getReporter().printlnErr("error: missing value for " + std::string(Key));
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

            if (Arg == "--list-tests" || Arg == "-l")
            {
                Options.ListOnly = true;
            }
            else if (auto V = ReadValue("--filter"))
            {
                Options.Filter = *V;
            }
            else if (auto V = ReadValue("--seed"))
            {
                Options.Seed = static_cast<std::uint32_t>(std::strtoul(V->c_str(), nullptr, 10));
            }
            else if (auto V = ReadValue("--repeat"))
            {
                Options.Repeat = std::atoi(V->c_str());
                if (Options.Repeat < 1)
                {
                    getReporter().printlnErr("error: --repeat must be >= 1");
                    return std::nullopt;
                }
            }
            else if (auto V = ReadValue("--out-dir"))
            {
                Options.OutputDir = *V;
            }
            else
            {
                getReporter().printlnErr("error: unknown test option: " + Arg);
                getReporter().printlnErr(
                    "usage: InfraTests test [--filter=<substr>] [--seed=<n>] "
                    "[--repeat=<n>] [--list-tests] [--out-dir=<path>]");
                return std::nullopt;
            }
        }
        if (Options.OutputDir.empty())
        {
            Options.OutputDir = defaultOutputDir();
        }
        return Options;
    }

    /** @brief 把 JUnit XML 写入文件；返回是否成功。 */
    inline bool writeJUnitXml(const std::string& Path, const std::vector<TestReport>& Reports,
                              double TotalSeconds)
    {
        std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
        if (!Output)
        {
            return false;
        }

        std::size_t Failures = 0;
        for (const TestReport& Report : Reports)
        {
            if (!Report.Passed)
            {
                ++Failures;
            }
        }

        Output << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n";
        Output << "<testsuites name=\"InfraTests\" tests=\"" << Reports.size()
               << "\" failures=\"" << Failures << "\" errors=\"0\" time=\""
               << formatDouble(TotalSeconds) << "\">\n";
        Output << "  <testsuite name=\"InfraTests\" tests=\"" << Reports.size()
               << "\" failures=\"" << Failures << "\" errors=\"0\" time=\""
               << formatDouble(TotalSeconds) << "\">\n";

        for (const TestReport& Report : Reports)
        {
            Output << "    <testcase name=\"" << xmlEscape(Report.Name)
                   << "\" classname=\"InfraTests\" time=\""
                   << formatDouble(Report.ElapsedMilliseconds / 1000.0) << "\">\n";
            if (!Report.Passed)
            {
                std::string Detail;
                for (const Failure& FailureItem : Report.Failures)
                {
                    if (!Detail.empty())
                    {
                        Detail += '\n';
                    }
                    Detail += FailureItem.Expression + ": " + FailureItem.Detail + " (" +
                              FailureItem.File + ":" + std::to_string(FailureItem.Line) + ")";
                }
                Output << "      <failure message=\""
                       << xmlEscape(Report.Failures.empty() ? "failed" : Report.Failures.front().Expression)
                       << "\">" << xmlEscape(Detail) << "</failure>\n";
            }
            Output << "    </testcase>\n";
        }

        Output << "  </testsuite>\n";
        Output << "</testsuites>\n";
        return static_cast<bool>(Output);
    }

    /** @brief 运行全部（经 filter/seed/repeat 处理后的）用例，返回进程退出码。 */
    inline int runAll(const RunOptions& Options)
    {
        Reporter& TheReporter = getReporter();

        // 收集通过 filter 的用例。
        std::vector<TestCase> Selected;
        for (const TestCase& Test : getRegistry())
        {
            if (Options.Filter.empty() ||
                std::string(Test.Name).find(Options.Filter) != std::string::npos)
            {
                Selected.push_back(Test);
            }
        }

        if (Options.ListOnly)
        {
            for (const TestCase& Test : Selected)
            {
                TheReporter.println(Test.Name);
            }
            TheReporter.println(std::to_string(Selected.size()) + " test(s) listed.");
            return 0;
        }

        if (Options.Seed != 0)
        {
            std::mt19937 Rng(Options.Seed);
            std::shuffle(Selected.begin(), Selected.end(), Rng);
            TheReporter.println("shuffling test order with seed " + std::to_string(Options.Seed));
        }

        std::vector<TestReport> Reports;
        int TotalPassed = 0;
        int TotalFailed = 0;
        const auto RunStart = std::chrono::steady_clock::now();

        for (int Round = 0; Round < Options.Repeat; ++Round)
        {
            for (const TestCase& Test : Selected)
            {
                std::string DisplayName = Test.Name;
                if (Options.Repeat > 1)
                {
                    DisplayName += "/" + std::to_string(Round);
                }

                TheReporter.println("[ RUN      ] " + DisplayName);
                getCurrentTestName() = Test.Name;
                const int FailuresBefore = getFailureCount();
                const std::size_t PendingFailures = getFailures().size();

                const auto TestStart = std::chrono::steady_clock::now();
                bool Threw = false;
                std::string ExceptionMessage;
                try
                {
                    Test.Function();
                }
                catch (const std::exception& Exception)
                {
                    Threw = true;
                    ExceptionMessage = Exception.what();
                }
                catch (...)
                {
                    Threw = true;
                    ExceptionMessage = "<non-std exception>";
                }
                const auto TestEnd = std::chrono::steady_clock::now();
                const double ElapsedMs =
                    std::chrono::duration<double, std::milli>(TestEnd - TestStart).count();

                if (Threw)
                {
                    reportFailure(Test.Name,
                                  "unexpected exception escaped the test body: " + ExceptionMessage,
                                  __FILE__, __LINE__);
                }

                TestReport Report;
                Report.Name = DisplayName;
                Report.Passed = (getFailureCount() == FailuresBefore);
                Report.ElapsedMilliseconds = ElapsedMs;
                for (std::size_t I = PendingFailures; I < getFailures().size(); ++I)
                {
                    Report.Failures.push_back(getFailures()[I]);
                }
                Reports.push_back(std::move(Report));

                if (Reports.back().Passed)
                {
                    ++TotalPassed;
                    char Buffer[128];
                    std::snprintf(Buffer, sizeof(Buffer), "[       OK ] %s (%.2f ms)", DisplayName.c_str(), ElapsedMs);
                    TheReporter.println(Buffer);
                }
                else
                {
                    ++TotalFailed;
                    char Buffer[128];
                    std::snprintf(Buffer, sizeof(Buffer), "[  FAILED  ] %s (%.2f ms)", DisplayName.c_str(), ElapsedMs);
                    TheReporter.println(Buffer);
                }
            }
        }

        const auto RunEnd = std::chrono::steady_clock::now();
        const double TotalSeconds = std::chrono::duration<double>(RunEnd - RunStart).count();

        char Summary[256];
        std::snprintf(Summary, sizeof(Summary),
                      "[==========] %d test(s) from %zu test case(s) ran (%.2f s total)",
                      TotalPassed + TotalFailed, Selected.size(), TotalSeconds);
        TheReporter.println(Summary);
        std::snprintf(Summary, sizeof(Summary), "[  PASSED  ] %d test(s).", TotalPassed);
        TheReporter.println(Summary);
        if (TotalFailed > 0)
        {
            std::snprintf(Summary, sizeof(Summary), "[  FAILED  ] %d test(s).", TotalFailed);
            TheReporter.println(Summary);
        }
        TheReporter.println("[==========] " + std::to_string(getCheckCount()) + " check(s), " +
                            std::to_string(getFailureCount()) + " failure(s).");

        // 文件输出：控制台文本镜像 + JUnit XML。
        std::error_code Ec;
        std::filesystem::create_directories(Options.OutputDir, Ec);
        const std::string TxtPath = (std::filesystem::path(Options.OutputDir) / "test_results.txt").string();
        const std::string XmlPath = (std::filesystem::path(Options.OutputDir) / "test_results.xml").string();

        {
            std::ofstream Txt(TxtPath, std::ios::binary | std::ios::trunc);
            if (Txt)
            {
                const std::string Text = TheReporter.mirror();
                Txt.write(Text.data(), static_cast<std::streamsize>(Text.size()));
            }
            else
            {
                TheReporter.printlnErr("warning: cannot write " + TxtPath);
            }
        }

        if (writeJUnitXml(XmlPath, Reports, TotalSeconds))
        {
            TheReporter.println("test report written to: " + TxtPath);
            TheReporter.println("JUnit XML written to:   " + XmlPath);
        }
        else
        {
            TheReporter.printlnErr("warning: cannot write " + XmlPath);
        }

        return TotalFailed == 0 ? 0 : 1;
    }

    /** @brief 无参数便捷入口（全部用例、默认选项、输出到默认目录）。 */
    inline int runAll()
    {
        RunOptions Options;
        Options.OutputDir = defaultOutputDir();
        return runAll(Options);
    }
} // namespace PictorTest

// ---------------------------------------------------------------- 注册宏

#define PICTOR_TEST_CASE(Name)                                                              \
    static void PictorTestBody_##Name();                                                    \
    static const ::PictorTest::Registrar PictorTestRegistrar_##Name{#Name,                  \
                                                                    &PictorTestBody_##Name}; \
    static void PictorTestBody_##Name()

// ---------------------------------------------------------------- 断言宏

#define PICTOR_CHECK(Expression)                                                            \
    do                                                                                      \
    {                                                                                       \
        if (Expression)                                                                     \
        {                                                                                   \
            ::PictorTest::reportSuccess();                                                  \
        }                                                                                   \
        else                                                                                \
        {                                                                                   \
            ::PictorTest::reportFailure(#Expression, "evaluated to false", __FILE__,        \
                                        __LINE__);                                          \
        }                                                                                   \
    } while (0)

#define PICTOR_CHECK_EQ(Actual, Expected)                                                   \
    do                                                                                      \
    {                                                                                       \
        const auto PictorCheckActual = (Actual);                                            \
        const auto PictorCheckExpected = (Expected);                                        \
        if (PictorCheckActual == PictorCheckExpected)                                       \
        {                                                                                   \
            ::PictorTest::reportSuccess();                                                  \
        }                                                                                   \
        else                                                                                \
        {                                                                                   \
            const std::string PictorCheckMessage =                                          \
                "expected [" + ::PictorTest::describe(PictorCheckExpected) +                \
                "] but got [" + ::PictorTest::describe(PictorCheckActual) + "]";            \
            ::PictorTest::reportFailure(#Actual " == " #Expected, PictorCheckMessage,       \
                                        __FILE__, __LINE__);                                \
        }                                                                                   \
    } while (0)

#define PICTOR_CHECK_NE(Actual, Expected)                                                   \
    do                                                                                      \
    {                                                                                       \
        const auto PictorCheckActual = (Actual);                                            \
        const auto PictorCheckExpected = (Expected);                                        \
        if (PictorCheckActual != PictorCheckExpected)                                       \
        {                                                                                   \
            ::PictorTest::reportSuccess();                                                  \
        }                                                                                   \
        else                                                                                \
        {                                                                                   \
            const std::string PictorCheckMessage =                                          \
                "both sides are [" + ::PictorTest::describe(PictorCheckActual) + "]";       \
            ::PictorTest::reportFailure(#Actual " != " #Expected, PictorCheckMessage,       \
                                        __FILE__, __LINE__);                                \
        }                                                                                   \
    } while (0)

#define PICTOR_CHECK_LT(Actual, Expected)                                                   \
    do                                                                                      \
    {                                                                                       \
        const auto PictorCheckActual = (Actual);                                            \
        const auto PictorCheckExpected = (Expected);                                        \
        if (PictorCheckActual < PictorCheckExpected)                                        \
        {                                                                                   \
            ::PictorTest::reportSuccess();                                                  \
        }                                                                                   \
        else                                                                                \
        {                                                                                   \
            const std::string PictorCheckMessage =                                          \
                "got [" + ::PictorTest::describe(PictorCheckActual) + "], expected < [" +   \
                ::PictorTest::describe(PictorCheckExpected) + "]";                          \
            ::PictorTest::reportFailure(#Actual " < " #Expected, PictorCheckMessage,        \
                                        __FILE__, __LINE__);                                \
        }                                                                                   \
    } while (0)

#define PICTOR_CHECK_LE(Actual, Expected)                                                   \
    do                                                                                      \
    {                                                                                       \
        const auto PictorCheckActual = (Actual);                                            \
        const auto PictorCheckExpected = (Expected);                                        \
        if (PictorCheckActual <= PictorCheckExpected)                                       \
        {                                                                                   \
            ::PictorTest::reportSuccess();                                                  \
        }                                                                                   \
        else                                                                                \
        {                                                                                   \
            const std::string PictorCheckMessage =                                          \
                "got [" + ::PictorTest::describe(PictorCheckActual) + "], expected <= [" +  \
                ::PictorTest::describe(PictorCheckExpected) + "]";                          \
            ::PictorTest::reportFailure(#Actual " <= " #Expected, PictorCheckMessage,       \
                                        __FILE__, __LINE__);                                \
        }                                                                                   \
    } while (0)

#define PICTOR_CHECK_GT(Actual, Expected)                                                   \
    do                                                                                      \
    {                                                                                       \
        const auto PictorCheckActual = (Actual);                                            \
        const auto PictorCheckExpected = (Expected);                                        \
        if (PictorCheckActual > PictorCheckExpected)                                        \
        {                                                                                   \
            ::PictorTest::reportSuccess();                                                  \
        }                                                                                   \
        else                                                                                \
        {                                                                                   \
            const std::string PictorCheckMessage =                                          \
                "got [" + ::PictorTest::describe(PictorCheckActual) + "], expected > [" +   \
                ::PictorTest::describe(PictorCheckExpected) + "]";                          \
            ::PictorTest::reportFailure(#Actual " > " #Expected, PictorCheckMessage,        \
                                        __FILE__, __LINE__);                                \
        }                                                                                   \
    } while (0)

#define PICTOR_CHECK_GE(Actual, Expected)                                                   \
    do                                                                                      \
    {                                                                                       \
        const auto PictorCheckActual = (Actual);                                            \
        const auto PictorCheckExpected = (Expected);                                        \
        if (PictorCheckActual >= PictorCheckExpected)                                       \
        {                                                                                   \
            ::PictorTest::reportSuccess();                                                  \
        }                                                                                   \
        else                                                                                \
        {                                                                                   \
            const std::string PictorCheckMessage =                                          \
                "got [" + ::PictorTest::describe(PictorCheckActual) + "], expected >= [" +  \
                ::PictorTest::describe(PictorCheckExpected) + "]";                          \
            ::PictorTest::reportFailure(#Actual " >= " #Expected, PictorCheckMessage,       \
                                        __FILE__, __LINE__);                                \
        }                                                                                   \
    } while (0)

#define PICTOR_CHECK_NEAR(Actual, Expected, Epsilon)                                        \
    do                                                                                      \
    {                                                                                       \
        const double PictorCheckActual = static_cast<double>(Actual);                       \
        const double PictorCheckExpected = static_cast<double>(Expected);                   \
        const double PictorCheckEpsilon = static_cast<double>(Epsilon);                     \
        const double PictorCheckDifference = PictorCheckActual - PictorCheckExpected;       \
        if (PictorCheckDifference <= PictorCheckEpsilon &&                                  \
            PictorCheckDifference >= -PictorCheckEpsilon)                                   \
        {                                                                                   \
            ::PictorTest::reportSuccess();                                                  \
        }                                                                                   \
        else                                                                                \
        {                                                                                   \
            std::ostringstream PictorCheckStream;                                           \
            PictorCheckStream << "expected [" << PictorCheckExpected << "] +/- "            \
                              << PictorCheckEpsilon << " but got [" << PictorCheckActual    \
                              << "]";                                                       \
            ::PictorTest::reportFailure(#Actual " ~= " #Expected, PictorCheckStream.str(),  \
                                        __FILE__, __LINE__);                                \
        }                                                                                   \
    } while (0)

// 异常断言：接受语句（类似 gtest 的 EXPECT_*，可含分号/逗号语句）。

#define PICTOR_CHECK_THROWS(Statement, ExceptionType)                                       \
    do                                                                                      \
    {                                                                                       \
        int PictorCheckOutcome = 0; /* 0=未抛 1=抛期望类型 2=抛其它类型 */                  \
        try                                                                                 \
        {                                                                                   \
            [&]() { Statement; }();                                                         \
        }                                                                                   \
        catch (const ExceptionType&)                                                        \
        {                                                                                   \
            PictorCheckOutcome = 1;                                                         \
        }                                                                                   \
        catch (...)                                                                         \
        {                                                                                   \
            PictorCheckOutcome = 2;                                                         \
        }                                                                                   \
        if (PictorCheckOutcome == 1)                                                        \
        {                                                                                   \
            ::PictorTest::reportSuccess();                                                  \
        }                                                                                   \
        else if (PictorCheckOutcome == 2)                                                   \
        {                                                                                   \
            ::PictorTest::reportFailure(#Statement " throws " #ExceptionType,               \
                                        "an exception of an unexpected type was thrown",    \
                                        __FILE__, __LINE__);                                \
        }                                                                                   \
        else                                                                                \
        {                                                                                   \
            ::PictorTest::reportFailure(#Statement " throws " #ExceptionType,               \
                                        "no exception was thrown", __FILE__, __LINE__);     \
        }                                                                                   \
    } while (0)

#define PICTOR_CHECK_ANY_THROW(Statement)                                                   \
    do                                                                                      \
    {                                                                                       \
        bool PictorCheckThrew = false;                                                      \
        try                                                                                 \
        {                                                                                   \
            [&]() { Statement; }();                                                         \
        }                                                                                   \
        catch (...)                                                                         \
        {                                                                                   \
            PictorCheckThrew = true;                                                        \
        }                                                                                   \
        if (PictorCheckThrew)                                                               \
        {                                                                                   \
            ::PictorTest::reportSuccess();                                                  \
        }                                                                                   \
        else                                                                                \
        {                                                                                   \
            ::PictorTest::reportFailure(#Statement " throws any exception",                 \
                                        "no exception was thrown", __FILE__, __LINE__);     \
        }                                                                                   \
    } while (0)

#define PICTOR_CHECK_NO_THROW(Statement)                                                    \
    do                                                                                      \
    {                                                                                       \
        bool PictorCheckThrew = false;                                                      \
        std::string PictorCheckMessage = "no exception was thrown";                         \
        try                                                                                 \
        {                                                                                   \
            [&]() { Statement; }();                                                         \
        }                                                                                   \
        catch (const std::exception& PictorCheckEx)                                         \
        {                                                                                   \
            PictorCheckThrew = true;                                                        \
            PictorCheckMessage = std::string("threw: ") + PictorCheckEx.what();             \
        }                                                                                   \
        catch (...)                                                                         \
        {                                                                                   \
            PictorCheckThrew = true;                                                        \
            PictorCheckMessage = "threw a non-std exception";                               \
        }                                                                                   \
        if (!PictorCheckThrew)                                                              \
        {                                                                                   \
            ::PictorTest::reportSuccess();                                                  \
        }                                                                                   \
        else                                                                                \
        {                                                                                   \
            ::PictorTest::reportFailure(#Statement " does not throw", PictorCheckMessage,   \
                                        __FILE__, __LINE__);                                \
        }                                                                                   \
    } while (0)

#endif // PICTOR_TEST_FRAMEWORK_H
