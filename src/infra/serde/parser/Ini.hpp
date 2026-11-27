//
// Created by Administrator on 2026/11/18.
//
// 2026/11/19 重构(现代 C++23):
//   1. 文件操作与解析解耦: 读取文件的职责移入文件层(见 IniFile.h 的
//      readIniFile / mapIniFile / writeIniFile), Ini 本身只消费文本。
//   2. Ini 内置 std::vector<std::string> 行缓冲区: 解析时逐行保存原文,
//      提供 releaseLineBuffer() 释放, flash() 把当前 Sections 反序列化回
//      文本并按行写回缓冲区(内存变更需经 flash 提现到缓冲区)。
//   3. parse / load 返回 expected<void, IniError>, 另提供 fromString /
//      fromFile 工厂返回 expected<Ini, IniError>。
//   4. 空白裁剪返回 "偏移 + 长度" 的 trimRange, 无视图逃逸风险。
//   5. 检查与解析解耦: IniChecker 依据 IniPolicy 检查字符串, 自由函数
//      parseIniKeyValue 只做语法切分。
//   6. 透明哈希 + 透明比较, string_view 异构查找 (P2363)。
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
// 2026/11 重构(C++11 兼容, C++20/23 功能不变):
//   1. 跨版本设施统一经 <infra/common/VersionCompatibility.hpp>:
//      string_view / Optional / to_chars / from_chars / bool_constant 等。
//   2. std::expected 为 C++23 设施, 改用 infra::serde_detail::Expected(见 SerdeADT.hpp,
//      接口按需对齐, C++11~C++20 均可用); 返回值类型整体替换。
//   3. std::variant 为 C++17 设施: C++17 起仍为 std::variant(行为不变),
//      C++11 用 ini_detail::Variant 手写垫片(覆盖 bool/int64_t/double/string/
//      列表五种候选, 经 ini_detail::getIf<T> 统一访问, 语义同 std::get_if)。
//   4. std::any 换成 infra::containers::Any(本就 C++11 兼容)。
//   5. 变量模板(IsConvertibleVector_v / IsBuiltinConvertible_v, C++14)改为
//      struct + ::value; 类内 inline static 成员变量模板 TypeTag(C++14)改为
//      ini_detail::TypeTag<T> 静态成员(同 containers/Any.hpp 的 TypeIdTag 模式)。
//   6. 异构查找(需 C++20 透明哈希/比较)改为查找侧显式构造临时 std::string,
//      C++11 与 C++20 语义统一。
//

#ifndef HELLOVULKAN_INI_H
#define HELLOVULKAN_INI_H

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <iosfwd>
#include <new>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include <infra/common/VersionCompatibility.hpp>
#include <infra/containers/Any.hpp>
#include <infra/serde/SerdeADT.hpp>

#if PICTOR_HAS_CPP17
#  include <variant>
#endif

// ============================================================================
// 策略选项类型
// ============================================================================

enum class ECaseMode : std::uint8_t {
    Default   = 0, // 区分大小写
    UpperCase = 1, // 所有字母转为大写
    LowerCase = 2, // 所有字母转为小写
};

// 如果 Section Name 解析失败(空名 / 名称含非法字符), 配置如何处理
enum class ESectionErrorHandle : std::uint8_t {
    Drop,              // 丢弃: 该畸形 Section 及其下所有 Key-Value 被忽略, 不报错
    TransferToDefault, // 合入: 该 Section 下的 Key-Value 并入默认 Section
};

// ============================================================================
// 错误模型
// ============================================================================

struct IniError {
    enum class Code : std::uint8_t {
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

    IniError() = default;

    Code Code              = Code::None;
    std::size_t LineNumber = 0; // 1-based; 0 表示与行无关(如文件层错误)
    std::string Detail;         // 出错行的内容 / 键名 / 文件路径等上下文

    // @notice 成员名 Code 与类型 Code 同名, 构造参数需用 elaborated-type-specifier
    IniError(enum Code InCode, std::size_t InLineNumber, std::string InDetail)
        : Code(InCode), LineNumber(InLineNumber), Detail(std::move(InDetail)) {
    }

    /** @brief 把错误格式化为人类可读文本(提示信息为英文)。 */
    PICTOR_NODISCARD static std::string asString(const IniError &Error);
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
    infra::compat::string_view DefaultSectionName = "Default";

    // ---- 值 ----
    bool EnableListValues         = true;   // key = [a, b, c] 列表值
    bool DuplicateKeysAsList      = false;  // 重复键收集为列表而非覆盖
    bool EnableLineContinuation   = true;   // 行尾 \ 续行
    bool EnableInterpolation      = true;   // ${VAR} 环境变量插值
    bool StrictInterpolation      = false;  // 未定义环境变量视为错误
    infra::compat::string_view DefaultValue = "NULL"; // 非严格模式: 空键替代名 / 缺分隔符默认值

    // ---- 输入 ----
    bool ValidateUtf8 = true; // 解析前校验 UTF-8

    // ---- 字符集合(可整体替换) ----
    infra::compat::string_view AnnotationChars       = "#;";
    infra::compat::string_view DelimiterChars        = "=:";
    infra::compat::string_view ExtraSectionNameChars = "_.-|";
    infra::compat::string_view ForbiddenKeyChars     = "\"'\\[]{}()";
};

// ============================================================================
// 检查器: 基于策略检查字符串, 与解析解耦
// ============================================================================

// 行分类: 依据首字符(以及策略的注释符集合)
enum class IniLineKind : std::uint8_t {
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

    PICTOR_NODISCARD const IniPolicy &policy() const noexcept {
        return Policy;
    }

    /** @brief 行分类(仅看首字符)。 */
    PICTOR_NODISCARD IniLineKind classify(infra::compat::string_view Line) const noexcept;

    /** @brief 检查 Section 名: 空名 / 非法字符。 */
    PICTOR_NODISCARD infra::compat::Optional<IniError> checkSectionName(infra::compat::string_view Name) const;

    /** @brief 检查 Key-Value: 禁止字符 / 键名校验 / 键内空白 / 严格模式。 */
    PICTOR_NODISCARD infra::compat::Optional<IniError> checkKeyValue(const IniKeyValueRaw &Raw) const;

    /** @brief 应用策略修正: 非严格模式的默认键名与默认值、键名大小写折叠。 */
    void finalize(IniKeyValueRaw &Raw) const;

    /** @brief 按 KeyCaseMode 规范化键名(供查找使用, 与 finalize 的折叠一致)。 */
    PICTOR_NODISCARD std::string normalizeKey(infra::compat::string_view Key) const;

private:
    IniPolicy Policy;
};

/**
 * @brief 纯解析: 只做语法切分(找分隔符、切键值、处理引号与转义),
 *        不做任何策略判断。策略检查交给 IniChecker。
 */
PICTOR_NODISCARD infra::serde_detail::Expected<IniKeyValueRaw, IniError> parseIniKeyValue(
    infra::compat::string_view Line,
    infra::compat::string_view DelimiterChars  = "=:",
    infra::compat::string_view AnnotationChars = "#;");

// ============================================================================
// 数据模型
// ============================================================================

// 字符串列表(数组/列表值)
using IniStringList = std::vector<std::string>;

namespace ini_detail {

// ----------------------------------------------------------------------------
// 值存储: C++17 起直接使用 std::variant(行为与重构前一致), C++11 用手写垫片。
// 访问统一走 ini_detail::getIf<T>(语义同 std::get_if: 类型不匹配返回 nullptr)。
// ----------------------------------------------------------------------------
#if PICTOR_HAS_CPP17
using Variant = std::variant<bool, std::int64_t, double, std::string, IniStringList>;

template <typename T>
T *getIf(Variant *V) {
    return std::get_if<T>(V);
}

template <typename T>
const T *getIf(const Variant *V) {
    return std::get_if<T>(V);
}
#else

// 候选类型 -> 标签编号(与 Variant 内 Tag_ 一致)
enum VariantTagValue {
    VariantTagEmpty = 0,
    VariantTagBool,
    VariantTagInt,
    VariantTagDouble,
    VariantTagString,
    VariantTagList,
};

template <typename T>
struct VariantTagOf {
    static const unsigned char Value = VariantTagEmpty;
};

template <>
struct VariantTagOf<bool> {
    static const unsigned char Value = VariantTagBool;
};

template <>
struct VariantTagOf<std::int64_t> {
    static const unsigned char Value = VariantTagInt;
};

template <>
struct VariantTagOf<double> {
    static const unsigned char Value = VariantTagDouble;
};

template <>
struct VariantTagOf<std::string> {
    static const unsigned char Value = VariantTagString;
};

template <>
struct VariantTagOf<IniStringList> {
    static const unsigned char Value = VariantTagList;
};

template <typename T>
struct VariantAccessor;

/**
 * @brief C++11 手写 variant 垫片: 只覆盖 IniVariant 的 5 个候选类型。
 * @notice 标量(bool/int64_t/double)直接存放于联合体; std::string / 列表经堆
 *         指针持有(移动即偷指针, 拷贝深拷贝), 避免 C++11 下在联合体成员上
 *         手工管理非平凡类型的构造/析构。
 */
class Variant {
public:
    // 默认构造等价 std::variant: 采用首个候选类型(bool false)
    Variant() : Tag_(VariantTagBool) {
        St_.Bool_ = false;
    }

    Variant(bool Value) : Tag_(VariantTagBool) {
        St_.Bool_ = Value;
    }

    Variant(std::int64_t Value) : Tag_(VariantTagInt) {
        St_.Int_ = Value;
    }

    Variant(double Value) : Tag_(VariantTagDouble) {
        St_.Double_ = Value;
    }

    Variant(const std::string &Value) : Tag_(VariantTagString) {
        St_.String_ = new std::string(Value);
    }

    Variant(std::string &&Value) : Tag_(VariantTagString) {
        St_.String_ = new std::string(std::move(Value));
    }

    Variant(const IniStringList &Value) : Tag_(VariantTagList) {
        St_.List_ = new IniStringList(Value);
    }

    Variant(IniStringList &&Value) : Tag_(VariantTagList) {
        St_.List_ = new IniStringList(std::move(Value));
    }

    Variant(const Variant &Other) : Tag_(Other.Tag_) {
        copyFrom(Other);
    }

    Variant(Variant &&Other) noexcept : Tag_(Other.Tag_) {
        moveFrom(Other);
        Other.Tag_ = VariantTagEmpty;
    }

    ~Variant() {
        destroy();
    }

    Variant &operator=(const Variant &Other) {
        if (this != &Other) {
            destroy();
            Tag_ = Other.Tag_;
            copyFrom(Other);
        }
        return *this;
    }

    Variant &operator=(Variant &&Other) noexcept {
        if (this != &Other) {
            destroy();
            Tag_ = Other.Tag_;
            moveFrom(Other);
            Other.Tag_ = VariantTagEmpty;
        }
        return *this;
    }

    template <typename T>
    friend T *getIf(Variant *V);

    template <typename T>
    friend const T *getIf(const Variant *V);

    template <typename T>
    friend struct VariantAccessor;

private:
    union Storage {
        bool Bool_;
        std::int64_t Int_;
        double Double_;
        std::string *String_;
        IniStringList *List_;
        Storage() : Int_(0) {
        }
    } St_;
    unsigned char Tag_;

    void destroy() {
        if (Tag_ == VariantTagString) {
            delete St_.String_;
            St_.String_ = nullptr;
        }
        else if (Tag_ == VariantTagList) {
            delete St_.List_;
            St_.List_ = nullptr;
        }
    }

    void copyFrom(const Variant &Other) {
        switch (Other.Tag_) {
            case VariantTagBool: St_.Bool_ = Other.St_.Bool_; break;
            case VariantTagInt: St_.Int_ = Other.St_.Int_; break;
            case VariantTagDouble: St_.Double_ = Other.St_.Double_; break;
            case VariantTagString: St_.String_ = new std::string(*Other.St_.String_); break;
            case VariantTagList: St_.List_ = new IniStringList(*Other.St_.List_); break;
            default: break;
        }
    }

    void moveFrom(Variant &Other) {
        switch (Other.Tag_) {
            case VariantTagBool: St_.Bool_ = Other.St_.Bool_; break;
            case VariantTagInt: St_.Int_ = Other.St_.Int_; break;
            case VariantTagDouble: St_.Double_ = Other.St_.Double_; break;
            case VariantTagString: St_.String_ = Other.St_.String_; break; // 偷指针
            case VariantTagList: St_.List_ = Other.St_.List_; break;
            default: break;
        }
    }
};

// 按标签取指针(仅供 getIf 使用; 访问 Variant 私有成员经 friend 授权)
template <typename T>
struct VariantAccessor {
    static T *getPtr(Variant *V) {
        (void) V;
        return nullptr;
    }

    static const T *getPtrConst(const Variant *V) {
        (void) V;
        return nullptr;
    }
};

template <>
struct VariantAccessor<bool> {
    static bool *getPtr(Variant *V) {
        return &V->St_.Bool_;
    }

    static const bool *getPtrConst(const Variant *V) {
        return &V->St_.Bool_;
    }
};

template <>
struct VariantAccessor<std::int64_t> {
    static std::int64_t *getPtr(Variant *V) {
        return &V->St_.Int_;
    }

    static const std::int64_t *getPtrConst(const Variant *V) {
        return &V->St_.Int_;
    }
};

template <>
struct VariantAccessor<double> {
    static double *getPtr(Variant *V) {
        return &V->St_.Double_;
    }

    static const double *getPtrConst(const Variant *V) {
        return &V->St_.Double_;
    }
};

template <>
struct VariantAccessor<std::string> {
    static std::string *getPtr(Variant *V) {
        return V->St_.String_;
    }

    static const std::string *getPtrConst(const Variant *V) {
        return V->St_.String_;
    }
};

template <>
struct VariantAccessor<IniStringList> {
    static IniStringList *getPtr(Variant *V) {
        return V->St_.List_;
    }

    static const IniStringList *getPtrConst(const Variant *V) {
        return V->St_.List_;
    }
};

// 类型不匹配返回 nullptr, 语义与 std::get_if 一致
template <typename T>
T *getIf(Variant *V) {
    if (V == nullptr || V->Tag_ != VariantTagOf<T>::Value) {
        return nullptr;
    }
    return VariantAccessor<T>::getPtr(V);
}

template <typename T>
const T *getIf(const Variant *V) {
    if (V == nullptr || V->Tag_ != VariantTagOf<T>::Value) {
        return nullptr;
    }
    return VariantAccessor<T>::getPtrConst(V);
}
#endif // PICTOR_HAS_CPP17

/**
 * @brief 优先级分派标签: 数字越大优先级越高。替代 if constexpr 分支链,
 *         靠"派生 -> 基类"的转换排名保持原有分支顺序。
 */
template <int N>
struct Priority : Priority<N - 1> {
};

template <>
struct Priority<0> {
};

/**
 * @brief 可转换为元素的 vector 检测。
 * @notice 置于 Ini 类声明之前: set<T> / get<T> 的分派重载声明需要引用其 ::value。
 */
template<typename T>
struct IsConvertibleVector : std::false_type {
};

template<typename E, typename A>
struct IsConvertibleVector<std::vector<E, A> >
    : infra::compat::bool_constant<std::is_integral<E>::value || std::is_floating_point<E>::value ||
                                   std::is_same<E, std::string>::value> {
};

/** @brief 内建可转换类型检测(get<T> 直接走 variant 转换)。 */
template<typename T>
struct IsBuiltinConvertible
    : infra::compat::bool_constant<std::is_same<T, bool>::value || std::is_same<T, std::int64_t>::value ||
                                   std::is_same<T, double>::value || std::is_same<T, std::string>::value ||
                                   std::is_same<T, IniStringList>::value || std::is_integral<T>::value ||
                                   std::is_floating_point<T>::value || IsConvertibleVector<T>::value> {
};

} // namespace ini_detail

// 值: bool / int64_t / double / std::string / 字符串列表
using IniVariant = ini_detail::Variant;

// 单个 Key-Value 条目: 附带该条目之前的注释与空行原文(往返保真)
struct IniEntry {
    std::string Key;
    IniVariant Value;
    std::vector<std::string> LeadingLines;
};

// 一个 Section: 附带头部注释、可选父 Section(继承)
struct IniSection {
    std::vector<std::string> HeaderLines;              // Section 头之前的注释与空行原文
    infra::compat::Optional<std::string> Parent;       // [Child : Parent]
    std::vector<IniEntry> Entries;                     // 有序 Key-Value
};

using KeyValueType = std::vector<IniEntry>;

/**
 * @brief Section 名哈希。
 * @notice 异构查找(以 std::string_view 直接查 std::string 键)需要 C++20 起
 *         透明的哈希与相等比较; 为统一 C++11 语义, 查找侧显式构造临时
 *         std::string, 哈希只面向 std::string。
 */
struct IniHash {
    std::size_t operator()(const std::string &Key) const noexcept {
        return std::hash<std::string>{}(Key);
    }
};

// 整个文件: Section 名 -> Section 内容。
using SectionType = std::unordered_map<std::string, IniSection, IniHash>;

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
    PICTOR_NODISCARD static infra::serde_detail::Expected<Ini, IniError> fromString(infra::compat::string_view IniContent);

    PICTOR_NODISCARD static infra::serde_detail::Expected<Ini, IniError> fromFile(infra::compat::string_view FilePath);

    // ---------- 加载与解析 ----------
    /** @brief 从文件加载并解析(读取委托文件层)。 */
    PICTOR_NODISCARD infra::serde_detail::Expected<void, IniError> load(infra::compat::string_view FilePath);

    /** @brief mmap 文件后解析(Windows: 文件映射; 其他平台退化为普通读取)。 */
    PICTOR_NODISCARD infra::serde_detail::Expected<void, IniError> loadMapped(infra::compat::string_view FilePath);

    /** @brief 解析内存中的 INI 内容。 */
    PICTOR_NODISCARD infra::serde_detail::Expected<void, IniError> parse(infra::compat::string_view IniContent);

    /** @brief 流式解析: 从输入流逐行解析, 不要求一次性持有全部内容。 */
    PICTOR_NODISCARD infra::serde_detail::Expected<void, IniError> parse(std::istream &Stream);

    /** @brief 写回: 把当前内容序列化后保存(无参数时写回 load 的来源文件)。 */
    PICTOR_NODISCARD infra::serde_detail::Expected<void, IniError> save(infra::compat::string_view FilePath) const;

    PICTOR_NODISCARD infra::serde_detail::Expected<void, IniError> save() const;

    PICTOR_NODISCARD bool isLoaded() const noexcept;

    /** @brief 最近一次失败的结构化错误; 成功后为 Code::None。 */
    PICTOR_NODISCARD const IniError &lastError() const noexcept;

    // ---------- 行缓冲区 ----------
    /** @brief 行缓冲区: 解析时逐行保存原文(已去掉行尾 \r), 含注释与空行;
     *         任何写入/删除都会使其失效, 需经 flash 重建。 */
    PICTOR_NODISCARD const std::vector<std::string> &lineBuffer() const noexcept;

    /** @brief 释放行缓冲区: 以移动语义取出全部行, 本对象缓冲区置空。 */
    PICTOR_NODISCARD std::vector<std::string> releaseLineBuffer();

    /** @brief 把当前 Sections 反序列化回 std::string, 逐行写回缓冲区并返回全文。 */
    PICTOR_NODISCARD std::string flash();

    /** @brief 把 Sections 反序列化回 std::string(不动缓冲区)。 */
    PICTOR_NODISCARD std::string serialize() const;

    // ---------- 策略与检查器 ----------
    PICTOR_NODISCARD const IniChecker &checker() const noexcept {
        return Checker;
    }

    // ---------- 结构查询 ----------
    /** @brief 所有 Section 名, 按文件出现顺序。 */
    PICTOR_NODISCARD std::vector<std::string> sections() const;

    /** @brief 指定 Section 自身(不含继承)的 Key, 按文件出现顺序。 */
    PICTOR_NODISCARD std::vector<std::string> keys(infra::compat::string_view SectionName) const;

    /** @brief 指定 Section 的结构; 不存在返回 nullptr。 */
    PICTOR_NODISCARD const IniSection *section(infra::compat::string_view SectionName) const noexcept;

    PICTOR_NODISCARD bool hasSection(infra::compat::string_view SectionName) const noexcept;

    /** @brief 是否含 Key(沿 [Child : Parent] 继承链查找)。 */
    PICTOR_NODISCARD bool hasKey(infra::compat::string_view SectionName, infra::compat::string_view Key) const noexcept;

    // ---------- 取值 ----------
    /** @brief 模板取值: 内建类型(bool/整数/浮点/字符串/字符串列表/算术 vector)
     *         直接转换; 已注册的自定义类型经 registerType 的转换函数取值。
     *         失败返回 Default。 */
    template<typename T>
    PICTOR_NODISCARD T get(infra::compat::string_view SectionName,
                           infra::compat::string_view Key,
                           T Default = {}) const;

    /** @brief 取原始 variant 值; 不存在返回 nullopt。 */
    PICTOR_NODISCARD infra::compat::Optional<IniVariant> getVariant(infra::compat::string_view SectionName,
                                                                    infra::compat::string_view Key) const;

    // ---------- 写入与删除 ----------
    /** @brief 写入/覆盖一个 Key-Value(字符串形式, 值与解析走同一类型推断)。 */
    PICTOR_NODISCARD infra::serde_detail::Expected<void, IniError> set(infra::compat::string_view SectionName,
                                                                infra::compat::string_view Key,
                                                                infra::compat::string_view Value);

    /** @brief 写入/覆盖: 内建类型直接字符串化; 自定义类型经 registerType 转换。 */
    template<typename T>
    PICTOR_NODISCARD infra::serde_detail::Expected<void, IniError> set(infra::compat::string_view SectionName,
                                                                infra::compat::string_view Key,
                                                                const T &Value);

    /** @brief 注册自定义类型转换: T <-> 字符串。 */
    template<typename T>
    void registerType(std::function<infra::compat::Optional<std::string>(const T &)> ToString,
                      std::function<infra::compat::Optional<T>(infra::compat::string_view)> FromString);

    /** @brief 删除指定 Key(仅自身, 不含继承), 成功返回 true。 */
    bool erase(infra::compat::string_view SectionName, infra::compat::string_view Key) noexcept;

    /** @brief 删除整个 Section(含顺序表), 成功返回 true。 */
    bool eraseSection(infra::compat::string_view SectionName) noexcept;

    // ---------- 其他 ----------
    /** @brief 把 Other 合并进当前对象: 相同 Section 合并, 相同 Key 后者覆盖。 */
    void merge(const Ini &Other);

private:
    struct ParseState;

    struct ConverterEntry {
        std::function<infra::compat::Optional<std::string>(const void *Value)> ToString; // Value 指向 T
        std::function<infra::compat::Optional<Any>(infra::compat::string_view Text)> FromString;
    };

    // get<T> 的三路分派: IniVariant 精确匹配 > 内建可转换 > 自定义注册
    template<typename T>
    T getImpl(const IniEntry *Entry, T Default, ini_detail::Priority<2>, std::true_type) const;

    template<typename T>
    infra::compat::enable_if_t<ini_detail::IsBuiltinConvertible<T>::value, T>
    getImpl(const IniEntry *Entry, T Default, ini_detail::Priority<1>, std::false_type) const;

    template<typename T>
    infra::compat::enable_if_t<!ini_detail::IsBuiltinConvertible<T>::value, T>
    getImpl(const IniEntry *Entry, T Default, ini_detail::Priority<0>, std::false_type) const;

    // set<T> 的五路分派: IniVariant > vector > 字符串类 > 标量 > 自定义注册
    // (各重载经 enable_if 互斥, ini_detail::Priority 只决定分支优先级)
    template<typename T>
    infra::compat::enable_if_t<std::is_same<T, IniVariant>::value, infra::serde_detail::Expected<void, IniError> >
    setImpl(infra::compat::string_view SectionName,
            infra::compat::string_view Key,
            const T &Value,
            ini_detail::Priority<4>);

    template<typename T>
    infra::compat::enable_if_t<ini_detail::IsConvertibleVector<infra::compat::decay_t<T> >::value,
                               infra::serde_detail::Expected<void, IniError> >
    setImpl(infra::compat::string_view SectionName,
            infra::compat::string_view Key,
            const T &Value,
            ini_detail::Priority<3>);

    template<typename T>
    infra::compat::enable_if_t<std::is_same<infra::compat::decay_t<T>, std::string>::value ||
                                   std::is_same<infra::compat::decay_t<T>, infra::compat::string_view>::value ||
                                   std::is_convertible<const T &, infra::compat::string_view>::value,
                               infra::serde_detail::Expected<void, IniError> >
    setImpl(infra::compat::string_view SectionName,
            infra::compat::string_view Key,
            const T &Value,
            ini_detail::Priority<2>);

    template<typename T>
    infra::compat::enable_if_t<std::is_same<infra::compat::decay_t<T>, bool>::value ||
                                   std::is_integral<infra::compat::decay_t<T> >::value ||
                                   std::is_floating_point<infra::compat::decay_t<T> >::value,
                               infra::serde_detail::Expected<void, IniError> >
    setImpl(infra::compat::string_view SectionName,
            infra::compat::string_view Key,
            const T &Value,
            ini_detail::Priority<1>);

    template<typename T>
    infra::compat::enable_if_t<!std::is_same<T, IniVariant>::value &&
                                   !ini_detail::IsConvertibleVector<infra::compat::decay_t<T> >::value &&
                                   !std::is_same<infra::compat::decay_t<T>, std::string>::value &&
                                   !std::is_same<infra::compat::decay_t<T>, infra::compat::string_view>::value &&
                                   !std::is_convertible<const T &, infra::compat::string_view>::value &&
                                   !std::is_same<infra::compat::decay_t<T>, bool>::value &&
                                   !std::is_integral<infra::compat::decay_t<T> >::value &&
                                   !std::is_floating_point<infra::compat::decay_t<T> >::value,
                               infra::serde_detail::Expected<void, IniError> >
    setImpl(infra::compat::string_view SectionName,
            infra::compat::string_view Key,
            const T &Value,
            ini_detail::Priority<0>);

    /** @brief 按 Section 名查找, 找不到返回 nullptr。 */
    PICTOR_NODISCARD const IniSection *findSection(infra::compat::string_view SectionName) const noexcept;

    IniSection *findSection(infra::compat::string_view SectionName) noexcept;

    /** @brief 在 Entries 中按策略折叠后的键名查找下标, 找不到返回 npos。 */
    PICTOR_NODISCARD std::size_t findKeyIndex(const KeyValueType &Entries,
                                              infra::compat::string_view Key) const noexcept;

    /** @brief 沿继承链查找条目(带环保护)。 */
    PICTOR_NODISCARD const IniEntry *findEntry(infra::compat::string_view SectionName,
                                               infra::compat::string_view Key) const noexcept;

    /** @brief 单行处理(parse 文本/流共用)。 */
    PICTOR_NODISCARD infra::compat::Optional<IniError> feedLine(infra::compat::string_view RawLine,
                                                                std::size_t LineNumber,
                                                                ParseState &State);

    /** @brief 提交一条已解析的键值对(检查/修正/插值/类型推断/存储)。 */
    PICTOR_NODISCARD infra::compat::Optional<IniError> commitEntry(IniKeyValueRaw &Raw,
                                                                   std::size_t LineNumber,
                                                                   ParseState &State);

    /** @brief 收尾: 提交残留续行与尾注, 标记加载完成。 */
    PICTOR_NODISCARD infra::compat::Optional<IniError> finishParse(ParseState &State);

    /** @brief 记录错误并返回 unexpected。 */
    PICTOR_NODISCARD infra::serde_detail::Expected<void, IniError> fail(IniError Error);

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

/** @brief string_view -> std::string(C++11 无 std::string(string_view) 构造, 统一走 data/size)。 */
inline std::string viewToString(infra::compat::string_view Text) {
    return std::string(Text.data(), Text.size());
}

/** @brief std::string -> string_view(两种标准一致的显式构造)。 */
inline infra::compat::string_view toStringView(const std::string &Text) noexcept {
    return infra::compat::string_view(Text.data(), Text.size());
}

/**
 * @brief 无 RTTI 的类型标识: 每个 T 一份静态字节, 地址即标签(COMDAT 合并)。
 * @notice C++11 无类内成员变量模板(C++14 设施), 故用类模板静态成员代替
 *         原 inline static constexpr char TypeTag; 模式同 containers/Any.hpp。
 */
template<typename T>
struct TypeTag {
    static const char Value;
};

template<typename T>
const char TypeTag<T>::Value = 0;

/** @brief 解析布尔文本。 */
inline bool tryParseBool(infra::compat::string_view Text, bool &Out) {
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
inline bool tryParseInt(infra::compat::string_view Text, std::int64_t &Out) noexcept {
    const char *const Begin             = Text.data();
    const char *const End               = Text.data() + Text.size();
    std::int64_t Value                  = 0;
    const infra::compat::from_chars_result Result = infra::compat::from_chars(Begin, End, Value);
    if (Result.ec != std::errc{} || Result.ptr != End) {
        return false;
    }
    Out = Value;
    return true;
}

/** @brief 解析浮点文本(必须完整消费)。 */
inline bool tryParseDouble(infra::compat::string_view Text, double &Out) noexcept {
    const char *const Begin             = Text.data();
    const char *const End               = Text.data() + Text.size();
    double Value                        = 0.0;
    const infra::compat::from_chars_result Result = infra::compat::from_chars(Begin, End, Value);
    if (Result.ec != std::errc{} || Result.ptr != End) {
        return false;
    }
    Out = Value;
    return true;
}

/** @brief 浮点 -> 文本。
 * @notice C++11 下 infra::compat::to_chars 的整数模板无 enable_if 约束, 与浮点
 *         重载构成调用歧义; 此处按垫片语义直接实现(snprintf %.17g, 与垫片一致),
 *         C++17 起仍走 std::to_chars 保证最短往返表示。 */
inline std::string doubleToString(double Value) {
#if PICTOR_HAS_CPP17
    char Buffer[64];
    const infra::compat::to_chars_result Result =
        infra::compat::to_chars(Buffer, Buffer + sizeof(Buffer), Value);
    return std::string(Buffer, Result.ptr);
#else
    char Buffer[64];
    const int Written = std::snprintf(Buffer, sizeof(Buffer), "%.17g", Value);
    if (Written < 0) {
        return std::string();
    }
    return std::string(Buffer, static_cast<std::size_t>(Written));
#endif
}

/** @brief 标量 -> 文本(按类型重载, 等价原 if constexpr 链)。 */
inline std::string scalarToString(bool Value) {
    return Value ? "true" : "false";
}

template<typename T>
inline infra::compat::enable_if_t<std::is_integral<T>::value && !std::is_same<T, bool>::value,
                                  std::string>
scalarToString(const T &Value) {
    return std::to_string(Value);
}

template<typename T>
inline infra::compat::enable_if_t<std::is_floating_point<T>::value, std::string>
scalarToString(const T &Value) {
    return doubleToString(static_cast<double>(Value));
}

inline std::string scalarToString(const std::string &Value) {
    return Value;
}

inline std::string scalarToString(infra::compat::string_view Value) {
    return viewToString(Value);
}

inline std::string scalarToString(const char *Value) {
    return std::string(Value);
}

/** @brief 值是否需要引号包裹。 */
PICTOR_NODISCARD inline bool needsQuoting(infra::compat::string_view Value) noexcept {
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
PICTOR_NODISCARD inline std::string escapeValue(infra::compat::string_view Value) {
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
PICTOR_NODISCARD inline std::string valueToText(infra::compat::string_view Value) {
    return needsQuoting(Value) ? escapeValue(Value) : viewToString(Value);
}

/** @brief 列表 -> "[a, b, c]" 文本。 */
PICTOR_NODISCARD inline std::string listToString(const IniStringList &List) {
    std::string Out = "[";
    for (std::size_t I = 0; I < List.size(); ++I) {
        if (I != 0) {
            Out += ", ";
        }
        Out += valueToText(toStringView(List[I]));
    }
    Out += ']';
    return Out;
}

/** @brief variant -> 文本(标量转字符串, 列表加方括号)。 */
PICTOR_NODISCARD inline std::string variantToString(const IniVariant &Value) {
    if (const bool *P = getIf<bool>(&Value)) {
        return scalarToString(*P);
    }
    if (const std::int64_t *P = getIf<std::int64_t>(&Value)) {
        return scalarToString(*P);
    }
    if (const double *P = getIf<double>(&Value)) {
        return scalarToString(*P);
    }
    if (const std::string *P = getIf<std::string>(&Value)) {
        return scalarToString(*P);
    }
    if (const IniStringList *P = getIf<IniStringList>(&Value)) {
        return listToString(*P);
    }
    return std::string(); // 兜底(空 variant, 理论上不可达)
}

/** @brief IniVariant -> T (失败返回 nullopt)。 */
template<typename T>
infra::compat::Optional<T> getAs(const IniVariant &V);

// ---- getAs 分派: 优先级链保持原 if constexpr 分支顺序 ----
// bool(最高优先级)
template<typename T>
inline infra::compat::enable_if_t<std::is_same<T, bool>::value, infra::compat::Optional<T> >
getAsImpl(const IniVariant &V, Priority<8>) {
    if (const bool *P = getIf<bool>(&V)) {
        return *P;
    }
    if (const std::int64_t *P = getIf<std::int64_t>(&V)) {
        return *P != 0;
    }
    if (const double *P = getIf<double>(&V)) {
        return *P != 0.0;
    }
    if (const std::string *P = getIf<std::string>(&V)) {
        bool Out = false;
        return tryParseBool(toStringView(*P), Out)
                   ? infra::compat::Optional<bool>(Out)
                   : infra::compat::nullopt;
    }
    return infra::compat::nullopt;
}

// std::int64_t
template<typename T>
inline infra::compat::enable_if_t<std::is_same<T, std::int64_t>::value, infra::compat::Optional<T> >
getAsImpl(const IniVariant &V, Priority<7>) {
    if (const std::int64_t *P = getIf<std::int64_t>(&V)) {
        return *P;
    }
    if (const double *P = getIf<double>(&V)) {
        return static_cast<std::int64_t>(*P);
    }
    if (const bool *P = getIf<bool>(&V)) {
        return *P ? 1 : 0;
    }
    if (const std::string *P = getIf<std::string>(&V)) {
        std::int64_t Out = 0;
        return tryParseInt(toStringView(*P), Out)
                   ? infra::compat::Optional<std::int64_t>(Out)
                   : infra::compat::nullopt;
    }
    return infra::compat::nullopt;
}

// double
template<typename T>
inline infra::compat::enable_if_t<std::is_same<T, double>::value, infra::compat::Optional<T> >
getAsImpl(const IniVariant &V, Priority<6>) {
    if (const double *P = getIf<double>(&V)) {
        return *P;
    }
    if (const std::int64_t *P = getIf<std::int64_t>(&V)) {
        return static_cast<double>(*P);
    }
    if (const bool *P = getIf<bool>(&V)) {
        return *P ? 1.0 : 0.0;
    }
    if (const std::string *P = getIf<std::string>(&V)) {
        double Out = 0.0;
        return tryParseDouble(toStringView(*P), Out)
                   ? infra::compat::Optional<double>(Out)
                   : infra::compat::nullopt;
    }
    return infra::compat::nullopt;
}

// std::string
template<typename T>
inline infra::compat::enable_if_t<std::is_same<T, std::string>::value, infra::compat::Optional<T> >
getAsImpl(const IniVariant &V, Priority<5>) {
    if (const std::string *P = getIf<std::string>(&V)) {
        return *P;
    }
    return variantToString(V);
}

// IniStringList
template<typename T>
inline infra::compat::enable_if_t<std::is_same<T, IniStringList>::value, infra::compat::Optional<T> >
getAsImpl(const IniVariant &V, Priority<4>) {
    if (const IniStringList *P = getIf<IniStringList>(&V)) {
        return *P;
    }
    if (const std::string *P = getIf<std::string>(&V)) {
        return IniStringList{*P}; // 标量 -> 单元素列表
    }
    return infra::compat::nullopt;
}

// 其余整数(bool 与 int64_t 已有更高优先级分支)
template<typename T>
inline infra::compat::enable_if_t<std::is_integral<T>::value && !std::is_same<T, bool>::value &&
                                      !std::is_same<T, std::int64_t>::value,
                                  infra::compat::Optional<T> >
getAsImpl(const IniVariant &V, Priority<3>) {
    const infra::compat::Optional<std::int64_t> I = getAs<std::int64_t>(V);
    if (I) {
        return infra::compat::Optional<T>(static_cast<T>(*I));
    }
    return infra::compat::nullopt;
}

// 其余浮点(double 已有更高优先级分支)
template<typename T>
inline infra::compat::enable_if_t<std::is_floating_point<T>::value && !std::is_same<T, double>::value,
                                  infra::compat::Optional<T> >
getAsImpl(const IniVariant &V, Priority<2>) {
    const infra::compat::Optional<double> D = getAs<double>(V);
    if (D) {
        return infra::compat::Optional<T>(static_cast<T>(*D));
    }
    return infra::compat::nullopt;
}

// 可转换 vector
template<typename T>
inline infra::compat::enable_if_t<IsConvertibleVector<T>::value, infra::compat::Optional<T> >
getAsImpl(const IniVariant &V, Priority<1>) {
    const IniStringList *List = getIf<IniStringList>(&V);
    if (List == nullptr) {
        return infra::compat::nullopt;
    }
    T Out;
    Out.reserve(List->size());
    for (std::size_t I = 0; I < List->size(); ++I) {
        const infra::compat::Optional<typename T::value_type> Converted =
            getAs<typename T::value_type>(IniVariant{(*List)[I]});
        if (!Converted.has_value()) {
            return infra::compat::nullopt;
        }
        Out.push_back(std::move(*Converted));
    }
    return Out;
}

// 兜底(最低优先级)
template<typename T>
inline infra::compat::Optional<T> getAsImpl(const IniVariant &V, Priority<0>) {
    (void) V;
    return infra::compat::nullopt; // 自定义类型由 Ini::get 处理
}

/** @brief IniVariant -> T (失败返回 nullopt)。 */
template<typename T>
infra::compat::Optional<T> getAs(const IniVariant &V) {
    return getAsImpl<T>(V, Priority<8>());
}

} // namespace ini_detail

template<typename T>
T Ini::get(infra::compat::string_view SectionName, infra::compat::string_view Key, T Default) const {
    const IniEntry *Entry = findEntry(SectionName, Key);
    if (Entry == nullptr) {
        return Default;
    }
    return getImpl<T>(Entry, Default, ini_detail::Priority<2>(), std::is_same<T, IniVariant>());
}

// IniVariant 精确匹配(最高优先级)
template<typename T>
T Ini::getImpl(const IniEntry *Entry, T Default, ini_detail::Priority<2>, std::true_type) const {
    (void) Default;
    return Entry->Value;
}

// 内建可转换类型: 直接经 variant 转换
template<typename T>
infra::compat::enable_if_t<ini_detail::IsBuiltinConvertible<T>::value, T>
Ini::getImpl(const IniEntry *Entry, T Default, ini_detail::Priority<1>, std::false_type) const {
    const infra::compat::Optional<T> Converted = ini_detail::getAs<T>(Entry->Value);
    if (Converted) {
        return *Converted;
    }
    return Default;
}

// 自定义类型: 经注册表转换
template<typename T>
infra::compat::enable_if_t<!ini_detail::IsBuiltinConvertible<T>::value, T>
Ini::getImpl(const IniEntry *Entry, T Default, ini_detail::Priority<0>, std::false_type) const {
    const auto It = Converters.find(&ini_detail::TypeTag<T>::Value);
    if (It == Converters.end()) {
        return Default;
    }
    const infra::compat::Optional<std::string> Text = ini_detail::getAs<std::string>(Entry->Value);
    if (!Text.has_value()) {
        return Default;
    }
    const infra::compat::Optional<Any> Converted = It->second.FromString(ini_detail::toStringView(*Text));
    if (!Converted.has_value()) {
        return Default;
    }
    if (const T *Ptr = Converted->castPtr<T>()) {
        return *Ptr;
    }
    return Default;
}

template<typename T>
infra::serde_detail::Expected<void, IniError> Ini::set(infra::compat::string_view SectionName,
                                                infra::compat::string_view Key,
                                                const T &Value) {
    return setImpl<T>(SectionName, Key, Value, ini_detail::Priority<4>());
}

// DT == IniVariant(最高优先级)
template<typename T>
infra::compat::enable_if_t<std::is_same<T, IniVariant>::value, infra::serde_detail::Expected<void, IniError> >
Ini::setImpl(infra::compat::string_view SectionName,
             infra::compat::string_view Key,
             const T &Value,
             ini_detail::Priority<4>) {
    return set(SectionName, Key, ini_detail::variantToString(Value));
}

// 可转换 vector -> 列表
template<typename T>
infra::compat::enable_if_t<ini_detail::IsConvertibleVector<infra::compat::decay_t<T> >::value,
                           infra::serde_detail::Expected<void, IniError> >
Ini::setImpl(infra::compat::string_view SectionName,
             infra::compat::string_view Key,
             const T &Value,
             ini_detail::Priority<3>) {
    IniStringList List;
    List.reserve(Value.size());
    for (const auto &Element: Value) {
        List.push_back(ini_detail::scalarToString(Element));
    }
    return set(SectionName, Key, ini_detail::listToString(List));
}

// 字符串类: std::string / string_view / char[N] / char* 等
template<typename T>
infra::compat::enable_if_t<std::is_same<infra::compat::decay_t<T>, std::string>::value ||
                               std::is_same<infra::compat::decay_t<T>, infra::compat::string_view>::value ||
                               std::is_convertible<const T &, infra::compat::string_view>::value,
                           infra::serde_detail::Expected<void, IniError> >
Ini::setImpl(infra::compat::string_view SectionName,
             infra::compat::string_view Key,
             const T &Value,
             ini_detail::Priority<2>) {
    return set(SectionName, Key, ini_detail::toStringView(Value));
}

// 标量: bool / 整数 / 浮点
template<typename T>
infra::compat::enable_if_t<std::is_same<infra::compat::decay_t<T>, bool>::value ||
                               std::is_integral<infra::compat::decay_t<T> >::value ||
                               std::is_floating_point<infra::compat::decay_t<T> >::value,
                           infra::serde_detail::Expected<void, IniError> >
Ini::setImpl(infra::compat::string_view SectionName,
             infra::compat::string_view Key,
             const T &Value,
             ini_detail::Priority<1>) {
    return set(SectionName, Key, ini_detail::scalarToString(Value));
}

// 自定义类型: 经注册表转换
template<typename T>
infra::compat::enable_if_t<!std::is_same<T, IniVariant>::value &&
                               !ini_detail::IsConvertibleVector<infra::compat::decay_t<T> >::value &&
                               !std::is_same<infra::compat::decay_t<T>, std::string>::value &&
                               !std::is_same<infra::compat::decay_t<T>, infra::compat::string_view>::value &&
                               !std::is_convertible<const T &, infra::compat::string_view>::value &&
                               !std::is_same<infra::compat::decay_t<T>, bool>::value &&
                               !std::is_integral<infra::compat::decay_t<T> >::value &&
                               !std::is_floating_point<infra::compat::decay_t<T> >::value,
                           infra::serde_detail::Expected<void, IniError> >
Ini::setImpl(infra::compat::string_view SectionName,
             infra::compat::string_view Key,
             const T &Value,
             ini_detail::Priority<0>) {
    using DT = infra::compat::decay_t<T>;
    const auto It = Converters.find(&ini_detail::TypeTag<DT>::Value);
    if (It == Converters.end()) {
        return fail(IniError{IniError::Code::UnsupportedValueType, 0, ini_detail::viewToString(Key)});
    }
    const infra::compat::Optional<std::string> Text = It->second.ToString(&Value);
    if (!Text.has_value()) {
        return fail(IniError{IniError::Code::UnsupportedValueType, 0, ini_detail::viewToString(Key)});
    }
    return set(SectionName, Key, ini_detail::toStringView(*Text));
}

template<typename T>
void Ini::registerType(std::function<infra::compat::Optional<std::string>(const T &)> ToString,
                       std::function<infra::compat::Optional<T>(infra::compat::string_view)> FromString) {
    using TClean = infra::compat::decay_t<T>;
    ConverterEntry Entry;
    // @notice C++11 无 init-capture([Fn = ...]), 直接按名捕获参数(内部本就持有拷贝)
    Entry.ToString = [ToString](const void *Ptr) {
        return ToString(*static_cast<const TClean *>(Ptr));
    };
    Entry.FromString = [FromString](infra::compat::string_view Text) -> infra::compat::Optional<Any> {
        infra::compat::Optional<TClean> Result = FromString(Text);
        if (Result) {
            return Any(std::move(*Result));
        }
        return infra::compat::nullopt;
    };
    Converters[&ini_detail::TypeTag<TClean>::Value] = std::move(Entry);
}

#endif //HELLOVULKAN_INI_H
