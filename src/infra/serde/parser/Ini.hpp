//
// Created by Administrator on 2026/11/18.
//
// 2026/11/19 重构(现代 C++23):
//   1. 文件操作与解析解耦: 读取文件的职责移入文件层(见 IniFile.h 的
//      readIniFile / mapIniFile / writeIniFile), Ini 本身只消费文本。
//   2. Ini 内置 std::vector<std::string> 行缓冲区: 解析时逐行保存原文,
//      提供 releaseLineBuffer() 释放, flash() 把当前 Sections 反序列化回
//      文本并按行写回缓冲区(内存变更需经 flash 提现到缓冲区)。
//   3. parse / load 返回 std::expected<void, IniError>, 另提供 fromString /
//      fromFile 工厂返回 std::expected<Ini, IniError>。
//   4. 空白裁剪返回 "偏移 + 长度" 的 trimRange, 无视图逃逸风险。
//   5. 检查与解析解耦: IniChecker 依据 IniPolicy 检查字符串, 自由函数
//      parseIniKeyValue 只做语法切分。
//   6. C++23 化: 透明哈希 + 透明比较, string_view 异构查找 (P2363)。
//   7. 写入与删除: set / erase / eraseSection, 任何变更使行缓冲区失效。
//   8. 值用 std::variant 存储(不再依赖 Any), 提供 get<T> 模板取值、
//      getVariant、set<T> 以及自定义类型注册 registerType<T>。
//   9. 现代特性(均可经 IniPolicy 开关):
//      - 值内注释/分隔符转义(\; \# \= \:) 与更全转义序列(\0 \xNN \a \b \f \v)
//      - 多行续行值(行尾 \ 续行)
//      - 数组/列表值(key = [a, b, c])
//      - ${VAR} 环境变量插值
//      - Section 继承([Child : Parent])
//      - 重复键收集为列表
//      - 保留注释与空行格式的往返编辑(parse -> flash)
//      - 引号包裹 Section 名(["Name With Spaces"])
//      - UTF-8 校验、流式解析(parse(std::istream&))、mmap 读取(loadMapped)、
//        save 写回文件
//

#ifndef HELLOVULKAN_INI_H
#define HELLOVULKAN_INI_H

#include <any>
#include <charconv>
#include <cstdint>
#include <expected>
#include <functional>
#include <iosfwd>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <unordered_map>
#include <variant>
#include <vector>

// ============================================================================
// 策略选项类型
// ============================================================================

enum class ECaseMode : uint8_t {
    Default   = 0, // 区分大小写
    UpperCase = 1, // 所有字母转为大写
    LowerCase = 2, // 所有字母转为小写
};

// 如果 Section Name 解析失败(空名 / 名称含非法字符), 配置如何处理
enum class ESectionErrorHandle : uint8_t {
    Drop,              // 丢弃: 该畸形 Section 及其下所有 Key-Value 被忽略, 不报错
    TransferToDefault, // 合入: 该 Section 下的 Key-Value 并入默认 Section
};

// ============================================================================
// 错误模型
// ============================================================================

struct IniError {
    enum class Code : uint8_t {
        None = 0,

        // ---- 文件层 ----
        CannotOpenFile,
        ReadFailed,
        WriteFailed,

        // ---- 内容级 ----
        InvalidUtf8, // 输入不是合法 UTF-8

        // ---- Section 头 ----
        UnclosedSectionHeader,          // "[Name" 缺少 ']'
        TrailingContentInSectionHeader, // "[Name] trailing"
        InvalidSectionName,             // 空名 / 名称含非法字符(检查器)
        UnclosedQuotedSectionName,      // ["Name 缺少闭合引号
        InvalidParentSectionName,       // [Child : <非法父名>]

        // ---- Key-Value 行结构(纯解析层) ----
        CommentInKey,           // 分隔符之前出现注释符, 该行无法成为 Key-Value
        SecondDelimiterInValue, // 值中(引号外)出现第二个分隔符
        InvalidEscapeSequence,  // 非法 \xNN 转义

        // ---- 策略检查层(IniChecker) ----
        KeyValueOutsideSection,  // Section 之外的 Key-Value
        EmptyKey,                // 严格模式下空 Key / 行内没有任何 Key 内容
        MissingDelimiter,        // 严格模式下缺少分隔符
        DigitLeadingKey,         // Key 以数字开头(键名校验开启时)
        InvalidKeyCharacter,     // Key 含禁止字符 / 非法字符
        InteriorSpaceInKey,      // Key 含空白但策略不允许
        UnresolvedInterpolation, // ${VAR} 未定义且严格插值开启
        UnsupportedValueType,    // set<T>: T 无内建转换也未注册
    };

    Code Code              = Code::None;
    std::size_t LineNumber = 0; // 1-based; 0 表示与行无关(如文件层错误)
    std::string Detail;         // 出错行的内容 / 键名 / 文件路径等上下文

    /** @brief 把错误格式化为人类可读文本(提示信息为英文)。 */
    [[nodiscard]] static std::string asString(const IniError &Error);
};

// ============================================================================
// 运行时策略: 检查器基于它工作, 调用方可以逐项定制
// ============================================================================

struct IniPolicy {
    // ---- 键名 ----
    ECaseMode KeyCaseMode   = ECaseMode::Default; // 键名大小写策略(存储与查找都会应用)
    bool StrictInspection   = true;               // "Key = Value" 三要素必须出现
    bool KeyNameValidation  = true;               // 字母/数字/下划线, 数字不得开头
    bool InteriorSpaceInKey = true;               // 键内空白(压缩为单空格)

    // ---- Section ----
    ESectionErrorHandle SectionErrorHandle = ESectionErrorHandle::Drop; // 畸形 Section 名策略
    bool AllowAnySectionName               = false;                     // true 时 Section 名只要求非空(供引擎/引号名使用)
    bool EnableSectionInheritance          = true;                      // [Child : Parent] 继承
    std::string_view DefaultSectionName    = "Default";

    // ---- 值 ----
    bool EnableListValues         = true;   // key = [a, b, c] 列表值
    bool DuplicateKeysAsList      = false;  // 重复键收集为列表而非覆盖
    bool EnableLineContinuation   = true;   // 行尾 \ 续行
    bool EnableInterpolation      = true;   // ${VAR} 环境变量插值
    bool StrictInterpolation      = false;  // 未定义环境变量视为错误
    std::string_view DefaultValue = "NULL"; // 非严格模式: 空键替代名 / 缺分隔符默认值

    // ---- 输入 ----
    bool ValidateUtf8 = true; // 解析前校验 UTF-8

    // ---- 字符集合(可整体替换) ----
    std::string_view AnnotationChars       = "#;";
    std::string_view DelimiterChars        = "=:";
    std::string_view ExtraSectionNameChars = "_.-|";
    std::string_view ForbiddenKeyChars     = "\"'\\[]{}()";
};

// ============================================================================
// 检查器: 基于策略检查字符串, 与解析解耦
// ============================================================================

// 行分类: 依据首字符(以及策略的注释符集合)
enum class IniLineKind : uint8_t {
    Blank,         // 空白行
    Comment,       // 注释行
    SectionHeader, // "[Name]" 行
    KeyValue,      // Key-Value 行
};

// 纯解析产物: 尚未经过任何策略检查与修正的原始键值对
struct IniKeyValueRaw {
    std::string Key;                  // 原始键(键内连续空白已规范化为单个空格)
    std::string Value;                // 原始值(已去引号、去转义、去行尾注释)
    bool HasDelimiter        = false; // 是否出现 '=' / ':'
    bool HasInteriorSpace    = false; // 键中间是否出现过空白
    bool HasLineContinuation = false; // 值以未转义的 '\' 结尾, 需续行
};

class IniChecker {
public:
    IniChecker() = default;

    explicit IniChecker(IniPolicy Policy) : Policy(Policy) {
    }

    [[nodiscard]] const IniPolicy &policy() const noexcept {
        return Policy;
    }

    /** @brief 行分类(仅看首字符)。 */
    [[nodiscard]] IniLineKind classify(std::string_view Line) const noexcept;

    /** @brief 检查 Section 名: 空名 / 非法字符。 */
    [[nodiscard]] std::optional<IniError> checkSectionName(std::string_view Name) const;

    /** @brief 检查 Key-Value: 禁止字符 / 键名校验 / 键内空白 / 严格模式。 */
    [[nodiscard]] std::optional<IniError> checkKeyValue(const IniKeyValueRaw &Raw) const;

    /** @brief 应用策略修正: 非严格模式的默认键名与默认值、键名大小写折叠。 */
    void finalize(IniKeyValueRaw &Raw) const;

    /** @brief 按 KeyCaseMode 规范化键名(供查找使用, 与 finalize 的折叠一致)。 */
    [[nodiscard]] std::string normalizeKey(std::string_view Key) const;

private:
    IniPolicy Policy;
};

/**
 * @brief 纯解析: 只做语法切分(找分隔符、切键值、处理引号与转义),
 *        不做任何策略判断。策略检查交给 IniChecker。
 */
[[nodiscard]] std::expected<IniKeyValueRaw, IniError> parseIniKeyValue(
    std::string_view Line,
    std::string_view DelimiterChars  = "=:",
    std::string_view AnnotationChars = "#;");

// ============================================================================
// 数据模型
// ============================================================================

// 字符串列表(数组/列表值)
using IniStringList = std::vector<std::string>;

// 值: bool / int64_t / double / std::string / 字符串列表
using IniVariant = std::variant<bool, std::int64_t, double, std::string, IniStringList>;

// 单个 Key-Value 条目: 附带该条目之前的注释与空行原文(往返保真)
struct IniEntry {
    std::string Key;
    IniVariant Value;
    std::vector<std::string> LeadingLines;
};

// 一个 Section: 附带头部注释、可选父 Section(继承)
struct IniSection {
    std::vector<std::string> HeaderLines; // Section 头之前的注释与空行原文
    std::optional<std::string> Parent;    // [Child : Parent]
    std::vector<IniEntry> Entries;        // 有序 Key-Value
};

using KeyValueType = std::vector<IniEntry>;

/**
 * @brief 透明哈希: 使 unordered_map 支持以 std::string_view 直接查找
 *        (C++23 异构查找), 免去每次查询构造临时 std::string。
 */
struct IniHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(std::string_view Key) const noexcept {
        return std::hash<std::string_view>{}(Key);
    }
};

// 整个文件: Section 名 -> Section 内容。
using SectionType = std::unordered_map<std::string, IniSection, IniHash, std::equal_to<> >;

// ============================================================================
// INI 读取器
// ============================================================================

class Ini {
public:
    Ini() = default;

    explicit Ini(IniPolicy Policy) : Checker(std::move(Policy)) {
    }

    ~Ini() = default;

    Ini(const Ini &) = default;

    Ini(Ini &&) noexcept = default;

    Ini &operator=(const Ini &) = default;

    Ini &operator=(Ini &&) noexcept = default;

    // ---------- 工厂 ----------
    [[nodiscard]] static std::expected<Ini, IniError> fromString(std::string_view IniContent);

    [[nodiscard]] static std::expected<Ini, IniError> fromFile(std::string_view FilePath);

    // ---------- 加载与解析 ----------
    /** @brief 从文件加载并解析(读取委托文件层)。 */
    [[nodiscard]] std::expected<void, IniError> load(std::string_view FilePath);

    /** @brief mmap 文件后解析(Windows: 文件映射; 其他平台退化为普通读取)。 */
    [[nodiscard]] std::expected<void, IniError> loadMapped(std::string_view FilePath);

    /** @brief 解析内存中的 INI 内容。 */
    [[nodiscard]] std::expected<void, IniError> parse(std::string_view IniContent);

    /** @brief 流式解析: 从输入流逐行解析, 不要求一次性持有全部内容。 */
    [[nodiscard]] std::expected<void, IniError> parse(std::istream &Stream);

    /** @brief 写回: 把当前内容序列化后保存(无参数时写回 load 的来源文件)。 */
    [[nodiscard]] std::expected<void, IniError> save(std::string_view FilePath) const;

    [[nodiscard]] std::expected<void, IniError> save() const;

    [[nodiscard]] bool isLoaded() const noexcept;

    /** @brief 最近一次失败的结构化错误; 成功后为 Code::None。 */
    [[nodiscard]] const IniError &lastError() const noexcept;

    // ---------- 行缓冲区 ----------
    /** @brief 行缓冲区: 解析时逐行保存原文(已去掉行尾 \r), 含注释与空行;
     *         任何写入/删除都会使其失效, 需经 flash 重建。 */
    [[nodiscard]] const std::vector<std::string> &lineBuffer() const noexcept;

    /** @brief 释放行缓冲区: 以移动语义取出全部行, 本对象缓冲区置空。 */
    [[nodiscard]] std::vector<std::string> releaseLineBuffer();

    /** @brief 把当前 Sections 反序列化回 std::string, 逐行写回缓冲区并返回全文。 */
    [[nodiscard]] std::string flash();

    /** @brief 把 Sections 反序列化回 std::string(不动缓冲区)。 */
    [[nodiscard]] std::string serialize() const;

    // ---------- 策略与检查器 ----------
    [[nodiscard]] const IniChecker &checker() const noexcept {
        return Checker;
    }

    // ---------- 结构查询 ----------
    /** @brief 所有 Section 名, 按文件出现顺序。 */
    [[nodiscard]] std::vector<std::string> sections() const;

    /** @brief 指定 Section 自身(不含继承)的 Key, 按文件出现顺序。 */
    [[nodiscard]] std::vector<std::string> keys(std::string_view SectionName) const;

    /** @brief 指定 Section 的结构; 不存在返回 nullptr。 */
    [[nodiscard]] const IniSection *section(std::string_view SectionName) const noexcept;

    [[nodiscard]] bool hasSection(std::string_view SectionName) const noexcept;

    /** @brief 是否含 Key(沿 [Child : Parent] 继承链查找)。 */
    [[nodiscard]] bool hasKey(std::string_view SectionName, std::string_view Key) const noexcept;

    // ---------- 取值 ----------
    /** @brief 模板取值: 内建类型(bool/整数/浮点/字符串/字符串列表/算术 vector)
     *         直接转换; 已注册的自定义类型经 registerType 的转换函数取值。
     *         失败返回 Default。 */
    template<typename T>
    [[nodiscard]] T get(std::string_view SectionName,
                        std::string_view Key,
                        T Default = {}) const;

    /** @brief 取原始 variant 值; 不存在返回 std::nullopt。 */
    [[nodiscard]] std::optional<IniVariant> getVariant(std::string_view SectionName,
                                                       std::string_view Key) const;

    // ---------- 写入与删除 ----------
    /** @brief 写入/覆盖一个 Key-Value(字符串形式, 值与解析走同一类型推断)。 */
    [[nodiscard]] std::expected<void, IniError> set(std::string_view SectionName,
                                                    std::string_view Key,
                                                    std::string_view Value);

    /** @brief 写入/覆盖: 内建类型直接字符串化; 自定义类型经 registerType 转换。 */
    template<typename T>
    [[nodiscard]] std::expected<void, IniError> set(std::string_view SectionName,
                                                    std::string_view Key,
                                                    const T &Value);

    /** @brief 注册自定义类型转换: T <-> 字符串。 */
    template<typename T>
    void registerType(std::function<std::optional<std::string>(const T &)> ToString,
                      std::function<std::optional<T>(std::string_view)> FromString);

    /** @brief 删除指定 Key(仅自身, 不含继承), 成功返回 true。 */
    bool erase(std::string_view SectionName, std::string_view Key) noexcept;

    /** @brief 删除整个 Section(含顺序表), 成功返回 true。 */
    bool eraseSection(std::string_view SectionName) noexcept;

    // ---------- 其他 ----------
    /** @brief 把 Other 合并进当前对象: 相同 Section 合并, 相同 Key 后者覆盖。 */
    void merge(const Ini &Other);

private:
    struct ParseState;

    struct ConverterEntry {
        std::function<std::optional<std::string>(const void *Value)> ToString; // Value 指向 T
        std::function<std::optional<std::any>(std::string_view Text)> FromString;
    };

    template<typename T>
    inline static constexpr char TypeTag = 0; // 无 RTTI 的类型标识

    /** @brief 按 Section 名查找(异构查找), 找不到返回 nullptr。 */
    [[nodiscard]] const IniSection *findSection(std::string_view SectionName) const noexcept;

    IniSection *findSection(std::string_view SectionName) noexcept;

    /** @brief 在 Entries 中按策略折叠后的键名查找下标, 找不到返回 npos。 */
    [[nodiscard]] std::size_t findKeyIndex(const KeyValueType &Entries,
                                           std::string_view Key) const noexcept;

    /** @brief 沿继承链查找条目(带环保护)。 */
    [[nodiscard]] const IniEntry *findEntry(std::string_view SectionName,
                                            std::string_view Key) const noexcept;

    /** @brief 单行处理(parse 文本/流共用)。 */
    [[nodiscard]] std::optional<IniError> feedLine(std::string_view RawLine,
                                                   std::size_t LineNumber,
                                                   ParseState &State);

    /** @brief 提交一条已解析的键值对(检查/修正/插值/类型推断/存储)。 */
    [[nodiscard]] std::optional<IniError> commitEntry(IniKeyValueRaw &Raw,
                                                      std::size_t LineNumber,
                                                      ParseState &State);

    /** @brief 收尾: 提交残留续行与尾注, 标记加载完成。 */
    [[nodiscard]] std::optional<IniError> finishParse(ParseState &State);

    /** @brief 记录错误并返回 unexpected。 */
    [[nodiscard]] std::expected<void, IniError> fail(IniError Error);

    IniChecker Checker;                                          // 策略检查器(策略随对象构造注入)
    SectionType Sections;                                        // Section 名 -> 内容
    std::vector<std::string> SectionOrder;                       // Section 出现顺序
    std::vector<std::string> LineBuffer;                         // 行缓冲区: 每一行原文
    std::vector<std::string> TailLines;                          // 文件末尾的注释与空行(往返保真)
    std::unordered_map<const void *, ConverterEntry> Converters; // 自定义类型注册表
    std::string SourcePath;                                      // load/loadMapped 的来源路径(save() 写回目标)
    bool Loaded = false;
    IniError LastError; // 最近一次失败的结构化错误
};

// ============================================================================
// 内建转换与文本化工具(模板实现, 需在头文件中可见)
// ============================================================================

namespace ini_detail{
/** @brief 解析布尔文本。 */
inline bool tryParseBool(std::string_view Text, bool &Out) {
    if (Text == "true" || Text == "1" || Text == "yes" || Text == "on" ||
        Text == "TRUE" || Text == "YES" || Text == "ON") {
        Out = true;
        return true;
    }
    if (Text == "false" || Text == "0" || Text == "no" || Text == "off" ||
        Text == "FALSE" || Text == "NO" || Text == "OFF") {
        Out = false;
        return true;
    }
    return false;
}

/** @brief 解析整数文本(必须完整消费)。 */
inline bool tryParseInt(std::string_view Text, std::int64_t &Out) noexcept {
    const char *const Begin             = Text.data();
    const char *const End               = Text.data() + Text.size();
    std::int64_t Value                  = 0;
    const std::from_chars_result Result = std::from_chars(Begin, End, Value);
    if (Result.ec != std::errc{} || Result.ptr != End) {
        return false;
    }
    Out = Value;
    return true;
}

/** @brief 解析浮点文本(必须完整消费)。 */
inline bool tryParseDouble(std::string_view Text, double &Out) noexcept {
    const char *const Begin             = Text.data();
    const char *const End               = Text.data() + Text.size();
    double Value                        = 0.0;
    const std::from_chars_result Result = std::from_chars(Begin, End, Value);
    if (Result.ec != std::errc{} || Result.ptr != End) {
        return false;
    }
    Out = Value;
    return true;
}

/** @brief 标量 -> 文本。 */
template<typename T>
[[nodiscard]] std::string scalarToString(const T &Value) {
    if constexpr (std::is_same_v<T, bool>) {
        return Value ? "true" : "false";
    }
    else if constexpr (std::is_same_v<T, std::string> || std::is_same_v<T, std::string_view>) {
        return std::string(Value);
    }
    else if constexpr (std::is_same_v<T, const char *>) {
        return std::string(Value);
    }
    else if constexpr (std::is_integral_v<T>) {
        return std::to_string(Value);
    }
    else {
        char Buffer[64];
        const std::to_chars_result Result =
            std::to_chars(Buffer, Buffer + sizeof(Buffer), Value);
        return std::string(Buffer, Result.ptr);
    }
}

/** @brief 值是否需要引号包裹。 */
[[nodiscard]] inline bool needsQuoting(std::string_view Value) noexcept {
    if (Value.empty()) {
        return false;
    }
    std::size_t Begin = 0;
    std::size_t End   = Value.size();
    while (Begin < End && (Value[Begin] == ' ' || Value[Begin] == '\t' || Value[Begin] == '\r')) {
        ++Begin;
    }
    while (End > Begin && (Value[End - 1] == ' ' || Value[End - 1] == '\t' || Value[End - 1] == '\r')) {
        --End;
    }
    if (Begin != 0 || End != Value.size()) {
        return true; // 首尾空白
    }
    // 形如 [..] 的字符串会被当作列表, 必须引号包裹
    if (Value.front() == '[' && Value.back() == ']') {
        return true;
    }
    for (const char C: Value) {
        if (C == ',' || C == ';' || C == '#' || C == '=' || C == ':' || C == '"' ||
            C == '\\' || C == '\n' || C == '\t' || C == '\r' || C == '\0' ||
            static_cast<unsigned char>(C) < 0x20) {
            return true;
        }
    }
    return false;
}

/** @brief 引号包裹并转义(与解析端成对)。 */
[[nodiscard]] inline std::string escapeValue(std::string_view Value) {
    std::string Out;
    Out.reserve(Value.size() + 2);
    Out.push_back('"');
    for (const char C: Value) {
        switch (C) {
            case '\\':
                Out += "\\\\";
                break;
            case '"':
                Out += "\\\"";
                break;
            case '\n':
                Out += "\\n";
                break;
            case '\t':
                Out += "\\t";
                break;
            case '\r':
                Out += "\\r";
                break;
            case '\0':
                Out += "\\0";
                break;
            case '\a':
                Out += "\\a";
                break;
            case '\b':
                Out += "\\b";
                break;
            case '\f':
                Out += "\\f";
                break;
            case '\v':
                Out += "\\v";
                break;
            default:
                if (static_cast<unsigned char>(C) < 0x20) {
                    constexpr char Hex[]  = "0123456789ABCDEF";
                    const unsigned char U = static_cast<unsigned char>(C);
                    Out                   += "\\x";
                    Out.push_back(Hex[U >> 4]);
                    Out.push_back(Hex[U & 0x0F]);
                }
                else {
                    Out.push_back(C);
                }
                break;
        }
    }
    Out.push_back('"');
    return Out;
}

/** @brief 值 -> 可写入文本(必要时引号包裹)。 */
[[nodiscard]] inline std::string valueToText(std::string_view Value) {
    return needsQuoting(Value) ? escapeValue(Value) : std::string(Value);
}

/** @brief 列表 -> "[a, b, c]" 文本。 */
[[nodiscard]] inline std::string listToString(const IniStringList &List) {
    std::string Out = "[";
    for (std::size_t I = 0; I < List.size(); ++I) {
        if (I != 0) {
            Out += ", ";
        }
        Out += valueToText(List[I]);
    }
    Out += ']';
    return Out;
}

/** @brief variant -> 文本(标量转字符串, 列表加方括号)。 */
[[nodiscard]] inline std::string variantToString(const IniVariant &Value) {
    return std::visit(
                      [](const auto &Arg) -> std::string {
                          using T = std::decay_t<decltype(Arg)>;
                          if constexpr (std::is_same_v<T, IniStringList>) {
                              return listToString(Arg);
                          }
                          else {
                              return scalarToString(Arg);
                          }
                      },
                      Value);
}

/** @brief 可转换为元素的 vector 检测。 */
template<typename T>
struct IsConvertibleVector : std::false_type {
};

template<typename E, typename A>
struct IsConvertibleVector<std::vector<E, A> >
    : std::bool_constant<std::is_integral_v<E> || std::is_floating_point_v<E> ||
                         std::is_same_v<E, std::string>> {
};

template<typename T>
inline constexpr bool IsConvertibleVector_v = IsConvertibleVector<T>::value;

/** @brief 内建可转换类型检测(get<T> 直接走 variant 转换)。 */
template<typename T>
inline constexpr bool IsBuiltinConvertible_v =
    std::is_same_v<T, bool> || std::is_same_v<T, std::int64_t> ||
    std::is_same_v<T, double> || std::is_same_v<T, std::string> ||
    std::is_same_v<T, IniStringList> || std::is_integral_v<T> ||
    std::is_floating_point_v<T> || IsConvertibleVector_v<T>;

/** @brief IniVariant -> T (失败返回 nullopt)。 */
template<typename T>
[[nodiscard]] std::optional<T> getAs(const IniVariant &V) {
    if constexpr (std::is_same_v<T, bool>) {
        if (const auto *P = std::get_if<bool>(&V)) {
            return *P;
        }
        if (const auto *P = std::get_if<std::int64_t>(&V)) {
            return *P != 0;
        }
        if (const auto *P = std::get_if<double>(&V)) {
            return *P != 0.0;
        }
        if (const auto *P = std::get_if<std::string>(&V)) {
            bool Out = false;
            return tryParseBool(*P, Out) ? std::optional<bool>{Out} : std::nullopt;
        }
        return std::nullopt;
    }
    else if constexpr (std::is_same_v<T, std::int64_t>) {
        if (const auto *P = std::get_if<std::int64_t>(&V)) {
            return *P;
        }
        if (const auto *P = std::get_if<double>(&V)) {
            return static_cast<std::int64_t>(*P);
        }
        if (const auto *P = std::get_if<bool>(&V)) {
            return *P ? 1 : 0;
        }
        if (const auto *P = std::get_if<std::string>(&V)) {
            std::int64_t Out = 0;
            return tryParseInt(*P, Out) ? std::optional<std::int64_t>{Out} : std::nullopt;
        }
        return std::nullopt;
    }
    else if constexpr (std::is_same_v<T, double>) {
        if (const auto *P = std::get_if<double>(&V)) {
            return *P;
        }
        if (const auto *P = std::get_if<std::int64_t>(&V)) {
            return static_cast<double>(*P);
        }
        if (const auto *P = std::get_if<bool>(&V)) {
            return *P ? 1.0 : 0.0;
        }
        if (const auto *P = std::get_if<std::string>(&V)) {
            double Out = 0.0;
            return tryParseDouble(*P, Out) ? std::optional<double>{Out} : std::nullopt;
        }
        return std::nullopt;
    }
    else if constexpr (std::is_same_v<T, std::string>) {
        if (const auto *P = std::get_if<std::string>(&V)) {
            return *P;
        }
        return variantToString(V);
    }
    else if constexpr (std::is_same_v<T, IniStringList>) {
        if (const auto *P = std::get_if<IniStringList>(&V)) {
            return *P;
        }
        if (const auto *P = std::get_if<std::string>(&V)) {
            return IniStringList{*P}; // 标量 -> 单元素列表
        }
        return std::nullopt;
    }
    else if constexpr (std::is_integral_v<T>) {
        const std::optional<std::int64_t> I = getAs<std::int64_t>(V);
        return I ? std::optional<T>{static_cast<T>(*I)} : std::nullopt;
    }
    else if constexpr (std::is_floating_point_v<T>) {
        const std::optional<double> D = getAs<double>(V);
        return D ? std::optional<T>{static_cast<T>(*D)} : std::nullopt;
    }
    else if constexpr (IsConvertibleVector_v<T>) {
        const auto *List = std::get_if<IniStringList>(&V);
        if (List == nullptr) {
            return std::nullopt;
        }
        T Out;
        Out.reserve(List->size());
        for (const std::string &Element: *List) {
            const std::optional<typename T::value_type> Converted =
                getAs<typename T::value_type>(IniVariant{Element});
            if (!Converted.has_value()) {
                return std::nullopt;
            }
            Out.push_back(std::move(*Converted));
        }
        return Out;
    }
    else {
        return std::nullopt; // 自定义类型由 Ini::get 处理
    }
}
} // namespace ini_detail

template<typename T>
T Ini::get(std::string_view SectionName, std::string_view Key, T Default) const {
    const IniEntry *Entry = findEntry(SectionName, Key);
    if (Entry == nullptr) {
        return Default;
    }

    if constexpr (std::is_same_v<T, IniVariant>) {
        return Entry->Value;
    }
    else if constexpr (ini_detail::IsBuiltinConvertible_v<T>) {
        const std::optional<T> Converted = ini_detail::getAs<T>(Entry->Value);
        return Converted.value_or(Default);
    }
    else {
        // 自定义类型: 经注册表转换
        const auto It = Converters.find(&TypeTag<T>);
        if (It == Converters.end()) {
            return Default;
        }
        const std::optional<std::string> Text = ini_detail::getAs<std::string>(Entry->Value);
        if (!Text.has_value()) {
            return Default;
        }
        const std::optional<std::any> Converted = It->second.FromString(*Text);
        if (!Converted.has_value()) {
            return Default;
        }
        if (const T *Ptr = std::any_cast<T>(&*Converted)) {
            return *Ptr;
        }
        return Default;
    }
}

template<typename T>
std::expected<void, IniError> Ini::set(std::string_view SectionName,
                                       std::string_view Key,
                                       const T &Value) {
    // 字符串字面量按值传入时会推导为 char[N], 先退化再分派
    using DT = std::decay_t<T>;

    if constexpr (std::is_same_v<DT, IniVariant>) {
        return set(SectionName, Key, ini_detail::variantToString(Value));
    }
    else if constexpr (ini_detail::IsConvertibleVector_v<DT>) {
        IniStringList List;
        List.reserve(Value.size());
        for (const auto &Element: Value) {
            List.push_back(ini_detail::scalarToString(Element));
        }
        return set(SectionName, Key, ini_detail::listToString(List));
    }
    else if constexpr (std::is_same_v<DT, std::string> || std::is_same_v<DT, std::string_view> ||
                       std::is_convertible_v<const T &, std::string_view>) {
        // 覆盖 std::string / std::string_view / char[N] / char* 等
        return set(SectionName, Key, std::string_view(Value));
    }
    else if constexpr (std::is_same_v<DT, bool> || std::is_integral_v<DT> ||
                       std::is_floating_point_v<DT>) {
        return set(SectionName, Key, ini_detail::scalarToString(Value));
    }
    else {
        // 自定义类型: 经注册表转换
        const auto It = Converters.find(&TypeTag<DT>);
        if (It == Converters.end()) {
            return fail(IniError{IniError::Code::UnsupportedValueType, 0, std::string(Key)});
        }
        const std::optional<std::string> Text = It->second.ToString(&Value);
        if (!Text.has_value()) {
            return fail(IniError{IniError::Code::UnsupportedValueType, 0, std::string(Key)});
        }
        return set(SectionName, Key, *Text);
    }
}

template<typename T>
void Ini::registerType(std::function<std::optional<std::string>(const T &)> ToString,
                       std::function<std::optional<T>(std::string_view)> FromString) {
    using TClean = std::decay_t<T>;
    ConverterEntry Entry;
    Entry.ToString = [Fn = std::move(ToString)](const void *Ptr) {
        return Fn(*static_cast<const TClean *>(Ptr));
    };
    Entry.FromString = [Fn = std::move(FromString)](std::string_view Text) -> std::optional<std::any> {
        if (std::optional<TClean> Result = Fn(Text)) {
            return std::any{std::move(*Result)};
        }
        return std::nullopt;
    };
    Converters[&TypeTag<TClean>] = std::move(Entry);
}

#endif //HELLOVULKAN_INI_H
