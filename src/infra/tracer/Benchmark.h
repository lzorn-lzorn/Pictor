// Benchmark.hpp
//
// 一个极简的 C++14 单头文件 benchmark / 性能分析工具. 
//
// 整个库只由这一个头文件组成, 仅依赖 C++ 标准库. 提供两种互补的模式: 
//
//   1. 迭代基准(Iteration benchmarks)—— 注册一个 void() 函数, 测量它运行 N 次的
//      耗时, 然后把结果序列化为 JSON. 
//
//   2. 按帧分析(Frame-aware profiling)—— 全局 manager 记录某段代码的耗时, 以及它
//      在每一帧里执行了多少次. 在帧首尾插入 beginFrame()/endFrame(), 用计时器或作用域
//      宏包裹代码段, 最后把累计统计写入 JSON. 
//
// 提供两种 manager, 各自是独立的单例: 
//
//   * BenchmarkManager             —— 单线程, 零加锁开销. 
//   * ThreadSafeBenchmarkManager   —— 线程安全, 内部用 std::mutex 保护. 
//
// 时间单位: 耗时始终在内部以纳秒累计, 仅在序列化时换算. 单位可选(默认 ms): 
// ETimeUnit::Hour / Minute / Second / Millisecond / Nanosecond. 生成的 JSON 还会附带
// "//" 注释说明每个字段(JSONC 风格), 可用相应开关关闭. 
//
// 单次测量输出槽(sink): 每次测量完成后, 还可立即把这次结果序列化为单行 JSON 并输出, 
// 这样不必等最后的 write 就能实时看到. 槽由 EMeasurementSink 选择
// (None / StdOut / StdErr / File / Callback)；累积的 JSON 保存在内存中, 可用
// writeMeasurementLog() 一次性写出. 
//
// 用法(迭代基准): 
//
//   #include "Benchmark.hpp"
//
//   static void benchmarkSqrt() { ... }
//   BENCHMARK(benchmarkSqrt);
//
//   int main()
//   {
//       Bm::BenchmarkOptions Options;
//       Options.TimeUnit = Bm::ETimeUnit::Millisecond; // 默认
//       return Bm::runAll("result.json", Options);
//   }
//
// 用法(单线程按帧分析): 
//
//   Bm::BenchmarkManager& Manager = Bm::BenchmarkManager::get();
//   while (Running)
//   {
//       Manager.beginFrame();
//       { Bm::BenchmarkTimer Timer("physics"); updatePhysics(); }
//       { Bm::BenchmarkTimer Timer("render");  updateRender();  }
//       Manager.endFrame();
//   }
//   Manager.writeJson("profile.json");
//
// 用法(线程安全按帧分析): 
//
//   Bm::ThreadSafeBenchmarkManager& Manager =
//       Bm::ThreadSafeBenchmarkManager::get();
//   // 任意线程现在都可以使用 ThreadSafeBenchmarkTimer / recordTime. 
//   { Bm::ThreadSafeBenchmarkTimer Timer("worker"); doWork(); }
//   Manager.writeJson("profile.json");
//
// 用法(单次测量输出槽): 
//
//   Bm::BenchmarkManager& Manager = Bm::BenchmarkManager::get();
//   Manager.setMeasurementSink(Bm::EMeasurementSink::StdOut);   // 实时可见
//   { Bm::BenchmarkTimer Timer("job"); doJob(); }               // 输出一行 JSON
//   Manager.writeMeasurementLog("measurements.json");           // 最后全部写出
//
// 用法(计时内打桩 / 记录点):
//
//   Bm::BenchmarkTimer Timer("job");
//   doA();
//   Timer.mark("afterA");   // 记录自起点到这里的耗时
//   doB();
//   Timer.mark("afterB");   // 记录自 afterA 到这里的耗时
//   doC();                  // 作用域结束时记录总耗时
//

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
namespace Bm
{

using Clock = std::chrono::high_resolution_clock;

// Time unit used when serializing durations to JSON. Durations are always
// accumulated internally in nanoseconds and converted only at output time.
enum class ETimeUnit
{
    Hour,
    Minute,
    Second,
    Millisecond,
    Nanosecond,
};

// ---------------------------------------------------------------------------
// Iteration-benchmark mode
// ---------------------------------------------------------------------------

// Tunables shared by every benchmark run.
struct BenchmarkOptions
{
    // Number of iterations to run; 0 (default) auto-calibrates so the total
    // measured time approaches TargetTotalNanoseconds.
    std::int64_t Iterations = 0;

    // Bounds applied when auto-calibrating.
    std::int64_t MinIterations = 1;
    std::int64_t MaxIterations = 1000000000LL;

    // Warm-up invocations performed before measuring.
    std::int64_t WarmupIterations = 1;

    // Target total measured time (nanoseconds) used by auto-calibration.
    std::int64_t TargetTotalNanoseconds = 10000000LL; // 10 ms

    // When true, every iteration is timed separately so min/max/median/stddev
    // are reported as well (this adds per-iteration clock overhead).
    bool ShouldMeasureIndividualTimes = false;

    // Unit used when serializing time values to JSON.
    ETimeUnit TimeUnit = ETimeUnit::Millisecond;

    // When true, "//" comments explaining each field are emitted into the JSON.
    bool ShouldEmitJsonComments = true;
};

// A single benchmark's measurement.
struct BenchmarkResult
{
    std::string Name;
    std::int64_t Iterations = 0;
    std::int64_t TotalTimeNanoseconds = 0;
    double MeanTimeNanoseconds = 0.0;
    double ThroughputOpsPerSecond = 0.0;

    // Populated only when ShouldMeasureIndividualTimes was enabled.
    bool HasDistribution = false;
    double MinTimeNanoseconds = 0.0;
    double MaxTimeNanoseconds = 0.0;
    double MedianTimeNanoseconds = 0.0;
    double StddevTimeNanoseconds = 0.0;
};

// Forward declaration used by Detail::BenchmarkRegistrar.
void registerBenchmark(std::string Name, std::function<void()> Function);

namespace Detail
{

struct BenchmarkEntry
{
    std::string Name;
    std::function<void()> Function;
};

// Function-local static: safely initialized on first use regardless of the
// order in which translation units run their static initializers.
inline std::vector<BenchmarkEntry>& getRegistry()
{
    static std::vector<BenchmarkEntry> Registry;
    return Registry;
}

inline Clock::time_point getTimePoint()
{
    return Clock::now();
}

inline std::int64_t getNanosecondsBetween(Clock::time_point Start, Clock::time_point End)
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(End - Start).count();
}

// Fixed-point formatting with trailing zeros removed, for compact JSON.
inline std::string formatDouble(double Value)
{
    if (Value == 0.0)
    {
        return "0";
    }

    std::ostringstream Stream;
    Stream.imbue(std::locale::classic());
    Stream << std::fixed << std::setprecision(9) << Value;
    std::string Text = Stream.str();

    if (Text.find('.') != std::string::npos)
    {
        while (!Text.empty() && Text.back() == '0')
        {
            Text.pop_back();
        }
        if (!Text.empty() && Text.back() == '.')
        {
            Text.pop_back();
        }
    }
    return Text;
}

// Minimal JSON string escaping.
inline std::string escapeJson(const std::string& Value)
{
    std::string Result;
    Result.reserve(Value.size());

    for (char Character : Value)
    {
        switch (Character)
        {
            case '"':  Result += "\\\""; break;
            case '\\': Result += "\\\\"; break;
            case '\b': Result += "\\b"; break;
            case '\f': Result += "\\f"; break;
            case '\n': Result += "\\n"; break;
            case '\r': Result += "\\r"; break;
            case '\t': Result += "\\t"; break;
            default:
                if (static_cast<unsigned char>(Character) < 0x20)
                {
                    std::ostringstream Stream;
                    Stream << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                           << static_cast<unsigned int>(static_cast<unsigned char>(Character));
                    Result += Stream.str();
                }
                else
                {
                    Result += Character;
                }
        }
    }

    return Result;
}

// ---- Time-unit helpers ----------------------------------------------------

inline const char* getTimeUnitName(ETimeUnit Unit)
{
    switch (Unit)
    {
        case ETimeUnit::Hour:        return "hour";
        case ETimeUnit::Minute:      return "min";
        case ETimeUnit::Second:      return "second";
        case ETimeUnit::Millisecond: return "ms";
        case ETimeUnit::Nanosecond:  return "ns";
    }
    return "ms";
}

// Number of nanoseconds in one unit.
inline double getNanosecondsPerUnit(ETimeUnit Unit)
{
    switch (Unit)
    {
        case ETimeUnit::Hour:        return 3.6e12;
        case ETimeUnit::Minute:      return 6.0e10;
        case ETimeUnit::Second:      return 1.0e9;
        case ETimeUnit::Millisecond: return 1.0e6;
        case ETimeUnit::Nanosecond:  return 1.0;
    }
    return 1.0e6;
}

inline double convertNanoseconds(std::int64_t Nanoseconds, ETimeUnit Unit)
{
    return static_cast<double>(Nanoseconds) / getNanosecondsPerUnit(Unit);
}

inline double convertNanoseconds(double Nanoseconds, ETimeUnit Unit)
{
    return Nanoseconds / getNanosecondsPerUnit(Unit);
}

inline std::string formatNanoseconds(std::int64_t Nanoseconds, ETimeUnit Unit)
{
    return formatDouble(convertNanoseconds(Nanoseconds, Unit));
}

inline std::string formatNanoseconds(double Nanoseconds, ETimeUnit Unit)
{
    return formatDouble(convertNanoseconds(Nanoseconds, Unit));
}

// Constructed by the BENCHMARK / BENCHMARK_NAME macros at static-init time.
struct BenchmarkRegistrar
{
    BenchmarkRegistrar(std::string Name, std::function<void()> Function)
    {
        registerBenchmark(std::move(Name), std::move(Function));
    }
};

} // namespace Detail

// Registers a benchmark under a unique name. Duplicate names are ignored, so a
// benchmark header included in several translation units registers only once.
inline void registerBenchmark(std::string Name, std::function<void()> Function)
{
    std::vector<Detail::BenchmarkEntry>& Registry = Detail::getRegistry();

    for (const Detail::BenchmarkEntry& Entry : Registry)
    {
        if (Entry.Name == Name)
        {
            return;
        }
    }

    Registry.push_back(Detail::BenchmarkEntry{std::move(Name), std::move(Function)});
}

inline BenchmarkResult runOne(const Detail::BenchmarkEntry& Entry, const BenchmarkOptions& Options)
{
    BenchmarkResult Result;
    Result.Name = Entry.Name;

    // Warm up (typically warms caches / branch predictors).
    for (std::int64_t Index = 0; Index < Options.WarmupIterations; ++Index)
    {
        Entry.Function();
    }

    // Resolve the iteration count, auto-calibrating when requested.
    std::int64_t Iterations = Options.Iterations;
    if (Iterations <= 0)
    {
        const Clock::time_point CalibrationStart = Detail::getTimePoint();
        Entry.Function();
        const Clock::time_point CalibrationEnd = Detail::getTimePoint();

        std::int64_t SingleCallNanoseconds = Detail::getNanosecondsBetween(CalibrationStart, CalibrationEnd);
        if (SingleCallNanoseconds <= 0)
        {
            SingleCallNanoseconds = 1;
        }

        Iterations = Options.TargetTotalNanoseconds / SingleCallNanoseconds;
        Iterations = (std::max)(Iterations, Options.MinIterations);
        Iterations = (std::min)(Iterations, Options.MaxIterations);
    }
    if (Iterations <= 0)
    {
        Iterations = 1;
    }
    Result.Iterations = Iterations;

    std::vector<double> PerIterationTimes;
    if (Options.ShouldMeasureIndividualTimes)
    {
        PerIterationTimes.reserve(
            static_cast<std::size_t>((std::min)(Iterations, static_cast<std::int64_t>(1LL << 20))));
    }

    const Clock::time_point Start = Detail::getTimePoint();
    for (std::int64_t Index = 0; Index < Iterations; ++Index)
    {
        if (Options.ShouldMeasureIndividualTimes)
        {
            const Clock::time_point IterationStart = Detail::getTimePoint();
            Entry.Function();
            const Clock::time_point IterationEnd = Detail::getTimePoint();
            PerIterationTimes.push_back(
                static_cast<double>(Detail::getNanosecondsBetween(IterationStart, IterationEnd)));
        }
        else
        {
            Entry.Function();
        }
    }
    const Clock::time_point End = Detail::getTimePoint();

    Result.TotalTimeNanoseconds = Detail::getNanosecondsBetween(Start, End);
    const double TotalSeconds = static_cast<double>(Result.TotalTimeNanoseconds) / 1e9;
    Result.MeanTimeNanoseconds =
        static_cast<double>(Result.TotalTimeNanoseconds) / static_cast<double>(Iterations);
    Result.ThroughputOpsPerSecond =
        TotalSeconds > 0.0 ? static_cast<double>(Iterations) / TotalSeconds : 0.0;

    if (Options.ShouldMeasureIndividualTimes && !PerIterationTimes.empty())
    {
        std::sort(PerIterationTimes.begin(), PerIterationTimes.end());

        Result.HasDistribution = true;
        Result.MinTimeNanoseconds = PerIterationTimes.front();
        Result.MaxTimeNanoseconds = PerIterationTimes.back();

        const std::size_t Count = PerIterationTimes.size();
        if (Count % 2 == 1)
        {
            Result.MedianTimeNanoseconds = PerIterationTimes[Count / 2];
        }
        else
        {
            Result.MedianTimeNanoseconds =
                (PerIterationTimes[Count / 2 - 1] + PerIterationTimes[Count / 2]) / 2.0;
        }

        double Sum = 0.0;
        for (double Time : PerIterationTimes)
        {
            Sum += Time;
        }
        const double Mean = Sum / static_cast<double>(Count);

        double SquaredSum = 0.0;
        for (double Time : PerIterationTimes)
        {
            const double Difference = Time - Mean;
            SquaredSum += Difference * Difference;
        }
        Result.StddevTimeNanoseconds = std::sqrt(SquaredSum / static_cast<double>(Count));
    }

    return Result;
}

// Runs a single registered benchmark by name.
inline BenchmarkResult runOne(const std::string& Name, const BenchmarkOptions& Options = BenchmarkOptions())
{
    const Detail::BenchmarkEntry* Found = nullptr;
    for (const Detail::BenchmarkEntry& Entry : Detail::getRegistry())
    {
        if (Entry.Name == Name)
        {
            Found = &Entry;
            break;
        }
    }

    if (Found == nullptr)
    {
        throw std::runtime_error("benchmark not found: " + Name);
    }

    return runOne(*Found, Options);
}

// Runs every registered benchmark, returning the results in registration order.
inline std::vector<BenchmarkResult> runAll(const BenchmarkOptions& Options = BenchmarkOptions())
{
    std::vector<BenchmarkResult> Results;
    for (const Detail::BenchmarkEntry& Entry : Detail::getRegistry())
    {
        Results.push_back(runOne(Entry, Options));
    }
    return Results;
}

// Serializes iteration-benchmark results to a JSON string (with field comments
// when Options.ShouldEmitJsonComments is true).
inline std::string serializeJson(const std::vector<BenchmarkResult>& Results,
                                 const BenchmarkOptions& Options)
{
    const bool WithComments = Options.ShouldEmitJsonComments;
    const std::string UnitName = Detail::getTimeUnitName(Options.TimeUnit);

    std::string Text;
    Text += "{\n";
    Text += "  \"time_unit\": \"" + UnitName + "\",";
    if (WithComments) Text += "  // time unit: hour / min / second / ms / ns";
    Text += "\n";
    Text += "  \"benchmarks\": [";
    if (WithComments) Text += "  // all benchmark results";
    Text += "\n";

    if (Results.empty())
    {
        Text += "  ]\n";
    }
    else
    {
        for (std::size_t Index = 0; Index < Results.size(); ++Index)
        {
            const BenchmarkResult& Result = Results[Index];

            Text += "    {\n";
            Text += "      \"name\": \"" + Detail::escapeJson(Result.Name) + "\",";
            if (WithComments) Text += "  // benchmark name";
            Text += "\n";

            Text += "      \"iterations\": " + std::to_string(Result.Iterations) + ",";
            if (WithComments) Text += "  // iteration count";
            Text += "\n";

            Text += "      \"total_time\": " + Detail::formatNanoseconds(Result.TotalTimeNanoseconds, Options.TimeUnit) + ",";
            if (WithComments) Text += "  // total time (unit in time_unit)";
            Text += "\n";

            Text += "      \"mean_time\": " + Detail::formatNanoseconds(Result.MeanTimeNanoseconds, Options.TimeUnit) + ",";
            if (WithComments) Text += "  // mean time per call";
            Text += "\n";

            if (Result.HasDistribution)
            {
                Text += "      \"min_time\": " + Detail::formatNanoseconds(Result.MinTimeNanoseconds, Options.TimeUnit) + ",";
                if (WithComments) Text += "  // min time per call";
                Text += "\n";

                Text += "      \"max_time\": " + Detail::formatNanoseconds(Result.MaxTimeNanoseconds, Options.TimeUnit) + ",";
                if (WithComments) Text += "  // max time per call";
                Text += "\n";

                Text += "      \"median_time\": " + Detail::formatNanoseconds(Result.MedianTimeNanoseconds, Options.TimeUnit) + ",";
                if (WithComments) Text += "  // median time per call";
                Text += "\n";

                Text += "      \"stddev_time\": " + Detail::formatNanoseconds(Result.StddevTimeNanoseconds, Options.TimeUnit) + ",";
                if (WithComments) Text += "  // stddev time per call";
                Text += "\n";
            }

            Text += "      \"throughput_ops_per_second\": " + Detail::formatDouble(Result.ThroughputOpsPerSecond);
            if (WithComments) Text += "  // throughput (calls per second)";
            Text += "\n";

            Text += "    }";
            if (Index + 1 < Results.size())
            {
                Text += ",";
            }
            Text += "\n";
        }
        Text += "  ]\n";
    }

    Text += "}\n";
    return Text;
}

// Writes iteration-benchmark results as JSON to the given path (overwrites).
inline void writeJson(const std::string& Path, const std::vector<BenchmarkResult>& Results,
                      const BenchmarkOptions& Options = BenchmarkOptions())
{
    std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
    if (!Output)
    {
        throw std::runtime_error("cannot open output file: " + Path);
    }

    const std::string Text = serializeJson(Results, Options);
    Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
    if (!Output)
    {
        throw std::runtime_error("failed to write output file: " + Path);
    }
}

// Convenience: runs all benchmarks and writes the JSON to JsonPath.
// Returns 0 on success, 1 on failure.
inline int runAll(const std::string& JsonPath, const BenchmarkOptions& Options = BenchmarkOptions())
{
    try
    {
        writeJson(JsonPath, runAll(Options), Options);
        return 0;
    }
    catch (const std::exception& Exception)
    {
        std::cerr << "benchmark error: " << Exception.what() << "\n";
        return 1;
    }
}

// ---------------------------------------------------------------------------
// Frame-aware profiling mode
// ---------------------------------------------------------------------------

// Aggregated statistics for one named code section, exposed to callers.
struct SectionStats
{
    std::string Name;

    // Across every recorded call.
    std::int64_t CallCount = 0;
    std::int64_t TotalTimeNanoseconds = 0;
    std::int64_t MinTimeNanoseconds = 0;
    std::int64_t MaxTimeNanoseconds = 0;
    double MeanTimeNanoseconds = 0.0;
    double StddevTimeNanoseconds = 0.0;

    // Across the frames in which the section appeared.
    std::int64_t AppearedFrameCount = 0;
    std::int64_t FrameCallCountMin = 0;
    std::int64_t FrameCallCountMax = 0;
    double FrameCallCountMean = 0.0;
    std::int64_t FrameTimeNanosecondsMin = 0;
    std::int64_t FrameTimeNanosecondsMax = 0;
    double FrameTimeNanosecondsMean = 0.0;
};

// Where a completed measurement's JSON is emitted immediately.
enum class EMeasurementSink
{
    None,      // no immediate output (measurements are only accumulated)
    StdOut,    // write each measurement JSON to stdout
    StdErr,    // write each measurement JSON to stderr
    File,      // append each measurement JSON to the configured file
    Callback,  // forward each measurement JSON to the configured callback
};

// A single checkpoint ("stub") recorded inside a timed scope. ElapsedNanoseconds
// is the time since the previous checkpoint (or since the timer started).
struct CheckpointRecord
{
    std::string Name;
    std::int64_t ElapsedNanoseconds = 0;
};

// A single completed measurement, serialized to JSON and emitted immediately.
struct MeasurementRecord
{
    std::string Name;
    std::int64_t ElapsedNanoseconds = 0;
    std::int64_t Sequence = 0;        // global order across all measurements
    std::int64_t FrameIndex = 0;      // 1-based current frame; 0 when no frame is open
    std::int64_t FrameCallIndex = 0;  // 1-based call index within the frame; 0 when no frame
    std::vector<CheckpointRecord> Checkpoints; // checkpoints recorded inside the timed scope
};

namespace Detail
{

// Internal per-section accumulator kept by the manager.
struct SectionAccumulator
{
    std::string Name;

    std::int64_t CallCount = 0;
    std::int64_t TotalTimeNanoseconds = 0;
    std::int64_t MinTimeNanoseconds = std::numeric_limits<std::int64_t>::max();
    std::int64_t MaxTimeNanoseconds = 0;
    double SumOfSquaresNanoseconds = 0.0;

    // Current (open) frame counters.
    std::int64_t FrameCallCount = 0;
    std::int64_t FrameTimeNanoseconds = 0;

    // Aggregation over frames the section appeared in.
    std::int64_t AppearedFrameCount = 0;
    std::int64_t FrameCallCountSum = 0;
    std::int64_t FrameCallCountMin = std::numeric_limits<std::int64_t>::max();
    std::int64_t FrameCallCountMax = 0;
    std::int64_t FrameTimeSum = 0;
    std::int64_t FrameTimeMin = std::numeric_limits<std::int64_t>::max();
    std::int64_t FrameTimeMax = 0;
};

// Lock policy used by the single-threaded manager: a no-op that the optimizer
// removes entirely, so the single-threaded version pays no synchronization cost.
struct NoOpLock
{
    void lock() {}
    void unlock() {}
};

// Serializes a single measurement to a compact single-line JSON object.
inline std::string serializeMeasurement(const MeasurementRecord& Record, ETimeUnit Unit)
{
    const std::string UnitName = getTimeUnitName(Unit);

    std::string Text;
    Text += "{\"name\":\"" + escapeJson(Record.Name) + "\"";
    Text += ",\"elapsed\":" + formatNanoseconds(Record.ElapsedNanoseconds, Unit);
    Text += ",\"unit\":\"" + UnitName + "\"";
    Text += ",\"sequence\":" + std::to_string(Record.Sequence);
    Text += ",\"frame_index\":" + std::to_string(Record.FrameIndex);
    Text += ",\"frame_call_index\":" + std::to_string(Record.FrameCallIndex);
    if (!Record.Checkpoints.empty())
    {
        Text += ",\"checkpoints\":[";
        for (std::size_t Index = 0; Index < Record.Checkpoints.size(); ++Index)
        {
            const CheckpointRecord& Checkpoint = Record.Checkpoints[Index];
            Text += "{\"name\":\"" + escapeJson(Checkpoint.Name) + "\"";
            Text += ",\"elapsed\":" + formatNanoseconds(Checkpoint.ElapsedNanoseconds, Unit);
            Text += "}";
            if (Index + 1 < Record.Checkpoints.size())
            {
                Text += ",";
            }
        }
        Text += "]";
    }
    Text += "}";
    return Text;
}

} // namespace Detail

template <typename ManagerT>
class BenchmarkTimerBase;

namespace Detail
{

// Shared implementation behind both managers. LockT selects the synchronization
// policy: Detail::NoOpLock for the single-threaded version, std::mutex for the
// thread-safe version. Both are separate types with separate singletons.
template <typename LockT>
class BenchmarkManagerBase
{
public:
    using TimerType = BenchmarkTimerBase<BenchmarkManagerBase<LockT>>;

    static BenchmarkManagerBase& get()
    {
        static BenchmarkManagerBase Instance;
        return Instance;
    }

    // Records one call of a named section taking Nanoseconds. Also emits the
    // completed measurement to the configured sink (see setMeasurementSink).
    void recordTime(const std::string& Name, std::int64_t Nanoseconds)
    {
        recordTime(Name, Nanoseconds, std::vector<CheckpointRecord>());
    }

    // Same as above, but also attaches the checkpoints recorded inside the
    // timed scope (each checkpoint's elapsed time is relative to the previous
    // checkpoint or to the timer start).
    void recordTime(const std::string& Name, std::int64_t Nanoseconds,
                    std::vector<CheckpointRecord> Checkpoints)
    {
        if (Nanoseconds < 0)
        {
            Nanoseconds = 0;
        }

        std::string Json;
        bool HasJson = false;
        EMeasurementSink SinkKind = EMeasurementSink::None;
        std::string FilePath;
        std::function<void(const std::string&)> Callback;

        {
            std::lock_guard<LockT> Guard(Lock);

            Detail::SectionAccumulator& Section = findOrCreate(Name);
            ++Section.CallCount;
            Section.TotalTimeNanoseconds += Nanoseconds;
            Section.MinTimeNanoseconds = (std::min)(Section.MinTimeNanoseconds, Nanoseconds);
            Section.MaxTimeNanoseconds = (std::max)(Section.MaxTimeNanoseconds, Nanoseconds);
            Section.SumOfSquaresNanoseconds +=
                static_cast<double>(Nanoseconds) * static_cast<double>(Nanoseconds);

            ++Section.FrameCallCount;
            Section.FrameTimeNanoseconds += Nanoseconds;

            HasJson = ShouldStoreMeasurements || MeasurementSink != EMeasurementSink::None;
            if (HasJson)
            {
                MeasurementRecord Record;
                Record.Name = Name;
                Record.ElapsedNanoseconds = Nanoseconds;
                Record.Sequence = ++MeasurementSequence;
                Record.FrameIndex = HasOpenFrame ? TotalFrameCount + 1 : 0;
                Record.FrameCallIndex = Section.FrameCallCount;
                Record.Checkpoints = std::move(Checkpoints);
                Json = serializeMeasurement(Record, TimeUnit);

                if (ShouldStoreMeasurements)
                {
                    MeasurementLog.push_back(Json);
                }
            }

            SinkKind = MeasurementSink;
            if (SinkKind == EMeasurementSink::File)
            {
                FilePath = MeasurementFilePath;
            }
            else if (SinkKind == EMeasurementSink::Callback)
            {
                Callback = MeasurementCallback;
            }
        }

        if (HasJson)
        {
            emitMeasurement(SinkKind, FilePath, Callback, Json);
        }
    }

    // Marks the start of a new frame. If a frame was already open it is
    // finalized first.
    void beginFrame()
    {
        std::lock_guard<LockT> Guard(Lock);

        if (HasOpenFrame)
        {
            finalizeCurrentFrame();
        }
        else
        {
            // Discard stray counters recorded before any frame opened.
            for (Detail::SectionAccumulator& Section : Sections)
            {
                Section.FrameCallCount = 0;
                Section.FrameTimeNanoseconds = 0;
            }
        }

        HasOpenFrame = true;
    }

    // Marks the end of the current frame and folds its counters into the
    // per-frame statistics.
    void endFrame()
    {
        std::lock_guard<LockT> Guard(Lock);

        if (!HasOpenFrame)
        {
            return;
        }

        finalizeCurrentFrame();
        HasOpenFrame = false;
    }

    // Returns an RAII timer that records the section on scope exit.
    TimerType measure(const std::string& Name);

    // Aggregated statistics in first-seen order.
    std::vector<SectionStats> getStats() const
    {
        std::lock_guard<LockT> Guard(Lock);
        return buildStats();
    }

    std::int64_t getTotalFrameCount() const
    {
        std::lock_guard<LockT> Guard(Lock);
        return TotalFrameCount;
    }

    // Clears all accumulated data (statistics and the measurement log).
    void reset()
    {
        std::lock_guard<LockT> Guard(Lock);
        Sections.clear();
        TotalFrameCount = 0;
        HasOpenFrame = false;
        MeasurementLog.clear();
        MeasurementSequence = 0;
    }

    // Sets the unit used when serializing time values to JSON.
    void setTimeUnit(ETimeUnit Unit)
    {
        std::lock_guard<LockT> Guard(Lock);
        TimeUnit = Unit;
    }

    ETimeUnit getTimeUnit() const
    {
        std::lock_guard<LockT> Guard(Lock);
        return TimeUnit;
    }

    // Controls whether "//" field comments are emitted into the JSON.
    void setShouldEmitJsonComments(bool ShouldEmit)
    {
        std::lock_guard<LockT> Guard(Lock);
        ShouldEmitJsonComments = ShouldEmit;
    }

    // Sets where each completed measurement's JSON is emitted immediately.
    void setMeasurementSink(EMeasurementSink SinkKind)
    {
        std::lock_guard<LockT> Guard(Lock);
        MeasurementSink = SinkKind;
    }

    // Sets the file used by EMeasurementSink::File (appended per measurement).
    void setMeasurementFile(const std::string& Path)
    {
        std::lock_guard<LockT> Guard(Lock);
        MeasurementFilePath = Path;
    }

    // Sets the callback used by EMeasurementSink::Callback.
    void setMeasurementCallback(std::function<void(const std::string&)> Callback)
    {
        std::lock_guard<LockT> Guard(Lock);
        MeasurementCallback = std::move(Callback);
    }

    // Controls whether completed measurements are accumulated in memory.
    void setShouldStoreMeasurements(bool ShouldStore)
    {
        std::lock_guard<LockT> Guard(Lock);
        ShouldStoreMeasurements = ShouldStore;
    }

    // Returns a copy of the accumulated per-measurement JSON strings.
    std::vector<std::string> getMeasurementLog() const
    {
        std::lock_guard<LockT> Guard(Lock);
        return MeasurementLog;
    }

    // Clears the accumulated per-measurement JSON strings (keeps the sink config).
    void clearMeasurementLog()
    {
        std::lock_guard<LockT> Guard(Lock);
        MeasurementLog.clear();
    }

    // Writes the accumulated per-measurement JSON strings to Path, one per
    // line (JSON Lines). Returns 0 on success, 1 on failure.
    int writeMeasurementLog(const std::string& Path) const
    {
        std::vector<std::string> Copy;
        bool WithComments = false;
        {
            std::lock_guard<LockT> Guard(Lock);
            Copy = MeasurementLog;
            WithComments = ShouldEmitJsonComments;
        }

        try
        {
            std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
            if (!Output)
            {
                throw std::runtime_error("cannot open output file: " + Path);
            }
            if (WithComments)
            {
                Output << "// name=section elapsed=time(in unit) unit=time unit sequence=global index"
                       << " frame_index=frame(0=none) frame_call_index=call index in frame(0=none)"
                       << " checkpoints=[{name,elapsed}] (elapsed=since previous checkpoint/start)\n";
            }
            for (const std::string& Json : Copy)
            {
                Output << Json << '\n';
            }
            if (!Output)
            {
                throw std::runtime_error("failed to write output file: " + Path);
            }
            return 0;
        }
        catch (const std::exception& Exception)
        {
            std::cerr << "benchmark error: " << Exception.what() << "\n";
            return 1;
        }
    }

    // Serializes the aggregated statistics to a JSON string (with field
    // comments when ShouldEmitJsonComments is true).
    std::string serializeJson() const
    {
        std::lock_guard<LockT> Guard(Lock);

        const bool WithComments = ShouldEmitJsonComments;
        const std::string UnitName = Detail::getTimeUnitName(TimeUnit);
        const std::vector<SectionStats> Stats = buildStats();

        std::string Text;
        Text += "{\n";
        Text += "  \"time_unit\": \"" + UnitName + "\",";
        if (WithComments) Text += "  // time unit: hour / min / second / ms / ns";
        Text += "\n";
        Text += "  \"frame_count\": " + std::to_string(TotalFrameCount) + ",";
        if (WithComments) Text += "  // total frames counted";
        Text += "\n";
        Text += "  \"sections\": [";
        if (WithComments) Text += "  // per-section statistics";
        Text += "\n";

        if (Stats.empty())
        {
            Text += "  ]\n";
        }
        else
        {
            for (std::size_t Index = 0; Index < Stats.size(); ++Index)
            {
                const SectionStats& Section = Stats[Index];

                Text += "    {\n";
                Text += "      \"name\": \"" + Detail::escapeJson(Section.Name) + "\",";
                if (WithComments) Text += "  // section name";
                Text += "\n";

                Text += "      \"call_count\": " + std::to_string(Section.CallCount) + ",";
                if (WithComments) Text += "  // total call count";
                Text += "\n";

                Text += "      \"total_time\": " + Detail::formatNanoseconds(Section.TotalTimeNanoseconds, TimeUnit) + ",";
                if (WithComments) Text += "  // total time (unit in time_unit)";
                Text += "\n";

                Text += "      \"min_time\": " + Detail::formatNanoseconds(Section.MinTimeNanoseconds, TimeUnit) + ",";
                if (WithComments) Text += "  // min time per call";
                Text += "\n";

                Text += "      \"max_time\": " + Detail::formatNanoseconds(Section.MaxTimeNanoseconds, TimeUnit) + ",";
                if (WithComments) Text += "  // max time per call";
                Text += "\n";

                Text += "      \"mean_time\": " + Detail::formatNanoseconds(Section.MeanTimeNanoseconds, TimeUnit) + ",";
                if (WithComments) Text += "  // mean time per call";
                Text += "\n";

                Text += "      \"stddev_time\": " + Detail::formatNanoseconds(Section.StddevTimeNanoseconds, TimeUnit) + ",";
                if (WithComments) Text += "  // stddev time per call";
                Text += "\n";

                Text += "      \"frame_count\": " + std::to_string(Section.AppearedFrameCount) + ",";
                if (WithComments) Text += "  // frames this section appeared in";
                Text += "\n";

                Text += "      \"frame_call_count_min\": " + std::to_string(Section.FrameCallCountMin) + ",";
                if (WithComments) Text += "  // min calls per frame";
                Text += "\n";

                Text += "      \"frame_call_count_max\": " + std::to_string(Section.FrameCallCountMax) + ",";
                if (WithComments) Text += "  // max calls per frame";
                Text += "\n";

                Text += "      \"frame_call_count_mean\": " + Detail::formatDouble(Section.FrameCallCountMean) + ",";
                if (WithComments) Text += "  // mean calls per frame";
                Text += "\n";

                Text += "      \"frame_time_min\": " + Detail::formatNanoseconds(Section.FrameTimeNanosecondsMin, TimeUnit) + ",";
                if (WithComments) Text += "  // min time per frame";
                Text += "\n";

                Text += "      \"frame_time_max\": " + Detail::formatNanoseconds(Section.FrameTimeNanosecondsMax, TimeUnit) + ",";
                if (WithComments) Text += "  // max time per frame";
                Text += "\n";

                Text += "      \"frame_time_mean\": " + Detail::formatNanoseconds(Section.FrameTimeNanosecondsMean, TimeUnit);
                if (WithComments) Text += "  // mean time per frame";
                Text += "\n";

                Text += "    }";
                if (Index + 1 < Stats.size())
                {
                    Text += ",";
                }
                Text += "\n";
            }
            Text += "  ]\n";
        }

        Text += "}\n";
        return Text;
    }

    // Writes the aggregated statistics as JSON to the given path (overwrites).
    // Returns 0 on success, 1 on failure.
    int writeJson(const std::string& Path) const
    {
        try
        {
            const std::string Text = serializeJson();

            std::ofstream Output(Path, std::ios::binary | std::ios::trunc);
            if (!Output)
            {
                throw std::runtime_error("cannot open output file: " + Path);
            }
            Output.write(Text.data(), static_cast<std::streamsize>(Text.size()));
            if (!Output)
            {
                throw std::runtime_error("failed to write output file: " + Path);
            }
            return 0;
        }
        catch (const std::exception& Exception)
        {
            std::cerr << "benchmark error: " << Exception.what() << "\n";
            return 1;
        }
    }

private:
    BenchmarkManagerBase() = default;
    ~BenchmarkManagerBase() = default;
    BenchmarkManagerBase(const BenchmarkManagerBase&) = delete;
    BenchmarkManagerBase& operator=(const BenchmarkManagerBase&) = delete;

    // The caller must hold the lock.
    Detail::SectionAccumulator& findOrCreate(const std::string& Name)
    {
        for (Detail::SectionAccumulator& Section : Sections)
        {
            if (Section.Name == Name)
            {
                return Section;
            }
        }

        Sections.push_back(Detail::SectionAccumulator());
        Sections.back().Name = Name;
        return Sections.back();
    }

    // The caller must hold the lock.
    void finalizeCurrentFrame()
    {
        for (Detail::SectionAccumulator& Section : Sections)
        {
            if (Section.FrameCallCount > 0)
            {
                ++Section.AppearedFrameCount;
                Section.FrameCallCountSum += Section.FrameCallCount;
                Section.FrameCallCountMin = (std::min)(Section.FrameCallCountMin, Section.FrameCallCount);
                Section.FrameCallCountMax = (std::max)(Section.FrameCallCountMax, Section.FrameCallCount);
                Section.FrameTimeSum += Section.FrameTimeNanoseconds;
                Section.FrameTimeMin = (std::min)(Section.FrameTimeMin, Section.FrameTimeNanoseconds);
                Section.FrameTimeMax = (std::max)(Section.FrameTimeMax, Section.FrameTimeNanoseconds);
            }

            Section.FrameCallCount = 0;
            Section.FrameTimeNanoseconds = 0;
        }

        ++TotalFrameCount;
    }

    // The caller must hold the lock.
    std::vector<SectionStats> buildStats() const
    {
        std::vector<SectionStats> Stats;
        Stats.reserve(Sections.size());

        for (const Detail::SectionAccumulator& Section : Sections)
        {
            SectionStats Item;
            Item.Name = Section.Name;
            Item.CallCount = Section.CallCount;
            Item.TotalTimeNanoseconds = Section.TotalTimeNanoseconds;
            Item.MinTimeNanoseconds = Section.CallCount > 0 ? Section.MinTimeNanoseconds : 0;
            Item.MaxTimeNanoseconds = Section.MaxTimeNanoseconds;

            if (Section.CallCount > 0)
            {
                Item.MeanTimeNanoseconds =
                    static_cast<double>(Section.TotalTimeNanoseconds) / static_cast<double>(Section.CallCount);
                const double Variance =
                    Section.SumOfSquaresNanoseconds / static_cast<double>(Section.CallCount) -
                    Item.MeanTimeNanoseconds * Item.MeanTimeNanoseconds;
                Item.StddevTimeNanoseconds = Variance > 0.0 ? std::sqrt(Variance) : 0.0;
            }

            Item.AppearedFrameCount = Section.AppearedFrameCount;
            Item.FrameCallCountMin = Section.AppearedFrameCount > 0 ? Section.FrameCallCountMin : 0;
            Item.FrameCallCountMax = Section.FrameCallCountMax;
            Item.FrameTimeNanosecondsMin = Section.AppearedFrameCount > 0 ? Section.FrameTimeMin : 0;
            Item.FrameTimeNanosecondsMax = Section.FrameTimeMax;

            if (Section.AppearedFrameCount > 0)
            {
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

    // Emits a single measurement JSON to the configured sink. Called without
    // holding the lock so the callback may re-enter the manager safely.
    static void emitMeasurement(EMeasurementSink SinkKind,
                                const std::string& FilePath,
                                const std::function<void(const std::string&)>& Callback,
                                const std::string& Json)
    {
        switch (SinkKind)
        {
            case EMeasurementSink::None:
                break;
            case EMeasurementSink::StdOut:
            {
                const std::string Line = Json + "\n";
                std::cout.write(Line.data(), static_cast<std::streamsize>(Line.size()));
                std::cout.flush();
                break;
            }
            case EMeasurementSink::StdErr:
            {
                const std::string Line = Json + "\n";
                std::cerr.write(Line.data(), static_cast<std::streamsize>(Line.size()));
                std::cerr.flush();
                break;
            }
            case EMeasurementSink::File:
            {
                std::ofstream Output(FilePath, std::ios::app | std::ios::binary);
                if (Output)
                {
                    Output << Json << '\n';
                }
                break;
            }
            case EMeasurementSink::Callback:
                if (Callback)
                {
                    Callback(Json);
                }
                break;
        }
    }

    mutable LockT Lock;
    std::vector<Detail::SectionAccumulator> Sections;
    std::int64_t TotalFrameCount = 0;
    bool HasOpenFrame = false;

    ETimeUnit TimeUnit = ETimeUnit::Millisecond;
    bool ShouldEmitJsonComments = true;

    EMeasurementSink MeasurementSink = EMeasurementSink::None;
    std::string MeasurementFilePath;
    std::function<void(const std::string&)> MeasurementCallback;
    std::vector<std::string> MeasurementLog;
    std::int64_t MeasurementSequence = 0;
    bool ShouldStoreMeasurements = true;
};

} // namespace Detail

// RAII timer that records a section's elapsed time when it leaves scope (or
// when stop() is called). ManagerT selects which manager singleton the time is
// recorded into.
template <typename ManagerT>
class BenchmarkTimerBase
{
public:
    explicit BenchmarkTimerBase(std::string Name)
        : Name(std::move(Name))
        , Start(Clock::now())
        , LastMark(Start)
        , Checkpoints()
        , IsActive(true)
    {
    }

    ~BenchmarkTimerBase()
    {
        if (IsActive)
        {
            ManagerT::get().recordTime(
                Name, Detail::getNanosecondsBetween(Start, Detail::getTimePoint()),
                std::move(Checkpoints));
        }
    }

    BenchmarkTimerBase(BenchmarkTimerBase&& Other) noexcept
        : Name(std::move(Other.Name))
        , Start(Other.Start)
        , LastMark(Other.LastMark)
        , Checkpoints(std::move(Other.Checkpoints))
        , IsActive(Other.IsActive)
    {
        Other.IsActive = false;
    }

    BenchmarkTimerBase& operator=(BenchmarkTimerBase&& Other) noexcept
    {
        if (this != &Other)
        {
            if (IsActive)
            {
                ManagerT::get().recordTime(
                    Name, Detail::getNanosecondsBetween(Start, Detail::getTimePoint()),
                    std::move(Checkpoints));
            }

            Name = std::move(Other.Name);
            Start = Other.Start;
            LastMark = Other.LastMark;
            Checkpoints = std::move(Other.Checkpoints);
            IsActive = Other.IsActive;
            Other.IsActive = false;
        }
        return *this;
    }

    BenchmarkTimerBase(const BenchmarkTimerBase&) = delete;
    BenchmarkTimerBase& operator=(const BenchmarkTimerBase&) = delete;

    // Records a named checkpoint ("stub") inside the timed scope. The elapsed
    // time is relative to the previous checkpoint (or to the timer start).
    void mark(const std::string& CheckpointName)
    {
        if (!IsActive)
        {
            return;
        }

        const Clock::time_point Now = Clock::now();
        Checkpoints.push_back(
            CheckpointRecord{CheckpointName, Detail::getNanosecondsBetween(LastMark, Now)});
        LastMark = Now;
    }

    // Stops the timer early and records the elapsed time immediately.
    void stop()
    {
        if (IsActive)
        {
            ManagerT::get().recordTime(
                Name, Detail::getNanosecondsBetween(Start, Detail::getTimePoint()),
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

namespace Detail
{

template <typename LockT>
inline typename BenchmarkManagerBase<LockT>::TimerType
BenchmarkManagerBase<LockT>::measure(const std::string& Name)
{
    return TimerType(Name);
}

} // namespace Detail

// Concrete single-threaded manager: no synchronization overhead.
using BenchmarkManager = Detail::BenchmarkManagerBase<Detail::NoOpLock>;

// Concrete thread-safe manager: every operation is guarded by std::mutex.
using ThreadSafeBenchmarkManager = Detail::BenchmarkManagerBase<std::mutex>;

// Matching RAII timers.
using BenchmarkTimer = BenchmarkTimerBase<BenchmarkManager>;
using ThreadSafeBenchmarkTimer = BenchmarkTimerBase<ThreadSafeBenchmarkManager>;

} // namespace Bm

// Token pasting helpers.
#define BM_CONCAT_INNER(Left, Right) Left##Right
#define BM_CONCAT(Left, Right) BM_CONCAT_INNER(Left, Right)

// Registers a benchmark function named "Function" under the string #Function.
// Place at namespace scope, one per line.
#define BENCHMARK(Function) \
    static ::Bm::Detail::BenchmarkRegistrar BM_CONCAT(BmBenchmarkRegistrar_, __LINE__)(#Function, &Function)

// Registers a benchmark function under an explicit name.
#define BENCHMARK_NAME(Name, Function) \
    static ::Bm::Detail::BenchmarkRegistrar BM_CONCAT(BmBenchmarkRegistrar_, __LINE__)(Name, &Function)

// Measures the enclosing scope as a named section until it exits
// (single-threaded manager).
#define BM_SCOPED_TIMER(Name) \
    ::Bm::BenchmarkTimer BM_CONCAT(BmTimer_, __LINE__)(Name)

// Measures the enclosing scope as a named section until it exits
// (thread-safe manager).
#define BM_SCOPED_TIMER_THREAD_SAFE(Name) \
    ::Bm::ThreadSafeBenchmarkTimer BM_CONCAT(BmTimerTs_, __LINE__)(Name)

#endif // BM_BENCHMARK_HPP
