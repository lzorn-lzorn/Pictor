//
// Created by Administrator on 2026/11/19.
//

#include "IniFile.h"

#include <fstream>
#include <utility>

#if defined(_WIN32)
#   ifndef WIN32_LEAN_AND_MEAN
#       define WIN32_LEAN_AND_MEAN
#   endif
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <windows.h>
#elif defined(__unix__) || defined(__APPLE__)
#   include <fcntl.h>
#   include <sys/mman.h>
#   include <sys/stat.h>
#   include <unistd.h>
#endif

std::expected<std::string, IniError> readIniFile(std::string_view FilePath)
{
    std::ifstream File(std::string(FilePath), std::ios::binary);
    if (!File.is_open())
    {
        return std::unexpected(
            IniError{ IniError::Code::CannotOpenFile, 0, std::string(FilePath) });
    }

    File.seekg(0, std::ios::end);
    const std::streamoff Size = File.tellg();
    if (Size < 0)
    {
        return std::unexpected(
            IniError{ IniError::Code::ReadFailed, 0, std::string(FilePath) });
    }
    File.seekg(0, std::ios::beg);

    std::string Content(static_cast<std::size_t>(Size), '\0');
    if (Size > 0 && !File.read(Content.data(), Size))
    {
        return std::unexpected(
            IniError{ IniError::Code::ReadFailed, 0, std::string(FilePath) });
    }

    // 剥离 UTF-8 BOM(部分编辑器会给 .ini 加上)。
    if (Content.starts_with("\xEF\xBB\xBF"))
    {
        Content.erase(0, 3);
    }

    return Content;
}

std::expected<void, IniError> writeIniFile(std::string_view FilePath, std::string_view Content)
{
    std::ofstream File(std::string(FilePath), std::ios::binary | std::ios::trunc);
    if (!File.is_open())
    {
        return std::unexpected(
            IniError{ IniError::Code::WriteFailed, 0, std::string(FilePath) });
    }

    File.write(Content.data(), static_cast<std::streamsize>(Content.size()));
    File.flush();
    if (!File)
    {
        return std::unexpected(
            IniError{ IniError::Code::WriteFailed, 0, std::string(FilePath) });
    }
    return {};
}

// ============================================================================
// IniMappedFile
// ============================================================================

IniMappedFile::IniMappedFile(IniMappedFile&& Other) noexcept
{
    steal(Other);
}

IniMappedFile& IniMappedFile::operator=(IniMappedFile&& Other) noexcept
{
    if (this != &Other)
    {
        close();
        steal(Other);
    }
    return *this;
}

IniMappedFile::~IniMappedFile()
{
    close();
}

std::string_view IniMappedFile::view() const noexcept
{
#if defined(_WIN32)
    return View == nullptr ? std::string_view{} : std::string_view{ static_cast<const char*>(View), Size };
#elif defined(__unix__) || defined(__APPLE__)
    return Data == nullptr ? std::string_view{} : std::string_view{ Data, Size };
#else
    return Owned;
#endif
}

void IniMappedFile::close() noexcept
{
#if defined(_WIN32)
    if (View != nullptr)
    {
        ::UnmapViewOfFile(View);
        View = nullptr;
    }
    if (Mapping != nullptr)
    {
        ::CloseHandle(Mapping);
        Mapping = nullptr;
    }
    if (File != nullptr)
    {
        ::CloseHandle(File);
        File = nullptr;
    }
#elif defined(__unix__) || defined(__APPLE__)
    if (Data != nullptr)
    {
        ::munmap(const_cast<char*>(Data), Size);
        Data = nullptr;
    }
    if (Fd >= 0)
    {
        ::close(Fd);
        Fd = -1;
    }
#else
    Owned.clear();
    Owned.shrink_to_fit();
#endif
    Size = 0;
    Valid = false;
}

void IniMappedFile::steal(IniMappedFile& Other) noexcept
{
#if defined(_WIN32)
    View = Other.View;
    Mapping = Other.Mapping;
    File = Other.File;
    Other.View = nullptr;
    Other.Mapping = nullptr;
    Other.File = nullptr;
#elif defined(__unix__) || defined(__APPLE__)
    Data = Other.Data;
    Fd = Other.Fd;
    Other.Data = nullptr;
    Other.Fd = -1;
#else
    Owned = std::move(Other.Owned);
#endif
    Size = Other.Size;
    Valid = Other.Valid;
    Other.Size = 0;
    Other.Valid = false;
}

std::expected<IniMappedFile, IniError> mapIniFile(std::string_view FilePath)
{
    const std::string Path(FilePath);

#if defined(_WIN32)
    HANDLE FileHandle = ::CreateFileA(Path.c_str(), GENERIC_READ,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (FileHandle == INVALID_HANDLE_VALUE)
    {
        return std::unexpected(
            IniError{ IniError::Code::CannotOpenFile, 0, Path });
    }

    LARGE_INTEGER FileSize{};
    if (!::GetFileSizeEx(FileHandle, &FileSize))
    {
        ::CloseHandle(FileHandle);
        return std::unexpected(IniError{ IniError::Code::ReadFailed, 0, Path });
    }
    if (FileSize.QuadPart == 0)
    {
        ::CloseHandle(FileHandle);
        IniMappedFile Empty;
        Empty.Valid = true; // 空文件: 空视图
        return Empty;
    }

    HANDLE MappingHandle =
        ::CreateFileMappingA(FileHandle, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (MappingHandle == nullptr)
    {
        ::CloseHandle(FileHandle);
        return std::unexpected(IniError{ IniError::Code::ReadFailed, 0, Path });
    }

    void* View = ::MapViewOfFile(MappingHandle, FILE_MAP_READ, 0, 0, 0);
    if (View == nullptr)
    {
        ::CloseHandle(MappingHandle);
        ::CloseHandle(FileHandle);
        return std::unexpected(IniError{ IniError::Code::ReadFailed, 0, Path });
    }

    IniMappedFile Result;
    Result.View = View;
    Result.Mapping = MappingHandle;
    Result.File = FileHandle;
    Result.Size = static_cast<std::size_t>(FileSize.QuadPart);
    Result.Valid = true;
    return Result;

#elif defined(__unix__) || defined(__APPLE__)
    const int Fd = ::open(Path.c_str(), O_RDONLY);
    if (Fd < 0)
    {
        return std::unexpected(IniError{ IniError::Code::CannotOpenFile, 0, Path });
    }

    struct stat Stat{};
    if (::fstat(Fd, &Stat) != 0)
    {
        ::close(Fd);
        return std::unexpected(IniError{ IniError::Code::ReadFailed, 0, Path });
    }
    if (Stat.st_size == 0)
    {
        ::close(Fd);
        IniMappedFile Empty;
        Empty.Valid = true;
        return Empty;
    }

    void* View = ::mmap(nullptr, static_cast<std::size_t>(Stat.st_size), PROT_READ,
                        MAP_PRIVATE, Fd, 0);
    if (View == MAP_FAILED)
    {
        ::close(Fd);
        return std::unexpected(IniError{ IniError::Code::ReadFailed, 0, Path });
    }

    IniMappedFile Result;
    Result.Data = static_cast<const char*>(View);
    Result.Fd = Fd;
    Result.Size = static_cast<std::size_t>(Stat.st_size);
    Result.Valid = true;
    return Result;

#else
    // 其余平台: 退化为普通读取(内容由对象持有)
    std::expected<std::string, IniError> Content = readIniFile(FilePath);
    if (!Content)
    {
        return std::unexpected(Content.error());
    }
    IniMappedFile Result;
    Result.Owned = std::move(*Content);
    Result.Size = Result.Owned.size();
    Result.Valid = true;
    return Result;
#endif
}
