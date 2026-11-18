// Tests for infra/serde/reader/Ini, driven by tests/config.ini.
//
// TEST_DATA_DIR is provided by tests/CMakeLists.txt so the tests locate their
// data files no matter which working directory they are launched from.

#include "TestFramework.h"

#include "infra/serde/reader/Ini.h"

namespace
{
    /** @brief 每个用例共享的、已加载 config.ini 的读取器。 */
    const Ini& config()
    {
        static const Ini Reader = [] {
            Ini R;
            R.load(TEST_DATA_DIR "/config.ini");
            return R;
        }();
        return Reader;
    }
} // namespace

PICTOR_TEST_CASE(LoadsConfigFile)
{
    Ini Reader;
    PICTOR_CHECK(Reader.load(TEST_DATA_DIR "/config.ini"));
    PICTOR_CHECK(Reader.isLoaded());
    PICTOR_CHECK(Reader.error().empty());
    PICTOR_CHECK_EQ(config().sections().size(), 10u);
}

PICTOR_TEST_CASE(ReportsSectionsAndKeysInFileOrder)
{
    const std::vector<std::string> Names = config().sections();
    const std::vector<std::string> Expected = {
        "General", "Window", "Graphics", "Renderer", "Audio",
        "Paths", "Logging", "Network", "KeyBindings", "Advanced"};
    PICTOR_CHECK_EQ(Names, Expected);

    const std::vector<std::string> WindowKeys = config().keys("Window");
    const std::vector<std::string> ExpectedKeys = {
        "title", "width", "height", "fullscreen", "resizable"};
    PICTOR_CHECK_EQ(WindowKeys, ExpectedKeys);
}

PICTOR_TEST_CASE(ReadsStringValues)
{
    const Ini& Reader = config();
    PICTOR_CHECK_EQ(Reader.get("General", "app_name"), "HelloVulkan");
    PICTOR_CHECK_EQ(Reader.get("General", "version"), "1.2.0");
    PICTOR_CHECK_EQ(Reader.get("General", "language"), "zh-CN");
    PICTOR_CHECK_EQ(Reader.get("Window", "title"), "Hello Vulkan");
    PICTOR_CHECK_EQ(Reader.get("Graphics", "backend"), "vulkan");
    PICTOR_CHECK_EQ(Reader.get("Renderer", "shadow_quality"), "high");
    PICTOR_CHECK_EQ(Reader.get("Renderer", "texture_filter"), "anisotropic");
    PICTOR_CHECK_EQ(Reader.get("Network", "host"), "127.0.0.1");
    PICTOR_CHECK_EQ(Reader.get("Paths", "asset_dir"), "./assets");
    PICTOR_CHECK_EQ(Reader.get("KeyBindings", "jump"), "Space");
}

PICTOR_TEST_CASE(ReadsIntegerValues)
{
    const Ini& Reader = config();
    PICTOR_CHECK_EQ(Reader.getInt("General", "build"), 20261118);
    PICTOR_CHECK_EQ(Reader.getInt("Window", "width"), 1280);
    PICTOR_CHECK_EQ(Reader.getInt("Window", "height"), 720);
    PICTOR_CHECK_EQ(Reader.getInt("Graphics", "msaa"), 4);
    PICTOR_CHECK_EQ(Reader.getInt("Graphics", "max_fps"), 144);
    PICTOR_CHECK_EQ(Reader.getInt("Graphics", "preferred_gpu"), 0);
    PICTOR_CHECK_EQ(Reader.getInt("Renderer", "anisotropy_level"), 16);
    PICTOR_CHECK_EQ(Reader.getInt("Network", "port"), 8080);
    PICTOR_CHECK_EQ(Reader.getInt("Network", "timeout_seconds"), 30);
    PICTOR_CHECK_EQ(Reader.getInt("Logging", "max_file_size_mb"), 50);
    PICTOR_CHECK_EQ(Reader.getInt("Logging", "rotate_count"), 3);
    PICTOR_CHECK_EQ(Reader.getInt("Advanced", "worker_threads"), 8);
}

PICTOR_TEST_CASE(ReadsFloatingPointValues)
{
    const Ini& Reader = config();
    PICTOR_CHECK_NEAR(Reader.getDouble("Renderer", "render_scale"), 1.0, 1e-9);
    PICTOR_CHECK_NEAR(Reader.getDouble("Audio", "master_volume"), 0.8, 1e-9);
    PICTOR_CHECK_NEAR(Reader.getDouble("Audio", "music_volume"), 0.6, 1e-9);
    PICTOR_CHECK_NEAR(Reader.getDouble("Audio", "sfx_volume"), 1.0, 1e-9);
    PICTOR_CHECK_NEAR(Reader.getDouble("Advanced", "time_scale"), 1.0, 1e-9);
}

PICTOR_TEST_CASE(ReadsBooleanValues)
{
    const Ini& Reader = config();
    PICTOR_CHECK_EQ(Reader.getBool("General", "debug"), false);
    PICTOR_CHECK_EQ(Reader.getBool("Window", "fullscreen"), false);
    PICTOR_CHECK_EQ(Reader.getBool("Window", "resizable"), true);
    PICTOR_CHECK_EQ(Reader.getBool("Graphics", "vsync"), true);
    PICTOR_CHECK_EQ(Reader.getBool("Audio", "mute"), false);
    PICTOR_CHECK_EQ(Reader.getBool("Logging", "console"), true);
    PICTOR_CHECK_EQ(Reader.getBool("Logging", "file"), true);
    PICTOR_CHECK_EQ(Reader.getBool("Network", "reconnect"), true);
    PICTOR_CHECK_EQ(Reader.getBool("Advanced", "enable_validation"), true);
}

PICTOR_TEST_CASE(PreservesSemicolonsInQuotedValues)
{
    // config.ini: motto = "Stay hungry; stay foolish"
    // 引号内的分号必须作为值的一部分保留，引号本身剥去。
    PICTOR_CHECK_EQ(config().get("KeyBindings", "motto"), "Stay hungry; stay foolish");
}

PICTOR_TEST_CASE(StoresEmptyValuesAsEmptyString)
{
    // config.ini: custom_flag =   (等号后留空)
    PICTOR_CHECK(config().hasKey("Advanced", "custom_flag"));
    PICTOR_CHECK_EQ(config().get("Advanced", "custom_flag"), "");
}

PICTOR_TEST_CASE(MissingKeysReturnFallback)
{
    const Ini& Reader = config();
    PICTOR_CHECK(!Reader.hasSection("NoSuchSection"));
    PICTOR_CHECK(!Reader.hasKey("General", "no_such_key"));

    PICTOR_CHECK_EQ(Reader.get("General", "no_such_key"), "");
    PICTOR_CHECK_EQ(Reader.get("NoSuchSection", "no_such_key", "dflt"), "dflt");
    PICTOR_CHECK_EQ(Reader.getInt("General", "no_such_key", 42), 42);
    PICTOR_CHECK_EQ(Reader.getDouble("General", "no_such_key", 2.5), 2.5);
    PICTOR_CHECK_EQ(Reader.getBool("General", "no_such_key", true), true);

    // 值存在但无法转换为目标类型时同样返回默认值。
    PICTOR_CHECK_EQ(Reader.getInt("General", "app_name", -1), -1);
    PICTOR_CHECK_EQ(Reader.getDouble("General", "app_name", -1.0), -1.0);
    PICTOR_CHECK_EQ(Reader.getBool("General", "app_name", true), true);
}

PICTOR_TEST_CASE(ParsesInlineContentAndComments)
{
    Ini Reader;
    const char* Inline =
        "# hash comment\n"
        "; semicolon comment\n"
        "   \n"
        "[Section]\n"
        "key = value\n"
        "quoted = \"a; b # c\"\n"
        "empty =\n";

    PICTOR_CHECK(Reader.parse(Inline));
    PICTOR_CHECK(Reader.isLoaded());
    PICTOR_CHECK(Reader.error().empty());
    PICTOR_CHECK_EQ(Reader.get("Section", "key"), "value");
    PICTOR_CHECK_EQ(Reader.get("Section", "quoted"), "a; b # c");
    PICTOR_CHECK(Reader.hasKey("Section", "empty"));
    PICTOR_CHECK_EQ(Reader.get("Section", "empty"), "");
}

PICTOR_TEST_CASE(MergesDuplicateSectionsAndOverridesDuplicateKeys)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[A]\nx = 1\n[B]\ny = 2\n[A]\nz = 3\n"));
    PICTOR_CHECK_EQ(Reader.sections().size(), 2u);
    PICTOR_CHECK_EQ(Reader.get("A", "x"), "1");
    PICTOR_CHECK_EQ(Reader.get("A", "z"), "3");
    PICTOR_CHECK_EQ(Reader.get("B", "y"), "2");

    Ini Duplicate;
    PICTOR_CHECK(Duplicate.parse("[S]\nk = first\nk = second\n"));
    PICTOR_CHECK_EQ(Duplicate.get("S", "k"), "second");
    PICTOR_CHECK_EQ(Duplicate.keys("S").size(), 1u);
}

PICTOR_TEST_CASE(RejectsMalformedInput)
{
    Ini Outside;
    PICTOR_CHECK(!Outside.parse("orphan = value\n"));
    PICTOR_CHECK(!Outside.isLoaded());
    PICTOR_CHECK(!Outside.error().empty());

    Ini BadLine;
    PICTOR_CHECK(!BadLine.parse("[Good]\nkey = value\nbad line\n"));
    PICTOR_CHECK(!BadLine.error().empty());

    Ini Unclosed;
    PICTOR_CHECK(!Unclosed.parse("[Unclosed\n"));
    PICTOR_CHECK(!Unclosed.error().empty());

    Ini Trailing;
    PICTOR_CHECK(!Trailing.parse("[Section] trailing\n"));
    PICTOR_CHECK(!Trailing.error().empty());
}

PICTOR_TEST_CASE(FailsGracefullyOnMissingFile)
{
    Ini Reader;
    PICTOR_CHECK(!Reader.load(TEST_DATA_DIR "/does_not_exist.ini"));
    PICTOR_CHECK(!Reader.isLoaded());
    PICTOR_CHECK(!Reader.error().empty());
}

// 通过 containers 的 Any 容器读取类型擦除值，验证 serde -> containers 依赖。
PICTOR_TEST_CASE(ExposesTypedValuesViaAny)
{
    const Ini& Reader = config();

    const Any Width = Reader.value("Window", "width");
    const std::int64_t* WidthPtr = Width.castPtr<std::int64_t>();
    PICTOR_CHECK(WidthPtr != nullptr);
    PICTOR_CHECK_EQ(*WidthPtr, 1280);

    const Any Volume = Reader.value("Audio", "master_volume");
    const double* VolumePtr = Volume.castPtr<double>();
    PICTOR_CHECK(VolumePtr != nullptr);
    PICTOR_CHECK_NEAR(*VolumePtr, 0.8, 1e-9);

    const Any Debug = Reader.value("General", "debug");
    const bool* DebugPtr = Debug.castPtr<bool>();
    PICTOR_CHECK(DebugPtr != nullptr);
    PICTOR_CHECK_EQ(*DebugPtr, false);

    const Any Name = Reader.value("General", "app_name");
    const std::string* NamePtr = Name.castPtr<std::string>();
    PICTOR_CHECK(NamePtr != nullptr);
    PICTOR_CHECK_EQ(*NamePtr, "HelloVulkan");

    const Any Missing = Reader.value("General", "no_such_key");
    PICTOR_CHECK(!Missing.HasValue());
}

int main()
{
    return ::PictorTest::runAll();
}
