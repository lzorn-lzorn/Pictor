// Tests for the serde core + INI backend:
//   - Ini 注册为 SerdeEngine 后端, 对象可经反射序列化为 Ini 文本
//   - SerdeADT <-> Ini 文本往返, 点分键展开
//   - SerdeServer / SerdeSpace / SerdeUnit / Tag 端到端走 Ini 后端
// (同时 Ini 仍是独立的配置文件解析器, 见 IniFileReaderTests.cpp)

#include "TestFramework.h"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

#include <infra/serde/Serde.hpp>
#include <infra/serde/parser/Ini.hpp>

using namespace infra;

namespace
{
    struct WindowConfig
    {
        REFLECT_CLASS(WindowConfig);

        REFLECT_FIELDS((int, Width),
                       (int, Height),
                       (bool, Fullscreen),
                       (std::string, Title));
    };

    struct GraphicsConfig
    {
        REFLECT_CLASS(GraphicsConfig);

        REFLECT_FIELDS((int, Msaa),
                       (std::vector<int>, Extensions));
    };

    struct AppConfig
    {
        REFLECT_CLASS(AppConfig);

        REFLECT_FIELDS((WindowConfig, Window),
                       (GraphicsConfig, Graphics));
    };
} // namespace

PICTOR_TEST_CASE(IniBackendIsRegistered)
{
    PICTOR_CHECK(getEngineFor(ESerdeBackend::Ini) != nullptr);
    PICTOR_CHECK_EQ(backendExtension(ESerdeBackend::Ini), "ini");
}

PICTOR_TEST_CASE(EngineRoundTripsReflectedObject)
{
    AppConfig Config;
    Config.Window.Width = 1280;
    Config.Window.Height = 720;
    Config.Window.Fullscreen = true;
    Config.Window.Title = "Pictor";
    Config.Graphics.Msaa = 8;
    Config.Graphics.Extensions = { 4, 8, 16 };

    const SerdeEngine* Engine = getEngineFor(ESerdeBackend::Ini);
    const std::string Text = Engine->serialize(infra::toAdt(Config));

    // 每个 object 成员成为一个 Section
    PICTOR_CHECK(Text.find("[Window]") != std::string::npos);
    PICTOR_CHECK(Text.find("[Graphics]") != std::string::npos);
    PICTOR_CHECK(Text.find("Width = 1280") != std::string::npos);
    PICTOR_CHECK(Text.find("Fullscreen = true") != std::string::npos);
    PICTOR_CHECK(Text.find("Title = Pictor") != std::string::npos);
    PICTOR_CHECK(Text.find("Extensions = [4, 8, 16]") != std::string::npos);

    const SerdeADT Parsed = Engine->parse(Text);
    const std::expected<AppConfig, std::string> Restored = infra::fromAdt<AppConfig>(Parsed);
    PICTOR_CHECK(Restored.has_value());
    if (Restored)
    {
        PICTOR_CHECK_EQ(Restored->Window.Width, 1280);
        PICTOR_CHECK_EQ(Restored->Window.Height, 720);
        PICTOR_CHECK_EQ(Restored->Window.Fullscreen, true);
        PICTOR_CHECK_EQ(Restored->Window.Title, "Pictor");
        PICTOR_CHECK_EQ(Restored->Graphics.Msaa, 8);
        PICTOR_CHECK_EQ(Restored->Graphics.Extensions, (std::vector<int>{ 4, 8, 16 }));
    }
}

PICTOR_TEST_CASE(EngineFlattensNestedObjectsToDottedKeys)
{
    SerdeObject Sub;
    Sub.emplace_back("X", SerdeADT{ 1 });
    SerdeObject Sec;
    Sec.emplace_back("Sub", SerdeADT{ std::move(Sub) });
    SerdeObject Root;
    Root.emplace_back("S", SerdeADT{ std::move(Sec) });
    const SerdeADT Adt{ std::move(Root) };

    const std::string Text = getEngineFor(ESerdeBackend::Ini)->serialize(Adt);
    PICTOR_CHECK(Text.find("Sub.X = 1") != std::string::npos);

    // 读回时点分键展开为嵌套对象
    const SerdeADT Parsed = getEngineFor(ESerdeBackend::Ini)->parse(Text);
    const SerdeADT* SecNode = Parsed.find("S");
    PICTOR_CHECK(SecNode != nullptr && SecNode->isObject());
    if (SecNode != nullptr)
    {
        const SerdeADT* SubNode = SecNode->find("Sub");
        PICTOR_CHECK(SubNode != nullptr && SubNode->isObject());
        if (SubNode != nullptr)
        {
            const SerdeADT* X = SubNode->find("X");
            PICTOR_CHECK(X != nullptr && X->isInt());
            if (X != nullptr)
            {
                PICTOR_CHECK_EQ(X->asInt(), 1);
            }
        }
    }
}

PICTOR_TEST_CASE(EngineRejectsNonObjectRoot)
{
    const SerdeADT Scalar{ 42 };
    bool Threw = false;
    try
    {
        (void)getEngineFor(ESerdeBackend::Ini)->serialize(Scalar);
    }
    catch (const std::runtime_error&)
    {
        Threw = true;
    }
    PICTOR_CHECK(Threw);
}

PICTOR_TEST_CASE(EngineReportsParseErrorsAsExceptions)
{
    bool Threw = false;
    try
    {
        (void)getEngineFor(ESerdeBackend::Ini)->parse("orphan = value\n");
    }
    catch (const std::runtime_error&)
    {
        Threw = true;
    }
    PICTOR_CHECK(Threw);
}

// SerdeServer / SerdeSpace / SerdeUnit / Tag 端到端, 经 Ini 后端落盘
PICTOR_TEST_CASE(SerdeServerEndToEndWithIniBackend)
{
    const std::filesystem::path TempRoot =
        std::filesystem::path(TEST_OUTPUT_DIR) / "serde_test_root";
    std::filesystem::remove_all(TempRoot);

    infra::SerdeServer::self().setRoot(TempRoot);

    AppConfig Config;
    Config.Window.Width = 1024;
    Config.Graphics.Extensions = { 2, 4 };

    // 真实 Tag: 由路径段构造, get() 拼接为 "Saved.TestConfig"
    const infra::Tag Key(std::vector<std::string>{ "Saved", "TestConfig" });

    const std::expected<void, std::string> Saved = infra::SerdeServer::self().serialize(
        infra::toAdt(Config), Key, ESerdeBackend::Ini,
        ESerdeMode::Overwrite);
    PICTOR_CHECK(Saved.has_value());

    const std::expected<SerdeADT, std::string> Loaded =
        infra::SerdeServer::self().deserialize(Key, ESerdeBackend::Ini);
    PICTOR_CHECK(Loaded.has_value());
    if (Loaded)
    {
        const std::expected<AppConfig, std::string> Restored = infra::fromAdt<AppConfig>(*Loaded);
        PICTOR_CHECK(Restored.has_value());
        if (Restored)
        {
            PICTOR_CHECK_EQ(Restored->Window.Width, 1024);
            PICTOR_CHECK_EQ(Restored->Graphics.Extensions, (std::vector<int>{ 2, 4 }));
        }
    }

    PICTOR_CHECK(std::filesystem::exists(TempRoot / "Saved" / "TestConfig.ini"));
    std::filesystem::remove_all(TempRoot);
}
