//
// SerdeJsonEngine.hpp — JSON 后端
//
// SerdeADT <-> JSON 文本; 附带后端私有的 base64(binary)与 ISO8601(datetime)工具。
// binary / datetime 以单键对象 {"$binary": "..."} / {"$datetime": "..."} 编码。
//
// C++11 兼容(2026/11 重构): Optional / string_view 走 infra::compat;
// std::byte 走 serde_detail::Byte; std::chrono::floor(C++17) 与 std::chrono::days
// (C++20) 用局部等价实现; structured bindings / if 初始化语句改写。
// 注意: C++11 下 SerdeADT 为优雅降级桩(见 SerdeADT.hpp), 本引擎可编译, 但
// parse/serialize 运行时经 SerdeADT 构造抛出 std::logic_error。
//

#ifndef INFRA_SERDE_JSON_ENGINE_HPP
#define INFRA_SERDE_JSON_ENGINE_HPP
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include <infra/common/VersionCompatibility.hpp>
#include <infra/serde/SerdeEngine.hpp>
#include <infra/serde/detail/vendor/json.hpp>

namespace infra{
// ========================= base64 / ISO8601 工具 =========================
// @notice 只读常量数据: 用 static constexpr 字符数组(C++11 可用; 每个 TU 一份
//         只读拷贝, 仅被本头文件的 inline 函数读取, 无 ODR 问题)。
static constexpr char kBase64Chars[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64Encode(infra::compat::string_view bytes) {
    const auto *p       = reinterpret_cast<const std::uint8_t *>(bytes.data());
    const std::size_t N = bytes.size();
    std::string Out;
    Out.reserve((N + 2) / 3 * 4);
    std::size_t I = 0;
    for (; I + 3 <= N; I += 3) {
        std::uint32_t V = (static_cast<std::uint32_t>(p[I]) << 16) |
                          (static_cast<std::uint32_t>(p[I + 1]) << 8) |
                          static_cast<std::uint32_t>(p[I + 2]);
        Out.push_back(kBase64Chars[(V >> 18) & 0x3F]);
        Out.push_back(kBase64Chars[(V >> 12) & 0x3F]);
        Out.push_back(kBase64Chars[(V >> 6) & 0x3F]);
        Out.push_back(kBase64Chars[V & 0x3F]);
    }
    const std::size_t rem = N - I;
    if (rem == 1) {
        std::uint32_t V = static_cast<std::uint32_t>(p[I]) << 16;
        Out.push_back(kBase64Chars[(V >> 18) & 0x3F]);
        Out.push_back(kBase64Chars[(V >> 12) & 0x3F]);
        Out.push_back('=');
        Out.push_back('=');
    }
    else if (rem == 2) {
        std::uint32_t V = (static_cast<std::uint32_t>(p[I]) << 16) |
                          (static_cast<std::uint32_t>(p[I + 1]) << 8);
        Out.push_back(kBase64Chars[(V >> 18) & 0x3F]);
        Out.push_back(kBase64Chars[(V >> 12) & 0x3F]);
        Out.push_back(kBase64Chars[(V >> 6) & 0x3F]);
        Out.push_back('=');
    }
    return Out;
}

inline infra::compat::Optional<SerdeBinary> base64Decode(infra::compat::string_view in) {
    static std::int8_t table[256] = {};
    static bool init              = [] {
        for (int I = 0; I < 256; ++I)
            table[I] = -1;
        for (int I = 0; I < 64; ++I)
            table[static_cast<unsigned char>(kBase64Chars[I])] = static_cast<std::int8_t>(I);
        return true;
    }();
    (void) init;

    SerdeBinary Out;
    Out.reserve(in.size() / 4 * 3 + 3);
    std::uint32_t Acc = 0;
    int Bits          = 0;
    for (char Ch: in) {
        if (Ch == '=' || Ch == '\n' || Ch == '\r')
            continue;
        auto V = table[static_cast<unsigned char>(Ch)];
        if (V < 0)
            return infra::compat::nullopt;
        Acc  = (Acc << 6) | static_cast<std::uint32_t>(V);
        Bits += 6;
        if (Bits >= 8) {
            Bits -= 8;
            Out.push_back(static_cast<serde_detail::Byte>((Acc >> Bits) & 0xFF));
        }
    }
    return Out;
}

// ---- 纯标准 C++ 的 UTC 历法换算 ----
// 不使用 gmtime_r / gmtime_s / timegm / _mkgmtime：前两者与后两者分别属于 POSIX 与 MSVC
// 两套互不兼容的扩展（参数顺序亦不同），且 timegm 在严格 ANSI 模式（-std=c++NN）下于部分
// libc 中不可见。这里改用整数历法运算（Howard Hinnant 算法）自行换算 UTC 日历字段，
// 不依赖平台 API、本地时区、区域设置与全局状态，Windows / macOS / Linux 结果完全一致，且线程安全。
namespace detail{

// std::chrono::floor 为 C++17 设施; C++11/14 退化为 duration_cast + 负值向下校正
template <typename To, typename Rep, typename Period>
To floorDuration(const std::chrono::duration<Rep, Period> &D) {
#if PICTOR_HAS_CPP17
    return std::chrono::floor<To>(D);
#else
    To T = std::chrono::duration_cast<To>(D);
    if (T > D) {
        T -= To(1);
    }
    return T;
#endif
}

// std::chrono::days 为 C++20 设施, 这里给出等价类型(C++20 下即同一类型)
using ChronoDays = std::chrono::duration<std::int64_t, std::ratio<86400> >;

// 公历日期 -> 自 1970-01-01 (UTC) 起的天数，支持 1970 之前的负天数
PICTOR_NODISCARD PICTOR_CONSTEXPR14 std::int64_t getDaysFromCivil(int Y, unsigned M, unsigned D) noexcept {
    Y                      -= static_cast<int>(M <= 2u);
    const std::int64_t Era = (Y >= 0 ? Y : Y - 399) / 400;
    const unsigned Yoe     = static_cast<unsigned>(Y - Era * 400);     // [0, 399]
    const unsigned Mp      = (M > 2u ? M - 3u : M + 9u);               // 3 月记为 0
    const unsigned Doy     = (153u * Mp + 2u) / 5u + (D - 1u);         // [0, 365]
    const unsigned Doe     = Yoe * 365u + Yoe / 4u - Yoe / 100u + Doy; // [0, 146096]
    return Era * 146097 + static_cast<std::int64_t>(Doe) - 719468;
}

// 自 1970-01-01 (UTC) 起的天数 -> 公历日期
PICTOR_CONSTEXPR14 void getCivilFromDays(std::int64_t Z, int &Y, unsigned &M, unsigned &D) noexcept {
    Z                  += 719468;
    const int64_t Era  = (Z >= 0 ? Z : Z - 146096) / 146097;
    const unsigned Doe = static_cast<unsigned>(Z - Era * 146097);                   // [0, 146096]
    const unsigned Yoe = (Doe - Doe / 1460u + Doe / 36524u - Doe / 146096u) / 365u; // [0, 399]
    const unsigned Doy = Doe - (365u * Yoe + Yoe / 4u - Yoe / 100u);                // [0, 365]
    const unsigned Mp  = (5u * Doy + 2u) / 153u;                                    // [0, 11]
    M                  = (Mp < 10u ? Mp + 3u : Mp - 9u);                            // [1, 12]
    D                  = Doy - (153u * Mp + 2u) / 5u + 1u;                          // [1, 31]
    Y                  = static_cast<int>(static_cast<std::int64_t>(Yoe) + Era * 400 + (M <= 2u ? 1 : 0));
}
} // namespace detail

inline std::string toIso8601(std::chrono::system_clock::time_point Tp) {
    // 向负无穷取整到秒 / 天：1970 之前的时间点同样正确（不依赖 to_time_t 的舍入方向）
    const std::chrono::seconds Epoch = detail::floorDuration<std::chrono::seconds>(Tp.time_since_epoch());
    const detail::ChronoDays Days     = detail::floorDuration<detail::ChronoDays>(Epoch);
    const std::chrono::seconds Tod    = Epoch - Days; // [0s, 86400s)

    int Y      = 0;
    unsigned M = 0;
    unsigned D = 0;
    detail::getCivilFromDays(Days.count(), Y, M, D);

    const std::chrono::hours Hour  = std::chrono::duration_cast<std::chrono::hours>(Tod);
    const std::chrono::minutes Min = std::chrono::duration_cast<std::chrono::minutes>(Tod) % 60;
    const std::chrono::seconds Sec = std::chrono::duration_cast<std::chrono::seconds>(Tod) % 60;

    char Buf[40];
    std::snprintf(Buf,
                  sizeof(Buf),
                  "%04d-%02u-%02uT%02lld:%02lld:%02lldZ",
                  Y,
                  M,
                  D,
                  static_cast<long long>(Hour.count()),
                  static_cast<long long>(Min.count()),
                  static_cast<long long>(Sec.count()));
    return Buf;
}

inline infra::compat::Optional<std::chrono::system_clock::time_point> fromIso8601(infra::compat::string_view s) {
    // 解析 toIso8601 的规范形式 "YYYY-MM-DDTHH:MM:SSZ"（结尾 Z 可省略），
    // 避免 std::get_time 对区域设置与流状态的依赖（其行为在不同标准库间并不一致）
    const auto ReadDigits = [s](std::size_t Pos, std::size_t Count, int &Out) -> bool {
        if (Pos + Count > s.size()) {
            return false;
        }
        int V = 0;
        for (std::size_t I = 0; I < Count; ++I) {
            const char C = s[Pos + I];
            if (C < '0' || C > '9') {
                return false;
            }
            V = V * 10 + (C - '0');
        }
        Out = V;
        return true;
    };

    if (s.size() < 19) {
        return infra::compat::nullopt;
    }
    if (s[4] != '-' || s[7] != '-' || s[13] != ':' || s[16] != ':') {
        return infra::compat::nullopt;
    }
    if (s[10] != 'T' && s[10] != 't' && s[10] != ' ') {
        return infra::compat::nullopt;
    }
    // @notice if 初始化语句为 C++17, 这里拆成声明 + 判断
    const infra::compat::string_view Tail = s.substr(19);
    if (!Tail.empty() && Tail != "Z" && Tail != "z") {
        return infra::compat::nullopt;
    }

    int Y = 0, Mo = 0, D = 0, H = 0, Mi = 0, Se = 0;
    if (!ReadDigits(0, 4, Y) || !ReadDigits(5, 2, Mo) || !ReadDigits(8, 2, D) ||
        !ReadDigits(11, 2, H) || !ReadDigits(14, 2, Mi) || !ReadDigits(17, 2, Se)) {
        return infra::compat::nullopt;
    }
    if (Mo < 1 || Mo > 12 || D < 1 || D > 31 || H > 23 || Mi > 59 || Se > 59) {
        return infra::compat::nullopt;
    }

    const std::int64_t Days =
        detail::getDaysFromCivil(Y, static_cast<unsigned>(Mo), static_cast<unsigned>(D));
    // 回代校验：拒绝 2 月 30 日这类历法上不存在的日期
    int Ye      = 0;
    unsigned Me = 0;
    unsigned De = 0;
    detail::getCivilFromDays(Days, Ye, Me, De);
    if (Ye != Y || Me != static_cast<unsigned>(Mo) || De != static_cast<unsigned>(D))
        return infra::compat::nullopt;

    const std::int64_t Secs = Days * 86400 + H * 3600 + Mi * 60 + Se;
    return std::chrono::system_clock::time_point{
        std::chrono::duration_cast<std::chrono::system_clock::duration>(
                                                                        std::chrono::seconds{Secs})
    };
}

// ========================= JSON 后端 =========================
class SerdeJsonEngine : public SerdeEngine {
    using Json = nlohmann::ordered_json;

public:
    SerdeADT parse(infra::compat::string_view Text) const override {
        // 先处理 \n \t 这类空白字符（含 BOM / CRLF），再交给 JSON 解析器
        const std::string Clean = getNormalizeText(Text);
        if (Clean.empty()) {
            throw std::runtime_error("JSON parse error: empty input");
        }

        try {
            return fromJson(Json::parse(Clean));
        }
        catch (const std::exception &E) {
            throw std::runtime_error(std::string("JSON parse error: ") + E.what());
        }
    }

    std::string serialize(const SerdeADT &Adt) const override {
        // 落盘为可读文本：每个层级用 \t 缩进、每行以 \n 结尾，末尾补一个 \n
        return toJson(Adt).dump(1, '\t') + "\n";
    }

private:
    static Json toJson(const SerdeADT &V) {
        if (V.isNull())
            return Json(nullptr);
        if (V.isBool())
            return Json(V.asBool());
        if (V.isInt())
            return Json(V.asInt());
        if (V.isUint())
            return Json(V.asUint());
        if (V.isDouble())
            return Json(V.asDouble());
        if (V.isString())
            return Json(V.asString());
        if (V.isBinary()) {
            Json O       = Json::object();
            const SerdeBinary &Bin = V.asBinary();
            O["$binary"] = base64Encode(infra::compat::string_view(
                reinterpret_cast<const char *>(Bin.data()), Bin.size()));
            return O;
        }
        if (V.isDatetime()) {
            Json O         = Json::object();
            O["$datetime"] = toIso8601(V.asDatetime());
            return O;
        }
        if (V.isArray()) {
            Json A = Json::array();
            for (const auto &E: V.asArray())
                A.push_back(toJson(E));
            return A;
        }
        if (V.isObject()) {
            Json O = Json::object();
            // @notice structured bindings 为 C++17, 统一按 pair 访问
            for (const auto &Pair: V.asObject())
                O[Pair.first] = toJson(Pair.second);
            return O;
        }
        throw std::runtime_error("toJson: unsupported value");
    }

    static SerdeADT fromJson(const Json &J) {
        if (J.is_null())
            return SerdeADT{nullptr};
        if (J.is_boolean())
            return SerdeADT{J.get<bool>()};
        if (J.is_number_integer())
            return SerdeADT{J.get<std::int64_t>()};
        if (J.is_number_unsigned())
            return SerdeADT{J.get<std::uint64_t>()};
        if (J.is_number_float())
            return SerdeADT{J.get<double>()};
        if (J.is_string())
            return SerdeADT{J.get<std::string>()};
        if (J.is_array()) {
            SerdeArray A;
            for (const auto &E: J)
                A.push_back(fromJson(E));
            return SerdeADT{std::move(A)};
        }
        if (J.is_object()) {
            if (J.size() == 1) {
                const Json::const_iterator BinaryIt = J.find("$binary");
                if (BinaryIt != J.end() && BinaryIt->is_string()) {
                    const std::string BinaryText = BinaryIt->get<std::string>();
                    infra::compat::Optional<SerdeBinary> Dec =
                        base64Decode(infra::compat::string_view(BinaryText.data(), BinaryText.size()));
                    if (Dec)
                        return SerdeADT{std::move(*Dec)};
                }
                const Json::const_iterator DateIt = J.find("$datetime");
                if (DateIt != J.end() && DateIt->is_string()) {
                    const std::string DateText = DateIt->get<std::string>();
                    infra::compat::Optional<std::chrono::system_clock::time_point> Tp =
                        fromIso8601(infra::compat::string_view(DateText.data(), DateText.size()));
                    if (Tp)
                        return SerdeADT{*Tp};
                }
            }
            SerdeObject O;
            for (const auto &Pair: J.items())
                O.emplace_back(Pair.key(), fromJson(Pair.value()));
            return SerdeADT{std::move(O)};
        }
        throw std::runtime_error("fromJson: unsupported json value");
    }
};
} // namespace infra

#endif // INFRA_SERDE_JSON_ENGINE_HPP
