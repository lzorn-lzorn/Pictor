//
// Created by Administrator on 2026/11/18.
//

#include "Ini.h"

#include <cctype>
#include <charconv>
#include <fstream>
#include <sstream>
#include <system_error>
#include <utility>

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

    /** @brief 判断 Value 是否属于编译期常量集合(分隔符 / 注释符 / Key 非法字符等)。 */
    template <typename T, std::size_t N>
    constexpr bool contains(const std::array<T, N>& Set, T Value) noexcept
    {
        for (const T Item : Set)
        {
            if (Item == Value)
            {
                return true;
            }
        }
        return false;
    }

    /** @brief 按 KeyCaseMode 对字母做大小写转换(假定传入的是字母)。 */
    char applyKeyCase(char Character) noexcept
    {
        switch (KeyCaseMode)
        {
            case ECaseMode::LowerCase: return static_cast<char>(std::tolower(static_cast<unsigned char>(Character)));
            case ECaseMode::UpperCase: return static_cast<char>(std::toupper(static_cast<unsigned char>(Character)));
            case ECaseMode::Default:
            default: return Character;
        }
    }

    /** @brief 是否空白字符(参数视为 unsigned char, 避免 std::isspace 对负值的未定义行为)。 */
    bool isWhitespace(char Character) noexcept
    {
        return std::isspace(static_cast<unsigned char>(Character)) != 0;
    }

    /** @brief 尝试把文本解析为 int64_t(必须完整消费整串)。 */
    bool tryParseInt(std::string_view Text, int64_t& Out) noexcept
    {
        const char* const Begin = Text.data();
        const char* const End = Text.data() + Text.size();
        int64_t Value = 0;
        const std::from_chars_result Result = std::from_chars(Begin, End, Value);
        if (Result.ec != std::errc{} || Result.ptr != End)
        {
            return false;
        }
        Out = Value;
        return true;
    }

    /** @brief 尝试把文本解析为 double(必须完整消费整串)。 */
    bool tryParseDouble(std::string_view Text, double& Out) noexcept
    {
        const char* const Begin = Text.data();
        const char* const End = Text.data() + Text.size();
        double Value = 0.0;
        const std::from_chars_result Result = std::from_chars(Begin, End, Value);
        if (Result.ec != std::errc{} || Result.ptr != End)
        {
            return false;
        }
        Out = Value;
        return true;
    }

    /** @brief Section 名只允许字母、数字与 SupportedExtraCharInSectionName。 */
    bool isValidSectionName(std::string_view Name) noexcept
    {
        for (const char Character : Name)
        {
            if (std::isalpha(static_cast<unsigned char>(Character)) != 0) continue;
            if (std::isdigit(static_cast<unsigned char>(Character)) != 0) continue;
            if (contains(SupportedExtraCharInSectionName, Character)) continue;
            return false;
        }
        return true;
    }

    /** @brief 依据文本内容推断存储类型: bool / int64_t / double / std::string。 */
    Any inferValue(std::string_view Text)
    {
        const std::string Lower = toLower(Text);
        if (Lower == "true" || Lower == "yes" || Lower == "on")
        {
            return Any(std::in_place_type<bool>, true);
        }
        if (Lower == "false" || Lower == "no" || Lower == "off")
        {
            return Any(std::in_place_type<bool>, false);
        }

        if (int64_t Int = 0; tryParseInt(Text, Int))
        {
            return Any(std::in_place_type<int64_t>, Int);
        }
        if (double Double = 0.0; tryParseDouble(Text, Double))
        {
            return Any(std::in_place_type<double>, Double);
        }
        return Any(std::in_place_type<std::string>, std::string(Text));
    }

    /** @brief 向 Section 插入/覆盖一个 Key-Value(重复 Key 后者覆盖, 保持首次出现顺序)。 */
    void upsertKeyValue(KeyValueType& Entries, IniValue Pair)
    {
        for (IniValue& Existing : Entries)
        {
            if (Existing.Key == Pair.Key)
            {
                Existing.Value = std::move(Pair.Value);
                return;
            }
        }
        Entries.push_back(std::move(Pair));
    }

    /** @brief 把 Any 中存储的值转成字符串表示(供 get 做字符串化)。 */
    std::string anyToString(const Any& Value)
    {
        if (const std::string* Ptr = Value.castPtr<std::string>())
        {
            return *Ptr;
        }
        if (const int64_t* Ptr = Value.castPtr<int64_t>())
        {
            return std::to_string(*Ptr);
        }
        if (const double* Ptr = Value.castPtr<double>())
        {
            char Buffer[64];
            const std::to_chars_result Result = std::to_chars(Buffer, Buffer + sizeof(Buffer), *Ptr);
            return std::string(Buffer, Result.ptr);
        }
        if (const bool* Ptr = Value.castPtr<bool>())
        {
            return *Ptr ? "true" : "false";
        }
        return {};
    }

} // namespace

Ini::Ini(std::string_view IniContent)
{
    parse(IniContent);
}

bool Ini::load(std::string_view FilePath)
{
    std::ifstream File(std::string(FilePath), std::ios::binary);
    if (!File.is_open())
    {
        Loaded = false;
        Error = "无法打开文件: ";
        Error += FilePath;
        return false;
    }

    std::ostringstream Buffer;
    Buffer << File.rdbuf();
    return parse(Buffer.str());
}

bool Ini::parse(std::string_view IniContent)
{
    Sections.clear();
    SectionOrder.clear();
    Error.clear();
    Loaded = false;

    std::string CurrentSection; // 当前 Section 名, 空表示尚未进入任何 Section
    bool Dropping = false;      // 是否处于"丢弃"状态(畸形 Section 名 + Drop 策略)
    std::string_view Remaining = IniContent;

    while (!Remaining.empty())
    {
        const std::size_t Newline = Remaining.find('\n');
        std::string_view Line = (Newline == std::string_view::npos) ? Remaining : Remaining.substr(0, Newline);
        Remaining = (Newline == std::string_view::npos) ? std::string_view{} : Remaining.substr(Newline + 1);

        Line = trim(Line);
        if (Line.empty())
        {
            continue; // 空行
        }
        if (contains(SupportedAnnotation, Line.front()))
        {
            continue; // 注释行
        }

        if (Line.front() == '[')
        {
            // ---------- Section 头 ----------
            const std::size_t Close = Line.find(']');
            if (Close == std::string_view::npos)
            {
                Error = "未闭合的 Section 头: ";
                Error += Line;
                return false;
            }

            // 闭合括号之后不允许再有多余内容(如 "[Section] trailing")。
            const std::string_view Rest = trim(Line.substr(Close + 1));
            if (!Rest.empty())
            {
                Error = "Section 头后存在多余内容: ";
                Error += Line;
                return false;
            }

            const std::string_view Name = trim(Line.substr(1, Close - 1));
            if (Name.empty() || !isValidSectionName(Name))
            {
                // Section 名解析失败: 按 SectionErrorHandle 策略处理, 不视为致命错误。
                if constexpr (SectionErrorHandle == ESectionErrorHandle::TransferToDefault)
                {
                    CurrentSection.assign(DefaultSectionName);
                    Dropping = false;
                    if (!hasSection(CurrentSection))
                    {
                        SectionOrder.push_back(CurrentSection);
                    }
                }
                else
                {
                    CurrentSection.clear();
                    Dropping = true;
                }
                continue;
            }

            CurrentSection.assign(Name);
            Dropping = false;
            if (!hasSection(CurrentSection))
            {
                SectionOrder.push_back(CurrentSection);
            }
            continue;
        }

        // ---------- Key-Value 行 ----------
        if (Dropping)
        {
            continue; // Drop 策略: 丢弃该 Section 下的 Key-Value, 不做解析
        }
        if (CurrentSection.empty())
        {
            Error = "Section 之外的 Key-Value: ";
            Error += Line;
            return false;
        }

        std::optional<IniValue> Pair = parseKeyValue(Line);
        if (!Pair.has_value())
        {
            Error = "非法的 Key-Value 行: ";
            Error += Line;
            return false;
        }

        upsertKeyValue(Sections[CurrentSection], std::move(*Pair));
    }

    Loaded = true;
    return true;
}

bool Ini::isLoaded() const noexcept
{
    return Loaded;
}

const std::string& Ini::error() const noexcept
{
    return Error;
}

const KeyValueType* Ini::findSection(std::string_view SectionName) const noexcept
{
    const auto It = Sections.find(std::string(SectionName));
    return It == Sections.end() ? nullptr : &It->second;
}

std::vector<std::string> Ini::sections() const
{
    return SectionOrder;
}

std::vector<std::string> Ini::keys(std::string_view SectionName) const
{
    std::vector<std::string> Result;
    const KeyValueType* Entries = findSection(SectionName);
    if (Entries == nullptr)
    {
        return Result;
    }
    Result.reserve(Entries->size());
    for (const IniValue& Entry : *Entries)
    {
        Result.push_back(Entry.Key);
    }
    return Result;
}

bool Ini::hasSection(std::string_view SectionName) const noexcept
{
    return Sections.find(std::string(SectionName)) != Sections.end();
}

bool Ini::hasKey(std::string_view SectionName, std::string_view Key) const noexcept
{
    const KeyValueType* Entries = findSection(SectionName);
    if (Entries == nullptr)
    {
        return false;
    }
    for (const IniValue& Entry : *Entries)
    {
        if (Entry.Key == Key)
        {
            return true;
        }
    }
    return false;
}

std::string Ini::get(std::string_view SectionName, std::string_view Key, std::string_view Default) const
{
    const KeyValueType* Entries = findSection(SectionName);
    if (Entries == nullptr)
    {
        return std::string(Default);
    }
    for (const IniValue& Entry : *Entries)
    {
        if (Entry.Key == Key)
        {
            return anyToString(Entry.Value);
        }
    }
    return std::string(Default);
}

int64_t Ini::getInt(std::string_view SectionName, std::string_view Key, int64_t Default) const
{
    const Any V = value(SectionName, Key);
    if (const int64_t* Ptr = V.castPtr<int64_t>()) return *Ptr;
    if (const double* Ptr = V.castPtr<double>()) return static_cast<int64_t>(*Ptr);
    if (const bool* Ptr = V.castPtr<bool>()) return *Ptr ? 1 : 0;
    if (const std::string* Ptr = V.castPtr<std::string>())
    {
        int64_t Out = 0;
        if (tryParseInt(*Ptr, Out)) return Out;
    }
    return Default;
}

double Ini::getDouble(std::string_view SectionName, std::string_view Key, double Default) const
{
    const Any V = value(SectionName, Key);
    if (const double* Ptr = V.castPtr<double>()) return *Ptr;
    if (const int64_t* Ptr = V.castPtr<int64_t>()) return static_cast<double>(*Ptr);
    if (const bool* Ptr = V.castPtr<bool>()) return *Ptr ? 1.0 : 0.0;
    if (const std::string* Ptr = V.castPtr<std::string>())
    {
        double Out = 0.0;
        if (tryParseDouble(*Ptr, Out)) return Out;
    }
    return Default;
}

bool Ini::getBool(std::string_view SectionName, std::string_view Key, bool Default) const
{
    const Any V = value(SectionName, Key);
    if (const bool* Ptr = V.castPtr<bool>()) return *Ptr;
    if (const int64_t* Ptr = V.castPtr<int64_t>()) return *Ptr != 0;
    if (const double* Ptr = V.castPtr<double>()) return *Ptr != 0.0;
    if (const std::string* Ptr = V.castPtr<std::string>())
    {
        bool Out = false;
        if (tryParseBool(*Ptr, Out)) return Out;
    }
    return Default;
}

Any Ini::value(std::string_view SectionName, std::string_view Key) const
{
    const KeyValueType* Entries = findSection(SectionName);
    if (Entries == nullptr)
    {
        return Any{};
    }
    for (const IniValue& Entry : *Entries)
    {
        if (Entry.Key == Key)
        {
            return Entry.Value;
        }
    }
    return Any{};
}

KeyValueType Ini::getSectionByName(std::string_view SectionName) const
{
    const KeyValueType* Entries = findSection(SectionName);
    return Entries == nullptr ? KeyValueType{} : *Entries;
}

bool Ini::isContain(std::string_view SectionName) const noexcept
{
    return hasSection(SectionName);
}

void Ini::merge(const Ini& Other)
{
    for (const std::string& SectionName : Other.SectionOrder)
    {
        if (!hasSection(SectionName))
        {
            SectionOrder.push_back(SectionName);
        }

        KeyValueType& Dst = Sections[SectionName];
        const KeyValueType& Src = Other.Sections.at(SectionName);
        for (const IniValue& Entry : Src)
        {
            upsertKeyValue(Dst, Entry);
        }
    }
}

std::optional<IniValue> Ini::parseKeyValue(std::string_view IniKeyValue)
{
    const std::size_t Length = IniKeyValue.size();
    std::size_t I = 0;

    // 跳过 Key 前导空白。
    while (I < Length && isWhitespace(IniKeyValue[I]))
    {
        ++I;
    }

    std::string Key;
    Key.reserve(DefaultIniKeyLength);

    bool HasDelimiter = false;
    bool PendingSpace = false; // 记录一个待补空格, 用于压缩 Key 中间连续空白

    // ---------- 解析 Key: 从行首到第一个分隔符('=' 或 ':') ----------
    for (; I < Length; ++I)
    {
        const char Character = IniKeyValue[I];

        // 分隔符: Key 结束。
        if (contains(SupportedEqualNotation, Character))
        {
            HasDelimiter = true;
            break;
        }

        // 分隔符之前遇到注释 -> 整行是注释, 非 Key-Value。
        if (contains(SupportedAnnotation, Character))
        {
            return std::nullopt;
        }

        // Key 中禁止的字符。
        if (contains(UnsupportedCharInKey, Character))
        {
            return std::nullopt;
        }

        // 空白。
        if (isWhitespace(Character))
        {
            if constexpr (EnableInteriorSpaceInKey)
            {
                // 连续空白压缩为一个空格; 尾随空白在遇到分隔符时被丢弃。
                if (!Key.empty())
                {
                    PendingSpace = true;
                }
                continue;
            }
            else
            {
                // 不允许 Key 中间有空格: 空白之后必须紧跟分隔符。
                std::size_t J = I;
                while (J < Length && isWhitespace(IniKeyValue[J]))
                {
                    ++J;
                }
                if (J >= Length || !contains(SupportedEqualNotation, IniKeyValue[J]))
                {
                    return std::nullopt;
                }
                HasDelimiter = true;
                I = J; // 下一轮 ++I 从分隔符之后继续
                break;
            }
        }

        // 普通字符。
        if (PendingSpace)
        {
            Key.push_back(' ');
            PendingSpace = false;
        }

        if (std::isalpha(static_cast<unsigned char>(Character)) != 0)
        {
            Key.push_back(applyKeyCase(Character));
        }
        else
        {
            if constexpr (EnableKeyNameValidationCheck)
            {
                if (std::isdigit(static_cast<unsigned char>(Character)) == 0 && Character != '_')
                {
                    return std::nullopt;
                }
            }
            Key.push_back(Character);
        }
    }

    // 行内没有 "=" / ":"。
    if (!HasDelimiter)
    {
        if constexpr (EnableStrictInspection)
        {
            return std::nullopt;
        }
        if (Key.empty())
        {
            return std::nullopt; // 空行 / 纯空白 / 纯注释
        }
    }

    // 空 Key, 例如 "= value"。
    if (Key.empty())
    {
        if constexpr (EnableStrictInspection)
        {
            return std::nullopt;
        }
        Key.assign(DefaultValue);
    }

    // ---------- 解析 Value ----------
    std::string Value;
    Value.reserve(DefaultIniValueLength);

    if (HasDelimiter)
    {
        // I 指向分隔符, 越过它并跳过 Value 前导空白。
        ++I;
        while (I < Length && isWhitespace(IniKeyValue[I]))
        {
            ++I;
        }

        bool InQuotes = false;
        std::size_t SignificantLength = 0; // 去掉尾部(引号外)空白后的有效长度

        for (; I < Length; ++I)
        {
            const char Character = IniKeyValue[I];

            // 转义(引号内外均生效)。
            if (Character == '\\')
            {
                if (I + 1 < Length)
                {
                    const char Next = IniKeyValue[I + 1];
                    if (Next == 'n')
                    {
                        Value.push_back('\n');
                        ++I;
                    }
                    else if (Next == 't')
                    {
                        Value.push_back('\t');
                        ++I;
                    }
                    else if (Next == '\"')
                    {
                        Value.push_back('\"');
                        ++I;
                    }
                    else
                    {
                        continue; // 未知转义: 丢弃反斜杠, Next 下一轮按普通字符处理
                    }
                }
                else
                {
                    break; // 行尾孤立反斜杠: 丢弃
                }
                SignificantLength = Value.size();
                continue;
            }

            // 引号切换(引号本身不进入 Value)。
            if (Character == '\"')
            {
                InQuotes = !InQuotes;
                continue;
            }

            // 引号外: 注释结束 Value; 再出现分隔符视为非法。
            if (!InQuotes)
            {
                if (contains(SupportedAnnotation, Character))
                {
                    break;
                }
                if (contains(SupportedEqualNotation, Character))
                {
                    return std::nullopt;
                }
            }

            Value.push_back(Character);
            if (InQuotes || !isWhitespace(Character))
            {
                SignificantLength = Value.size();
            }
        }

        Value.resize(SignificantLength);
    }
    else
    {
        // 非严格模式且无分隔符: 值取默认值。
        Value.assign(DefaultValue);
    }

    return IniValue{ std::move(Key), inferValue(Value) };
}
