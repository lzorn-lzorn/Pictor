//
// SerdeTomlEngine.hpp — TOML 后端
//
// SerdeADT <-> TOML 文本。TOML 无原生 binary / datetime / null 支持,
// 相应值在序列化时跳过, 读取时映射为字符串(日期时间)。
//
// C++11 兼容(2026/11 重构): vendored toml++ 自身 #error 要求 C++17。
// C++17 起保持原实现; C++11/14 下类接口照常声明, parse/serialize 抛出
// std::logic_error 优雅降级(SerdeEngineFactory 在 C++11 下不注册本引擎)。
//

#ifndef INFRA_SERDE_TOML_ENGINE_HPP
#define INFRA_SERDE_TOML_ENGINE_HPP

#include <stdexcept>
#include <string>

#include <infra/common/VersionCompatibility.hpp>
#include <infra/serde/SerdeEngine.hpp>

#if PICTOR_HAS_CPP17

#  include <sstream>
#  include <infra/serde/detail/vendor/toml.hpp>

namespace infra{
// ========================= TOML 后端 =========================
class SerdeTomlEngine : public SerdeEngine {
public:
    SerdeADT parse(infra::compat::string_view Text) const override {
        try {
            // 同样先规整 \n \t / BOM / CRLF 再交给解析器
            toml::table Tbl = toml::parse(getNormalizeText(Text)); // TOML_EXCEPTIONS=1 时 parse 直接返回 table，失败抛异常
            return fromTomlTable(Tbl);
        }
        catch (const std::exception &E) {
            throw std::runtime_error(std::string("TOML parse error: ") + E.what());
        }
    }

    std::string serialize(const SerdeADT &Adt) const override {
        if (!Adt.isObject())
            throw std::runtime_error("TOML root must be A table");
        toml::table Tbl = toTomlTable(Adt.asObject());
        std::ostringstream Oss;
        Oss << Tbl;

        std::string Text = Oss.str();
        if (Text.empty() || Text.back() != '\n') // 文件末尾统一补一个换行
        {
            Text.push_back('\n');
        }
        return Text;
    }

private:
    static void insertToml(toml::table &Tbl, const std::string &Key, const SerdeADT &V) {
        if (V.isString()) {
            Tbl.insert_or_assign(Key, V.asString());
        }
        else if (V.isInt()) {
            Tbl.insert_or_assign(Key, V.asInt());
        }
        else if (V.isUint()) {
            Tbl.insert_or_assign(Key, static_cast<std::int64_t>(V.asUint()));
        }
        else if (V.isDouble()) {
            Tbl.insert_or_assign(Key, V.asDouble());
        }
        else if (V.isBool()) {
            Tbl.insert_or_assign(Key, V.asBool());
        }
        else if (V.isArray()) {
            toml::array Arr;
            for (const auto &E: V.asArray()) {
                if (E.isObject()) {
                    toml::table Sub;
                    for (const auto &[K2, E2]: E.asObject())
                        insertToml(Sub, K2, E2);
                    Arr.push_back(std::move(Sub));
                }
                else if (E.isString())
                    Arr.push_back(E.asString());
                else if (E.isInt())
                    Arr.push_back(E.asInt());
                else if (E.isUint())
                    Arr.push_back(static_cast<std::int64_t>(E.asUint()));
                else if (E.isDouble())
                    Arr.push_back(E.asDouble());
                else
                    if (E.isBool())
                        Arr.push_back(E.asBool());
                // null / binary / datetime：TOML 无原生支持，跳过
            }
            Tbl.insert_or_assign(Key, std::move(Arr));
        }
        else if (V.isObject()) {
            toml::table Sub;
            for (const auto &[K2, E2]: V.asObject())
                insertToml(Sub, K2, E2);
            Tbl.insert_or_assign(Key, std::move(Sub));
        }
        // null / binary / datetime：跳过
    }

    static toml::table toTomlTable(const SerdeObject &Obj) {
        toml::table Tbl;
        for (const auto &[K, V]: Obj)
            insertToml(Tbl, K, V);
        return Tbl;
    }

    static SerdeADT fromTomlNode(const toml::node &node) {
        if (node.is_string())
            return SerdeADT{node.value<std::string>().value_or("")};
        if (node.is_integer())
            return SerdeADT{node.value<std::int64_t>().value_or(0)};
        if (node.is_floating_point())
            return SerdeADT{node.value<double>().value_or(0.0)};
        if (node.is_boolean())
            return SerdeADT{node.value<bool>().value_or(false)};
        if (node.is_array()) {
            SerdeArray Arr;
            for (const auto &E: *node.as_array()) {
                Arr.push_back(fromTomlNode(E));
            }
            return SerdeADT{std::move(Arr)};
        }
        if (node.is_table()) {
            SerdeObject Obj;
            for (const auto &[K, V]: *node.as_table()) {
                Obj.emplace_back(K, fromTomlNode(V));
            }
            return SerdeADT{std::move(Obj)};
        }
        if (node.is_date_time()) {
            std::ostringstream Oss;
            Oss << *node.as_date_time();
            return SerdeADT{Oss.str()};
        }
        return SerdeADT{nullptr};
    }

    static SerdeADT fromTomlTable(const toml::table &Tbl) {
        SerdeObject Obj;
        for (const auto &[K, V]: Tbl) {
            Obj.emplace_back(K, fromTomlNode(V));
        }
        return SerdeADT{std::move(Obj)};
    }
};
} // namespace infra

#else // !PICTOR_HAS_CPP17

// @notice C++11 优雅降级: vendored toml++ 自身 #error 要求 C++17。C++11/14 下
//         类接口照常声明, parse/serialize 抛出 std::logic_error;
//         SerdeEngineFactory 亦不注册本引擎(getEngineFor 返回 nullptr)。
namespace infra{
class SerdeTomlEngine : public SerdeEngine {
public:
    SerdeADT parse(infra::compat::string_view Text) const override {
        (void) Text;
        throw std::logic_error("TOML 引擎需要 C++17");
    }

    std::string serialize(const SerdeADT &Adt) const override {
        (void) Adt;
        throw std::logic_error("TOML 引擎需要 C++17");
    }
};
} // namespace infra

#endif // PICTOR_HAS_CPP17

#endif // INFRA_SERDE_TOML_ENGINE_HPP
