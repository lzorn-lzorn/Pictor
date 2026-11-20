//
// SerdeServer.hpp — 对外提供服务的单例 + 目录(SerdeSpace)/文件(SerdeUnit)抽象
//
// 架构：外部对象 -> SerdeADT（经反射 toAdt）-> SerdeServer -> SerdeSpace(目录)
//       -> SerdeUnit(文件) -> SerdeEngine(具体后端) -> 文件系统。
// SerdeServer 维护根目录；SerdeSpace/SerdeUnit 本质是路径抽象，内部通过文件系统操作。
//

#ifndef INFRA_SERDE_SERVER_HPP
#define INFRA_SERDE_SERVER_HPP

#include <expected>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <infra/functions/Tag.hpp>
#include <infra/serde/SerdeADT.hpp>
#include <infra/serde/SerdeEngineFactory.hpp>

namespace infra
{

// ========================= 序列化的目标文件（文件的抽象） =========================
class SerdeUnit
{
public:
    SerdeUnit() = default;
    SerdeUnit(std::filesystem::path InPath, ESerdeBackend InBackend)
        : Path(std::move(InPath)), Backend(InBackend) {}

    // 让本 Unit 对应的文件在磁盘上真实存在（必要时连同父目录一起创建）；
    // 已存在的文件保持原样，不截断、不覆盖
    std::expected<void, std::string> create()
    {
        if (Path.empty())
        {
            return std::unexpected("SerdeUnit::create: empty path");
        }

        try
        {
            if (const std::filesystem::path Parent = Path.parent_path(); !Parent.empty())
            {
                std::filesystem::create_directories(Parent);
            }

            if (!std::filesystem::exists(Path))
            {
                // 二进制模式：避免 Windows 文本模式把 \n 悄悄改写成 CRLF
                std::ofstream Ofs(Path, std::ios::binary | std::ios::out | std::ios::app);
                if (!Ofs)
                {
                    return std::unexpected("SerdeUnit::create: cannot create " + Path.string());
                }
            }
        }
        catch (const std::exception &E)
        {
            return std::unexpected(E.what());
        }
        return {};
    }

    // 将 SerdeADT 经对应后端引擎写入文件
    std::expected<void, std::string> save(const SerdeADT &InAdt, ESerdeMode InMode)
    {
        SerdeEngine *Engine = getEngineFor(Backend);
        if (!Engine) 
        {
            return std::unexpected("SerdeUnit::save: no Engine for backend");
        }
        // 文件系统操作同样可能失败（权限、只读卷、非法路径…），必须转成 expected 错误返回，
        // 否则异常会穿透本函数直接把进程 terminate 掉
        try
        {
            if (const std::filesystem::path Parent = Path.parent_path(); !Parent.empty())
            {
                std::filesystem::create_directories(Parent);
            }

            // 二进制模式读写：保证 \n 在所有平台上落盘一致（否则 Windows 会写成 CRLF）
            std::ofstream Ofs(Path, std::ios::binary | std::ios::out | (InMode == ESerdeMode::Append ? std::ios::app : std::ios::trunc));
            if (!Ofs) 
            {
                return std::unexpected("SerdeUnit::save: cannot open " + Path.string());
            }

            Ofs << Engine->serialize(InAdt);
        }
        catch (const std::exception &E)
        {
            return std::unexpected(E.what());
        }
        return {};
    }

    // 从文件读取并经对应后端引擎解析为 SerdeADT
    std::expected<SerdeADT, std::string> load()
    {
        SerdeEngine *Engine = getEngineFor(Backend);
        if (!Engine) return std::unexpected("SerdeUnit::load: no Engine for backend");

        std::ifstream Ifs(Path, std::ios::binary); // 二进制模式读，原样交给引擎预处理
        if (!Ifs) return std::unexpected("SerdeUnit::load: cannot open " + Path.string());
        std::ostringstream Oss;
        Oss << Ifs.rdbuf();

        try
        {
            return Engine->parse(Oss.str());
        }
        catch (const std::exception &E)
        {
            return std::unexpected(E.what());
        }
    }

    [[nodiscard]] bool isExist() const noexcept { return std::filesystem::exists(Path); }
    [[nodiscard]] const std::filesystem::path &path() const noexcept { return Path; }
    [[nodiscard]] ESerdeBackend backend() const noexcept { return Backend; }

public:
    Tag Key; // 完整 Key, eg "Saved.Player.2026-12-8.ThisUnit"
private:
    std::filesystem::path Path;
    ESerdeBackend Backend { ESerdeBackend::None };
};

// ========================= 目录的抽象 =========================
class SerdeSpace
{
public:
    SerdeSpace() = default;

    // 构造即对应磁盘上的真实目录：立即创建（已存在则复用）；
    // bCreateDir = false 时只做路径抽象，不碰磁盘（纯读取路径用，避免凭空留下空目录）
    explicit SerdeSpace(std::filesystem::path InDirPath, bool bCreateDir = true) : Dir(std::move(InDirPath))
    {
        if (bCreateDir && !Dir.empty())
        {
            std::error_code Ec;
            std::filesystem::create_directories(Dir, Ec); // 不抛异常，失败可用 isExist() 检查
        }
    }

    // 生成该目录下的一个 Unit 文件（dir/name.ext）
    SerdeUnit unit(std::string_view Name, ESerdeBackend InBackend) const
    {
        std::filesystem::path Path = Dir / (std::string(Name) + "." + std::string(backendExtension(InBackend)));
        return SerdeUnit(std::move(Path), InBackend);
    }

    // 生成并在磁盘上立即落地一个 Unit 文件（目录与空文件都真实存在）
    SerdeUnit createUnit(std::string_view Name, ESerdeBackend InBackend) const
    {
        SerdeUnit U = unit(Name, InBackend);
        (void)U.create();
        return U;
    }

    [[nodiscard]] bool isExist() const noexcept { return std::filesystem::exists(Dir); }
    [[nodiscard]] const std::filesystem::path &dir() const noexcept { return Dir; }

public:
    Tag Key; // 完整 Key, eg "Saved.Player.2026-12-8"
private:
    std::filesystem::path Dir;
};

// ========================= 对外单例 =========================
class SerdeServer
{
    constexpr static std::string_view WorkSpace = "Serde"; // 序列化根目录名

public:
    static SerdeServer &self()
    {
        static SerdeServer Instance;
        return Instance;
    }

    void setRoot(std::filesystem::path InRoot) { Root = std::move(InRoot); }
    [[nodiscard]] const std::filesystem::path &root() const noexcept { return Root; }

    // 根目录下的某个子目录（构造 SerdeSpace 即创建真实目录）
    SerdeSpace space(std::string_view Name) const
    {
        return SerdeSpace(Root / Name);
    }

    // 便捷：按 Tag（点分路径）序列化 / 反序列化
    std::expected<void, std::string>
    serialize(const SerdeADT &InAdt, const Tag &Key, ESerdeBackend InBackend, ESerdeMode InMode)
    {
        return unitFor(Key, InBackend).save(InAdt, InMode);
    }

    std::expected<SerdeADT, std::string>
    deserialize(const Tag &Key, ESerdeBackend InBackend)
    {
        return unitFor(Key, InBackend, /*bCreateDir=*/false).load();
    }

private:
    SerdeServer() : Root(WorkSpace) {}

    // Tag -> Unit：父目录各段交给 SerdeSpace（真实目录），末段交给 SerdeUnit（真实文件）
    [[nodiscard]] SerdeUnit unitFor(const Tag &Key, ESerdeBackend InBackend, bool bCreateDir = true) const
    {
        const std::vector<std::string> Parts = split(Key.get(), '.');
        const std::string Name = Parts.empty() ? std::string{} : Parts.back();

        std::filesystem::path Dir = Root;
        for (std::size_t I = 0; I + 1 < Parts.size(); ++I)
        {
            Dir /= Parts[I];
        }

        SerdeSpace Space(Dir, bCreateDir);
        Space.Key = Key;

        SerdeUnit Unit = Space.unit(Name, InBackend);
        Unit.Key = Key;
        return Unit;
    }

    static std::vector<std::string> split(std::string_view S, char Delim)
    {
        std::vector<std::string> Parts;
        std::size_t Start = 0;
        while (Start <= S.size())
        {
            auto Pos = S.find(Delim, Start);
            Parts.emplace_back(S.substr(Start, Pos == std::string_view::npos ? std::string_view::npos : Pos - Start));
            if (Pos == std::string_view::npos) break;
            Start = Pos + 1;
        }
        return Parts;
    }

private:
    std::filesystem::path Root;
};

} // namespace infra

#endif // INFRA_SERDE_SERVER_HPP
