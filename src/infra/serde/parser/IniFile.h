//
// Created by Administrator on 2026/11/19.
//
// INI 文件层: 只负责文件的读/写/映射, 与 Ini 解析完全解耦。
// Ini 自身不直接打开文件; load / loadMapped / save 只是本层函数 + 解析的组合。
//

#ifndef HELLOVULKAN_INI_FILE_H
#define HELLOVULKAN_INI_FILE_H

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

#include "infra/serde/parser/Ini.h"

/**
 * @brief 把文件完整读入内存字符串。
 * @return 文件内容(已剥离 UTF-8 BOM); 打开/读取失败返回 IniError。
 * @notice 本层不关心内容是否为合法 INI, 也不做任何行级解析。
 */
[[nodiscard]] std::expected<std::string, IniError> readIniFile(std::string_view FilePath);

/**
 * @brief 把文本写入文件(二进制模式, 不转换换行)。
 */
[[nodiscard]] std::expected<void, IniError> writeIniFile(std::string_view FilePath,
                                                         std::string_view Content);

/**
 * @brief 内存映射的文件视图 (RAII)。
 * @notice 移动语义, 不可拷贝; view() 在对象存活期间有效。
 *         Windows 用 CreateFileMapping / MapViewOfFile; POSIX 用 open / mmap;
 *         其余平台退化为普通读取(内容由对象持有)。
 */
class IniMappedFile
{
public:
    IniMappedFile() noexcept = default;
    IniMappedFile(IniMappedFile&& Other) noexcept;
    IniMappedFile& operator=(IniMappedFile&& Other) noexcept;
    IniMappedFile(const IniMappedFile&) = delete;
    IniMappedFile& operator=(const IniMappedFile&) = delete;
    ~IniMappedFile();

    /** @brief 映射内容视图(可能为空, 例如空文件)。 */
    [[nodiscard]] std::string_view view() const noexcept;
    /** @brief 是否持有有效映射。 */
    [[nodiscard]] bool valid() const noexcept { return Valid; }

private:
    void close() noexcept;
    void steal(IniMappedFile& Other) noexcept;

#if defined(_WIN32)
    void* View = nullptr;    // 映射视图
    void* Mapping = nullptr; // 文件映射对象
    void* File = nullptr;    // 文件句柄
#elif defined(__unix__) || defined(__APPLE__)
    const char* Data = nullptr;
    int Fd = -1;
#else
    std::string Owned; // 退化为普通读取: 内容由对象持有
#endif
    std::size_t Size = 0;
    bool Valid = false;

    friend std::expected<IniMappedFile, IniError> mapIniFile(std::string_view FilePath);
};

/**
 * @brief 把文件映射进内存(不拷贝)。
 * @return 文件视图; 失败返回 IniError。空文件返回空视图(valid() == true)。
 */
[[nodiscard]] std::expected<IniMappedFile, IniError> mapIniFile(std::string_view FilePath);

#endif //HELLOVULKAN_INI_FILE_H
