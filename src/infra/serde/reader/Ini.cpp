//
// Created by Administrator on 2026/11/18.
//

#include "Ini.h"

#include <cctype>
#include <algorithm>
namespace
{
    /** @brief 去除两端空白(空格、制表符与回车)。 */
    std::string_view trim(std::string_view Text)
    {
        while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t' || Text.front() == '\r'))
        {
            Text.remove_prefix(1);
        }
        while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t' || Text.back() == '\r'))
        {
            Text.remove_suffix(1);
        }
        return Text;
    }

    /** @brief 若值两端带双引号则剥去(内部内容原样保留)。 */
    std::string unquote(std::string_view Text)
    {
        if (Text.size() >= 2 && Text.front() == '"' && Text.back() == '"')
        {
            return std::string(Text.substr(1, Text.size() - 2));
        }
        return std::string(Text);
    }

    /** @brief 把字符串统一为小写。 */
    std::string toLower(std::string_view Text)
    {
        std::string Lower;
        Lower.reserve(Text.size());
        for (const char Character : Text)
        {
            Lower.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(Character))));
        }
        return Lower;
    }

    /** @brief 解析布尔值，失败返回 false 并保持 Out 不变。 */
    bool tryParseBool(std::string_view Text, bool& Out)
    {
        const std::string Lower = toLower(trim(Text));
        if (Lower == "true" || Lower == "1" || Lower == "yes" || Lower == "on")
        {
            Out = true;
            return true;
        }
        if (Lower == "false" || Lower == "0" || Lower == "no" || Lower == "off")
        {
            Out = false;
            return true;
        }
        return false;
    }

} // namespace

bool Ini::parse(std::string_view IniContent)
{
    return true;
}

std::optional<SectionType> Ini::parseKeyValue(std::string_view IniContent)
{
    std::vector<std::string> Symbols;

    Symbols.reserve(DefaultIniItemSymbolNumber);

    std::string Symbol;
    Symbol.reserve(DefaultIniKeyLength);
    for (char Character : IniContent)
    {
        if (std::isspace(static_cast<unsigned char>(Character)) != 0)
        {
            if (!Symbol.empty())
            {
                Symbols.push_back(Symbol);
                Symbol.clear();
            }
            continue;
        }
        if (std::ranges::find(SupportedAnnotationDelim, Character) != SupportedAnnotationDelim.end())
        {
            break;
        }

        switch (ReaderConfig.KeyCaseMode)
        {
            case ECaseMode::Default: Symbol.push_back(Character); break;
            case ECaseMode::LowerCase: Symbol.push_back(static_cast<char>(std::tolower(Character))); break;
            case ECaseMode::UpperCase: Symbol.push_back(static_cast<char>(std::toupper(Character))); break;
            default: Symbol.push_back(Character); break;
        }
    }

    std::string Key, Value;
    Key.reserve(DefaultIniKeyLength);
    Value.reserve(DefaultIniValueLength);

    if (ReaderConfig.IsAllowInteriorSpaceInKey)
    {
        size_t I = 0;
        for (; I < Symbols.size(); ++I)
        {
            std::string const& Token = Symbols[I];
            if (Token == "=")
            {
                if (Key.empty())
                {
                    Key.pop_back(); // 推出最后一个空格
                }
                ++I;
                break;
            }
            Key.append(Token).append(" ");
        }

        for (; I < Symbols.size(); ++I)
        {
            std::string const& Token = Symbols[I];
            Value.append(Token);
        }

    }
    else
    {
        if (Symbols.size() < 2 || Symbols[1] != "=")
        {
            return std::nullopt;
        }


        Key = Symbols[0];
        Value = Symbols[1];
    }


    return SectionType{};
}