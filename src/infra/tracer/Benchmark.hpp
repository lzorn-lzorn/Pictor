/**
 * @file Benchmark.hpp
 *
 * @brief 极简的 C++11 单头文件 benchmark / 性能分析工具。
 *
 * @notice 整个库只由这一个头文件组成，仅依赖 C++ 标准库。提供两种互补的模式：
 *   - 迭代基准：注册一个 void() 函数，测量运行 N 次的耗时，序列化为 JSON。
 *   - 按帧分析：记录某段代码的耗时与每帧调用次数，写入 JSON。
 *   提供单线程 BenchmarkManager 与线程安全 ThreadSafeBenchmarkManager 两个独立单例；
 *   支持时间单位切换(默认 ms)、单次测量输出槽、计时内打桩(mark)。
 *   所有 JSON 输出统一由 JsonWriter 模块生成；同时支持导出 Chrome Trace 事件格式，便于
 *   在 chrome://tracing 或 Perfetto 中可视化。
 *
 * @code
 *   #include "Benchmark.hpp"
 *
 *   static void benchmarkSqrt() { ... }
 *   BENCHMARK(benchmarkSqrt);
 *
 *   int main()
 *   {
 *       Bm::BenchmarkOptions Options;
 *       Options.TimeUnit = Bm::ETimeUnit::Millisecond; // 默认
 *       return Bm::runAll("result.json", Options);
 *   }
 * @endcode
 */

#ifndef BM_BENCHMARK_HPP
#define BM_BENCHMARK_HPP

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif
namespace Bm{
using Clock = std::chrono::high_resolution_clock;

/**
 * @brief 序列化到 JSON 时使用的时长单位。
 * @notice 时长始终在内部以纳秒累计，仅在输出时换算。
 */
enum class ETimeUnit {
    Hour,
    Minute,
    Second,
    Millisecond,
    Nanosecond,
};

/** @brief 迭代基准模式 */

/**
 * @brief 迭代基准的配置项。
 */
struct BenchmarkOptions {
    // @brief 迭代次数；0 表示自动校准，使总耗时接近 TargetTotalNanoseconds。
    std::int64_t Iterations = 0;

    // @brief 自动校准时使用的下界。
    std::int64_t MinIterations = 1;
    // @brief 自动校准时使用的上界。
    std::int64_t MaxIterations = 1000000000LL;

    // @brief 测量前执行的预热次数。
    std::int64_t WarmupIterations = 1;

    // @brief 自动校准的目标总耗时(纳秒)。
    std::int64_t TargetTotalNanoseconds = 10000000LL; // @notice 10 ms

    // @brief 为 true 时逐次计时，输出 min/max/median/stddev(会引入单次计时开销)。
    bool ShouldMeasureIndividualTimes = false;

    // @brief 序列化到 JSON 时使用的时长单位。
    ETimeUnit TimeUnit = ETimeUnit::Millisecond;
};

/**
 * @brief 单个基准的测量结果。
 */
struct BenchmarkResult {
    std::string Name;
    std::int64_t Iterations           = 0;
    std::int64_t TotalTimeNanoseconds = 0;
    double MeanTimeNanoseconds        = 0.0;
    double ThroughputOpsPerSecond     = 0.0;

    // @brief 仅在 ShouldMeasureIndividualTimes 开启时被填充。
    bool HasDistribution         = false;
    double MinTimeNanoseconds    = 0.0;
    double MaxTimeNanoseconds    = 0.0;
    double MedianTimeNanoseconds = 0.0;
    double StddevTimeNanoseconds = 0.0;
};

// @brief 供 Detail::BenchmarkRegistrar 使用的前置声明。
void registerBenchmark(std::string Name, std::function<void()> Function);

namespace Detail{
struct BenchmarkEntry {
    std::string Name;
    std::function<void()> Function;
};

/**
 * @brief 返回基准注册表。
 * @return 注册表引用。
 * @notice 函数内 static，首次使用时安全初始化，与各翻译单元的静态初始化顺序无关。
 */
inline std::vector<BenchmarkEntry> &getRegistry() {
    static std::vector<BenchmarkEntry> Registry;
    return Registry;
}

/**
 * @brief 返回当前时间点。
 * @return 当前 Clock::time_point。
 */
inline Clock::time_point getTimePoint() {
    return Clock::now();
}

/**
 * @brief 返回某时间点自 epoch 起的纳秒数。
 * @param Time 时间点。
 * @return epoch 纳秒数。
 */
inline std::int64_t getEpochNanoseconds(Clock::time_point Time) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(Time.time_since_epoch()).count();
}

/**
 * @brief 计算两个时间点之间的纳秒数。
 * @param Start 起始时间点。
 * @param End 结束时间点。
 * @return 两者之间的纳秒数。
 */
inline std::int64_t getNanosecondsBetween(Clock::time_point Start, Clock::time_point End) {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(End - Start).count();
}

/**
 * @brief 以固定精度格式化浮点数并去除末尾多余的 0。
 * @param Value 待格式化的值。
 * @return 格式化后的字符串。
 */
inline std::string formatDouble(double Value) {
    if (Value == 0.0) {
        return "0";
    }

    std::ostringstream Stream;
    Stream.imbue(std::locale::classic());
    Stream << std::fixed << std::setprecision(9) << Value;
    std::string Text = Stream.str();

    if (Text.find('.') != std::string::npos) {
        while (!Text.empty() && Text.back() == '0') {
            Text.pop_back();
        }
        if (!Text.empty() && Text.back() == '.') {
            Text.pop_back();
        }
    }
    return Text;
}

/**
 * @brief 最小化的 JSON 字符串转义。
 * @param Value 待转义的字符串。
 * @return 转义后的字符串。
 */
inline std::string escapeJson(const std::string &Value) {
    std::string Result;
    Result.reserve(Value.size());

    for (char Character: Value) {
        switch (Character) {
            case '"':
                Result += "\\\"";
                break;
            case '\\':
                Result += "\\\\";
                break;
            case '\b':
                Result += "\\b";
                break;
            case '\f':
                Result += "\\f";
                break;
            case '\n':
                Result += "\\n";
                break;
            case '\r':
                Result += "\\r";
                break;
            case '\t':
                Result += "\\t";
                break;
            default:
                if (static_cast<unsigned char>(Character) < 0x20) {
                    std::ostringstream Stream;
                    Stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                        << static_cast<unsigned int>(static_cast<unsigned char>(Character));
                    Result += Stream.str();
                }
                else {
                    Result += Character;
                }
        }
    }

    return Result;
}

/**
 * @brief 极简的 JSON 写出器，支持缩进。
 * @notice 采用“尾随逗号 + 关闭容器时去除”策略，稳定生成合法 JSON；所有输出模块共享它。
 */
class JsonWriter {
public:
    /**
     * @brief 构造。
     * @param Pretty 是否缩进美化。
     * @param IndentSize 每层缩进空格数。
     */
    explicit JsonWriter(bool Pretty = true, int IndentSize = 2)
        : Pretty(Pretty),
          IndentSize(IndentSize) {
    }

    void beginObject() {
        writeValueStart();
        Out += '{';
        Stack.push_back(Frame());
    }

    void endObject() {
        endContainer('}');
    }

    void beginArray() {
        writeValueStart();
        Out += '[';
        Stack.push_back(Frame());
    }

    void endArray() {
        endContainer(']');
    }

    void writeKey(const std::string &Key) {
        writeNewlineAndIndent();
        Out      += '"' + escapeJson(Key) + "\": ";
        AfterKey = true;
    }

    void writeString(const std::string &Value) {
        writeValueStart();
        Out += '"' + escapeJson(Value) + '"';
        writeValueEnd();
    }

    void writeInt(std::int64_t Value) {
        writeValueStart();
        Out += std::to_string(Value);
        writeValueEnd();
    }

    void writeDouble(double Value) {
        writeValueStart();
        Out += formatDouble(Value);
        writeValueEnd();
    }

    void writeBool(bool Value) {
        writeValueStart();
        Out += Value ? "true" : "false";
        writeValueEnd();
    }

    void writeNull() {
        writeValueStart();
        Out += "null";
        writeValueEnd();
    }

    void writeRaw(const std::string &Raw) {
        writeValueStart();
        Out += Raw;
        writeValueEnd();
    }

    std::string toString() const {
        return Out;
    }

private:
    struct Frame {
        bool HasElement      = false;
        std::size_t CommaPos = 0;
    };

    /**
     * @brief 值写入前的准备：处理缩进并标记当前容器已有元素。
     */
    void writeValueStart() {
        if (!AfterKey) {
            writeNewlineAndIndent();
        }
        AfterKey = false;

        if (!Stack.empty()) {
            Stack.back().HasElement = true;
        }
    }

    /**
     * @brief 值写入后追加尾随逗号并记录其位置。
     */
    void writeValueEnd() {
        if (!Stack.empty()) {
            Out                   += ',';
            Stack.back().CommaPos = Out.size() - 1;
        }
    }

    /**
     * @brief 结束容器：去除最后一个尾随逗号后写入闭合符。
     * @param Closing 闭合字符('}' 或 ']')。
     */
    void endContainer(char Closing) {
        if (!Stack.empty() && Stack.back().HasElement) {
            Out.erase(Stack.back().CommaPos, 1);
        }
        Stack.pop_back();
        writeNewlineAndIndent();
        Out += Closing;

        if (!Stack.empty()) {
            writeValueEnd();
        }
    }

    /**
     * @brief 写出换行与缩进。
     */
    void writeNewlineAndIndent() {
        if (Pretty && !Out.empty()) {
            Out += '\n';
            Out.append(static_cast<std::size_t>(Stack.size() * IndentSize), ' ');
        }
    }

    std::string Out;
    std::vector<Frame> Stack;
    bool Pretty;
    int IndentSize;
    bool AfterKey = false;
};

/** @brief 时间单位换算辅助 */

/**
 * @brief 返回时间单位的名称。
 * @param Unit 时间单位。
 * @return 单位名称字符串。
 */
inline const char *getTimeUnitName(ETimeUnit Unit) {
    switch (Unit) {
        case ETimeUnit::Hour:
            return "hour";
        case ETimeUnit::Minute:
            return "min";
        case ETimeUnit::Second:
            return "second";
        case ETimeUnit::Millisecond:
            return "ms";
        case ETimeUnit::Nanosecond:
            return "ns";
    }
    return "ms";
}

/**
 * @brief 返回一个单位包含的纳秒数。
 * @param Unit 时间单位。
 * @return 该单位包含的纳秒数。
 */
inline double getNanosecondsPerUnit(ETimeUnit Unit) {
    switch (Unit) {
        case ETimeUnit::Hour:
            return 3.6e12;
        case ETimeUnit::Minute:
            return 6.0e10;
        case ETimeUnit::Second:
            return 1.0e9;
        case ETimeUnit::Millisecond:
            return 1.0e6;
        case ETimeUnit::Nanosecond:
            return 1.0;
    }
    return 1.0e6;
}

/**
 * @brief 将纳秒换算为目标单位。
 * @param Nanoseconds 纳秒值。
 * @param Unit 目标单位。
 * @return 换算后的值。
 */
inline double convertNanoseconds(std::int64_t Nanoseconds, ETimeUnit Unit) {
    return static_cast<double>(Nanoseconds) / getNanosecondsPerUnit(Unit);
}

/**
 * @brief 将纳秒换算为目标单位。
 * @param Nanoseconds 纳秒值。
 * @param Unit 目标单位。
 * @return 换算后的值。
 */
inline double convertNanoseconds(double Nanoseconds, ETimeUnit Unit) {
    return Nanoseconds / getNanosecondsPerUnit(Unit);
}

/**
 * @brief 将纳秒按目标单位格式化。
 * @param Nanoseconds 纳秒值。
 * @param Unit 目标单位。
 * @return 格式化后的字符串。
 */
inline std::string formatNanoseconds(std::int64_t Nanoseconds, ETimeUnit Unit) {
    return formatDouble(convertNanoseconds(Nanoseconds, Unit));
}

/**
 * @brief 将纳秒按目标单位格式化。
 * @param Nanoseconds 纳秒值。
 * @param Unit 目标单位。
 * @return 格式化后的字符串。
 */
inline std::string formatNanoseconds(double Nanoseconds, ETimeUnit Unit) {
    return formatDouble(convertNanoseconds(Nanoseconds, Unit));
}

/**
 * @brief 由 BENCHMARK / BENCHMARK_NAME 宏在静态初始化时构造，完成注册。
 */
struct BenchmarkRegistrar {
    BenchmarkRegistrar(std::string Name, std::function<void()> Function) {
        registerBenchmark(std::move(Name), std::move(Function));
    }
};
} // namespace Detail

/**
 * @brief 以唯一名称注册基准。
 * @param Name 基准名。
 * @param Function 待测量的 void() 函数。
 * @notice 同名注册会被忽略，因此被多个翻译单元包含的基准头只会注册一次。
 */
inline void registerBenchmark(std::string Name, std::function<void()> Function) {
    std::vector<Detail::BenchmarkEntry> &Registry = Detail::getRegistry();

    for (const Detail::BenchmarkEntry &Entry: Registry) {
        if (Entry.Name == Name) {
            return;
        }
    }

    Registry.push_back(Detail::BenchmarkEntry{std::move(Name), std::move(Function)});
}

/**
 * @brief 运行单个已注册的基准。
 * @param Entry 基准条目。
 * @param Options 配置项。
 * @return 该基准的测量结果。
 */
inline BenchmarkResult runOne(const Detail::BenchmarkEntry &Entry, const BenchmarkOptions &Options) {
    BenchmarkResult Result;
    Result.Name = Entry.Name;

    // @brief 预热(通常用于预热缓存 / 分支预测)。
    for (std::int64_t Index = 0; Index < Options.WarmupIterations; ++Index) {
        Entry.Function();
    }

    // @brief 确定迭代次数，需要时进行自动校准。
    std::int64_t Iterations = Options.Iterations;
    if (Iterations <= 0) {
        const Clock::time_point CalibrationStart = Detail::getTimePoint();
        Entry.Function();
        const Clock::time_point CalibrationEnd = Detail::getTimePoint();

        std::int64_t SingleCallNanoseconds = Detail::getNanosecondsBetween(CalibrationStart, CalibrationEnd);
        if (SingleCallNanoseconds <= 0) {
            SingleCallNanoseconds = 1;
        }

        Iterations = Options.TargetTotalNanoseconds / SingleCallNanoseconds;
        Iterations = (std::max)(Iterations, Options.MinIterations);
        Iterations = (std::min)(Iterations, Options.MaxIterations);
    }
    if (Iterations <= 0) {
        Iterations = 1;
    }
    Result.Iterations = Iterations;

    std::vector<double> PerIterationTimes;
    if (Options.ShouldMeasureIndividualTimes) {
        PerIterationTimes.reserve(
                                  static_cast<std::size_t>((
                                      std::min)(Iterations, static_cast<std::int64_t>(1LL << 20))));
    }

    const Clock::time_point Start = Detail::getTimePoint();
    for (std::int64_t Index = 0; Index < Iterations; ++Index) {
        if (Options.ShouldMeasureIndividualTimes) {
            const Clock::time_point IterationStart = Detail::getTimePoint();
            Entry.Function();
            const Clock::time_point IterationEnd = Detail::getTimePoint();
            PerIterationTimes.push_back(
                                        static_cast<double>(
                                            Detail::getNanosecondsBetween(IterationStart, IterationEnd)));
        }
        else {
            Entry.Function();
        }
    }
    const Clock::time_point End = Detail::getTimePoint();

    Result.TotalTimeNanoseconds = Detail::getNanosecondsBetween(Start, End);
    const double TotalSeconds   = static_cast<double>(Result.TotalTimeNanoseconds) / 1e9;
    Result.MeanTimeNanoseconds  =
        static_cast<double>(Result.TotalTimeNanoseconds) / static_cast<double>(Iterations);
    Result.ThroughputOpsPerSecond =
        TotalSeconds > 0.0 ? static_cast<double>(Iterations) / TotalSeconds : 0.0;

    if (Options.ShouldMeasureIndividualTimes && !PerIterationTimes.empty()) {
        std::sort(PerIterationTimes.begin(), PerIterationTimes.end());

        Result.HasDistribution    = true;
        Result.MinTimeNanoseconds = PerIterationTimes.front();
        Result.MaxTimeNanoseconds = PerIterationTimes.back();

        const std::size_t Count = PerIterationTimes.size();
        if (Count % 2 == 1) {
            Result.MedianTimeNanoseconds = PerIterationTimes[Count / 2];
        }
        else {
            Result.MedianTimeNanoseconds =
                (PerIterationTimes[Count / 2 - 1] + PerIterationTimes[Count / 2]) / 2.0;
        }

        double Sum = 0.0;
        for (double Time: PerIterationTimes) {
            Sum += Time;
        }
        const double Mean = Sum / static_cast<double>(Count);

        double SquaredSum = 0.0;
        for (double Time: PerIterationTimes) {
            const double Difference = Time - Mean;
            SquaredSum              += Difference * Difference;
        }
        Result.StddevTimeNanoseconds = std::sqrt(SquaredSum / static_cast<double>(Count));
    }

    return Result;
}

/**
 * @brief 按名字运行单个已注册的基准。
 * @param Name 基准名。
 * @param Options 配置项。
 * @return 该基准的测量结果。
 */
inline BenchmarkResult runOne(const std::string &Name, const BenchmarkOptions &Options = BenchmarkOptions()) {
    const Detail::BenchmarkEntry *Found = nullptr;
    for (const Detail::BenchmarkEntry &Entry: Detail::getRegistry()) {
        if (Entry.Name == Name) {
            Found = &Entry;
            break;
        }
    }

    if (Found == nullptr) {
        throw std::runtime_error("benchmark not found: " + Name);
    }

    return runOne(*Found, Options);
}

/**
 * @brief 运行所有已注册的基准。
 * @param Options 配置项。
 * @return 按注册顺序排列的测量结果。
 */
inline std::vector<BenchmarkResult> runAll(const BenchmarkOptions &Options = BenchmarkOptions()) {
    std::vector<BenchmarkResult> Results;
    for (const Detail::BenchmarkEntry &Entry: Detail::getRegistry()) {
        Results.push_back(runOne(Entry, Options));
    }
    return Results;
}

/**
 * @brief 将迭代基准结果序列化为 JSON 字符串。
 * @param Results 测量结果列表。
 * @param Options 配置项。
 * @return JSON 字符串。
 */
inline std::string serializeJson(const std::vector<BenchmarkResult> &Results,
                                 const BenchmarkOptions &Options) {
    Detail::JsonWriter Writer(true, 2);
    Writer.beginObject();

    Writer.writeKey("time_unit");
    Writer.writeString(Detail::getTimeUnitName(Options.TimeUnit));

    Writer.writeKey("benchmarks");
    Writer.beginArray();

    for (const BenchmarkResult &Result: Results) {
        Writer.beginObject();

        Writer.writeKey("name");
        Writer.writeString(Result.Name);

        Writer.writeKey("iterations");
        Writer.writeInt(Result.Iterations);

        Writer.writeKey("total_time");
        Writer.writeDouble(Detail::convertNanoseconds(Result.TotalTimeNanoseconds, Options.TimeUnit));

        Writer.writeKey("mean_time");
        Writer.writeDouble(Detail::convertNanoseconds(Result.MeanTimeNanoseconds, Options.TimeUnit));

        if (Result.HasDistribution) {
            Writer.writeKey("min_time");
            Writer.writeDouble(Detail::convertNanoseconds(Result.MinTimeNanoseconds, Options.TimeUnit));

            Writer.writeKey("max_time");
            Writer.writeDouble(Detail::convertNanoseconds(Result.MaxTimeNanoseconds, Options.TimeUnit));

            Writer.writeKey("median_time");
            Writer.writeDouble(Detail::convertNanoseconds(Result.MedianTimeNanoseconds, Options.TimeUnit));

            Writer.writeKey("stddev_time");
            Writer.writeDouble(Detail::convertNanoseconds(Result.StddevTimeNanoseconds, Options.TimeUnit));
        }

        Writer.writeKey("throughput_ops_per_second");
        Writer.writeDouble(Result.ThroughputOpsPerSecond);

        Writer.endObject();
    }

    Writer.endArray();
    Writer.endObject();
    return Writer.toString();
}

/**
 * @brief 将迭代基准结果以 JSON 写入文件。
 * @param Path 输出路径。
 * @param Results 测量结果列表。
 * @param Options 配置项。
 * @notice 会覆盖已有文件。
 */
inline void writeJson(const std::string &Path,
                      const std::vector<BenchmarkResult> &Results,
                      const BenchmarkOptions &Options = BenchmarkOptions()) {
    std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
    if (!Output) {
        throw std::runtime_error("cannot open output file: " + Path);
    }

    const std::string Text = serializeJson(Results, Options);
    Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
    if (!Output) {
        throw std::runtime_error("failed to write output file: " + Path);
    }
}

/**
 * @brief 运行所有基准并将 JSON 写入文件。
 * @param JsonPath 输出路径。
 * @param Options 配置项。
 * @return 成功返回 0，失败返回 1。
 */
inline int runAll(const std::string &JsonPath, const BenchmarkOptions &Options = BenchmarkOptions()) {
    try {
        writeJson(JsonPath, runAll(Options), Options);
        return 0;
    }
    catch (const std::exception &Exception) {
        std::cerr << "benchmark error: " << Exception.what() << "\n";
        return 1;
    }
}

/** @brief 按帧分析模式 */

/**
 * @brief 单个命名代码段的聚合统计。
 */
struct SectionStats {
    std::string Name;

    // @brief 跨所有记录的调用统计。
    std::int64_t CallCount            = 0;
    std::int64_t TotalTimeNanoseconds = 0;
    std::int64_t MinTimeNanoseconds   = 0;
    std::int64_t MaxTimeNanoseconds   = 0;
    double MeanTimeNanoseconds        = 0.0;
    double StddevTimeNanoseconds      = 0.0;

    // @brief 跨该段出现过的帧的统计。
    std::int64_t AppearedFrameCount      = 0;
    std::int64_t FrameCallCountMin       = 0;
    std::int64_t FrameCallCountMax       = 0;
    double FrameCallCountMean            = 0.0;
    std::int64_t FrameTimeNanosecondsMin = 0;
    std::int64_t FrameTimeNanosecondsMax = 0;
    double FrameTimeNanosecondsMean      = 0.0;
};

/**
 * @brief 单次测量完成后的 JSON 立即输出目标。
 */
enum class EMeasurementSink {
    None,     // @brief 不立即输出(仅累积)。
    StdOut,   // @brief 写入 stdout。
    StdErr,   // @brief 写入 stderr。
    File,     // @brief 追加写入配置的文件。
    Callback, // @brief 转发给配置的回调。
};

/**
 * @brief 计时作用域内记录的一个打桩点。
 * @notice ElapsedNanoseconds 表示相对上一个打桩点(或计时起点)的耗时。
 */
struct CheckpointRecord {
    std::string Name;
    std::int64_t StartNanoseconds   = 0; // @brief 该段开始的 epoch 纳秒时间。
    std::int64_t ElapsedNanoseconds = 0; // @brief 该段耗时。
};

/**
 * @brief 一次完成的测量。
 */
struct MeasurementRecord {
    std::string Name;
    std::int64_t StartNanoseconds   = 0;       // @brief 测量开始的 epoch 纳秒时间。
    std::int64_t ElapsedNanoseconds = 0;       // @brief 本次总耗时。
    std::int64_t Sequence           = 0;       // @brief 所有测量中的全局序号。
    std::int64_t FrameIndex         = 0;       // @brief 1 起算的当前帧；未开帧为 0。
    std::int64_t FrameCallIndex     = 0;       // @brief 帧内第几次调用(1 起)；未开帧为 0。
    std::vector<CheckpointRecord> Checkpoints; // @brief 计时作用域内记录的打桩点。
};

namespace Detail{
/**
 * @brief manager 内部的按段累加器。
 */
struct SectionAccumulator {
    std::string Name;

    std::int64_t CallCount            = 0;
    std::int64_t TotalTimeNanoseconds = 0;
    std::int64_t MinTimeNanoseconds   = std::numeric_limits<std::int64_t>::max();
    std::int64_t MaxTimeNanoseconds   = 0;
    double SumOfSquaresNanoseconds    = 0.0;

    // @brief 当前(未结束)帧的计数。
    std::int64_t FrameCallCount       = 0;
    std::int64_t FrameTimeNanoseconds = 0;

    // @brief 跨该段出现过的帧的聚合。
    std::int64_t AppearedFrameCount = 0;
    std::int64_t FrameCallCountSum  = 0;
    std::int64_t FrameCallCountMin  = std::numeric_limits<std::int64_t>::max();
    std::int64_t FrameCallCountMax  = 0;
    std::int64_t FrameTimeSum       = 0;
    std::int64_t FrameTimeMin       = std::numeric_limits<std::int64_t>::max();
    std::int64_t FrameTimeMax       = 0;
};

/**
 * @brief 单线程 manager 使用的空锁策略。
 * @notice 空实现会被优化器完全消除，因此单线程版本零同步开销。
 */
struct NoOpLock {
    void lock() {
    }

    void unlock() {
    }
};

/**
 * @brief 将单次测量序列化为单行 JSON 对象。
 * @param Record 测量记录。
 * @param Unit 时间单位。
 * @return JSON 字符串。
 */
inline std::string serializeMeasurement(const MeasurementRecord &Record, ETimeUnit Unit) {
    JsonWriter Writer(false);

    Writer.beginObject();
    Writer.writeKey("name");
    Writer.writeString(Record.Name);
    Writer.writeKey("elapsed");
    Writer.writeDouble(convertNanoseconds(Record.ElapsedNanoseconds, Unit));
    Writer.writeKey("unit");
    Writer.writeString(getTimeUnitName(Unit));
    Writer.writeKey("start_ns");
    Writer.writeInt(Record.StartNanoseconds);
    Writer.writeKey("sequence");
    Writer.writeInt(Record.Sequence);
    Writer.writeKey("frame_index");
    Writer.writeInt(Record.FrameIndex);
    Writer.writeKey("frame_call_index");
    Writer.writeInt(Record.FrameCallIndex);

    if (!Record.Checkpoints.empty()) {
        Writer.writeKey("checkpoints");
        Writer.beginArray();
        for (const CheckpointRecord &Checkpoint: Record.Checkpoints) {
            Writer.beginObject();
            Writer.writeKey("name");
            Writer.writeString(Checkpoint.Name);
            Writer.writeKey("start_ns");
            Writer.writeInt(Checkpoint.StartNanoseconds);
            Writer.writeKey("elapsed");
            Writer.writeDouble(convertNanoseconds(Checkpoint.ElapsedNanoseconds, Unit));
            Writer.endObject();
        }
        Writer.endArray();
    }

    Writer.endObject();
    return Writer.toString();
}
} // namespace Detail

template<typename ManagerT>
class BenchmarkTimerBase;

namespace Detail{
/**
 * @brief 两个 manager 共享的实现。
 * @param LockT 同步策略：单线程用 NoOpLock，线程安全用 std::mutex。
 * @notice 两者是独立的类型、独立的单例。
 */
template<typename LockT>
class BenchmarkManagerBase {
public:
    using TimerType = BenchmarkTimerBase<BenchmarkManagerBase<LockT> >;

    /**
     * @brief 返回单例。
     * @return 单例引用。
     */
    static BenchmarkManagerBase &get() {
        static BenchmarkManagerBase Instance;
        return Instance;
    }

    /**
     * @brief 记录命名段的一次调用，耗时 Nanoseconds。
     * @param Name 段名。
     * @param Nanoseconds 本次耗时。
     * @notice 同时把完成的测量发往配置的 sink(见 setMeasurementSink)。
     */
    void recordTime(const std::string &Name, std::int64_t Nanoseconds) {
        recordTime(Name, Nanoseconds, 0, std::vector<CheckpointRecord>());
    }

    /**
     * @brief 记录命名段的一次调用，并附带起始时间与打桩点。
     * @param Name 段名。
     * @param Nanoseconds 本次耗时。
     * @param StartNanoseconds 测量开始的 epoch 纳秒时间。
     * @param Checkpoints 打桩点列表。
     * @notice 每个打桩点的耗时相对上一个打桩点(或计时起点)。
     */
    void recordTime(const std::string &Name,
                    std::int64_t Nanoseconds,
                    std::int64_t StartNanoseconds,
                    std::vector<CheckpointRecord> Checkpoints) {
        if (Nanoseconds < 0) {
            Nanoseconds = 0;
        }

        std::string Json;
        bool HasJson              = false;
        EMeasurementSink SinkKind = EMeasurementSink::None;
        std::string FilePath;
        std::function<void(const std::string &)> Callback;

        {
            std::lock_guard<LockT> Guard(Lock);

            Detail::SectionAccumulator &Section = findOrCreate(Name);
            ++Section.CallCount;
            Section.TotalTimeNanoseconds    += Nanoseconds;
            Section.MinTimeNanoseconds      = (std::min)(Section.MinTimeNanoseconds, Nanoseconds);
            Section.MaxTimeNanoseconds      = (std::max)(Section.MaxTimeNanoseconds, Nanoseconds);
            Section.SumOfSquaresNanoseconds +=
                static_cast<double>(Nanoseconds) * static_cast<double>(Nanoseconds);

            ++Section.FrameCallCount;
            Section.FrameTimeNanoseconds += Nanoseconds;

            HasJson = ShouldStoreMeasurements || MeasurementSink != EMeasurementSink::None;
            if (HasJson) {
                MeasurementRecord Record;
                Record.Name               = Name;
                Record.StartNanoseconds   = StartNanoseconds;
                Record.ElapsedNanoseconds = Nanoseconds;
                Record.Sequence           = ++MeasurementSequence;
                Record.FrameIndex         = HasOpenFrame ? TotalFrameCount + 1 : 0;
                Record.FrameCallIndex     = Section.FrameCallCount;
                Record.Checkpoints        = std::move(Checkpoints);

                Json = serializeMeasurement(Record, TimeUnit);

                if (ShouldStoreMeasurements) {
                    MeasurementRecords.push_back(std::move(Record));
                }
            }

            SinkKind = MeasurementSink;
            if (SinkKind == EMeasurementSink::File) {
                FilePath = MeasurementFilePath;
            }
            else if (SinkKind == EMeasurementSink::Callback) {
                Callback = MeasurementCallback;
            }
        }

        if (HasJson) {
            emitMeasurement(SinkKind, FilePath, Callback, Json);
        }
    }

    /**
     * @brief 标记新一帧的开始。
     * @notice 若已有未结束的帧，会先将其结算。
     */
    void beginFrame() {
        std::lock_guard<LockT> Guard(Lock);

        if (HasOpenFrame) {
            finalizeCurrentFrame();
        }
        else {
            // @brief 丢弃任何帧开始前记录的游离计数。
            for (Detail::SectionAccumulator &Section: Sections) {
                Section.FrameCallCount       = 0;
                Section.FrameTimeNanoseconds = 0;
            }
        }

        HasOpenFrame = true;
    }

    /**
     * @brief 标记当前帧结束，并把计数并入按帧统计。
     */
    void endFrame() {
        std::lock_guard<LockT> Guard(Lock);

        if (!HasOpenFrame) {
            return;
        }

        finalizeCurrentFrame();
        HasOpenFrame = false;
    }

    /**
     * @brief 返回一个在作用域结束时记录的 RAII 计时器。
     * @param Name 段名。
     * @return RAII 计时器。
     */
    TimerType measure(const std::string &Name);

    /**
     * @brief 返回按首次出现顺序排列的聚合统计。
     * @return SectionStats 列表。
     */
    std::vector<SectionStats> getStats() const {
        std::lock_guard<LockT> Guard(Lock);
        return buildStats();
    }

    /**
     * @brief 返回已统计的总帧数。
     * @return 总帧数。
     */
    std::int64_t getTotalFrameCount() const {
        std::lock_guard<LockT> Guard(Lock);
        return TotalFrameCount;
    }

    /**
     * @brief 清空所有累计数据(统计与测量记录)。
     */
    void reset() {
        std::lock_guard<LockT> Guard(Lock);
        Sections.clear();
        TotalFrameCount = 0;
        HasOpenFrame    = false;
        MeasurementRecords.clear();
        MeasurementSequence = 0;
    }

    /**
     * @brief 设置序列化到 JSON 时使用的时长单位。
     * @param Unit 时间单位。
     */
    void setTimeUnit(ETimeUnit Unit) {
        std::lock_guard<LockT> Guard(Lock);
        TimeUnit = Unit;
    }

    /**
     * @brief 返回当前时长单位。
     * @return 时间单位。
     */
    ETimeUnit getTimeUnit() const {
        std::lock_guard<LockT> Guard(Lock);
        return TimeUnit;
    }

    /**
     * @brief 设置单次测量 JSON 的立即输出目标。
     * @param SinkKind 输出目标。
     */
    void setMeasurementSink(EMeasurementSink SinkKind) {
        std::lock_guard<LockT> Guard(Lock);
        MeasurementSink = SinkKind;
    }

    /**
     * @brief 设置 EMeasurementSink::File 使用的文件。
     * @param Path 文件路径。
     * @notice 每次测量追加一行。
     */
    void setMeasurementFile(const std::string &Path) {
        std::lock_guard<LockT> Guard(Lock);
        MeasurementFilePath = Path;
    }

    /**
     * @brief 设置 EMeasurementSink::Callback 使用的回调。
     * @param Callback 回调函数。
     */
    void setMeasurementCallback(std::function<void(const std::string &)> Callback) {
        std::lock_guard<LockT> Guard(Lock);
        MeasurementCallback = std::move(Callback);
    }

    /**
     * @brief 控制是否在内存中累积完成的测量。
     * @param ShouldStore 是否累积。
     */
    void setShouldStoreMeasurements(bool ShouldStore) {
        std::lock_guard<LockT> Guard(Lock);
        ShouldStoreMeasurements = ShouldStore;
    }

    /**
     * @brief 返回已累积的测量记录副本。
     * @return MeasurementRecord 列表。
     */
    std::vector<MeasurementRecord> getMeasurementRecords() const {
        std::lock_guard<LockT> Guard(Lock);
        return MeasurementRecords;
    }

    /**
     * @brief 返回已累积的单次测量 JSON 列表(即时序列化)。
     * @return JSON 字符串列表。
     */
    std::vector<std::string> getMeasurementLog() const {
        std::lock_guard<LockT> Guard(Lock);
        std::vector<std::string> Result;
        Result.reserve(MeasurementRecords.size());
        for (const MeasurementRecord &Record: MeasurementRecords) {
            Result.push_back(serializeMeasurement(Record, TimeUnit));
        }
        return Result;
    }

    /**
     * @brief 清空已累积的测量记录(保留 sink 配置)。
     */
    void clearMeasurementLog() {
        std::lock_guard<LockT> Guard(Lock);
        MeasurementRecords.clear();
    }

    /**
     * @brief 把累积的单次测量写入文件，每行一条(JSON Lines)。
     * @param Path 输出路径。
     * @return 成功返回 0，失败返回 1。
     */
    int writeMeasurementLog(const std::string &Path) const {
        std::vector<MeasurementRecord> Copy;
        ETimeUnit Unit = ETimeUnit::Millisecond;
        {
            std::lock_guard<LockT> Guard(Lock);
            Copy = MeasurementRecords;
            Unit = TimeUnit;
        }

        try {
            std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
            if (!Output) {
                throw std::runtime_error("cannot open output file: " + Path);
            }
            for (const MeasurementRecord &Record: Copy) {
                Output << serializeMeasurement(Record, Unit) << '\n';
            }
            if (!Output) {
                throw std::runtime_error("failed to write output file: " + Path);
            }
            return 0;
        }
        catch (const std::exception &Exception) {
            std::cerr << "benchmark error: " << Exception.what() << "\n";
            return 1;
        }
    }

    /**
     * @brief 将聚合统计序列化为 JSON 字符串。
     * @return JSON 字符串。
     */
    std::string serializeJson() const {
        std::lock_guard<LockT> Guard(Lock);

        const std::vector<SectionStats> Stats = buildStats();

        JsonWriter Writer(true, 2);
        Writer.beginObject();

        Writer.writeKey("time_unit");
        Writer.writeString(getTimeUnitName(TimeUnit));

        Writer.writeKey("frame_count");
        Writer.writeInt(TotalFrameCount);

        Writer.writeKey("sections");
        Writer.beginArray();

        for (const SectionStats &Section: Stats) {
            Writer.beginObject();

            Writer.writeKey("name");
            Writer.writeString(Section.Name);

            Writer.writeKey("call_count");
            Writer.writeInt(Section.CallCount);

            Writer.writeKey("total_time");
            Writer.writeDouble(convertNanoseconds(Section.TotalTimeNanoseconds, TimeUnit));

            Writer.writeKey("min_time");
            Writer.writeDouble(convertNanoseconds(Section.MinTimeNanoseconds, TimeUnit));

            Writer.writeKey("max_time");
            Writer.writeDouble(convertNanoseconds(Section.MaxTimeNanoseconds, TimeUnit));

            Writer.writeKey("mean_time");
            Writer.writeDouble(convertNanoseconds(Section.MeanTimeNanoseconds, TimeUnit));

            Writer.writeKey("stddev_time");
            Writer.writeDouble(convertNanoseconds(Section.StddevTimeNanoseconds, TimeUnit));

            Writer.writeKey("frame_count");
            Writer.writeInt(Section.AppearedFrameCount);

            Writer.writeKey("frame_call_count_min");
            Writer.writeInt(Section.FrameCallCountMin);

            Writer.writeKey("frame_call_count_max");
            Writer.writeInt(Section.FrameCallCountMax);

            Writer.writeKey("frame_call_count_mean");
            Writer.writeDouble(Section.FrameCallCountMean);

            Writer.writeKey("frame_time_min");
            Writer.writeDouble(convertNanoseconds(Section.FrameTimeNanosecondsMin, TimeUnit));

            Writer.writeKey("frame_time_max");
            Writer.writeDouble(convertNanoseconds(Section.FrameTimeNanosecondsMax, TimeUnit));

            Writer.writeKey("frame_time_mean");
            Writer.writeDouble(convertNanoseconds(Section.FrameTimeNanosecondsMean, TimeUnit));

            Writer.endObject();
        }

        Writer.endArray();
        Writer.endObject();
        return Writer.toString();
    }

    /**
     * @brief 将聚合统计以 JSON 写入文件。
     * @param Path 输出路径。
     * @return 成功返回 0，失败返回 1。
     * @notice 会覆盖已有文件。
     */
    int writeJson(const std::string &Path) const {
        try {
            const std::string Text = serializeJson();

            std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
            if (!Output) {
                throw std::runtime_error("cannot open output file: " + Path);
            }
            Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
            if (!Output) {
                throw std::runtime_error("failed to write output file: " + Path);
            }
            return 0;
        }
        catch (const std::exception &Exception) {
            std::cerr << "benchmark error: " << Exception.what() << "\n";
            return 1;
        }
    }

    /**
     * @brief 将累积的测量序列化为 Chrome Trace 事件格式。
     * @return trace JSON 字符串。
     * @notice 输出可直接在 chrome://tracing 或 Perfetto 中打开；时间单位为微秒。
     */
    std::string serializeTrace() const {
        std::vector<MeasurementRecord> Copy;
        {
            std::lock_guard<LockT> Guard(Lock);
            Copy = MeasurementRecords;
        }

        JsonWriter Writer(false);

        Writer.beginObject();
        Writer.writeKey("traceEvents");
        Writer.beginArray();

        for (const MeasurementRecord &Record: Copy) {
            Writer.beginObject();
            Writer.writeKey("name");
            Writer.writeString(Record.Name);
            Writer.writeKey("cat");
            Writer.writeString("benchmark");
            Writer.writeKey("ph");
            Writer.writeString("X");
            Writer.writeKey("ts");
            Writer.writeDouble(static_cast<double>(Record.StartNanoseconds) / 1000.0);
            Writer.writeKey("dur");
            Writer.writeDouble(static_cast<double>(Record.ElapsedNanoseconds) / 1000.0);
            Writer.writeKey("pid");
            Writer.writeInt(0);
            Writer.writeKey("tid");
            Writer.writeInt(0);
            Writer.endObject();

            for (const CheckpointRecord &Checkpoint: Record.Checkpoints) {
                Writer.beginObject();
                Writer.writeKey("name");
                Writer.writeString(Checkpoint.Name);
                Writer.writeKey("cat");
                Writer.writeString("benchmark");
                Writer.writeKey("ph");
                Writer.writeString("X");
                Writer.writeKey("ts");
                Writer.writeDouble(static_cast<double>(Checkpoint.StartNanoseconds) / 1000.0);
                Writer.writeKey("dur");
                Writer.writeDouble(static_cast<double>(Checkpoint.ElapsedNanoseconds) / 1000.0);
                Writer.writeKey("pid");
                Writer.writeInt(0);
                Writer.writeKey("tid");
                Writer.writeInt(0);
                Writer.endObject();
            }
        }

        Writer.endArray();
        Writer.writeKey("displayTimeUnit");
        Writer.writeString("ms");
        Writer.endObject();
        return Writer.toString();
    }

    /**
     * @brief 将 Chrome Trace 事件写入文件。
     * @param Path 输出路径。
     * @return 成功返回 0，失败返回 1。
     */
    int writeTrace(const std::string &Path) const {
        try {
            const std::string Text = serializeTrace();

            std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
            if (!Output) {
                throw std::runtime_error("cannot open output file: " + Path);
            }
            Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
            if (!Output) {
                throw std::runtime_error("failed to write output file: " + Path);
            }
            return 0;
        }
        catch (const std::exception &Exception) {
            std::cerr << "benchmark error: " << Exception.what() << "\n";
            return 1;
        }
    }

private:
    BenchmarkManagerBase() = default;

    ~BenchmarkManagerBase() = default;

    BenchmarkManagerBase(const BenchmarkManagerBase &) = delete;

    BenchmarkManagerBase &operator=(const BenchmarkManagerBase &) = delete;

    /**
     * @brief 查找或创建指定名称的累加器。
     * @param Name 段名。
     * @return 累加器引用。
     * @notice 调用方必须已持有锁。
     */
    Detail::SectionAccumulator &findOrCreate(const std::string &Name) {
        for (Detail::SectionAccumulator &Section: Sections) {
            if (Section.Name == Name) {
                return Section;
            }
        }

        Sections.push_back(Detail::SectionAccumulator());
        Sections.back().Name = Name;
        return Sections.back();
    }

    /**
     * @brief 结算当前帧。
     * @notice 调用方必须已持有锁。
     */
    void finalizeCurrentFrame() {
        for (Detail::SectionAccumulator &Section: Sections) {
            if (Section.FrameCallCount > 0) {
                ++Section.AppearedFrameCount;
                Section.FrameCallCountSum += Section.FrameCallCount;
                Section.FrameCallCountMin = (std::min)(Section.FrameCallCountMin, Section.FrameCallCount);
                Section.FrameCallCountMax = (std::max)(Section.FrameCallCountMax, Section.FrameCallCount);
                Section.FrameTimeSum      += Section.FrameTimeNanoseconds;
                Section.FrameTimeMin      = (std::min)(Section.FrameTimeMin, Section.FrameTimeNanoseconds);
                Section.FrameTimeMax      = (std::max)(Section.FrameTimeMax, Section.FrameTimeNanoseconds);
            }

            Section.FrameCallCount       = 0;
            Section.FrameTimeNanoseconds = 0;
        }

        ++TotalFrameCount;
    }

    /**
     * @brief 由内部累加器构建统计列表。
     * @return SectionStats 列表。
     * @notice 调用方必须已持有锁。
     */
    std::vector<SectionStats> buildStats() const {
        std::vector<SectionStats> Stats;
        Stats.reserve(Sections.size());

        for (const Detail::SectionAccumulator &Section: Sections) {
            SectionStats Item;
            Item.Name                 = Section.Name;
            Item.CallCount            = Section.CallCount;
            Item.TotalTimeNanoseconds = Section.TotalTimeNanoseconds;
            Item.MinTimeNanoseconds   = Section.CallCount > 0 ? Section.MinTimeNanoseconds : 0;
            Item.MaxTimeNanoseconds   = Section.MaxTimeNanoseconds;

            if (Section.CallCount > 0) {
                Item.MeanTimeNanoseconds =
                    static_cast<double>(Section.TotalTimeNanoseconds) / static_cast<double>(Section.CallCount);
                const double Variance =
                    Section.SumOfSquaresNanoseconds / static_cast<double>(Section.CallCount) -
                    Item.MeanTimeNanoseconds * Item.MeanTimeNanoseconds;
                Item.StddevTimeNanoseconds = Variance > 0.0 ? std::sqrt(Variance) : 0.0;
            }

            Item.AppearedFrameCount      = Section.AppearedFrameCount;
            Item.FrameCallCountMin       = Section.AppearedFrameCount > 0 ? Section.FrameCallCountMin : 0;
            Item.FrameCallCountMax       = Section.FrameCallCountMax;
            Item.FrameTimeNanosecondsMin = Section.AppearedFrameCount > 0 ? Section.FrameTimeMin : 0;
            Item.FrameTimeNanosecondsMax = Section.FrameTimeMax;

            if (Section.AppearedFrameCount > 0) {
                Item.FrameCallCountMean =
                    static_cast<double>(Section.FrameCallCountSum) /
                    static_cast<double>(Section.AppearedFrameCount);
                Item.FrameTimeNanosecondsMean =
                    static_cast<double>(Section.FrameTimeSum) /
                    static_cast<double>(Section.AppearedFrameCount);
            }

            Stats.push_back(std::move(Item));
        }

        return Stats;
    }

    /**
     * @brief 把单次测量 JSON 发往配置的 sink。
     * @param SinkKind 输出目标。
     * @param FilePath 文件路径。
     * @param Callback 回调。
     * @param Json JSON 字符串。
     * @notice 在未持有锁时调用，因此回调可安全地重入 manager。
     */
    static void emitMeasurement(EMeasurementSink SinkKind,
                                const std::string &FilePath,
                                const std::function<void(const std::string &)> &Callback,
                                const std::string &Json) {
        switch (SinkKind) {
            case EMeasurementSink::None:
                break;
            case EMeasurementSink::StdOut: {
                const std::string Line = Json + "\n";
                std::cout.write(Line.data(), static_cast<std::streamsize>(Line.size()));
                std::cout.flush();
                break;
            }
            case EMeasurementSink::StdErr: {
                const std::string Line = Json + "\n";
                std::cerr.write(Line.data(), static_cast<std::streamsize>(Line.size()));
                std::cerr.flush();
                break;
            }
            case EMeasurementSink::File: {
                std::ofstream Output(FilePath, std::ios::app | std::ios::binary);
                if (Output) {
                    Output << Json << '\n';
                }
                break;
            }
            case EMeasurementSink::Callback:
                if (Callback) {
                    Callback(Json);
                }
                break;
        }
    }

    mutable LockT Lock;
    std::vector<Detail::SectionAccumulator> Sections;
    std::int64_t TotalFrameCount = 0;
    bool HasOpenFrame            = false;

    ETimeUnit TimeUnit = ETimeUnit::Millisecond;

    EMeasurementSink MeasurementSink = EMeasurementSink::None;
    std::string MeasurementFilePath;
    std::function<void(const std::string &)> MeasurementCallback;
    std::vector<MeasurementRecord> MeasurementRecords;
    std::int64_t MeasurementSequence = 0;
    bool ShouldStoreMeasurements     = true;
};
} // namespace Detail

/**
 * @brief RAII 计时器，在离开作用域(或调用 stop)时记录该段的耗时。
 * @param ManagerT 决定把耗时记录到哪个 manager 单例。
 */
template<typename ManagerT>
class BenchmarkTimerBase {
public:
    /**
     * @brief 构造计时器并开始计时。
     * @param Name 段名。
     */
    explicit BenchmarkTimerBase(std::string Name)
        : Name(std::move(Name)),
          Start(Clock::now()),
          LastMark(Start),
          Checkpoints(),
          IsActive(true) {
    }

    /**
     * @brief 析构时若仍处于激活状态，则记录本次耗时。
     */
    ~BenchmarkTimerBase() {
        if (IsActive) {
            ManagerT::get().recordTime(
                                       Name,
                                       Detail::getNanosecondsBetween(Start, Detail::getTimePoint()),
                                       Detail::getEpochNanoseconds(Start),
                                       std::move(Checkpoints));
        }
    }

    BenchmarkTimerBase(BenchmarkTimerBase &&Other) noexcept
        : Name(std::move(Other.Name)),
          Start(Other.Start),
          LastMark(Other.LastMark),
          Checkpoints(std::move(Other.Checkpoints)),
          IsActive(Other.IsActive) {
        Other.IsActive = false;
    }

    BenchmarkTimerBase &operator=(BenchmarkTimerBase &&Other) noexcept {
        if (this != &Other) {
            if (IsActive) {
                ManagerT::get().recordTime(
                                           Name,
                                           Detail::getNanosecondsBetween(Start, Detail::getTimePoint()),
                                           Detail::getEpochNanoseconds(Start),
                                           std::move(Checkpoints));
            }

            Name           = std::move(Other.Name);
            Start          = Other.Start;
            LastMark       = Other.LastMark;
            Checkpoints    = std::move(Other.Checkpoints);
            IsActive       = Other.IsActive;
            Other.IsActive = false;
        }
        return *this;
    }

    BenchmarkTimerBase(const BenchmarkTimerBase &) = delete;

    BenchmarkTimerBase &operator=(const BenchmarkTimerBase &) = delete;

    /**
     * @brief 在计时作用域内记录一个命名打桩点。
     * @param CheckpointName 打桩点名。
     * @notice 耗时相对上一个打桩点(或计时起点)。
     */
    void mark(const std::string &CheckpointName) {
        if (!IsActive) {
            return;
        }

        const Clock::time_point Now = Clock::now();
        // @notice C++11 下带默认成员初始化器的类型不是聚合体, 不能用花括号聚合
        //         初始化, 因此先默认构造再逐字段赋值。
        CheckpointRecord Record;
        Record.Name               = CheckpointName;
        Record.StartNanoseconds   = Detail::getEpochNanoseconds(LastMark);
        Record.ElapsedNanoseconds = Detail::getNanosecondsBetween(LastMark, Now);
        Checkpoints.push_back(Record);
        LastMark = Now;
    }

    void stop() {
        if (IsActive) {
            ManagerT::get().recordTime(
                                       Name,
                                       Detail::getNanosecondsBetween(Start, Detail::getTimePoint()),
                                       Detail::getEpochNanoseconds(Start),
                                       std::move(Checkpoints));
            IsActive = false;
        }
    }

private:
    std::string Name;
    Clock::time_point Start;
    Clock::time_point LastMark;
    std::vector<CheckpointRecord> Checkpoints;
    bool IsActive;
};

namespace Detail{
/**
 * @brief 返回一个在作用域结束时记录的 RAII 计时器。
 * @param Name 段名。
 * @return RAII 计时器。
 */
template<typename LockT>
inline typename BenchmarkManagerBase<LockT>::TimerType
BenchmarkManagerBase<LockT>::measure(const std::string &Name) {
    return TimerType(Name);
}
} // namespace Detail

// @brief 具体单线程 manager：零同步开销。
using BenchmarkManager = Detail::BenchmarkManagerBase<Detail::NoOpLock>;

// @brief 具体线程安全 manager：每个操作由 std::mutex 保护。
using ThreadSafeBenchmarkManager = Detail::BenchmarkManagerBase<std::mutex>;

// @brief 对应的 RAII 计时器。
using BenchmarkTimer           = BenchmarkTimerBase<BenchmarkManager>;
using ThreadSafeBenchmarkTimer = BenchmarkTimerBase<ThreadSafeBenchmarkManager>;
} // namespace Bm

// @brief 宏拼接辅助。
#define BM_CONCAT_INNER(Left, Right) Left##Right
#define BM_CONCAT(Left, Right) BM_CONCAT_INNER(Left, Right)

// @brief 注册名为 "Function" 的基准函数。
// @notice 放在命名空间作用域，每行一个。
#define BENCHMARK(Function) \
    static ::Bm::Detail::BenchmarkRegistrar BM_CONCAT(BmBenchmarkRegistrar_, __LINE__)(#Function, &Function)

// @brief 以显式名字注册基准函数。
#define BENCHMARK_NAME(Name, Function) \
    static ::Bm::Detail::BenchmarkRegistrar BM_CONCAT(BmBenchmarkRegistrar_, __LINE__)(Name, &Function)

// @brief 测量所在作用域(单线程 manager)。
#define BM_SCOPED_TIMER(Name) \
    ::Bm::BenchmarkTimer BM_CONCAT(BmTimer_, __LINE__)(Name)

// @brief 测量所在作用域(线程安全 manager)。
#define BM_SCOPED_TIMER_THREAD_SAFE(Name) \
    ::Bm::ThreadSafeBenchmarkTimer BM_CONCAT(BmTimerTs_, __LINE__)(Name)

#endif // BM_BENCHMARK_HPP
