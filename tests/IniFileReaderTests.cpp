// Tests for infra/serde/parser/Ini, driven by tests/config.ini.
//
// TEST_DATA_DIR is provided by tests/CMakeLists.txt so the tests locate their
// data files no matter which working directory they are launched from.

#include "TestFramework.h"

#include <charconv>
#include <cstdio>
#include <filesystem>
#include <sstream>

#include "infra/serde/parser/Ini.hpp"
#include "infra/serde/parser/IniFile.hpp"

namespace
{
    /** @brief 每个用例共享的、已加载 config.ini 的读取器。 */
    const Ini& config()
    {
        static const Ini Reader = [] {
            Ini R;
            (void)R.load(TEST_DATA_DIR "/config.ini");
            return R;
        }();
        return Reader;
    }

    /** @brief 临时文件路径(位于构建目录, 用例结束时清理)。 */
    std::filesystem::path tempIniPath()
    {
        return std::filesystem::path(TEST_OUTPUT_DIR) / "pictor_ini_test_roundtrip.ini";
    }

    void setEnvVar(const char* Name, const char* Value)
    {
#ifdef _WIN32
        _putenv_s(Name, Value);
#else
        ::setenv(Name, Value, 1);
#endif
    }

    struct Color
    {
        int R = 0;
        int G = 0;
        int B = 0;
    };

} // namespace

PICTOR_TEST_CASE(LoadsConfigFile)
{
    Ini Reader;
    PICTOR_CHECK(Reader.load(TEST_DATA_DIR "/config.ini").has_value());
    PICTOR_CHECK(Reader.isLoaded());
    PICTOR_CHECK_EQ(Reader.lastError().Code, IniError::Code::None);
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
    PICTOR_CHECK_EQ(Reader.get<std::string>("General", "app_name"), "HelloVulkan");
    PICTOR_CHECK_EQ(Reader.get<std::string>("General", "version"), "1.2.0");
    PICTOR_CHECK_EQ(Reader.get<std::string>("General", "language"), "zh-CN");
    PICTOR_CHECK_EQ(Reader.get<std::string>("Window", "title"), "Hello Vulkan");
    PICTOR_CHECK_EQ(Reader.get<std::string>("Graphics", "backend"), "vulkan");
    PICTOR_CHECK_EQ(Reader.get<std::string>("Renderer", "shadow_quality"), "high");
    PICTOR_CHECK_EQ(Reader.get<std::string>("Renderer", "texture_filter"), "anisotropic");
    PICTOR_CHECK_EQ(Reader.get<std::string>("Network", "host"), "127.0.0.1");
    PICTOR_CHECK_EQ(Reader.get<std::string>("Paths", "asset_dir"), "./assets");
    PICTOR_CHECK_EQ(Reader.get<std::string>("KeyBindings", "jump"), "Space");
}

PICTOR_TEST_CASE(ReadsIntegerValues)
{
    const Ini& Reader = config();
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("General", "build"), 20261118);
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Window", "width"), 1280);
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Window", "height"), 720);
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Graphics", "msaa"), 4);
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Graphics", "max_fps"), 144);
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Graphics", "preferred_gpu"), 0);
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Renderer", "anisotropy_level"), 16);
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Network", "port"), 8080);
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Network", "timeout_seconds"), 30);
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Logging", "max_file_size_mb"), 50);
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Logging", "rotate_count"), 3);
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Advanced", "worker_threads"), 8);
}

PICTOR_TEST_CASE(ReadsFloatingPointValues)
{
    const Ini& Reader = config();
    PICTOR_CHECK_NEAR(Reader.get<double>("Renderer", "render_scale"), 1.0, 1e-9);
    PICTOR_CHECK_NEAR(Reader.get<double>("Audio", "master_volume"), 0.8, 1e-9);
    PICTOR_CHECK_NEAR(Reader.get<double>("Audio", "music_volume"), 0.6, 1e-9);
    PICTOR_CHECK_NEAR(Reader.get<double>("Audio", "sfx_volume"), 1.0, 1e-9);
    PICTOR_CHECK_NEAR(Reader.get<double>("Advanced", "time_scale"), 1.0, 1e-9);
}

PICTOR_TEST_CASE(ReadsBooleanValues)
{
    const Ini& Reader = config();
    PICTOR_CHECK_EQ(Reader.get<bool>("General", "debug"), false);
    PICTOR_CHECK_EQ(Reader.get<bool>("Window", "fullscreen"), false);
    PICTOR_CHECK_EQ(Reader.get<bool>("Window", "resizable"), true);
    PICTOR_CHECK_EQ(Reader.get<bool>("Graphics", "vsync"), true);
    PICTOR_CHECK_EQ(Reader.get<bool>("Audio", "mute"), false);
    PICTOR_CHECK_EQ(Reader.get<bool>("Logging", "console"), true);
    PICTOR_CHECK_EQ(Reader.get<bool>("Logging", "file"), true);
    PICTOR_CHECK_EQ(Reader.get<bool>("Network", "reconnect"), true);
    PICTOR_CHECK_EQ(Reader.get<bool>("Advanced", "enable_validation"), true);
}

// 值以 std::variant 存储, 可通过 getVariant 直接取得类型化视图。
PICTOR_TEST_CASE(ExposesTypedValuesViaVariant)
{
    const Ini& Reader = config();

    const std::optional<IniVariant> Width = Reader.getVariant("Window", "width");
    PICTOR_CHECK(Width.has_value());
    if (Width)
    {
        const std::int64_t* Ptr = std::get_if<std::int64_t>(&*Width);
        PICTOR_CHECK(Ptr != nullptr);
        PICTOR_CHECK_EQ(*Ptr, 1280);
    }

    const std::optional<IniVariant> Volume = Reader.getVariant("Audio", "master_volume");
    PICTOR_CHECK(Volume.has_value());
    if (Volume)
    {
        const double* Ptr = std::get_if<double>(&*Volume);
        PICTOR_CHECK(Ptr != nullptr);
        PICTOR_CHECK_NEAR(*Ptr, 0.8, 1e-9);
    }

    const std::optional<IniVariant> Debug = Reader.getVariant("General", "debug");
    PICTOR_CHECK(Debug.has_value());
    if (Debug)
    {
        const bool* Ptr = std::get_if<bool>(&*Debug);
        PICTOR_CHECK(Ptr != nullptr);
        PICTOR_CHECK_EQ(*Ptr, false);
    }

    const std::optional<IniVariant> Name = Reader.getVariant("General", "app_name");
    PICTOR_CHECK(Name.has_value());
    if (Name)
    {
        const std::string* Ptr = std::get_if<std::string>(&*Name);
        PICTOR_CHECK(Ptr != nullptr);
        PICTOR_CHECK_EQ(*Ptr, "HelloVulkan");
    }

    PICTOR_CHECK(!Reader.getVariant("General", "no_such_key").has_value());
}

PICTOR_TEST_CASE(PreservesSemicolonsInQuotedValues)
{
    // config.ini: motto = "Stay hungry; stay foolish"
    PICTOR_CHECK_EQ(config().get<std::string>("KeyBindings", "motto"), "Stay hungry; stay foolish");
}

PICTOR_TEST_CASE(StoresEmptyValuesAsEmptyString)
{
    // config.ini: custom_flag =   (等号后留空)
    PICTOR_CHECK(config().hasKey("Advanced", "custom_flag"));
    PICTOR_CHECK_EQ(config().get<std::string>("Advanced", "custom_flag"), "");
}

PICTOR_TEST_CASE(MissingKeysReturnFallback)
{
    const Ini& Reader = config();
    PICTOR_CHECK(!Reader.hasSection("NoSuchSection"));
    PICTOR_CHECK(!Reader.hasKey("General", "no_such_key"));

    PICTOR_CHECK_EQ(Reader.get<std::string>("General", "no_such_key"), "");
    PICTOR_CHECK_EQ(Reader.get<std::string>("NoSuchSection", "no_such_key", "dflt"), "dflt");
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("General", "no_such_key", 42), 42);
    PICTOR_CHECK_EQ(Reader.get<double>("General", "no_such_key", 2.5), 2.5);
    PICTOR_CHECK_EQ(Reader.get<bool>("General", "no_such_key", true), true);

    // 值存在但无法转换为目标类型时同样返回默认值。
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("General", "app_name", -1), -1);
    PICTOR_CHECK_EQ(Reader.get<double>("General", "app_name", -1.0), -1.0);
    PICTOR_CHECK_EQ(Reader.get<bool>("General", "app_name", true), true);
}

PICTOR_TEST_CASE(ParsesInlineContentAndComments)
{
    Ini Reader;
    const char* Inline =
        "# hash comment\n"
        "; semicolon comment\n"
        "   \n"
        "[Section]\n"
        "key = getValue\n"
        "quoted = \"a; b # c\"\n"
        "empty =\n";

    PICTOR_CHECK(Reader.parse(Inline).has_value());
    PICTOR_CHECK(Reader.isLoaded());
    PICTOR_CHECK_EQ(Reader.lastError().Code, IniError::Code::None);
    PICTOR_CHECK_EQ(Reader.get<std::string>("Section", "key"), "getValue");
    PICTOR_CHECK_EQ(Reader.get<std::string>("Section", "quoted"), "a; b # c");
    PICTOR_CHECK(Reader.hasKey("Section", "empty"));
    PICTOR_CHECK_EQ(Reader.get<std::string>("Section", "empty"), "");
}

PICTOR_TEST_CASE(MergesDuplicateSectionsAndOverridesDuplicateKeys)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[A]\nx = 1\n[B]\ny = 2\n[A]\nz = 3\n").has_value());
    PICTOR_CHECK_EQ(Reader.sections().size(), 2u);
    PICTOR_CHECK_EQ(Reader.get<std::string>("A", "x"), "1");
    PICTOR_CHECK_EQ(Reader.get<std::string>("A", "z"), "3");
    PICTOR_CHECK_EQ(Reader.get<std::string>("B", "y"), "2");

    Ini Duplicate;
    PICTOR_CHECK(Duplicate.parse("[S]\nk = first\nk = second\n").has_value());
    PICTOR_CHECK_EQ(Duplicate.get<std::string>("S", "k"), "second");
    PICTOR_CHECK_EQ(Duplicate.keys("S").size(), 1u);
}

PICTOR_TEST_CASE(RejectsMalformedInput)
{
    Ini Outside;
    PICTOR_CHECK(!Outside.parse("orphan = getValue\n").has_value());
    PICTOR_CHECK(!Outside.isLoaded());
    PICTOR_CHECK_EQ(Outside.lastError().Code, IniError::Code::KeyValueOutsideSection);

    Ini BadLine;
    PICTOR_CHECK(!BadLine.parse("[Good]\nkey = getValue\nbad line\n").has_value());
    PICTOR_CHECK_EQ(BadLine.lastError().Code, IniError::Code::MissingDelimiter);

    Ini Unclosed;
    PICTOR_CHECK(!Unclosed.parse("[Unclosed\n").has_value());
    PICTOR_CHECK_EQ(Unclosed.lastError().Code, IniError::Code::UnclosedSectionHeader);

    Ini Trailing;
    PICTOR_CHECK(!Trailing.parse("[Section] trailing\n").has_value());
    PICTOR_CHECK_EQ(Trailing.lastError().Code, IniError::Code::TrailingContentInSectionHeader);
}

PICTOR_TEST_CASE(FailsGracefullyOnMissingFile)
{
    Ini Reader;
    PICTOR_CHECK(!Reader.load(TEST_DATA_DIR "/does_not_exist.ini").has_value());
    PICTOR_CHECK(!Reader.isLoaded());
    PICTOR_CHECK_EQ(Reader.lastError().Code, IniError::Code::CannotOpenFile);
}

PICTOR_TEST_CASE(ParseReturnsStructuredError)
{
    Ini Reader;
    const auto Result = Reader.parse("orphan = value\n");
    PICTOR_CHECK(!Result.has_value());
    PICTOR_CHECK_EQ(Result.error().Code, IniError::Code::KeyValueOutsideSection);
    PICTOR_CHECK_EQ(Result.error().LineNumber, 1u);
    PICTOR_CHECK(!Result.error().Detail.empty());
    PICTOR_CHECK_EQ(IniError::asString(Result.error()),
                    "line 1: key-value outside any section: orphan = value");
    PICTOR_CHECK(!Reader.isLoaded());
    PICTOR_CHECK_EQ(Reader.lastError().Code, IniError::Code::KeyValueOutsideSection);
}

PICTOR_TEST_CASE(LineBufferStoresEveryLine)
{
    const char* Inline = "# comment\n[S]\nkey = 1\n\nlast = x\n";
    Ini Reader;
    PICTOR_CHECK(Reader.parse(Inline).has_value());

    const std::vector<std::string> Expected = {
        "# comment", "[S]", "key = 1", "", "last = x"};
    PICTOR_CHECK_EQ(Reader.lineBuffer(), Expected);
}

PICTOR_TEST_CASE(ReleaseLineBufferMovesLinesOut)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[S]\na = 1\n").has_value());

    std::vector<std::string> Lines = Reader.releaseLineBuffer();
    PICTOR_CHECK_EQ(Lines.size(), 2u);
    PICTOR_CHECK(Reader.lineBuffer().empty());
}

PICTOR_TEST_CASE(HandlesCrLfLineEndings)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[S]\r\nk = v\r\n").has_value());
    PICTOR_CHECK_EQ(Reader.lineBuffer().size(), 2u);
    PICTOR_CHECK_EQ(Reader.lineBuffer()[0], "[S]");
    PICTOR_CHECK_EQ(Reader.lineBuffer()[1], "k = v");
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "k"), "v");
}

PICTOR_TEST_CASE(SerializeRoundTripsSections)
{
    const char* Source =
        "[General]\n"
        "app_name = HelloVulkan\n"
        "build = 20261118\n"
        "ratio = 1.5\n"
        "debug = false\n"
        "[Window]\n"
        "title = Hello Vulkan\n"
        "motto = \"Stay hungry; stay foolish\"\n"
        "escaped = \"line1\\nline2\"\n"
        "empty =\n";

    Ini Original;
    PICTOR_CHECK(Original.parse(Source).has_value());

    const std::string Text = Original.serialize();
    Ini Reparsed;
    PICTOR_CHECK(Reparsed.parse(Text).has_value());

    PICTOR_CHECK_EQ(Reparsed.sections().size(), 2u);
    PICTOR_CHECK_EQ(Reparsed.get<std::string>("General", "app_name"), "HelloVulkan");
    PICTOR_CHECK_EQ(Reparsed.get<std::int64_t>("General", "build"), 20261118);
    PICTOR_CHECK_NEAR(Reparsed.get<double>("General", "ratio"), 1.5, 1e-9);
    PICTOR_CHECK_EQ(Reparsed.get<bool>("General", "debug"), false);
    PICTOR_CHECK_EQ(Reparsed.get<std::string>("Window", "title"), "Hello Vulkan");
    PICTOR_CHECK_EQ(Reparsed.get<std::string>("Window", "motto"), "Stay hungry; stay foolish");
    PICTOR_CHECK_EQ(Reparsed.get<std::string>("Window", "escaped"), "line1\nline2");
    PICTOR_CHECK(Reparsed.hasKey("Window", "empty"));
    PICTOR_CHECK_EQ(Reparsed.get<std::string>("Window", "empty"), "");
}

PICTOR_TEST_CASE(FlashWritesSectionsIntoLineBuffer)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[General]\na = 1\nb = 2\n").has_value());

    const std::string Text = Reader.flash();
    // [General] 头 + 2 个条目 = 3 行
    PICTOR_CHECK_EQ(Reader.lineBuffer().size(), 3u);
    PICTOR_CHECK_EQ(Reader.lineBuffer()[0], "[General]");
    PICTOR_CHECK_EQ(Reader.lineBuffer()[1], "a = 1");
    PICTOR_CHECK_EQ(Reader.lineBuffer()[2], "b = 2");
    PICTOR_CHECK_EQ(Text, "[General]\na = 1\nb = 2\n");
}

PICTOR_TEST_CASE(FileLayerIsDecoupledFromParsing)
{
    const auto Content = readIniFile(TEST_DATA_DIR "/config.ini");
    PICTOR_CHECK(Content.has_value());
    if (Content)
    {
        PICTOR_CHECK(Content->find("[General]") != std::string::npos);
        PICTOR_CHECK(Content->find("app_name = HelloVulkan") != std::string::npos);
    }

    const auto Missing = readIniFile(TEST_DATA_DIR "/does_not_exist.ini");
    PICTOR_CHECK(!Missing.has_value());
    PICTOR_CHECK_EQ(Missing.error().Code, IniError::Code::CannotOpenFile);
    // 错误信息携带完整路径, 随平台/机器而异; 只校验文件名片段以保证可移植。
    const std::string MissingMessage = IniError::asString(Missing.error());
    PICTOR_CHECK(MissingMessage.find("does_not_exist.ini") != std::string::npos);

    Ini Reader;
    PICTOR_CHECK(Reader.load(TEST_DATA_DIR "/config.ini").has_value());
}

PICTOR_TEST_CASE(FromStringAndFromFileFactories)
{
    const auto FromString = Ini::fromString("[S]\nk = 1\n");
    PICTOR_CHECK(FromString.has_value());
    if (FromString)
    {
        PICTOR_CHECK_EQ(FromString->get<std::int64_t>("S", "k"), 1);
    }

    const auto FromFile = Ini::fromFile(TEST_DATA_DIR "/config.ini");
    PICTOR_CHECK(FromFile.has_value());
    if (FromFile)
    {
        PICTOR_CHECK_EQ(FromFile->sections().size(), 10u);
    }

    const auto Bad = Ini::fromString("orphan = x\n");
    PICTOR_CHECK(!Bad.has_value());
}

PICTOR_TEST_CASE(StrictPolicyRejectsMissingDelimiter)
{
    Ini Reader; // 默认策略: 严格检查
    const auto Result = Reader.parse("[S]\nno_delimiter\n");
    PICTOR_CHECK(!Result.has_value());
    PICTOR_CHECK_EQ(Result.error().Code, IniError::Code::MissingDelimiter);
    PICTOR_CHECK_EQ(Result.error().LineNumber, 2u);
}

PICTOR_TEST_CASE(NonStrictPolicyAppliesDefaultKeyAndValue)
{
    IniPolicy Policy;
    Policy.StrictInspection = false;
    Ini Reader(Policy);

    PICTOR_CHECK(Reader.parse("[S]\nbare\n= v\n").has_value());
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "bare"), "NULL"); // 缺分隔符 -> 默认值
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "NULL"), "v");    // 空键 -> 默认键名
    PICTOR_CHECK_EQ(Reader.keys("S").size(), 2u);
}

PICTOR_TEST_CASE(InteriorSpacePolicyRejectsSpacesInKey)
{
    IniPolicy Policy;
    Policy.InteriorSpaceInKey = false;

    Ini Bad(Policy);
    const auto Result = Bad.parse("[S]\nVar Name = 1\n");
    PICTOR_CHECK(!Result.has_value());
    PICTOR_CHECK_EQ(Result.error().Code, IniError::Code::InteriorSpaceInKey);

    Ini Ok(Policy);
    PICTOR_CHECK(Ok.parse("[S]\nVar = 1\n").has_value());
    PICTOR_CHECK_EQ(Ok.get<std::string>("S", "Var"), "1");
}

PICTOR_TEST_CASE(UpperCasePolicyFoldsKeysAndLookups)
{
    IniPolicy Policy;
    Policy.KeyCaseMode = ECaseMode::UpperCase;
    Ini Reader(Policy);

    PICTOR_CHECK(Reader.parse("[S]\nKeyName = value\n").has_value());
    PICTOR_CHECK_EQ(Reader.keys("S"), std::vector<std::string>{"KEYNAME"});
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "keyname"), "value"); // 查找同样折叠
    PICTOR_CHECK(Reader.hasKey("S", "KEYNAME"));

    // set 写入的键名同样经 finalize 折叠
    PICTOR_CHECK(Reader.set("S", "another", "x").has_value());
    PICTOR_CHECK(Reader.hasKey("S", "ANOTHER"));
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "another"), "x");
}

PICTOR_TEST_CASE(CheckerRejectsDigitLeadingKey)
{
    Ini Reader;
    const auto Result = Reader.parse("[S]\n1abc = x\n");
    PICTOR_CHECK(!Result.has_value());
    PICTOR_CHECK_EQ(Result.error().Code, IniError::Code::DigitLeadingKey);
}

PICTOR_TEST_CASE(ParsesKeyValueWithoutPolicyChecks)
{
    const auto Raw = parseIniKeyValue("  Var Name  =  \"a = b\"  ; tail");
    PICTOR_CHECK(Raw.has_value());
    if (Raw)
    {
        PICTOR_CHECK(Raw->HasDelimiter);
        PICTOR_CHECK(Raw->HasInteriorSpace);
        PICTOR_CHECK_EQ(Raw->Key, "Var Name");
        PICTOR_CHECK_EQ(Raw->Value, "a = b");
    }

    const auto Bad = parseIniKeyValue("key = a = b");
    PICTOR_CHECK(!Bad.has_value());
    PICTOR_CHECK_EQ(Bad.error().Code, IniError::Code::SecondDelimiterInValue);

    // 行尾反斜杠 -> 续行标志(策略决定是否生效)
    const auto Continued = parseIniKeyValue("key = abc\\");
    PICTOR_CHECK(Continued.has_value());
    if (Continued)
    {
        PICTOR_CHECK(Continued->HasLineContinuation);
        PICTOR_CHECK_EQ(Continued->Value, "abc");
    }
}

// ---- 写入与删除 ----

PICTOR_TEST_CASE(SetWritesAndOverridesKeys)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[S]\na = 1\n").has_value());

    PICTOR_CHECK(Reader.set("S", "b", "2").has_value());
    PICTOR_CHECK(Reader.hasKey("S", "b"));
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "b"), "2");
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("S", "b"), 2);

    // 覆盖已有键
    PICTOR_CHECK(Reader.set("S", "a", "changed").has_value());
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "a"), "changed");
    PICTOR_CHECK_EQ(Reader.keys("S").size(), 2u);

    // 新 Section 追加到顺序表末尾
    PICTOR_CHECK(Reader.set("NewSection", "k", "v").has_value());
    PICTOR_CHECK(Reader.hasSection("NewSection"));
    PICTOR_CHECK_EQ(Reader.sections().back(), "NewSection");
    PICTOR_CHECK_EQ(Reader.get<std::string>("NewSection", "k"), "v");
}

PICTOR_TEST_CASE(SetValidatesNamesViaChecker)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[S]\na = 1\n").has_value());

    const auto Digit = Reader.set("S", "1bad", "x");
    PICTOR_CHECK(!Digit.has_value());
    PICTOR_CHECK_EQ(Digit.error().Code, IniError::Code::DigitLeadingKey);

    const auto Char = Reader.set("S", "k\"ey", "x");
    PICTOR_CHECK(!Char.has_value());
    PICTOR_CHECK_EQ(Char.error().Code, IniError::Code::InvalidKeyCharacter);

    const auto Empty = Reader.set("S", "", "x");
    PICTOR_CHECK(!Empty.has_value());
    PICTOR_CHECK_EQ(Empty.error().Code, IniError::Code::EmptyKey);

    const auto BadSection = Reader.set("Bad Name!", "k", "x");
    PICTOR_CHECK(!BadSection.has_value());
    PICTOR_CHECK_EQ(BadSection.error().Code, IniError::Code::InvalidSectionName);

    PICTOR_CHECK(!Reader.hasKey("S", "1bad"));
    PICTOR_CHECK(!Reader.hasSection("Bad Name!"));
    PICTOR_CHECK(Reader.isLoaded());
}

PICTOR_TEST_CASE(EraseRemovesKeys)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[S]\na = 1\nb = 2\n").has_value());

    PICTOR_CHECK(Reader.erase("S", "a"));
    PICTOR_CHECK(!Reader.hasKey("S", "a"));
    PICTOR_CHECK_EQ(Reader.keys("S").size(), 1u);

    PICTOR_CHECK(!Reader.erase("S", "no_such_key"));
    PICTOR_CHECK(!Reader.erase("NoSection", "a"));
}

PICTOR_TEST_CASE(EraseSectionRemovesWholeSection)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[A]\nx = 1\n[B]\ny = 2\n").has_value());

    PICTOR_CHECK(Reader.eraseSection("A"));
    PICTOR_CHECK(!Reader.hasSection("A"));
    PICTOR_CHECK_EQ(Reader.sections().size(), 1u);
    PICTOR_CHECK_EQ(Reader.sections().front(), "B");
    PICTOR_CHECK(Reader.hasKey("B", "y"));

    PICTOR_CHECK(!Reader.eraseSection("NoSuch"));
}

PICTOR_TEST_CASE(FlashReflectsMutationsIntoBuffer)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[S]\na = 1\nb = 2\n").has_value());

    PICTOR_CHECK(Reader.set("S", "c", "3").has_value());
    PICTOR_CHECK(Reader.erase("S", "a"));
    PICTOR_CHECK(Reader.lineBuffer().empty()); // 变更使缓冲区失效

    const std::string Text = Reader.flash();
    PICTOR_CHECK_EQ(Text, "[S]\nb = 2\nc = 3\n");
    PICTOR_CHECK_EQ(Reader.lineBuffer().size(), 3u);
    PICTOR_CHECK_EQ(Reader.lineBuffer()[0], "[S]");

    Ini Reparsed;
    PICTOR_CHECK(Reparsed.parse(Text).has_value());
    PICTOR_CHECK(!Reparsed.hasKey("S", "a"));
    PICTOR_CHECK_EQ(Reparsed.get<std::string>("S", "b"), "2");
    PICTOR_CHECK_EQ(Reparsed.get<std::string>("S", "c"), "3");
}

// ---- 列表值 ----

PICTOR_TEST_CASE(ListValues)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[S]\nnums = [1, 2, 3]\nwords = [a, \"b, c\", d]\n").has_value());

    PICTOR_CHECK_EQ(Reader.get<std::vector<std::int64_t>>("S", "nums"),
                    (std::vector<std::int64_t>{1, 2, 3}));
    PICTOR_CHECK_EQ(Reader.get<IniStringList>("S", "words"),
                    (IniStringList{"a", "b, c", "d"}));
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "nums"), "[1, 2, 3]");

    // set<vector> -> 列表, 序列化往返
    PICTOR_CHECK(Reader.set<std::vector<std::string>>("S", "more", {"x", "y"}).has_value());
    PICTOR_CHECK_EQ(Reader.get<IniStringList>("S", "more"), (IniStringList{"x", "y"}));

    Ini Reparsed;
    PICTOR_CHECK(Reparsed.parse(Reader.serialize()).has_value());
    PICTOR_CHECK_EQ(Reparsed.get<std::vector<std::int64_t>>("S", "nums"),
                    (std::vector<std::int64_t>{1, 2, 3}));
    PICTOR_CHECK_EQ(Reparsed.get<IniStringList>("S", "words"), (IniStringList{"a", "b, c", "d"}));
}

// ---- 重复键收集为列表 ----

PICTOR_TEST_CASE(DuplicateKeysCollectedAsList)
{
    IniPolicy Policy;
    Policy.DuplicateKeysAsList = true;
    Ini Reader(Policy);

    PICTOR_CHECK(Reader.parse("[S]\nk = a\nk = b\nk = c\n").has_value());
    PICTOR_CHECK_EQ(Reader.keys("S").size(), 1u);
    PICTOR_CHECK_EQ(Reader.get<IniStringList>("S", "k"), (IniStringList{"a", "b", "c"}));

    // 关闭时仍是后者覆盖
    Ini Last(Policy);
    IniPolicy Override;
    Override.DuplicateKeysAsList = false;
    Ini Reader2(Override);
    PICTOR_CHECK(Reader2.parse("[S]\nk = a\nk = b\n").has_value());
    PICTOR_CHECK_EQ(Reader2.get<std::string>("S", "k"), "b");
}

// ---- 环境变量与 ${var} 插值 ----

PICTOR_TEST_CASE(EnvironmentVariableInterpolation)
{
    setEnvVar("PICTOR_INI_TEST_VAR", "expanded-value");

    Ini Reader;
    PICTOR_CHECK(Reader.parse("[S]\nname = ${PICTOR_INI_TEST_VAR}\n").has_value());
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "name"), "expanded-value");

    // 未定义且非严格 -> 保留原文
    PICTOR_CHECK(Reader.parse("[S]\nx = ${PICTOR_INI_UNDEFINED_VAR}\n").has_value());
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "x"), "${PICTOR_INI_UNDEFINED_VAR}");

    // 未定义且严格 -> 报错
    IniPolicy Policy;
    Policy.StrictInterpolation = true;
    Ini Strict(Policy);
    const auto Result = Strict.parse("[S]\nx = ${PICTOR_INI_UNDEFINED_VAR}\n");
    PICTOR_CHECK(!Result.has_value());
    PICTOR_CHECK_EQ(Result.error().Code, IniError::Code::UnresolvedInterpolation);

    setEnvVar("PICTOR_INI_TEST_VAR", "");
}

// ---- Section 继承 ----

PICTOR_TEST_CASE(SectionInheritance)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse(
        "[Base]\nhost = localhost\nport = 80\n"
        "[Child : Base]\nport = 8080\n").has_value());

    // 子 Section 覆盖自身键, 回退到父 Section
    PICTOR_CHECK_EQ(Reader.get<std::string>("Child", "host"), "localhost");
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Child", "port"), 8080);
    PICTOR_CHECK(Reader.hasKey("Child", "host"));
    PICTOR_CHECK_EQ(Reader.keys("Child"), std::vector<std::string>{"port"});
    PICTOR_CHECK(Reader.section("Child") != nullptr);
    PICTOR_CHECK_EQ(Reader.section("Child")->Parent.value(), "Base");

    // 序列化保留继承头并可重新解析
    const std::string Text = Reader.serialize();
    PICTOR_CHECK(Text.find("[Child : Base]") != std::string::npos);
    Ini Reparsed;
    PICTOR_CHECK(Reparsed.parse(Text).has_value());
    PICTOR_CHECK_EQ(Reparsed.get<std::string>("Child", "host"), "localhost");
    PICTOR_CHECK_EQ(Reparsed.get<std::int64_t>("Child", "port"), 8080);
}

// ---- 引号包裹 Section 名 ----

PICTOR_TEST_CASE(QuotedSectionNames)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[\"Name With Spaces\"]\nx = 1\n").has_value());
    PICTOR_CHECK(Reader.hasSection("Name With Spaces"));
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Name With Spaces", "x"), 1);

    // 序列化写回引号形式, 可往返
    const std::string Text = Reader.serialize();
    PICTOR_CHECK(Text.find("[\"Name With Spaces\"]") != std::string::npos);
    Ini Reparsed;
    PICTOR_CHECK(Reparsed.parse(Text).has_value());
    PICTOR_CHECK_EQ(Reparsed.get<std::int64_t>("Name With Spaces", "x"), 1);

    // 引号名 + 继承
    PICTOR_CHECK(Reader.parse("[Base]\nh = 1\n[\"Child Name\" : Base]\nx = 2\n").has_value());
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Child Name", "h"), 1);
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("Child Name", "x"), 2);
}

// ---- 多行续行值 ----

PICTOR_TEST_CASE(LineContinuationValues)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[S]\nmsg = first part \\\nsecond part\n").has_value());
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "msg"), "first part second part");

    // 多段续行
    PICTOR_CHECK(Reader.parse("[S]\npath = a \\\nb \\\nc\n").has_value());
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "path"), "a b c");

    // 续行不吞掉后续 Section 头
    PICTOR_CHECK(Reader.parse("[S]\nmsg = text \\\n[T]\nx = 1\n").has_value());
    PICTOR_CHECK(Reader.hasSection("T"));
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "msg"), "text");
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("T", "x"), 1);
}

// ---- 值内转义 ----

PICTOR_TEST_CASE(EscapeSequencesInValues)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse(
        "[S]\n"
        "hex = \\x41\\x42\n"
        "nul = a\\0b\n"
        "semi = a\\;b ; tail\n"
        "hash = a\\#b\n"
        "sep = a\\=b\n"
        "col = a\\:b\n").has_value());

    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "hex"), "AB");
    const std::string Nul = Reader.get<std::string>("S", "nul");
    PICTOR_CHECK_EQ(Nul.size(), 3u);
    PICTOR_CHECK_EQ(Nul[1], '\0');
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "semi"), "a;b");
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "hash"), "a#b");
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "sep"), "a=b");
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "col"), "a:b");

    // 非法 \x -> 结构化错误
    const auto Bad = Reader.parse("[S]\nbad = \\xZZ\n");
    PICTOR_CHECK(!Bad.has_value());
    PICTOR_CHECK_EQ(Bad.error().Code, IniError::Code::InvalidEscapeSequence);
}

// ---- 注释与格式的往返保真 ----

PICTOR_TEST_CASE(PreservesCommentsAndFormatOnRoundTrip)
{
    const char* Source =
        "; file header\n"
        "\n"
        "[S]\n"
        "; comment for a\n"
        "a = 1\n"
        "\n"
        "b = 2\n"
        "; tail comment\n";

    Ini Reader;
    PICTOR_CHECK(Reader.parse(Source).has_value());

    PICTOR_CHECK(Reader.set("S", "c", "3").has_value());
    PICTOR_CHECK(Reader.erase("S", "b")); // 连同其前导空行一起移除

    const std::string Text = Reader.flash();
    PICTOR_CHECK(Text.find("; file header") != std::string::npos);
    PICTOR_CHECK(Text.find("; comment for a") != std::string::npos);
    PICTOR_CHECK(Text.find("a = 1") != std::string::npos);
    PICTOR_CHECK(Text.find("c = 3") != std::string::npos);
    PICTOR_CHECK(Text.find("b = 2") == std::string::npos);
    PICTOR_CHECK(Text.find("; tail comment") != std::string::npos);

    Ini Reparsed;
    PICTOR_CHECK(Reparsed.parse(Text).has_value());
    PICTOR_CHECK_EQ(Reparsed.get<std::string>("S", "a"), "1");
    PICTOR_CHECK_EQ(Reparsed.get<std::string>("S", "c"), "3");
}

// ---- UTF-8 校验 ----

PICTOR_TEST_CASE(Utf8Validation)
{
    Ini Reader; // 默认开启校验

    // 合法 UTF-8(中文)
    PICTOR_CHECK(Reader.parse("[S]\nname = \xE4\xB8\xAD\xE6\x96\x87\n").has_value());
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "name"), "\xE4\xB8\xAD\xE6\x96\x87");

    // 非法序列(0xC3 后跟非续字节)
    const auto Bad = Reader.parse("[S]\nbad = \xC3\x28\n");
    PICTOR_CHECK(!Bad.has_value());
    PICTOR_CHECK_EQ(Bad.error().Code, IniError::Code::InvalidUtf8);

    // 关闭校验后按字节放行
    IniPolicy Policy;
    Policy.ValidateUtf8 = false;
    Ini Lenient(Policy);
    PICTOR_CHECK(Lenient.parse("[S]\nbad = \xC3\x28\n").has_value());
}

// ---- 流式解析 ----

PICTOR_TEST_CASE(StreamingParse)
{
    std::istringstream Stream("# comment\n[S]\na = 1\nb = two\n");
    Ini Reader;
    PICTOR_CHECK(Reader.parse(Stream).has_value());
    PICTOR_CHECK_EQ(Reader.get<std::int64_t>("S", "a"), 1);
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "b"), "two");
    PICTOR_CHECK_EQ(Reader.lineBuffer().size(), 4u);

    // 流式 + 续行
    std::istringstream Continuation("[S]\nmsg = one \\\ntwo\n");
    Ini Reader2;
    PICTOR_CHECK(Reader2.parse(Continuation).has_value());
    PICTOR_CHECK_EQ(Reader2.get<std::string>("S", "msg"), "one two");
}

// ---- mmap 读取 ----

PICTOR_TEST_CASE(MmapLoadsConfigFile)
{
    Ini Reader;
    PICTOR_CHECK(Reader.loadMapped(TEST_DATA_DIR "/config.ini").has_value());
    PICTOR_CHECK(Reader.isLoaded());
    PICTOR_CHECK_EQ(Reader.sections().size(), 10u);
    PICTOR_CHECK_EQ(Reader.get<std::string>("General", "app_name"), "HelloVulkan");

    const auto Missing = mapIniFile(TEST_DATA_DIR "/does_not_exist.ini");
    PICTOR_CHECK(!Missing.has_value());
    PICTOR_CHECK_EQ(Missing.error().Code, IniError::Code::CannotOpenFile);
}

// ---- save 写回 ----

PICTOR_TEST_CASE(SaveWritesBackToDisk)
{
    const std::filesystem::path Temp = tempIniPath();
    std::filesystem::remove(Temp);

    Ini Writer;
    PICTOR_CHECK(Writer.parse("[S]\na = 1\n").has_value());
    PICTOR_CHECK(Writer.set("S", "b", "2").has_value());
    PICTOR_CHECK(Writer.save(Temp.string()).has_value());

    Ini Reloaded;
    PICTOR_CHECK(Reloaded.load(Temp.string()).has_value());
    PICTOR_CHECK_EQ(Reloaded.get<std::string>("S", "a"), "1");
    PICTOR_CHECK_EQ(Reloaded.get<std::string>("S", "b"), "2");

    // save() 无参数: 写回 load 的来源文件
    PICTOR_CHECK(Reloaded.set("S", "c", "3").has_value());
    PICTOR_CHECK(Reloaded.save().has_value());
    Ini Again;
    PICTOR_CHECK(Again.load(Temp.string()).has_value());
    PICTOR_CHECK_EQ(Again.get<std::string>("S", "c"), "3");

    // 无来源路径时 save() 报错
    Ini Fresh;
    PICTOR_CHECK(Fresh.parse("[S]\nx = 1\n").has_value());
    const auto NoSource = Fresh.save();
    PICTOR_CHECK(!NoSource.has_value());
    PICTOR_CHECK_EQ(NoSource.error().Code, IniError::Code::CannotOpenFile);

    std::filesystem::remove(Temp);
}

// ---- get<T> 与自定义类型注册 ----

PICTOR_TEST_CASE(CustomTypeRegistry)
{
    Ini Reader;
    PICTOR_CHECK(Reader.parse("[S]\n").has_value());

    Reader.registerType<Color>(
        [](const Color& C) -> std::optional<std::string>
        {
            char Buffer[16];
            std::snprintf(Buffer, sizeof(Buffer), "#%02X%02X%02X", C.R, C.G, C.B);
            return std::string(Buffer);
        },
        [](std::string_view S) -> std::optional<Color>
        {
            if (S.size() != 7 || S.front() != '#')
            {
                return std::nullopt;
            }
            std::uint32_t V = 0;
            const std::from_chars_result Res =
                std::from_chars(S.data() + 1, S.data() + 7, V, 16);
            if (Res.ec != std::errc{} || Res.ptr != S.data() + 7)
            {
                return std::nullopt;
            }
            return Color{ static_cast<int>((V >> 16) & 0xFF),
                          static_cast<int>((V >> 8) & 0xFF),
                          static_cast<int>(V & 0xFF) };
        });

    PICTOR_CHECK(Reader.set<Color>("S", "color", Color{ 0x12, 0x34, 0xAB }).has_value());
    PICTOR_CHECK_EQ(Reader.get<std::string>("S", "color"), "#1234AB");

    const Color Restored = Reader.get<Color>("S", "color");
    PICTOR_CHECK_EQ(Restored.R, 0x12);
    PICTOR_CHECK_EQ(Restored.G, 0x34);
    PICTOR_CHECK_EQ(Restored.B, 0xAB);

    // 序列化往返后仍可读回
    Ini Reparsed;
    PICTOR_CHECK(Reparsed.parse(Reader.serialize()).has_value());
    PICTOR_CHECK_EQ(Reparsed.get<std::string>("S", "color"), "#1234AB");

    // 未注册类型: set 报错, get 返回默认
    struct Unregistered
    {
    };
    const auto Failed = Reader.set<Unregistered>("S", "x", Unregistered{});
    PICTOR_CHECK(!Failed.has_value());
    PICTOR_CHECK_EQ(Failed.error().Code, IniError::Code::UnsupportedValueType);
}

int main()
{
    return ::PictorTest::runAll();
}
