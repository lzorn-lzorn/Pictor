//
// SerdeIniEngine.hpp — INI 后端
//
// SerdeADT(对象树) <-> Ini 文本的映射:
//   - 根对象的每个 object 成员 -> 一个 [Section]; 其标量/数组成员 -> Key = Value
//   - 嵌套 object -> 点分键(parent.child = value), 读回时展开为嵌套对象
//   - 数组 -> 列表值 [a, b, c]; 列表元素要求为标量(数组嵌套对象不支持)
//   - null -> 空值; binary / datetime 不受 Ini 支持, 序列化时报错
//
// 注意: 引擎读写两侧使用宽容策略(任意 Section 名、键名不校验)。Ini 的解析与
//       查找语义(继承/插值/重复键等)在引擎路径上按默认策略生效; 需要更细粒度
//       控制时直接使用 ::Ini —— 配置文件仍是 Ini 独立用法的主流场景。
//
// C++11 兼容(2026/11 重构): std::expected 走 infra::serde_detail::Expected;
// std::visit 改 ini_detail::getIf 链; structured bindings / if 初始化语句改写;
// std::to_chars 走兼容层(浮点经 ini_detail::doubleToString 规避 C++11 垫片的
// 重载歧义)。C++11 下 SerdeADT 为优雅降级桩(见 SerdeADT.hpp), 本引擎可编译,
// 但 parse/serialize 运行时经 SerdeADT 构造抛出 std::logic_error。
//

#ifndef INFRA_SERDE_INI_ENGINE_HPP
#define INFRA_SERDE_INI_ENGINE_HPP
#pragma once

#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include <infra/common/VersionCompatibility.hpp>
#include <infra/serde/SerdeEngine.hpp>
#include <infra/serde/parser/Ini.hpp>

namespace infra{
// ========================= INI 后端 =========================
class SerdeIniEngine : public SerdeEngine {
public:
    SerdeADT parse(infra::compat::string_view Text) const override {
        // 与 serialize 对称的宽容策略: 点分键(sub.key)与任意 Section 名均可读回
        IniPolicy Policy;
        Policy.AllowAnySectionName = true;
        Policy.KeyNameValidation   = false;
        Policy.ForbiddenKeyChars   = "";
        ::Ini Doc(Policy);

        infra::serde_detail::Expected<void, IniError> Parsed = Doc.parse(Text);
        if (!Parsed) {
            throw std::runtime_error("INI parse error: " + IniError::asString(Parsed.error()));
        }

        SerdeObject Root;
        for (const std::string &Name: Doc.sections()) {
            const IniSection *Sec = Doc.section(ini_detail::toStringView(Name));
            if (Sec == nullptr) {
                continue;
            }
            SerdeObject Obj;
            for (const IniEntry &Entry: Sec->Entries) {
                // 序列化时的点分键(sub.key)在此展开回嵌套对象
                insertDotted(Obj, ini_detail::toStringView(Entry.Key), variantToAdt(Entry.Value));
            }
            Root.emplace_back(Name, SerdeADT{std::move(Obj)});
        }
        return SerdeADT{std::move(Root)};
    }

    std::string serialize(const SerdeADT &Adt) const override {
        if (!Adt.isObject()) {
            throw std::runtime_error("INI serialize: root must be an object of sections");
        }

        // 引擎写入用宽容策略: 任意非空 Section 名(写回时自动引号包裹),
        // 键名不做标识符校验(注意: 含特殊字符的键名无法被 Ini 解析回读)
        IniPolicy Policy;
        Policy.AllowAnySectionName = true;
        Policy.KeyNameValidation   = false;
        Policy.ForbiddenKeyChars   = "";
        ::Ini Doc(Policy);

        for (const auto &Pair: Adt.asObject()) {
            const std::string &SectionName = Pair.first;
            const SerdeADT &SectionValue   = Pair.second;
            if (!SectionValue.isObject()) {
                // 根级标量/数组 -> 放入默认 Section
                putEntry(Doc, std::string(Policy.DefaultSectionName.data(), Policy.DefaultSectionName.size()),
                         SectionName, SectionValue);
                continue;
            }
            for (const auto &InnerPair: SectionValue.asObject()) {
                putEntry(Doc, SectionName, InnerPair.first, InnerPair.second);
            }
        }
        return Doc.serialize();
    }

private:
    /** @brief 把点分键 sub.key 展开为嵌套对象(覆盖已有值)。 */
    static void insertDotted(SerdeObject &Obj, infra::compat::string_view Key, SerdeADT Value) {
        const std::size_t Dot = Key.find('.');
        if (Dot == infra::compat::string_view::npos) {
            for (SerdeObject::iterator It = Obj.begin(); It != Obj.end(); ++It) {
                if (ini_detail::toStringView(It->first) == Key) {
                    It->second = std::move(Value);
                    return;
                }
            }
            Obj.emplace_back(ini_detail::viewToString(Key), std::move(Value));
            return;
        }

        const std::string Head = ini_detail::viewToString(infra::compat::string_view(Key.data(), Dot));
        const infra::compat::string_view Tail = Key.substr(Dot + 1);
        for (SerdeObject::iterator It = Obj.begin(); It != Obj.end(); ++It) {
            if (It->first == Head) {
                if (!It->second.isObject()) {
                    It->second = SerdeADT{SerdeObject{}};
                }
                insertDotted(It->second.asObject(), Tail, std::move(Value));
                return;
            }
        }
        SerdeObject Sub;
        insertDotted(Sub, Tail, std::move(Value));
        Obj.emplace_back(Head, SerdeADT{std::move(Sub)});
    }

    static SerdeADT variantToAdt(const IniVariant &V) {
        // 列表元素推断类型, 保证 vector<int>/vector<double> 可往返
        if (const IniStringList *List = ini_detail::getIf<IniStringList>(&V)) {
            SerdeArray Arr;
            for (const std::string &Element: *List) {
                std::int64_t Int = 0;
                if (ini_detail::tryParseInt(ini_detail::toStringView(Element), Int)) {
                    Arr.emplace_back(Int);
                    continue;
                }
                double Double = 0.0;
                if (ini_detail::tryParseDouble(ini_detail::toStringView(Element), Double)) {
                    Arr.emplace_back(Double);
                    continue;
                }
                Arr.emplace_back(Element);
            }
            return SerdeADT{std::move(Arr)};
        }
        if (const bool *B = ini_detail::getIf<bool>(&V)) {
            return SerdeADT{*B};
        }
        if (const std::int64_t *I = ini_detail::getIf<std::int64_t>(&V)) {
            return SerdeADT{*I};
        }
        if (const double *D = ini_detail::getIf<double>(&V)) {
            return SerdeADT{*D};
        }
        if (const std::string *S = ini_detail::getIf<std::string>(&V)) {
            return SerdeADT{*S};
        }
        return SerdeADT{nullptr}; // 兜底(理论上不可达)
    }

    static void putEntry(::Ini &Doc,
                         const std::string &SectionName,
                         const std::string &Key,
                         const SerdeADT &Value) {
        if (Value.isObject()) {
            // 嵌套对象 -> 点分键
            for (const auto &Pair: Value.asObject()) {
                putEntry(Doc, SectionName, Key + "." + Pair.first, Pair.second);
            }
            return;
        }

        infra::serde_detail::Expected<void, IniError> Result;
        const infra::compat::string_view SectionView = ini_detail::toStringView(SectionName);
        const infra::compat::string_view KeyView     = ini_detail::toStringView(Key);
        if (Value.isNull())
            Result = Doc.set(SectionView, KeyView, "");
        else if (Value.isBool())
            Result = Doc.set(SectionView, KeyView, Value.asBool());
        else if (Value.isInt())
            Result = Doc.set(SectionView, KeyView, Value.asInt());
        else if (Value.isUint())
            Result = Doc.set(SectionView, KeyView, Value.asUint());
        else if (Value.isDouble())
            Result = Doc.set(SectionView, KeyView, Value.asDouble());
        else if (Value.isString())
            Result = Doc.set(SectionView, KeyView, ini_detail::toStringView(Value.asString()));
        else if (Value.isArray()) {
            std::vector<std::string> List;
            for (const SerdeADT &Element: Value.asArray()) {
                if (Element.isObject()) {
                    throw std::runtime_error("INI serialize: array of objects is not supported");
                }
                List.push_back(adtToText(Element));
            }
            Result = Doc.set(SectionView, KeyView, List);
        }
        else {
            throw std::runtime_error("INI serialize: binary/datetime values are not supported by INI");
        }

        if (!Result) {
            throw std::runtime_error("INI serialize: " + IniError::asString(Result.error()));
        }
    }

    static std::string adtToText(const SerdeADT &V) {
        if (V.isString())
            return V.asString();
        if (V.isBool())
            return V.asBool() ? "true" : "false";
        if (V.isInt())
            return std::to_string(V.asInt());
        if (V.isUint())
            return std::to_string(V.asUint());
        if (V.isDouble())
            return ini_detail::doubleToString(V.asDouble());
        if (V.isNull())
            return "";
        throw std::runtime_error("INI serialize: unsupported array element");
    }
};
} // namespace infra

#endif // INFRA_SERDE_INI_ENGINE_HPP
