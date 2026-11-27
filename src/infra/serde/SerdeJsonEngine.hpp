//
// SerdeJsonEngine.hpp — JSON 后端
//
// SerdeADT <-> JSON 文本; 附带后端私有的 base64(binary)与 ISO8601(datetime)工具。
// binary / datetime 以单键对象 {"$binary": "..."} / {"$datetime": "..."} 编码。
//

#ifndef INFRA_SERDE_JSON_ENGINE_HPP
#define INFRA_SERDE_JSON_ENGINE_HPP
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <infra/serde/SerdeEngine.hpp>
#include <infra/serde/detail/vendor/json.hpp>

namespace infra{
// ========================= base64 / ISO8601 工具 =========================
inline constexpr std::string_view kBase64Chars =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline std::string base64Encode(std::string_view bytes) {
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

inline std::optional<SerdeBinary> base64Decode(std::string_view in) {
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
            return std::nullopt;
        Acc  = (Acc << 6) | static_cast<std::uint32_t>(V);
        Bits += 6;
        if (Bits >= 8) {
            Bits -= 8;
            Out.push_back(static_cast<std::byte>((Acc >> Bits) & 0xFF));
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
// 公历日期 -> 自 1970-01-01 (UTC) 起的天数，支持 1970 之前的负天数
[[nodiscard]] constexpr std::int64_t getDaysFromCivil(int Y, unsigned M, unsigned D) noexcept {
    Y                      -= static_cast<int>(M <= 2u);
    const std::int64_t Era = (Y >= 0 ? Y : Y - 399) / 400;
    const unsigned Yoe     = static_cast<unsigned>(Y - Era * 400);     // [0, 399]
    const unsigned Mp      = (M > 2u ? M - 3u : M + 9u);               // 3 月记为 0
    const unsigned Doy     = (153u * Mp + 2u) / 5u + (D - 1u);         // [0, 365]
    const unsigned Doe     = Yoe * 365u + Yoe / 4u - Yoe / 100u + Doy; // [0, 146096]
    return Era * 146097 + static_cast<std::int64_t>(Doe) - 719468;
}

// 自 1970-01-01 (UTC) 起的天数 -> 公历日期
constexpr void getCivilFromDays(std::int64_t Z, int &Y, unsigned &M, unsigned &D) noexcept {
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
    const auto Epoch = std::chrono::floor<std::chrono::seconds>(Tp.time_since_epoch());
    const auto Days  = std::chrono::floor<std::chrono::days>(Epoch);
    const auto Tod   = Epoch - Days; // [0s, 86400s)

    int Y      = 0;
    unsigned M = 0;
    unsigned D = 0;
    detail::getCivilFromDays(Days.count(), Y, M, D);

    const auto Hour = std::chrono::duration_cast<std::chrono::hours>(Tod).count();
    const auto Min  = std::chrono::duration_cast<std::chrono::minutes>(Tod).count() % 60;
    const auto Sec  = std::chrono::duration_cast<std::chrono::seconds>(Tod).count() % 60;

    char Buf[40];
    std::snprintf(Buf,
                  sizeof(Buf),
                  "%04d-%02u-%02uT%02lld:%02lld:%02lldZ",
                  Y,
                  M,
                  D,
                  static_cast<long long>(Hour),
                  static_cast<long long>(Min),
                  static_cast<long long>(Sec));
    return Buf;
}

inline std::optional<std::chrono::system_clock::time_point> fromIso8601(std::string_view s) {
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
        return std::nullopt;
    }
    if (s[4] != '-' || s[7] != '-' || s[13] != ':' || s[16] != ':') {
        return std::nullopt;
    }
    if (s[10] != 'T' && s[10] != 't' && s[10] != ' ') {
        return std::nullopt;
    }
    if (const std::string_view Tail = s.substr(19); !Tail.empty() && Tail != "Z" && Tail != "z") {
        return std::nullopt;
    }

    int Y = 0, Mo = 0, D = 0, H = 0, Mi = 0, Se = 0;
    if (!ReadDigits(0, 4, Y) || !ReadDigits(5, 2, Mo) || !ReadDigits(8, 2, D) ||
        !ReadDigits(11, 2, H) || !ReadDigits(14, 2, Mi) || !ReadDigits(17, 2, Se)) {
        return std::nullopt;
    }
    if (Mo < 1 || Mo > 12 || D < 1 || D > 31 || H > 23 || Mi > 59 || Se > 59) {
        return std::nullopt;
    }

    const std::int64_t Days =
        detail::getDaysFromCivil(Y, static_cast<unsigned>(Mo), static_cast<unsigned>(D));
    // 回代校验：拒绝 2 月 30 日这类历法上不存在的日期
    int Ye      = 0;
    unsigned Me = 0;
    unsigned De = 0;
    detail::getCivilFromDays(Days, Ye, Me, De);
    if (Ye != Y || Me != static_cast<unsigned>(Mo) || De != static_cast<unsigned>(D))
        return std::nullopt;

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
    SerdeADT parse(std::string_view Text) const override {
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
            O["$binary"] = base64Encode({
                                            reinterpret_cast<const char *>(V.asBinary().data()),
                                            V.asBinary().size()
                                        });
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
            for (const auto &[K, E]: V.asObject())
                O[K] = toJson(E);
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
                if (auto It = J.find("$binary"); It != J.end() && It->is_string()) {
                    if (auto Dec = base64Decode(It->get<std::string>()))
                        return SerdeADT{std::move(*Dec)};
                }
                if (auto It = J.find("$datetime"); It != J.end() && It->is_string()) {
                    if (auto Tp = fromIso8601(It->get<std::string>()))
                        return SerdeADT{*Tp};
                }
            }
            SerdeObject O;
            for (const auto &[K, E]: J.items())
                O.emplace_back(K, fromJson(E));
            return SerdeADT{std::move(O)};
        }
        throw std::runtime_error("fromJson: unsupported json value");
    }
};
} // namespace infra

#endif // INFRA_SERDE_JSON_ENGINE_HPP
