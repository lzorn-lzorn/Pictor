// PictorTest — a tiny, dependency-free test harness for the tests/ directory.
//
// Usage:
//   #include "TestFramework.h"
//   PICTOR_TEST_CASE(MyTest) { PICTOR_CHECK(1 + 1 == 2); }
//   int main() { return PictorTest::runAll(); }

#ifndef PICTOR_TEST_FRAMEWORK_H
#define PICTOR_TEST_FRAMEWORK_H

#include <cstdio>
#include <sstream>
#include <string>
#include <vector>

namespace PictorTest
{
    struct TestCase
    {
        const char* Name;
        void (*Function)();
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

    inline void reportSuccess()
    {
        ++getCheckCount();
    }

    inline void reportFailure(const char* Expression, const std::string& Detail,
                              const char* File, int Line)
    {
        ++getCheckCount();
        ++getFailureCount();
        std::fprintf(stderr, "  FAILED: %s\n           %s (%s:%d)\n",
                     Expression, Detail.c_str(), File, Line);
    }

    /** @brief 运行全部已注册用例，返回进程退出码(0 表示全部通过)。 */
    inline int runAll()
    {
        int Passed = 0;
        int Failed = 0;

        for (const TestCase& Test : getRegistry())
        {
            const int FailuresBefore = getFailureCount();
            std::printf("[ RUN      ] %s\n", Test.Name);
            Test.Function();

            if (getFailureCount() == FailuresBefore)
            {
                ++Passed;
                std::printf("[       OK ] %s\n", Test.Name);
            }
            else
            {
                ++Failed;
                std::printf("[  FAILED  ] %s\n", Test.Name);
            }
        }

        std::printf("[==========] %d test(s), %d check(s), %d failure(s)\n",
                    Passed + Failed, getCheckCount(), getFailureCount());
        return Failed == 0 ? 0 : 1;
    }
} // namespace PictorTest

#define PICTOR_TEST_CASE(Name)                                                              \
    static void PictorTestBody_##Name();                                                    \
    static const ::PictorTest::Registrar PictorTestRegistrar_##Name{#Name,                  \
                                                                    &PictorTestBody_##Name}; \
    static void PictorTestBody_##Name()

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

#endif // PICTOR_TEST_FRAMEWORK_H
