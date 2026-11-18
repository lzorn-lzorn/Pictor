//
// Created by Administrator on 2026/11/18.
//

#ifndef HELLOVULKAN_INI_H
#define HELLOVULKAN_INI_H

#include <cstdint>
#include <optional>
#include <array>
#include <string_view>
#include <unordered_map>
#include <vector>

// serde 对 container 模块的代码级依赖：使用 containers 提供的 Any 类型擦除容器。
#include "infra/containers/Any.h"


using SectionType = std::unordered_map<std::string_view, Any>;

constexpr std::array SupportedAnnotationDelim = { '#', ';' };
constexpr std::array SupportedEqualNotation = { '=', ':' };
constexpr std::array UnsupportedChar = { "" };

enum class ECaseMode : uint8_t
{
    Default = 0, // 区分大小写
    UpperCase = 1, // 所有字母转为大写
    LowerCase = 2, // 所有字母转为小写
};


struct IniReaderConfig
{
    // 是否允许 Key 中间带有空格, 例如 "Var Name = Name", "Var" 和 "Name" 中间的空格是否允许
    bool IsAllowInteriorSpaceInKey = false;

    // Key 是否区分大小写
    ECaseMode KeyCaseMode = ECaseMode::Default;
};

class Ini
{
    constexpr static int32_t DefaultIniKeyLength = 64;
    constexpr static int32_t DefaultIniValueLength = 32;
    constexpr static int32_t DefaultIniItemSymbolNumber = 6;
public:
    Ini() = default;
    explicit Ini(std::string_view IniContent);

    ~Ini() = default;
    Ini(const Ini&) = default;
    Ini(Ini&&) noexcept = default;
    Ini& operator=(const Ini&) = default;
    Ini& operator=(Ini&&) noexcept = default;

private:
    bool parse(std::string_view IniContent);

    /**
     * @brief 解析一组 Key-Value: Version = 1.1.4
     * @param IniContent "Version = 1.1.4"
     * @return
     */
    std::optional<SectionType> parseKeyValue(std::string_view IniContent);

    IniReaderConfig ReaderConfig;
    std::vector<SectionType> Sections;
};

#endif //HELLOVULKAN_INI_H
