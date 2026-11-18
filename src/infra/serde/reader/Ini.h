//
// Created by Administrator on 2026/11/18.
//

#ifndef HELLOVULKAN_INI_H
#define HELLOVULKAN_INI_H

#include <cstdint>
#include <optional>
#include <array>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

// serde 对 container 模块的代码级依赖：使用 containers 提供的 Any 类型擦除容器。
#include "infra/containers/Any.h"

enum class ECaseMode : uint8_t
{
    Default = 0, // 区分大小写
    UpperCase = 1, // 所有字母转为大写
    LowerCase = 2, // 所有字母转为小写
};

constexpr std::array SupportedAnnotation = { '#', ';' };
constexpr std::array SupportedEqualNotation = { '=', ':' };

// 如果 Section Name 解析失败(空名 / 名称含非法字符), 配置如何处理
enum class ESectionErrorHandle : uint8_t
{
    Drop,             // 丢弃: 该畸形 Section 及其下所有 Key-Value 被忽略, 不报错
    TransferToDefault, // 合入: 该 Section 下的 Key-Value 并入默认 Section
};

// 默认 Section 的名字, 如果出现重名则合并为 Default
constexpr std::string_view DefaultSectionName = "Default";

// 实际采用的畸形 Section 名处理策略(编译期开关)
constexpr auto SectionErrorHandle = ESectionErrorHandle::Drop;

// 在 Section 中除了 数字, 字母之外还可以使用的字符
constexpr std::array SupportedExtraCharInSectionName = { '_', '.', '-', '|'};
// Key 中禁止使用的字符, 遇到会直接跳过该 Key-Value
constexpr std::array UnsupportedCharInKey = { '\"', '\'', '\\', '[', ']', '{', '}', '(', ')' };

// Key 是否区分大小写
constexpr auto KeyCaseMode = ECaseMode::Default;

// 以下开关均是, 开启时出现不匹配则跳过. 关闭时
// 是否启用严格检查, 严格检查即 Key = Value (其中 "=" 是 SupportedEqualNotation 中的元素), 这三个要素必须出现
// 开启时: 不匹配则跳过
// 关闭时: 不匹配会为 Key 赋值为 DefaultValue
constexpr bool EnableStrictInspection = true;
constexpr std::string_view DefaultValue = "NULL";

// 是否启用 Key 的合法性检查: Key 只能由 字母, 数字, 下划线组成, 且数字不能为开头第一个字符, 与 EnableInteriorSpaceInKey 不冲突
// 开启时: 不匹配则跳过
// 关闭时: 匹配成功
constexpr bool EnableKeyNameValidationCheck = true;

// 是否启用 Key 中间带有空格, 例如 "Var Name = Name", "Var" 和 "Name" 中间的空格是否允许. 与 EnableKeyNameValidationCheck 不冲突
// 开启时: "Var Name = Name" 成功通过, Key = "Var Name"
// 关闭时: 则跳过 "Var Name" 这一项
constexpr bool EnableInteriorSpaceInKey = true;

// 单个 Key-Value 条目: Key 为键名, Value 为经类型推断后的值(int64_t / double / bool / std::string)。
struct IniValue
{
    std::string Key;
    Any Value;
};

// 一个 Section: 按文件顺序保存的 Key-Value 列表, 重复 Key 后者覆盖前者。
using KeyValueType = std::vector<IniValue>;

// 整个文件: Section 名 -> Section 内容。
using SectionType = std::unordered_map<std::string, KeyValueType>;

class Ini
{
    constexpr static int32_t DefaultIniKeyLength = 64;
    constexpr static int32_t DefaultIniValueLength = 32;
public:
    Ini() = default;
    explicit Ini(std::string_view IniContent);

    ~Ini() = default;
    Ini(const Ini&) = default;
    Ini(Ini&&) noexcept = default;
    Ini& operator=(const Ini&) = default;
    Ini& operator=(Ini&&) noexcept = default;

    // ---------- 加载与解析 ----------
    /** @brief 从文件加载并解析, 失败返回 false 并记录 error。 */
    bool load(std::string_view FilePath);
    /** @brief 解析内存中的 INI 内容, 失败返回 false 并记录 error。 */
    bool parse(std::string_view IniContent);

    [[nodiscard]] bool isLoaded() const noexcept;
    [[nodiscard]] const std::string& error() const noexcept;

    // ---------- 结构查询 ----------
    /** @brief 所有 Section 名, 按文件出现顺序。 */
    [[nodiscard]] std::vector<std::string> sections() const;
    /** @brief 指定 Section 内的所有 Key, 按文件出现顺序(去重)。 */
    [[nodiscard]] std::vector<std::string> keys(std::string_view SectionName) const;
    [[nodiscard]] bool hasSection(std::string_view SectionName) const noexcept;
    [[nodiscard]] bool hasKey(std::string_view SectionName, std::string_view Key) const noexcept;

    // ---------- 取值 ----------
    /** @brief 取字符串值; 不存在或不是字符串时返回 Default。 */
    [[nodiscard]] std::string get(std::string_view SectionName, std::string_view Key, std::string_view Default = "") const;
    [[nodiscard]] int64_t getInt(std::string_view SectionName, std::string_view Key, int64_t Default = 0) const;
    [[nodiscard]] double getDouble(std::string_view SectionName, std::string_view Key, double Default = 0.0) const;
    [[nodiscard]] bool getBool(std::string_view SectionName, std::string_view Key, bool Default = false) const;
    /** @brief 取原始类型擦除值; 不存在返回空 Any。 */
    [[nodiscard]] Any value(std::string_view SectionName, std::string_view Key) const;

    // ---------- 其他 ----------
    /** @brief 返回指定 Section 的有序 Key-Value 列表, 找不到返回空列表。 */
    [[nodiscard]] KeyValueType getSectionByName(std::string_view SectionName) const;
    /** @brief 是否包含指定 Section(等价 hasSection)。 */
    [[nodiscard]] bool isContain(std::string_view SectionName) const noexcept;
    /** @brief 把 Other 合并进当前对象: 相同 Section 合并, 相同 Key 后者覆盖。 */
    void merge(const Ini& Other);

private:
    /** @brief 解析一组 Key-Value: "Version = 1.1.4" -> IniValue{ "Version", Any(...) }。 */
    static std::optional<IniValue> parseKeyValue(std::string_view IniKeyValue);

    /** @brief 按 Section 名查找其内容, 找不到返回 nullptr。 */
    const KeyValueType* findSection(std::string_view SectionName) const noexcept;

    SectionType Sections;                 // Section 名 -> 有序 Key-Value
    std::vector<std::string> SectionOrder; // Section 出现顺序
    bool Loaded = false;
    std::string Error;
};

#endif //HELLOVULKAN_INI_H
