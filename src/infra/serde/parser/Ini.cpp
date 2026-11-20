//
// Created by Administrator on 2026/11/18.
//

// getenv 在 MSVC 下被标记为 unsafe, 本文件只用其读取环境变量(不做写入)
#define _CRT_SECURE_NO_WARNINGS

#include "Ini.hpp"

#include "IniFile.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <istream>
#include <utility>

namespace
{
    constexpr std::size_t DefaultIniKeyLength = 64;
    constexpr std::size_t DefaultIniValueLength = 32;

    /**
     * @brief 修剪结果: 偏移 + 长度, 而非视图。
     * @notice 偏移/长度是纯值, 由调用方把范围应用到自己的存储上, 免疫悬垂。
     */
    struct TrimmedRange
    {
        std::size_t Offset = 0;
        std::size_t Length = 0;
    };

    bool isTrimChar(char Character) noexcept
    {
        return Character == ' ' || Character == '\t' || Character == '\r';
    }

    [[nodiscard]] TrimmedRange trimRange(std::string_view Text) noexcept
    {
        std::size_t Begin = 0;
        std::size_t End = Text.size();
        while (Begin < End && isTrimChar(Text[Begin]))
        {
            ++Begin;
        }
        while (End > Begin && isTrimChar(Text[End - 1]))
        {
            --End;
        }
        return TrimmedRange{ Begin, End - Begin };
    }

    [[nodiscard]] std::string_view applyTrim(std::string_view Text, TrimmedRange Range) noexcept
    {
        return Text.substr(Range.Offset, Range.Length);
    }

    bool isWhitespace(char Character) noexcept
    {
        return std::isspace(static_cast<unsigned char>(Character)) != 0;
    }

    /** @brief ASCII 大小写不敏感比较。 */
    bool equalsIgnoreCase(std::string_view A, std::string_view B) noexcept
    {
        if (A.size() != B.size())
        {
            return false;
        }
        for (std::size_t I = 0; I < A.size(); ++I)
        {
            const char CA = static_cast<char>(
                std::tolower(static_cast<unsigned char>(A[I])));
            const char CB = static_cast<char>(
                std::tolower(static_cast<unsigned char>(B[I])));
            if (CA != CB)
            {
                return false;
            }
        }
        return true;
    }

    /** @brief UTF-8 合法性校验(拒绝过长编码/代理区/超范围码点)。 */
    [[nodiscard]] bool isValidUtf8(std::string_view Text) noexcept
    {
        const std::size_t N = Text.size();
        std::size_t I = 0;
        while (I < N)
        {
            const unsigned char C = static_cast<unsigned char>(Text[I]);
            if (C < 0x80)
            {
                ++I;
                continue;
            }

            int Additional = 0;
            std::uint32_t MinCodePoint = 0;
            if ((C & 0xE0) == 0xC0)
            {
                Additional = 1;
                MinCodePoint = 0x80;
            }
            else if ((C & 0xF0) == 0xE0)
            {
                Additional = 2;
                MinCodePoint = 0x800;
            }
            else if ((C & 0xF8) == 0xF0)
            {
                Additional = 3;
                MinCodePoint = 0x10000;
            }
            else
            {
                return false;
            }

            if (I + static_cast<std::size_t>(Additional) >= N)
            {
                return false;
            }
            std::uint32_t CodePoint = C & (0x3F >> Additional);
            for (int K = 1; K <= Additional; ++K)
            {
                const unsigned char Cont = static_cast<unsigned char>(Text[I + K]);
                if ((Cont & 0xC0) != 0x80)
                {
                    return false;
                }
                CodePoint = (CodePoint << 6) | (Cont & 0x3F);
            }
            if (CodePoint < MinCodePoint || CodePoint > 0x10FFFF ||
                (CodePoint >= 0xD800 && CodePoint <= 0xDFFF))
            {
                return false;
            }
            I += static_cast<std::size_t>(Additional) + 1;
        }
        return true;
    }

    int hexValue(char Character) noexcept
    {
        if (Character >= '0' && Character <= '9')
        {
            return Character - '0';
        }
        if (Character >= 'a' && Character <= 'f')
        {
            return Character - 'a' + 10;
        }
        if (Character >= 'A' && Character <= 'F')
        {
            return Character - 'A' + 10;
        }
        return -1;
    }

    /** @brief 值/列表元素的通用转义处理: 消费 Line[I] 处的反斜杠转义并追加到 Out。 */
    void appendEscape(std::string_view Line, std::size_t& I, std::string& Out, bool& BadHex)
    {
        if (I + 1 >= Line.size())
        {
            return; // 行尾孤立反斜杠(由调用方按续行/丢弃处理)
        }
        switch (Line[I + 1])
        {
            case 'n': Out.push_back('\n'); ++I; break;
            case 't': Out.push_back('\t'); ++I; break;
            case 'r': Out.push_back('\r'); ++I; break;
            case 'a': Out.push_back('\a'); ++I; break;
            case 'b': Out.push_back('\b'); ++I; break;
            case 'f': Out.push_back('\f'); ++I; break;
            case 'v': Out.push_back('\v'); ++I; break;
            case '0': Out.push_back('\0'); ++I; break;
            case '"': Out.push_back('"'); ++I; break;
            case '\'': Out.push_back('\''); ++I; break;
            case '\\': Out.push_back('\\'); ++I; break;
            case ';': Out.push_back(';'); ++I; break; // 值内注释符转义
            case '#': Out.push_back('#'); ++I; break;
            case '=': Out.push_back('='); ++I; break; // 值内分隔符转义
            case ':': Out.push_back(':'); ++I; break;
            case 'x':
            {
                std::size_t J = I + 2;
                int Value = 0;
                int Digits = 0;
                while (J < Line.size() && Digits < 2 && hexValue(Line[J]) >= 0)
                {
                    Value = Value * 16 + hexValue(Line[J]);
                    ++J;
                    ++Digits;
                }
                if (Digits == 0)
                {
                    BadHex = true;
                    return;
                }
                Out.push_back(static_cast<char>(Value));
                I = J - 1;
                break;
            }
            default: break; // 未知转义: 丢弃反斜杠, 下一字符按普通字符处理
        }
    }

    /** @brief 列表内部 "[a, "b,c", d]" -> {"a", "b,c", "d"}(失败返回 false, 整体按字符串)。 */
    [[nodiscard]] bool parseListElements(std::string_view Inner, IniStringList& Out)
    {
        const TrimmedRange Whole = trimRange(Inner);
        if (Whole.Length == 0)
        {
            return true; // "[]" -> 空列表
        }

        std::string Element;
        bool InQuotes = false;
        bool BadHex = false;
        for (std::size_t I = 0; I < Inner.size(); ++I)
        {
            const char C = Inner[I];
            if (C == '\\')
            {
                appendEscape(Inner, I, Element, BadHex);
                if (BadHex)
                {
                    return false;
                }
                continue;
            }
            if (C == '"')
            {
                InQuotes = !InQuotes;
                continue;
            }
            if (C == ',' && !InQuotes)
            {
                Out.push_back(std::string(applyTrim(Element, trimRange(Element))));
                Element.clear();
                continue;
            }
            Element.push_back(C);
        }
        if (InQuotes)
        {
            return false;
        }
        Out.push_back(std::string(applyTrim(Element, trimRange(Element))));
        return true;
    }

    /** @brief 值类型推断: bool / int64_t / double / std::string / 列表。 */
    IniVariant inferValue(std::string Value, bool EnableLists)
    {
        if (EnableLists && Value.size() >= 2 && Value.front() == '[' && Value.back() == ']')
        {
            IniStringList List;
            if (parseListElements(std::string_view(Value).substr(1, Value.size() - 2), List))
            {
                return List;
            }
            // 列表解析失败 -> 作为普通字符串
        }

        const std::string_view V = Value;
        if (equalsIgnoreCase(V, "true") || equalsIgnoreCase(V, "yes") || equalsIgnoreCase(V, "on"))
        {
            return true;
        }
        if (equalsIgnoreCase(V, "false") || equalsIgnoreCase(V, "no") || equalsIgnoreCase(V, "off"))
        {
            return false;
        }
        if (std::int64_t Int = 0; ini_detail::tryParseInt(V, Int))
        {
            return Int;
        }
        if (double Double = 0.0; ini_detail::tryParseDouble(V, Double))
        {
            return Double;
        }
        return std::move(Value);
    }

    /** @brief 插入/覆盖条目; AsList 开启时重复键收集为列表。 */
    void upsertEntry(IniSection& Section, IniEntry Entry, bool AsList)
    {
        for (IniEntry& Existing : Section.Entries)
        {
            if (Existing.Key != Entry.Key)
            {
                continue;
            }
            if (!AsList)
            {
                Existing.Value = std::move(Entry.Value); // 覆盖, 保留原注释
                return;
            }

            IniStringList List;
            if (const auto* Old = std::get_if<IniStringList>(&Existing.Value))
            {
                List = *Old;
            }
            else
            {
                List.push_back(ini_detail::variantToString(Existing.Value));
            }
            if (const auto* New = std::get_if<IniStringList>(&Entry.Value))
            {
                for (const std::string& Element : *New)
                {
                    List.push_back(Element);
                }
            }
            else
            {
                List.push_back(ini_detail::variantToString(Entry.Value));
            }
            Existing.Value = std::move(List);
            return;
        }
        Section.Entries.push_back(std::move(Entry));
    }

    /** @brief ${VAR} 环境变量插值(失败/严格模式返回错误)。 */
    std::optional<IniError> interpolate(std::string& Value, bool Strict)
    {
        std::string Out;
        std::size_t Pos = 0;
        while (true)
        {
            const std::size_t Start = Value.find("${", Pos);
            if (Start == std::string::npos)
            {
                Out.append(Value, Pos, std::string::npos);
                break;
            }
            Out.append(Value, Pos, Start - Pos);
            const std::size_t End = Value.find('}', Start + 2);
            if (End == std::string::npos)
            {
                Out.append(Value, Start, std::string::npos); // 未闭合 -> 原样保留
                break;
            }
            const std::string Name(Value.substr(Start + 2, End - Start - 2));
            if (const char* Env = std::getenv(Name.c_str()))
            {
                Out += Env;
            }
            else if (Strict)
            {
                return IniError{ IniError::Code::UnresolvedInterpolation, 0, Name };
            }
            else
            {
                Out.append(Value, Start, End - Start + 1); // 未定义 -> 保留原文
            }
            Pos = End + 1;
        }
        Value = std::move(Out);
        return std::nullopt;
    }

    /** @brief Section 名是否需要引号包裹(含策略附加字符集)。 */
    [[nodiscard]] bool needsSectionQuoting(std::string_view Name,
                                           std::string_view ExtraSectionNameChars) noexcept
    {
        if (Name.empty())
        {
            return true;
        }
        if (isTrimChar(Name.front()) || isTrimChar(Name.back()))
        {
            return true;
        }
        for (const char C : Name)
        {
            if (std::isalnum(static_cast<unsigned char>(C)) != 0)
            {
                continue;
            }
            if (ExtraSectionNameChars.find(C) != std::string_view::npos)
            {
                continue;
            }
            return true;
        }
        return false;
    }

    /** @brief 引号包裹 Section 名(转义 \ 与 ")。 */
    std::string quoteSectionName(std::string_view Name)
    {
        std::string Out = "\"";
        for (const char C : Name)
        {
            if (C == '\\' || C == '"')
            {
                Out.push_back('\\');
            }
            Out.push_back(C);
        }
        Out.push_back('"');
        return Out;
    }

    /** @brief variant -> 可写入文本(字符串按需引号, 列表加方括号)。 */
    std::string valueToIniText(const IniVariant& Value)
    {
        return std::visit(
            [](const auto& Arg) -> std::string
            {
                using T = std::decay_t<decltype(Arg)>;
                if constexpr (std::is_same_v<T, std::string>)
                {
                    return ini_detail::valueToText(Arg);
                }
                else if constexpr (std::is_same_v<T, IniStringList>)
                {
                    return ini_detail::listToString(Arg);
                }
                else
                {
                    return ini_detail::scalarToString(Arg);
                }
            },
            Value);
    }

    void appendLines(std::string& Out, const std::vector<std::string>& Lines)
    {
        for (const std::string& Line : Lines)
        {
            Out += Line;
            Out.push_back('\n');
        }
    }

} // namespace

// ============================================================================
// IniError
// ============================================================================

std::string IniError::asString(const IniError& Error)
{
    std::string Out;
    if (Error.LineNumber != 0)
    {
        Out += "line ";
        Out += std::to_string(Error.LineNumber);
        Out += ": ";
    }

    switch (Error.Code)
    {
        case IniError::Code::CannotOpenFile: Out += "cannot open file"; break;
        case IniError::Code::ReadFailed: Out += "failed to read file"; break;
        case IniError::Code::WriteFailed: Out += "failed to write file"; break;
        case IniError::Code::InvalidUtf8: Out += "input is not valid UTF-8"; break;
        case IniError::Code::UnclosedSectionHeader: Out += "unclosed section header"; break;
        case IniError::Code::TrailingContentInSectionHeader: Out += "trailing content after section header"; break;
        case IniError::Code::InvalidSectionName: Out += "invalid section name"; break;
        case IniError::Code::UnclosedQuotedSectionName: Out += "unclosed quoted section name"; break;
        case IniError::Code::InvalidParentSectionName: Out += "invalid parent section name"; break;
        case IniError::Code::CommentInKey: Out += "comment character in key"; break;
        case IniError::Code::SecondDelimiterInValue: Out += "second delimiter in value"; break;
        case IniError::Code::InvalidEscapeSequence: Out += "invalid escape sequence"; break;
        case IniError::Code::KeyValueOutsideSection: Out += "key-value outside any section"; break;
        case IniError::Code::EmptyKey: Out += "empty key"; break;
        case IniError::Code::MissingDelimiter: Out += "missing delimiter"; break;
        case IniError::Code::DigitLeadingKey: Out += "key must not start with a digit"; break;
        case IniError::Code::InvalidKeyCharacter: Out += "invalid character in key"; break;
        case IniError::Code::InteriorSpaceInKey: Out += "whitespace in key is not allowed"; break;
        case IniError::Code::UnresolvedInterpolation: Out += "unresolved interpolation variable"; break;
        case IniError::Code::UnsupportedValueType: Out += "unsupported value type"; break;
        case IniError::Code::None:
        default: Out += "unknown error"; break;
    }

    if (!Error.Detail.empty())
    {
        Out += ": ";
        Out += Error.Detail;
    }
    return Out;
}

// ============================================================================
// IniChecker
// ============================================================================

IniLineKind IniChecker::classify(std::string_view Line) const noexcept
{
    if (Line.empty())
    {
        return IniLineKind::Blank;
    }
    if (Policy.AnnotationChars.find(Line.front()) != std::string_view::npos)
    {
        return IniLineKind::Comment;
    }
    if (Line.front() == '[')
    {
        return IniLineKind::SectionHeader;
    }
    return IniLineKind::KeyValue;
}

std::optional<IniError> IniChecker::checkSectionName(std::string_view Name) const
{
    if (Name.empty())
    {
        return IniError{ IniError::Code::InvalidSectionName, 0, std::string(Name) };
    }
    if (Policy.AllowAnySectionName)
    {
        return std::nullopt;
    }
    for (const char Character : Name)
    {
        if (std::isalpha(static_cast<unsigned char>(Character)) != 0)
        {
            continue;
        }
        if (std::isdigit(static_cast<unsigned char>(Character)) != 0)
        {
            continue;
        }
        if (Policy.ExtraSectionNameChars.find(Character) != std::string_view::npos)
        {
            continue;
        }
        return IniError{ IniError::Code::InvalidSectionName, 0, std::string(Name) };
    }
    return std::nullopt;
}

std::optional<IniError> IniChecker::checkKeyValue(const IniKeyValueRaw& Raw) const
{
    if (Raw.HasInteriorSpace && !Policy.InteriorSpaceInKey)
    {
        return IniError{ IniError::Code::InteriorSpaceInKey, 0, Raw.Key };
    }

    for (const char Character : Raw.Key)
    {
        if (Policy.ForbiddenKeyChars.find(Character) != std::string_view::npos)
        {
            return IniError{ IniError::Code::InvalidKeyCharacter, 0, Raw.Key };
        }
    }

    if (!Raw.Key.empty() && Policy.KeyNameValidation)
    {
        if (std::isdigit(static_cast<unsigned char>(Raw.Key.front())) != 0)
        {
            return IniError{ IniError::Code::DigitLeadingKey, 0, Raw.Key };
        }
        for (const char Character : Raw.Key)
        {
            if (std::isalpha(static_cast<unsigned char>(Character)) != 0)
            {
                continue;
            }
            if (std::isdigit(static_cast<unsigned char>(Character)) != 0)
            {
                continue;
            }
            if (Character == '_')
            {
                continue;
            }
            if (isWhitespace(Character))
            {
                continue;
            }
            return IniError{ IniError::Code::InvalidKeyCharacter, 0, Raw.Key };
        }
    }

    if (Policy.StrictInspection)
    {
        if (!Raw.HasDelimiter)
        {
            return IniError{ IniError::Code::MissingDelimiter, 0, Raw.Key };
        }
        if (Raw.Key.empty())
        {
            return IniError{ IniError::Code::EmptyKey, 0, {} };
        }
    }
    else if (Raw.Key.empty() && !Raw.HasDelimiter)
    {
        return IniError{ IniError::Code::EmptyKey, 0, {} };
    }

    return std::nullopt;
}

void IniChecker::finalize(IniKeyValueRaw& Raw) const
{
    if (!Raw.HasDelimiter)
    {
        Raw.Value.assign(Policy.DefaultValue);
    }
    if (Raw.Key.empty())
    {
        Raw.Key.assign(Policy.DefaultValue);
    }
    if (Policy.KeyCaseMode != ECaseMode::Default)
    {
        Raw.Key = normalizeKey(Raw.Key);
    }
}

std::string IniChecker::normalizeKey(std::string_view Key) const
{
    std::string Out;
    Out.reserve(Key.size());
    for (const char Character : Key)
    {
        const unsigned char C = static_cast<unsigned char>(Character);
        if (std::isalpha(C) != 0)
        {
            Out.push_back(Policy.KeyCaseMode == ECaseMode::UpperCase
                              ? static_cast<char>(std::toupper(C))
                              : static_cast<char>(std::tolower(C)));
        }
        else
        {
            Out.push_back(Character);
        }
    }
    return Out;
}

// ============================================================================
// 纯解析: 只做语法切分, 不含任何策略判断
// ============================================================================

std::expected<IniKeyValueRaw, IniError> parseIniKeyValue(std::string_view Line,
                                                         std::string_view DelimiterChars,
                                                         std::string_view AnnotationChars)
{
    const std::size_t Length = Line.size();
    std::size_t I = 0;

    while (I < Length && isWhitespace(Line[I]))
    {
        ++I;
    }

    IniKeyValueRaw Raw;
    Raw.Key.reserve(DefaultIniKeyLength);
    Raw.Value.reserve(DefaultIniValueLength);

    bool PendingSpace = false;

    // ---------- 切 Key ----------
    for (; I < Length; ++I)
    {
        const char Character = Line[I];

        if (DelimiterChars.find(Character) != std::string_view::npos)
        {
            Raw.HasDelimiter = true;
            break;
        }
        if (AnnotationChars.find(Character) != std::string_view::npos)
        {
            return std::unexpected(
                IniError{ IniError::Code::CommentInKey, 0, std::string(Line) });
        }
        if (isWhitespace(Character))
        {
            if (!Raw.Key.empty())
            {
                PendingSpace = true;
            }
            continue;
        }
        if (PendingSpace)
        {
            Raw.Key.push_back(' ');
            PendingSpace = false;
            Raw.HasInteriorSpace = true;
        }
        Raw.Key.push_back(Character);
    }

    // ---------- 切 Value ----------
    if (Raw.HasDelimiter)
    {
        ++I;
        while (I < Length && isWhitespace(Line[I]))
        {
            ++I;
        }

        bool InQuotes = false;
        std::size_t SignificantLength = 0;

        // 列表候选: 值以 '[' 开头 -> 原样拷贝(引号/转义交给列表解析),
        // 仅按引号感知剥离尾注释; 分隔符不再视为错误(元素内可含 = / :)
        if (I < Length && Line[I] == '[')
        {
            for (; I < Length; ++I)
            {
                const char Character = Line[I];
                if (Character == '\\' && I + 1 < Length)
                {
                    Raw.Value.push_back(Character);
                    Raw.Value.push_back(Line[I + 1]);
                    ++I;
                    SignificantLength = Raw.Value.size();
                    continue;
                }
                if (Character == '"')
                {
                    InQuotes = !InQuotes;
                    Raw.Value.push_back(Character);
                    SignificantLength = Raw.Value.size();
                    continue;
                }
                if (!InQuotes && AnnotationChars.find(Character) != std::string_view::npos)
                {
                    break; // 尾注释
                }
                Raw.Value.push_back(Character);
                if (InQuotes || !isWhitespace(Character))
                {
                    SignificantLength = Raw.Value.size();
                }
            }
            Raw.Value.resize(SignificantLength);
            return Raw;
        }

        for (; I < Length; ++I)
        {
            const char Character = Line[I];

            if (Character == '\\')
            {
                if (I + 1 >= Length)
                {
                    // 行尾孤立反斜杠: 续行标志(是否生效由策略决定);
                    // 反斜杠前的空白是值的一部分, 一并保留
                    SignificantLength = Raw.Value.size();
                    Raw.HasLineContinuation = true;
                    break;
                }
                bool BadHex = false;
                appendEscape(Line, I, Raw.Value, BadHex);
                if (BadHex)
                {
                    return std::unexpected(
                        IniError{ IniError::Code::InvalidEscapeSequence, 0, std::string(Line) });
                }
                SignificantLength = Raw.Value.size();
                continue;
            }

            if (Character == '"')
            {
                InQuotes = !InQuotes;
                continue;
            }

            if (!InQuotes)
            {
                if (AnnotationChars.find(Character) != std::string_view::npos)
                {
                    break;
                }
                if (DelimiterChars.find(Character) != std::string_view::npos)
                {
                    return std::unexpected(
                        IniError{ IniError::Code::SecondDelimiterInValue, 0, std::string(Line) });
                }
            }

            Raw.Value.push_back(Character);
            if (InQuotes || !isWhitespace(Character))
            {
                SignificantLength = Raw.Value.size();
            }
        }

        Raw.Value.resize(SignificantLength);
    }

    return Raw;
}

// ============================================================================
// Ini
// ============================================================================

struct Ini::ParseState
{
    std::string CurrentSection;             // 空 = 尚未进入任何 Section
    bool Dropping = false;                  // Drop 策略: 丢弃该 Section 下的一切
    std::vector<std::string> PendingLines;  // 注释/空行, 附给下一个结构元素
    IniKeyValueRaw PendingRaw;              // 续行中的键值对
    bool HasPendingRaw = false;
    std::size_t PendingRawLine = 0;
    bool HasContent = false;
};

std::expected<Ini, IniError> Ini::fromString(std::string_view IniContent)
{
    Ini Result;
    if (std::expected<void, IniError> Parsed = Result.parse(IniContent); !Parsed)
    {
        return std::unexpected(Parsed.error());
    }
    return Result;
}

std::expected<Ini, IniError> Ini::fromFile(std::string_view FilePath)
{
    std::expected<std::string, IniError> Content = readIniFile(FilePath);
    if (!Content)
    {
        return std::unexpected(Content.error());
    }
    Ini Result;
    if (std::expected<void, IniError> Parsed = Result.parse(*Content); !Parsed)
    {
        return std::unexpected(Parsed.error());
    }
    Result.SourcePath.assign(FilePath);
    return Result;
}

std::expected<void, IniError> Ini::load(std::string_view FilePath)
{
    std::expected<std::string, IniError> Content = readIniFile(FilePath);
    if (!Content)
    {
        return fail(Content.error());
    }
    std::expected<void, IniError> Parsed = parse(*Content);
    if (Parsed)
    {
        SourcePath.assign(FilePath);
    }
    return Parsed;
}

std::expected<void, IniError> Ini::loadMapped(std::string_view FilePath)
{
    std::expected<IniMappedFile, IniError> Mapped = mapIniFile(FilePath);
    if (!Mapped)
    {
        return fail(Mapped.error());
    }
    std::expected<void, IniError> Parsed = parse(Mapped->view());
    if (Parsed)
    {
        SourcePath.assign(FilePath);
    }
    return Parsed;
}

std::expected<void, IniError> Ini::parse(std::string_view IniContent)
{
    Sections.clear();
    SectionOrder.clear();
    LineBuffer.clear();
    TailLines.clear();
    LastError = IniError{};
    Loaded = false;

    if (Checker.policy().ValidateUtf8 && !isValidUtf8(IniContent))
    {
        return fail(IniError{ IniError::Code::InvalidUtf8, 0, {} });
    }

    ParseState State;
    std::string_view Remaining = IniContent;
    std::size_t LineNumber = 0;
    while (!Remaining.empty())
    {
        ++LineNumber;
        const std::size_t Newline = Remaining.find('\n');
        std::string_view RawLine =
            (Newline == std::string_view::npos) ? Remaining : Remaining.substr(0, Newline);
        Remaining =
            (Newline == std::string_view::npos) ? std::string_view{} : Remaining.substr(Newline + 1);

        if (!RawLine.empty() && RawLine.back() == '\r')
        {
            RawLine.remove_suffix(1); // CRLF
        }
        if (std::optional<IniError> Error = feedLine(RawLine, LineNumber, State))
        {
            return fail(*Error);
        }
    }
    if (std::optional<IniError> Error = finishParse(State))
    {
        return fail(*Error);
    }
    return {};
}

std::expected<void, IniError> Ini::parse(std::istream& Stream)
{
    Sections.clear();
    SectionOrder.clear();
    LineBuffer.clear();
    TailLines.clear();
    LastError = IniError{};
    Loaded = false;

    ParseState State;
    std::string Line;
    std::size_t LineNumber = 0;
    while (std::getline(Stream, Line))
    {
        ++LineNumber;
        if (!Line.empty() && Line.back() == '\r')
        {
            Line.pop_back();
        }
        if (Checker.policy().ValidateUtf8 && !isValidUtf8(Line))
        {
            return fail(IniError{ IniError::Code::InvalidUtf8, LineNumber, {} });
        }
        if (std::optional<IniError> Error = feedLine(Line, LineNumber, State))
        {
            return fail(*Error);
        }
    }
    if (std::optional<IniError> Error = finishParse(State))
    {
        return fail(*Error);
    }
    return {};
}

std::optional<IniError> Ini::feedLine(std::string_view RawLine, std::size_t LineNumber,
                                      ParseState& State)
{
    LineBuffer.push_back(std::string(RawLine));

    // ---------- 续行内容: 原样拼接 ----------
    if (State.HasPendingRaw)
    {
        const std::string_view Line = applyTrim(RawLine, trimRange(RawLine));
        if (Checker.classify(Line) == IniLineKind::SectionHeader)
        {
            // 防呆: 续行遇到新的 Section 头 -> 终止续行(去掉末尾空白), 本行按普通行重新处理
            const TrimmedRange Range = trimRange(State.PendingRaw.Value);
            State.PendingRaw.Value.resize(Range.Offset + Range.Length);
            if (std::optional<IniError> Error = commitEntry(State.PendingRaw, State.PendingRawLine, State))
            {
                return Error;
            }
            State.HasPendingRaw = false;
            // 落入下方普通处理
        }
        else
        {
            if (!Line.empty() && Line.back() == '\\')
            {
                State.PendingRaw.Value.append(Line.substr(0, Line.size() - 1));
                return std::nullopt; // 继续续行
            }
            State.PendingRaw.Value.append(Line);
            if (std::optional<IniError> Error = commitEntry(State.PendingRaw, State.PendingRawLine, State))
            {
                return Error;
            }
            State.HasPendingRaw = false;
            return std::nullopt;
        }
    }

    const std::string_view Line = applyTrim(RawLine, trimRange(RawLine));
    if (Line.empty())
    {
        if (!State.Dropping)
        {
            State.PendingLines.push_back(std::string(RawLine));
        }
        return std::nullopt;
    }

    if (State.Dropping && Checker.classify(Line) != IniLineKind::SectionHeader)
    {
        return std::nullopt; // Drop 策略: 该 Section 下的一切被忽略
    }

    switch (Checker.classify(Line))
    {
        case IniLineKind::Comment:
        case IniLineKind::Blank:
            State.PendingLines.push_back(std::string(RawLine));
            return std::nullopt;

        case IniLineKind::SectionHeader:
        {
            std::string Name;
            std::optional<std::string> Parent;
            bool Quoted = false;
            std::size_t Pos = 1;

            if (Pos < Line.size() && Line[Pos] == '"')
            {
                // 引号包裹的 Section 名: 支持 \" 与 \\ 转义
                Quoted = true;
                ++Pos;
                bool Closed = false;
                while (Pos < Line.size())
                {
                    const char C = Line[Pos];
                    if (C == '\\' && Pos + 1 < Line.size() &&
                        (Line[Pos + 1] == '"' || Line[Pos + 1] == '\\'))
                    {
                        Name.push_back(Line[Pos + 1]);
                        Pos += 2;
                        continue;
                    }
                    if (C == '"')
                    {
                        Closed = true;
                        ++Pos;
                        break;
                    }
                    Name.push_back(C);
                    ++Pos;
                }
                if (!Closed)
                {
                    return IniError{ IniError::Code::UnclosedQuotedSectionName, LineNumber,
                                     std::string(Line) };
                }

                const std::string_view Tail = applyTrim(Line.substr(Pos), trimRange(Line.substr(Pos)));
                if (Checker.policy().EnableSectionInheritance && Tail.size() > 1 &&
                    Tail.front() == ':' && Tail.back() == ']')
                {
                    const std::string_view ParentRaw =
                        applyTrim(Tail.substr(1, Tail.size() - 2),
                                  trimRange(Tail.substr(1, Tail.size() - 2)));
                    if (ParentRaw.empty() || Checker.checkSectionName(ParentRaw).has_value())
                    {
                        return IniError{ IniError::Code::InvalidParentSectionName, LineNumber,
                                         std::string(Line) };
                    }
                    Parent = std::string(ParentRaw);
                }
                else if (Tail != "]")
                {
                    return IniError{ IniError::Code::TrailingContentInSectionHeader, LineNumber,
                                     std::string(Line) };
                }
            }
            else
            {
                const std::size_t Close = Line.find(']');
                if (Close == std::string_view::npos)
                {
                    return IniError{ IniError::Code::UnclosedSectionHeader, LineNumber,
                                     std::string(Line) };
                }
                const std::string_view Tail = applyTrim(Line.substr(Close + 1),
                                                        trimRange(Line.substr(Close + 1)));
                if (!Tail.empty())
                {
                    return IniError{ IniError::Code::TrailingContentInSectionHeader, LineNumber,
                                     std::string(Line) };
                }

                std::string_view NameRaw = applyTrim(Line.substr(1, Close - 1),
                                                     trimRange(Line.substr(1, Close - 1)));
                if (Checker.policy().EnableSectionInheritance)
                {
                    const std::size_t Colon = NameRaw.find(':');
                    if (Colon != std::string_view::npos)
                    {
                        const std::string_view ParentRaw =
                            applyTrim(NameRaw.substr(Colon + 1), trimRange(NameRaw.substr(Colon + 1)));
                        NameRaw = applyTrim(NameRaw.substr(0, Colon), trimRange(NameRaw.substr(0, Colon)));
                        if (ParentRaw.empty() || Checker.checkSectionName(ParentRaw).has_value())
                        {
                            return IniError{ IniError::Code::InvalidParentSectionName, LineNumber,
                                             std::string(Line) };
                        }
                        Parent = std::string(ParentRaw);
                    }
                }
                Name.assign(NameRaw);
            }

            // 名称检查(引号名只要求非空)
            const bool NameInvalid = Quoted ? Name.empty() : Checker.checkSectionName(Name).has_value();
            if (NameInvalid)
            {
                State.PendingLines.clear();
                if (Checker.policy().SectionErrorHandle == ESectionErrorHandle::TransferToDefault)
                {
                    State.CurrentSection.assign(Checker.policy().DefaultSectionName);
                    State.Dropping = false;
                    if (!Sections.contains(State.CurrentSection))
                    {
                        SectionOrder.push_back(State.CurrentSection);
                    }
                }
                else
                {
                    State.CurrentSection.clear();
                    State.Dropping = true;
                }
                return std::nullopt;
            }

            State.CurrentSection = Name;
            State.Dropping = false;
            const bool Exists = Sections.contains(State.CurrentSection);
            IniSection& Sec = Sections[State.CurrentSection];
            if (!Exists)
            {
                Sec.HeaderLines = std::move(State.PendingLines);
                Sec.Parent = std::move(Parent);
                SectionOrder.push_back(State.CurrentSection);
            }
            else if (!Sec.Parent.has_value() && Parent.has_value())
            {
                Sec.Parent = std::move(Parent); // 重开 Section 时采纳首次出现的父声明
            }
            State.PendingLines.clear();
            State.HasContent = true;
            return std::nullopt;
        }

        case IniLineKind::KeyValue:
        {
            if (State.CurrentSection.empty())
            {
                return IniError{ IniError::Code::KeyValueOutsideSection, LineNumber,
                                 std::string(Line) };
            }

            std::expected<IniKeyValueRaw, IniError> Raw =
                parseIniKeyValue(Line, Checker.policy().DelimiterChars,
                                 Checker.policy().AnnotationChars);
            if (!Raw)
            {
                Raw.error().LineNumber = LineNumber;
                return Raw.error();
            }

            if (Raw->HasLineContinuation && Checker.policy().EnableLineContinuation)
            {
                State.PendingRaw = std::move(*Raw);
                State.HasPendingRaw = true;
                State.PendingRawLine = LineNumber;
                return std::nullopt;
            }
            return commitEntry(*Raw, LineNumber, State);
        }
    }
    return std::nullopt;
}

std::optional<IniError> Ini::commitEntry(IniKeyValueRaw& Raw, std::size_t LineNumber,
                                         ParseState& State)
{
    if (std::optional<IniError> Check = Checker.checkKeyValue(Raw))
    {
        Check->LineNumber = LineNumber;
        return Check;
    }
    Checker.finalize(Raw);

    if (Checker.policy().EnableInterpolation)
    {
        if (std::optional<IniError> Error = interpolate(Raw.Value, Checker.policy().StrictInterpolation))
        {
            Error->LineNumber = LineNumber;
            return Error;
        }
    }

    IniEntry Entry;
    Entry.Key = std::move(Raw.Key);
    Entry.Value = inferValue(std::move(Raw.Value), Checker.policy().EnableListValues);
    Entry.LeadingLines = std::move(State.PendingLines);
    upsertEntry(Sections[State.CurrentSection], std::move(Entry),
                Checker.policy().DuplicateKeysAsList);
    State.HasContent = true;
    return std::nullopt;
}

std::optional<IniError> Ini::finishParse(ParseState& State)
{
    if (State.HasPendingRaw)
    {
        if (std::optional<IniError> Error = commitEntry(State.PendingRaw, State.PendingRawLine, State))
        {
            return Error;
        }
        State.HasPendingRaw = false;
    }
    TailLines = std::move(State.PendingLines);
    Loaded = true;
    return std::nullopt;
}

std::expected<void, IniError> Ini::save(std::string_view FilePath) const
{
    return writeIniFile(FilePath, serialize());
}

std::expected<void, IniError> Ini::save() const
{
    if (SourcePath.empty())
    {
        return std::unexpected(IniError{ IniError::Code::CannotOpenFile, 0,
                                         "no source path (call save(FilePath))" });
    }
    return save(SourcePath);
}

bool Ini::isLoaded() const noexcept
{
    return Loaded;
}

const IniError& Ini::lastError() const noexcept
{
    return LastError;
}

const std::vector<std::string>& Ini::lineBuffer() const noexcept
{
    return LineBuffer;
}

std::vector<std::string> Ini::releaseLineBuffer()
{
    std::vector<std::string> Released;
    Released.swap(LineBuffer);
    return Released;
}

std::string Ini::serialize() const
{
    std::string Out;
    for (const std::string& Name : SectionOrder)
    {
        const auto It = Sections.find(Name);
        if (It == Sections.end())
        {
            continue;
        }
        const IniSection& Sec = It->second;

        appendLines(Out, Sec.HeaderLines);
        Out.push_back('[');
        if (needsSectionQuoting(Name, Checker.policy().ExtraSectionNameChars))
        {
            Out += quoteSectionName(Name);
        }
        else
        {
            Out += Name;
        }
        if (Sec.Parent.has_value())
        {
            Out += " : ";
            Out += *Sec.Parent;
        }
        Out += "]\n";

        for (const IniEntry& Entry : Sec.Entries)
        {
            appendLines(Out, Entry.LeadingLines);
            Out += Entry.Key;
            Out += " = ";
            Out += valueToIniText(Entry.Value);
            Out.push_back('\n');
        }
    }
    appendLines(Out, TailLines);
    return Out;
}

std::string Ini::flash()
{
    const std::string Text = serialize();

    LineBuffer.clear();
    std::string_view Remaining = Text;
    while (!Remaining.empty())
    {
        const std::size_t Newline = Remaining.find('\n');
        LineBuffer.emplace_back(
            (Newline == std::string_view::npos) ? Remaining : Remaining.substr(0, Newline));
        Remaining =
            (Newline == std::string_view::npos) ? std::string_view{} : Remaining.substr(Newline + 1);
    }
    return Text;
}

const IniSection* Ini::findSection(std::string_view SectionName) const noexcept
{
    const auto It = Sections.find(SectionName);
    return It == Sections.end() ? nullptr : &It->second;
}

IniSection* Ini::findSection(std::string_view SectionName) noexcept
{
    const auto It = Sections.find(SectionName);
    return It == Sections.end() ? nullptr : &It->second;
}

std::size_t Ini::findKeyIndex(const KeyValueType& Entries, std::string_view Key) const noexcept
{
    if (Checker.policy().KeyCaseMode == ECaseMode::Default)
    {
        for (std::size_t Index = 0; Index < Entries.size(); ++Index)
        {
            if (Entries[Index].Key == Key)
            {
                return Index;
            }
        }
        return std::string_view::npos;
    }

    const std::string Normalized = Checker.normalizeKey(Key);
    for (std::size_t Index = 0; Index < Entries.size(); ++Index)
    {
        if (Entries[Index].Key == Normalized)
        {
            return Index;
        }
    }
    return std::string_view::npos;
}

const IniEntry* Ini::findEntry(std::string_view SectionName, std::string_view Key) const noexcept
{
    // 沿 [Child : Parent] 继承链查找, 带环保护
    std::vector<std::string_view> Visited;
    std::string_view Current = SectionName;
    while (true)
    {
        for (const std::string_view Seen : Visited)
        {
            if (Seen == Current)
            {
                return nullptr; // 继承环
            }
        }
        Visited.push_back(Current);

        const auto It = Sections.find(Current);
        if (It == Sections.end())
        {
            return nullptr;
        }
        const std::size_t Index = findKeyIndex(It->second.Entries, Key);
        if (Index != std::string_view::npos)
        {
            return &It->second.Entries[Index];
        }
        if (!It->second.Parent.has_value())
        {
            return nullptr;
        }
        Current = *It->second.Parent;
    }
}

std::expected<void, IniError> Ini::fail(IniError Failure)
{
    Loaded = false;
    LastError = std::move(Failure);
    return std::unexpected(LastError);
}

std::vector<std::string> Ini::sections() const
{
    return SectionOrder;
}

std::vector<std::string> Ini::keys(std::string_view SectionName) const
{
    std::vector<std::string> Result;
    const IniSection* Sec = findSection(SectionName);
    if (Sec == nullptr)
    {
        return Result;
    }
    Result.reserve(Sec->Entries.size());
    for (const IniEntry& Entry : Sec->Entries)
    {
        Result.push_back(Entry.Key);
    }
    return Result;
}

const IniSection* Ini::section(std::string_view SectionName) const noexcept
{
    return findSection(SectionName);
}

bool Ini::hasSection(std::string_view SectionName) const noexcept
{
    return Sections.find(SectionName) != Sections.end();
}

bool Ini::hasKey(std::string_view SectionName, std::string_view Key) const noexcept
{
    return findEntry(SectionName, Key) != nullptr;
}

std::optional<IniVariant> Ini::getVariant(std::string_view SectionName,
                                          std::string_view Key) const
{
    const IniEntry* Entry = findEntry(SectionName, Key);
    if (Entry == nullptr)
    {
        return std::nullopt;
    }
    return Entry->Value;
}

std::expected<void, IniError> Ini::set(std::string_view SectionName, std::string_view Key,
                                       std::string_view Value)
{
    if (std::optional<IniError> Check = Checker.checkSectionName(SectionName))
    {
        LastError = std::move(*Check);
        return std::unexpected(LastError);
    }

    IniKeyValueRaw Raw;
    Raw.Key.assign(Key);
    Raw.Value.assign(Value);
    Raw.HasDelimiter = true;
    if (std::optional<IniError> Check = Checker.checkKeyValue(Raw))
    {
        LastError = std::move(*Check);
        return std::unexpected(LastError);
    }
    Checker.finalize(Raw);

    if (Checker.policy().EnableInterpolation)
    {
        if (std::optional<IniError> Error = interpolate(Raw.Value, Checker.policy().StrictInterpolation))
        {
            LastError = std::move(*Error);
            return std::unexpected(LastError);
        }
    }

    IniEntry Entry;
    Entry.Key = std::move(Raw.Key);
    Entry.Value = inferValue(std::move(Raw.Value), Checker.policy().EnableListValues);

    if (!hasSection(SectionName))
    {
        SectionOrder.push_back(std::string(SectionName));
    }
    IniSection& Sec = Sections[std::string(SectionName)];
    upsertEntry(Sec, std::move(Entry), Checker.policy().DuplicateKeysAsList);

    LineBuffer.clear();
    LastError = IniError{};
    return {};
}

bool Ini::erase(std::string_view SectionName, std::string_view Key) noexcept
{
    const auto It = Sections.find(SectionName);
    if (It == Sections.end())
    {
        return false;
    }
    const std::size_t Index = findKeyIndex(It->second.Entries, Key);
    if (Index == std::string_view::npos)
    {
        return false;
    }
    It->second.Entries.erase(It->second.Entries.begin() + static_cast<std::ptrdiff_t>(Index));

    LineBuffer.clear();
    return true;
}

bool Ini::eraseSection(std::string_view SectionName) noexcept
{
    const auto It = Sections.find(SectionName);
    if (It == Sections.end())
    {
        return false;
    }
    Sections.erase(It);
    std::erase(SectionOrder, SectionName);

    LineBuffer.clear();
    return true;
}

void Ini::merge(const Ini& Other)
{
    for (const std::string& SectionName : Other.SectionOrder)
    {
        const auto& Src = Other.Sections.at(SectionName);

        IniSection* Dst = findSection(SectionName);
        if (Dst == nullptr)
        {
            SectionOrder.push_back(SectionName);
            Dst = &Sections[SectionName];
            Dst->HeaderLines = Src.HeaderLines;
            Dst->Parent = Src.Parent;
        }
        else if (!Dst->Parent.has_value() && Src.Parent.has_value())
        {
            Dst->Parent = Src.Parent;
        }

        for (const IniEntry& Entry : Src.Entries)
        {
            IniEntry Copy = Entry;
            Copy.LeadingLines.clear(); // 合并不复制注释, 避免重复
            upsertEntry(*Dst, std::move(Copy), Checker.policy().DuplicateKeysAsList);
        }
    }

    LineBuffer.clear();
}
