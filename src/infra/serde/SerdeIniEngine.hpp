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

#ifndef INFRA_SERDE_INI_ENGINE_HPP
#define INFRA_SERDE_INI_ENGINE_HPP
#pragma once

#include <charconv>
#include <expected>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <variant>
#include <vector>

#include <infra/serde/SerdeEngine.hpp>
#include <infra/serde/parser/Ini.hpp>

namespace infra
{

// ========================= INI 后端 =========================
class SerdeIniEngine : public SerdeEngine
{
public:
    SerdeADT parse(std::string_view Text) const override
    {
        // 与 serialize 对称的宽容策略: 点分键(sub.key)与任意 Section 名均可读回
        IniPolicy Policy;
        Policy.AllowAnySectionName = true;
        Policy.KeyNameValidation = false;
        Policy.ForbiddenKeyChars = "";
        ::Ini Doc(Policy);

        if (const std::expected<void, IniError> Parsed = Doc.parse(Text); !Parsed)
        {
            throw std::runtime_error("INI parse error: " + IniError::asString(Parsed.error()));
        }

        SerdeObject Root;
        for (const std::string& Name : Doc.sections())
        {
            const IniSection* Sec = Doc.section(Name);
            if (Sec == nullptr)
            {
                continue;
            }
            SerdeObject Obj;
            for (const IniEntry& Entry : Sec->Entries)
            {
                // 序列化时的点分键(sub.key)在此展开回嵌套对象
                insertDotted(Obj, Entry.Key, variantToAdt(Entry.Value));
            }
            Root.emplace_back(Name, SerdeADT{ std::move(Obj) });
        }
        return SerdeADT{ std::move(Root) };
    }

    std::string serialize(const SerdeADT &Adt) const override
    {
        if (!Adt.isObject())
        {
            throw std::runtime_error("INI serialize: root must be an object of sections");
        }

        // 引擎写入用宽容策略: 任意非空 Section 名(写回时自动引号包裹),
        // 键名不做标识符校验(注意: 含特殊字符的键名无法被 Ini 解析回读)
        IniPolicy Policy;
        Policy.AllowAnySectionName = true;
        Policy.KeyNameValidation = false;
        Policy.ForbiddenKeyChars = "";
        ::Ini Doc(Policy);

        for (const auto& [SectionName, SectionValue] : Adt.asObject())
        {
            if (!SectionValue.isObject())
            {
                // 根级标量/数组 -> 放入默认 Section
                putEntry(Doc, std::string(Policy.DefaultSectionName), SectionName, SectionValue);
                continue;
            }
            for (const auto& [Key, Value] : SectionValue.asObject())
            {
                putEntry(Doc, SectionName, Key, Value);
            }
        }
        return Doc.serialize();
    }

private:
    /** @brief 把点分键 sub.key 展开为嵌套对象(覆盖已有值)。 */
    static void insertDotted(SerdeObject& Obj, std::string_view Key, SerdeADT Value)
    {
        const std::size_t Dot = Key.find('.');
        if (Dot == std::string_view::npos)
        {
            for (auto& [K, V] : Obj)
            {
                if (K == Key)
                {
                    V = std::move(Value);
                    return;
                }
            }
            Obj.emplace_back(std::string(Key), std::move(Value));
            return;
        }

        const std::string Head(Key.substr(0, Dot));
        const std::string_view Tail = Key.substr(Dot + 1);
        for (auto& [K, V] : Obj)
        {
            if (K == Head)
            {
                if (!V.isObject())
                {
                    V = SerdeADT{ SerdeObject{} };
                }
                insertDotted(V.asObject(), Tail, std::move(Value));
                return;
            }
        }
        SerdeObject Sub;
        insertDotted(Sub, Tail, std::move(Value));
        Obj.emplace_back(Head, SerdeADT{ std::move(Sub) });
    }

    static SerdeADT variantToAdt(const IniVariant& V)
    {
        return std::visit(
            [](const auto& Arg) -> SerdeADT
            {
                using T = std::decay_t<decltype(Arg)>;
                if constexpr (std::is_same_v<T, IniStringList>)
                {
                    // 列表元素推断类型, 保证 vector<int>/vector<double> 可往返
                    SerdeArray Arr;
                    for (const std::string& Element : Arg)
                    {
                        if (std::int64_t Int = 0; ini_detail::tryParseInt(Element, Int))
                        {
                            Arr.emplace_back(Int);
                        }
                        else if (double Double = 0.0; ini_detail::tryParseDouble(Element, Double))
                        {
                            Arr.emplace_back(Double);
                        }
                        else
                        {
                            Arr.emplace_back(Element);
                        }
                    }
                    return SerdeADT{ std::move(Arr) };
                }
                else
                {
                    return SerdeADT{ Arg }; // bool / int64_t / double / string
                }
            },
            V);
    }

    static void putEntry(::Ini& Doc, const std::string& SectionName, const std::string& Key,
                         const SerdeADT& Value)
    {
        if (Value.isObject())
        {
            // 嵌套对象 -> 点分键
            for (const auto& [SubKey, SubValue] : Value.asObject())
            {
                putEntry(Doc, SectionName, Key + "." + SubKey, SubValue);
            }
            return;
        }

        std::expected<void, IniError> Result;
        if (Value.isNull())         Result = Doc.set(SectionName, Key, "");
        else if (Value.isBool())    Result = Doc.set(SectionName, Key, Value.asBool());
        else if (Value.isInt())     Result = Doc.set(SectionName, Key, Value.asInt());
        else if (Value.isUint())    Result = Doc.set(SectionName, Key, Value.asUint());
        else if (Value.isDouble())  Result = Doc.set(SectionName, Key, Value.asDouble());
        else if (Value.isString())  Result = Doc.set(SectionName, Key, Value.asString());
        else if (Value.isArray())
        {
            std::vector<std::string> List;
            for (const SerdeADT& Element : Value.asArray())
            {
                if (Element.isObject())
                {
                    throw std::runtime_error("INI serialize: array of objects is not supported");
                }
                List.push_back(adtToText(Element));
            }
            Result = Doc.set(SectionName, Key, List);
        }
        else
        {
            throw std::runtime_error("INI serialize: binary/datetime values are not supported by INI");
        }

        if (!Result)
        {
            throw std::runtime_error("INI serialize: " + IniError::asString(Result.error()));
        }
    }

    static std::string adtToText(const SerdeADT& V)
    {
        if (V.isString()) return V.asString();
        if (V.isBool())   return V.asBool() ? "true" : "false";
        if (V.isInt())    return std::to_string(V.asInt());
        if (V.isUint())   return std::to_string(V.asUint());
        if (V.isDouble())
        {
            char Buffer[64];
            const std::to_chars_result Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), V.asDouble());
            return std::string(Buffer, Result.ptr);
        }
        if (V.isNull()) return "";
        throw std::runtime_error("INI serialize: unsupported array element");
    }
};

} // namespace infra

#endif // INFRA_SERDE_INI_ENGINE_HPP
