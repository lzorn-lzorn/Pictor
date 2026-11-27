//
// Created by Administrator on 2026/11/18.
//
// C++11 兼容(2026/11 重构): string_view / Optional / to_chars / from_chars 走
// infra::compat; std::expected 走 infra::serde_detail::Expected; std::variant 访问经
// ini_detail::getIf<T>; if 初始化语句/contains/std::erase 等 C++17/20 写法改写。
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
        TrimmedRange() = default;

        TrimmedRange(std::size_t InOffset, std::size_t InLength)
            : Offset(InOffset), Length(InLength)
        {
        }

        std::size_t Offset = 0;
        std::size_t Length = 0;
    };

    bool isTrimChar(char Character) noexcept
    {
        return Character == ' ' || Character == '\t' || Character == '\r';
    }

    PICTOR_NODISCARD TrimmedRange trimRange(infra::compat::string_view Text) noexcept
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

    PICTOR_NODISCARD infra::compat::string_view applyTrim(infra::compat::string_view Text, TrimmedRange Range) noexcept
    {
        return infra::compat::string_view(Text.data() + Range.Offset, Range.Length);
    }

    bool isWhitespace(char Character) noexcept
    {
        return std::isspace(static_cast<unsigned char>(Character)) != 0;
    }

    /** @brief ASCII 大小写不敏感比较。 */
    bool equalsIgnoreCase(infra::compat::string_view A, infra::compat::string_view B) noexcept
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
    PICTOR_NODISCARD bool isValidUtf8(infra::compat::string_view Text) noexcept
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
    void appendEscape(infra::compat::string_view Line, std::size_t& I, std::string& Out, bool& BadHex)
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
    PICTOR_NODISCARD bool parseListElements(infra::compat::string_view Inner, IniStringList& Out)
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
                Out.push_back(ini_detail::viewToString(
                    applyTrim(ini_detail::toStringView(Element), trimRange(ini_detail::toStringView(Element)))));
                Element.clear();
                continue;
            }
            Element.push_back(C);
        }
        if (InQuotes)
        {
            return false;
        }
        Out.push_back(ini_detail::viewToString(
            applyTrim(ini_detail::toStringView(Element), trimRange(ini_detail::toStringView(Element)))));
        return true;
    }

    /** @brief 值类型推断: bool / int64_t / double / std::string / 列表。 */
    IniVariant inferValue(std::string Value, bool EnableLists)
    {
        if (EnableLists && Value.size() >= 2 && Value.front() == '[' && Value.back() == ']')
        {
            IniStringList List;
            if (parseListElements(infra::compat::string_view(Value.data() + 1, Value.size() - 2), List))
            {
                return List;
            }
            // 列表解析失败 -> 作为普通字符串
        }

        const infra::compat::string_view V = ini_detail::toStringView(Value);
        if (equalsIgnoreCase(V, "true") || equalsIgnoreCase(V, "yes") || equalsIgnoreCase(V, "on"))
        {
            return true;
        }
        if (equalsIgnoreCase(V, "false") || equalsIgnoreCase(V, "no") || equalsIgnoreCase(V, "off"))
        {
            return false;
        }
        std::int64_t Int = 0;
        if (ini_detail::tryParseInt(V, Int))
        {
            return Int;
        }
        double Double = 0.0;
        if (ini_detail::tryParseDouble(V, Double))
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
            if (const auto* Old = ini_detail::getIf<IniStringList>(&Existing.Value))
            {
                List = *Old;
            }
            else
            {
                List.push_back(ini_detail::variantToString(Existing.Value));
            }
            if (const auto* New = ini_detail::getIf<IniStringList>(&Entry.Value))
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
    infra::compat::Optional<IniError> interpolate(std::string& Value, bool Strict)
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
        return infra::compat::nullopt;
    }

    /** @brief Section 名是否需要引号包裹(含策略附加字符集)。 */
    PICTOR_NODISCARD bool needsSectionQuoting(infra::compat::string_view Name,
                                              infra::compat::string_view ExtraSectionNameChars) noexcept
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
            if (ExtraSectionNameChars.find(C) != infra::compat::string_view::npos)
            {
                continue;
            }
            return true;
        }
        return false;
    }

    /** @brief 引号包裹 Section 名(转义 \ 与 ")。 */
    std::string quoteSectionName(infra::compat::string_view Name)
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
        if (const std::string* S = ini_detail::getIf<std::string>(&Value))
        {
            return ini_detail::valueToText(ini_detail::toStringView(*S));
        }
        if (const IniStringList* L = ini_detail::getIf<IniStringList>(&Value))
        {
            return ini_detail::listToString(*L);
        }
        if (const bool* B = ini_detail::getIf<bool>(&Value))
        {
            return ini_detail::scalarToString(*B);
        }
        if (const std::int64_t* I = ini_detail::getIf<std::int64_t>(&Value))
        {
            return ini_detail::scalarToString(*I);
        }
        if (const double* D = ini_detail::getIf<double>(&Value))
        {
            return ini_detail::scalarToString(*D);
        }
        return std::string();
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

IniLineKind IniChecker::classify(infra::compat::string_view Line) const noexcept
{
    if (Line.empty())
    {
        return IniLineKind::Blank;
    }
    if (Policy.AnnotationChars.find(Line.front()) != infra::compat::string_view::npos)
    {
        return IniLineKind::Comment;
    }
    if (Line.front() == '[')
    {
        return IniLineKind::SectionHeader;
    }
    return IniLineKind::KeyValue;
}

infra::compat::Optional<IniError> IniChecker::checkSectionName(infra::compat::string_view Name) const
{
    if (Name.empty())
    {
        return IniError{ IniError::Code::InvalidSectionName, 0, ini_detail::viewToString(Name) };
    }
    if (Policy.AllowAnySectionName)
    {
        return infra::compat::nullopt;
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
        if (Policy.ExtraSectionNameChars.find(Character) != infra::compat::string_view::npos)
        {
            continue;
        }
        return IniError{ IniError::Code::InvalidSectionName, 0, ini_detail::viewToString(Name) };
    }
    return infra::compat::nullopt;
}

infra::compat::Optional<IniError> IniChecker::checkKeyValue(const IniKeyValueRaw& Raw) const
{
    if (Raw.HasInteriorSpace && !Policy.InteriorSpaceInKey)
    {
        return IniError{ IniError::Code::InteriorSpaceInKey, 0, Raw.Key };
    }

    for (const char Character : Raw.Key)
    {
        if (Policy.ForbiddenKeyChars.find(Character) != infra::compat::string_view::npos)
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

    return infra::compat::nullopt;
}

void IniChecker::finalize(IniKeyValueRaw& Raw) const
{
    if (!Raw.HasDelimiter)
    {
        Raw.Value.assign(Policy.DefaultValue.data(), Policy.DefaultValue.size());
    }
    if (Raw.Key.empty())
    {
        Raw.Key.assign(Policy.DefaultValue.data(), Policy.DefaultValue.size());
    }
    if (Policy.KeyCaseMode != ECaseMode::Default)
    {
        Raw.Key = normalizeKey(ini_detail::toStringView(Raw.Key));
    }
}

std::string IniChecker::normalizeKey(infra::compat::string_view Key) const
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

infra::serde_detail::Expected<IniKeyValueRaw, IniError> parseIniKeyValue(infra::compat::string_view Line,
                                                                  infra::compat::string_view DelimiterChars,
                                                                  infra::compat::string_view AnnotationChars)
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

        if (DelimiterChars.find(Character) != infra::compat::string_view::npos)
        {
            Raw.HasDelimiter = true;
            break;
        }
        if (AnnotationChars.find(Character) != infra::compat::string_view::npos)
        {
            return infra::serde_detail::unexpected(
                IniError{ IniError::Code::CommentInKey, 0, ini_detail::viewToString(Line) });
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
                if (!InQuotes && AnnotationChars.find(Character) != infra::compat::string_view::npos)
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
                    return infra::serde_detail::unexpected(
                        IniError{ IniError::Code::InvalidEscapeSequence, 0, ini_detail::viewToString(Line) });
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
                if (AnnotationChars.find(Character) != infra::compat::string_view::npos)
                {
                    break;
                }
                if (DelimiterChars.find(Character) != infra::compat::string_view::npos)
                {
                    return infra::serde_detail::unexpected(
                        IniError{ IniError::Code::SecondDelimiterInValue, 0, ini_detail::viewToString(Line) });
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

infra::serde_detail::Expected<Ini, IniError> Ini::fromString(infra::compat::string_view IniContent)
{
    Ini Result;
    infra::serde_detail::Expected<void, IniError> Parsed = Result.parse(IniContent);
    if (!Parsed)
    {
        return infra::serde_detail::unexpected(Parsed.error());
    }
    return Result;
}

infra::serde_detail::Expected<Ini, IniError> Ini::fromFile(infra::compat::string_view FilePath)
{
    infra::serde_detail::Expected<std::string, IniError> Content = readIniFile(FilePath);
    if (!Content)
    {
        return infra::serde_detail::unexpected(Content.error());
    }
    Ini Result;
    infra::serde_detail::Expected<void, IniError> Parsed = Result.parse(ini_detail::toStringView(*Content));
    if (!Parsed)
    {
        return infra::serde_detail::unexpected(Parsed.error());
    }
    Result.SourcePath.assign(FilePath.data(), FilePath.size());
    return Result;
}

infra::serde_detail::Expected<void, IniError> Ini::load(infra::compat::string_view FilePath)
{
    infra::serde_detail::Expected<std::string, IniError> Content = readIniFile(FilePath);
    if (!Content)
    {
        return fail(Content.error());
    }
    infra::serde_detail::Expected<void, IniError> Parsed = parse(ini_detail::toStringView(*Content));
    if (Parsed)
    {
        SourcePath.assign(FilePath.data(), FilePath.size());
    }
    return Parsed;
}

infra::serde_detail::Expected<void, IniError> Ini::loadMapped(infra::compat::string_view FilePath)
{
    infra::serde_detail::Expected<IniMappedFile, IniError> Mapped = mapIniFile(FilePath);
    if (!Mapped)
    {
        return fail(Mapped.error());
    }
    infra::serde_detail::Expected<void, IniError> Parsed = parse(Mapped->view());
    if (Parsed)
    {
        SourcePath.assign(FilePath.data(), FilePath.size());
    }
    return Parsed;
}

infra::serde_detail::Expected<void, IniError> Ini::parse(infra::compat::string_view IniContent)
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
    infra::compat::string_view Remaining = IniContent;
    std::size_t LineNumber = 0;
    while (!Remaining.empty())
    {
        ++LineNumber;
        const std::size_t Newline = Remaining.find('\n');
        infra::compat::string_view RawLine =
            (Newline == infra::compat::string_view::npos)
                ? Remaining
                : infra::compat::string_view(Remaining.data(), Newline);
        Remaining =
            (Newline == infra::compat::string_view::npos)
                ? infra::compat::string_view{}
                : Remaining.substr(Newline + 1);

        if (!RawLine.empty() && RawLine.back() == '\r')
        {
            RawLine = infra::compat::string_view(RawLine.data(), RawLine.size() - 1); // CRLF
        }
        infra::compat::Optional<IniError> Error = feedLine(RawLine, LineNumber, State);
        if (Error)
        {
            return fail(*Error);
        }
    }
    infra::compat::Optional<IniError> Error = finishParse(State);
    if (Error)
    {
        return fail(*Error);
    }
    return {};
}

infra::serde_detail::Expected<void, IniError> Ini::parse(std::istream& Stream)
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
        if (Checker.policy().ValidateUtf8 && !isValidUtf8(ini_detail::toStringView(Line)))
        {
            return fail(IniError{ IniError::Code::InvalidUtf8, LineNumber, {} });
        }
        infra::compat::Optional<IniError> Error = feedLine(ini_detail::toStringView(Line), LineNumber, State);
        if (Error)
        {
            return fail(*Error);
        }
    }
    infra::compat::Optional<IniError> Error = finishParse(State);
    if (Error)
    {
        return fail(*Error);
    }
    return {};
}

infra::compat::Optional<IniError> Ini::feedLine(infra::compat::string_view RawLine, std::size_t LineNumber,
                                                ParseState& State)
{
    LineBuffer.push_back(ini_detail::viewToString(RawLine));

    // ---------- 续行内容: 原样拼接 ----------
    if (State.HasPendingRaw)
    {
        const infra::compat::string_view Line = applyTrim(RawLine, trimRange(RawLine));
        if (Checker.classify(Line) == IniLineKind::SectionHeader)
        {
            // 防呆: 续行遇到新的 Section 头 -> 终止续行(去掉末尾空白), 本行按普通行重新处理
            const TrimmedRange Range = trimRange(ini_detail::toStringView(State.PendingRaw.Value));
            State.PendingRaw.Value.resize(Range.Offset + Range.Length);
            infra::compat::Optional<IniError> Error = commitEntry(State.PendingRaw, State.PendingRawLine, State);
            if (Error)
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
                State.PendingRaw.Value.append(Line.data(), Line.size() - 1);
                return infra::compat::nullopt; // 继续续行
            }
            State.PendingRaw.Value.append(Line.data(), Line.size());
            infra::compat::Optional<IniError> Error = commitEntry(State.PendingRaw, State.PendingRawLine, State);
            if (Error)
            {
                return Error;
            }
            State.HasPendingRaw = false;
            return infra::compat::nullopt;
        }
    }

    const infra::compat::string_view Line = applyTrim(RawLine, trimRange(RawLine));
    if (Line.empty())
    {
        if (!State.Dropping)
        {
            State.PendingLines.push_back(ini_detail::viewToString(RawLine));
        }
        return infra::compat::nullopt;
    }

    if (State.Dropping && Checker.classify(Line) != IniLineKind::SectionHeader)
    {
        return infra::compat::nullopt; // Drop 策略: 该 Section 下的一切被忽略
    }

    switch (Checker.classify(Line))
    {
        case IniLineKind::Comment:
        case IniLineKind::Blank:
            State.PendingLines.push_back(ini_detail::viewToString(RawLine));
            return infra::compat::nullopt;

        case IniLineKind::SectionHeader:
        {
            std::string Name;
            infra::compat::Optional<std::string> Parent;
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
                                     ini_detail::viewToString(Line) };
                }

                const infra::compat::string_view Tail = applyTrim(Line.substr(Pos), trimRange(Line.substr(Pos)));
                if (Checker.policy().EnableSectionInheritance && Tail.size() > 1 &&
                    Tail.front() == ':' && Tail.back() == ']')
                {
                    const infra::compat::string_view ParentRaw =
                        applyTrim(infra::compat::string_view(Tail.data() + 1, Tail.size() - 2),
                                  trimRange(infra::compat::string_view(Tail.data() + 1, Tail.size() - 2)));
                    if (ParentRaw.empty() || Checker.checkSectionName(ParentRaw).has_value())
                    {
                        return IniError{ IniError::Code::InvalidParentSectionName, LineNumber,
                                         ini_detail::viewToString(Line) };
                    }
                    Parent = ini_detail::viewToString(ParentRaw);
                }
                else if (Tail != "]")
                {
                    return IniError{ IniError::Code::TrailingContentInSectionHeader, LineNumber,
                                     ini_detail::viewToString(Line) };
                }
            }
            else
            {
                const std::size_t Close = Line.find(']');
                if (Close == infra::compat::string_view::npos)
                {
                    return IniError{ IniError::Code::UnclosedSectionHeader, LineNumber,
                                     ini_detail::viewToString(Line) };
                }
                const infra::compat::string_view Tail = applyTrim(Line.substr(Close + 1),
                                                                  trimRange(Line.substr(Close + 1)));
                if (!Tail.empty())
                {
                    return IniError{ IniError::Code::TrailingContentInSectionHeader, LineNumber,
                                     ini_detail::viewToString(Line) };
                }

                infra::compat::string_view NameRaw = applyTrim(
                    infra::compat::string_view(Line.data() + 1, Close - 1),
                    trimRange(infra::compat::string_view(Line.data() + 1, Close - 1)));
                if (Checker.policy().EnableSectionInheritance)
                {
                    const std::size_t Colon = NameRaw.find(':');
                    if (Colon != infra::compat::string_view::npos)
                    {
                        const infra::compat::string_view ParentRaw =
                            applyTrim(NameRaw.substr(Colon + 1), trimRange(NameRaw.substr(Colon + 1)));
                        NameRaw = applyTrim(infra::compat::string_view(NameRaw.data(), Colon),
                                            trimRange(infra::compat::string_view(NameRaw.data(), Colon)));
                        if (ParentRaw.empty() || Checker.checkSectionName(ParentRaw).has_value())
                        {
                            return IniError{ IniError::Code::InvalidParentSectionName, LineNumber,
                                             ini_detail::viewToString(Line) };
                        }
                        Parent = ini_detail::viewToString(ParentRaw);
                    }
                }
                Name.assign(NameRaw.data(), NameRaw.size());
            }

            // 名称检查(引号名只要求非空)
            const bool NameInvalid = Quoted ? Name.empty()
                                            : Checker.checkSectionName(ini_detail::toStringView(Name)).has_value();
            if (NameInvalid)
            {
                State.PendingLines.clear();
                if (Checker.policy().SectionErrorHandle == ESectionErrorHandle::TransferToDefault)
                {
                    State.CurrentSection.assign(Checker.policy().DefaultSectionName.data(),
                                                Checker.policy().DefaultSectionName.size());
                    State.Dropping = false;
                    if (Sections.find(State.CurrentSection) == Sections.end())
                    {
                        SectionOrder.push_back(State.CurrentSection);
                    }
                }
                else
                {
                    State.CurrentSection.clear();
                    State.Dropping = true;
                }
                return infra::compat::nullopt;
            }

            State.CurrentSection = Name;
            State.Dropping = false;
            const bool Exists = Sections.find(State.CurrentSection) != Sections.end();
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
            return infra::compat::nullopt;
        }

        case IniLineKind::KeyValue:
        {
            if (State.CurrentSection.empty())
            {
                return IniError{ IniError::Code::KeyValueOutsideSection, LineNumber,
                                 ini_detail::viewToString(Line) };
            }

            infra::serde_detail::Expected<IniKeyValueRaw, IniError> Raw =
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
                return infra::compat::nullopt;
            }
            return commitEntry(*Raw, LineNumber, State);
        }
    }
    return infra::compat::nullopt;
}

infra::compat::Optional<IniError> Ini::commitEntry(IniKeyValueRaw& Raw, std::size_t LineNumber,
                                                   ParseState& State)
{
    infra::compat::Optional<IniError> Check = Checker.checkKeyValue(Raw);
    if (Check)
    {
        Check->LineNumber = LineNumber;
        return Check;
    }
    Checker.finalize(Raw);

    if (Checker.policy().EnableInterpolation)
    {
        infra::compat::Optional<IniError> Error = interpolate(Raw.Value, Checker.policy().StrictInterpolation);
        if (Error)
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
    return infra::compat::nullopt;
}

infra::compat::Optional<IniError> Ini::finishParse(ParseState& State)
{
    if (State.HasPendingRaw)
    {
        infra::compat::Optional<IniError> Error = commitEntry(State.PendingRaw, State.PendingRawLine, State);
        if (Error)
        {
            return Error;
        }
        State.HasPendingRaw = false;
    }
    TailLines = std::move(State.PendingLines);
    Loaded = true;
    return infra::compat::nullopt;
}

infra::serde_detail::Expected<void, IniError> Ini::save(infra::compat::string_view FilePath) const
{
    return writeIniFile(FilePath, ini_detail::toStringView(serialize()));
}

infra::serde_detail::Expected<void, IniError> Ini::save() const
{
    if (SourcePath.empty())
    {
        return infra::serde_detail::unexpected(IniError{ IniError::Code::CannotOpenFile, 0,
                                                  "no source path (call save(FilePath))" });
    }
    return save(ini_detail::toStringView(SourcePath));
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
        if (needsSectionQuoting(ini_detail::toStringView(Name), Checker.policy().ExtraSectionNameChars))
        {
            Out += quoteSectionName(ini_detail::toStringView(Name));
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
    infra::compat::string_view Remaining = ini_detail::toStringView(Text);
    while (!Remaining.empty())
    {
        const std::size_t Newline = Remaining.find('\n');
        LineBuffer.emplace_back(
            (Newline == infra::compat::string_view::npos)
                ? ini_detail::viewToString(Remaining)
                : ini_detail::viewToString(infra::compat::string_view(Remaining.data(), Newline)));
        Remaining =
            (Newline == infra::compat::string_view::npos)
                ? infra::compat::string_view{}
                : Remaining.substr(Newline + 1);
    }
    return Text;
}

const IniSection* Ini::findSection(infra::compat::string_view SectionName) const noexcept
{
    const auto It = Sections.find(ini_detail::viewToString(SectionName));
    return It == Sections.end() ? nullptr : &It->second;
}

IniSection* Ini::findSection(infra::compat::string_view SectionName) noexcept
{
    const auto It = Sections.find(ini_detail::viewToString(SectionName));
    return It == Sections.end() ? nullptr : &It->second;
}

std::size_t Ini::findKeyIndex(const KeyValueType& Entries, infra::compat::string_view Key) const noexcept
{
    if (Checker.policy().KeyCaseMode == ECaseMode::Default)
    {
        for (std::size_t Index = 0; Index < Entries.size(); ++Index)
        {
            if (ini_detail::toStringView(Entries[Index].Key) == Key)
            {
                return Index;
            }
        }
        return infra::compat::string_view::npos;
    }

    const std::string Normalized = Checker.normalizeKey(Key);
    for (std::size_t Index = 0; Index < Entries.size(); ++Index)
    {
        if (Entries[Index].Key == Normalized)
        {
            return Index;
        }
    }
    return infra::compat::string_view::npos;
}

const IniEntry* Ini::findEntry(infra::compat::string_view SectionName, infra::compat::string_view Key) const noexcept
{
    // 沿 [Child : Parent] 继承链查找, 带环保护
    std::vector<infra::compat::string_view> Visited;
    infra::compat::string_view Current = SectionName;
    while (true)
    {
        for (const infra::compat::string_view Seen : Visited)
        {
            if (Seen == Current)
            {
                return nullptr; // 继承环
            }
        }
        Visited.push_back(Current);

        const auto It = Sections.find(ini_detail::viewToString(Current));
        if (It == Sections.end())
        {
            return nullptr;
        }
        const std::size_t Index = findKeyIndex(It->second.Entries, Key);
        if (Index != infra::compat::string_view::npos)
        {
            return &It->second.Entries[Index];
        }
        if (!It->second.Parent.has_value())
        {
            return nullptr;
        }
        Current = ini_detail::toStringView(*It->second.Parent);
    }
}

infra::serde_detail::Expected<void, IniError> Ini::fail(IniError Failure)
{
    Loaded = false;
    LastError = std::move(Failure);
    return infra::serde_detail::unexpected(LastError);
}

std::vector<std::string> Ini::sections() const
{
    return SectionOrder;
}

std::vector<std::string> Ini::keys(infra::compat::string_view SectionName) const
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

const IniSection* Ini::section(infra::compat::string_view SectionName) const noexcept
{
    return findSection(SectionName);
}

bool Ini::hasSection(infra::compat::string_view SectionName) const noexcept
{
    return Sections.find(ini_detail::viewToString(SectionName)) != Sections.end();
}

bool Ini::hasKey(infra::compat::string_view SectionName, infra::compat::string_view Key) const noexcept
{
    return findEntry(SectionName, Key) != nullptr;
}

infra::compat::Optional<IniVariant> Ini::getVariant(infra::compat::string_view SectionName,
                                                    infra::compat::string_view Key) const
{
    const IniEntry* Entry = findEntry(SectionName, Key);
    if (Entry == nullptr)
    {
        return infra::compat::nullopt;
    }
    return Entry->Value;
}

infra::serde_detail::Expected<void, IniError> Ini::set(infra::compat::string_view SectionName, infra::compat::string_view Key,
                                                infra::compat::string_view Value)
{
    infra::compat::Optional<IniError> Check = Checker.checkSectionName(SectionName);
    if (Check)
    {
        LastError = std::move(*Check);
        return infra::serde_detail::unexpected(LastError);
    }

    IniKeyValueRaw Raw;
    Raw.Key.assign(Key.data(), Key.size());
    Raw.Value.assign(Value.data(), Value.size());
    Raw.HasDelimiter = true;
    infra::compat::Optional<IniError> CheckKey = Checker.checkKeyValue(Raw);
    if (CheckKey)
    {
        LastError = std::move(*CheckKey);
        return infra::serde_detail::unexpected(LastError);
    }
    Checker.finalize(Raw);

    if (Checker.policy().EnableInterpolation)
    {
        infra::compat::Optional<IniError> Error = interpolate(Raw.Value, Checker.policy().StrictInterpolation);
        if (Error)
        {
            LastError = std::move(*Error);
            return infra::serde_detail::unexpected(LastError);
        }
    }

    IniEntry Entry;
    Entry.Key = std::move(Raw.Key);
    Entry.Value = inferValue(std::move(Raw.Value), Checker.policy().EnableListValues);

    if (!hasSection(SectionName))
    {
        SectionOrder.push_back(ini_detail::viewToString(SectionName));
    }
    IniSection& Sec = Sections[ini_detail::viewToString(SectionName)];
    upsertEntry(Sec, std::move(Entry), Checker.policy().DuplicateKeysAsList);

    LineBuffer.clear();
    LastError = IniError{};
    return {};
}

bool Ini::erase(infra::compat::string_view SectionName, infra::compat::string_view Key) noexcept
{
    const auto It = Sections.find(ini_detail::viewToString(SectionName));
    if (It == Sections.end())
    {
        return false;
    }
    const std::size_t Index = findKeyIndex(It->second.Entries, Key);
    if (Index == infra::compat::string_view::npos)
    {
        return false;
    }
    It->second.Entries.erase(It->second.Entries.begin() + static_cast<std::ptrdiff_t>(Index));

    LineBuffer.clear();
    return true;
}

bool Ini::eraseSection(infra::compat::string_view SectionName) noexcept
{
    const auto It = Sections.find(ini_detail::viewToString(SectionName));
    if (It == Sections.end())
    {
        return false;
    }
    Sections.erase(It);
    // @notice std::erase(容器, 值) 为 C++20, 这里按 C++11 写法等价替换
    for (std::vector<std::string>::iterator Pos = SectionOrder.begin(); Pos != SectionOrder.end(); ++Pos)
    {
        if (*Pos == ini_detail::viewToString(SectionName))
        {
            SectionOrder.erase(Pos);
            break;
        }
    }

    LineBuffer.clear();
    return true;
}

void Ini::merge(const Ini& Other)
{
    for (const std::string& SectionName : Other.SectionOrder)
    {
        const auto& Src = Other.Sections.at(SectionName);

        IniSection* Dst = findSection(ini_detail::toStringView(SectionName));
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
