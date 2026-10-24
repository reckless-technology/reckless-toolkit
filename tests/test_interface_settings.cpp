// The suite's window mode (core/InterfaceSettings.h): interface.json, a JSON object whose "window_mode" says
// scroll or scale - scale for a file or a member that is missing or unreadable - the file's other members kept when
// the mode is written, in the per-user settings folder.
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "InterfaceSettings.h"
#include "JsonFile.h"
#include "ScratchPath.h"

namespace
{
using reckless::core::InterfaceSettings;
using Mode     = InterfaceSettings::WindowMode;
namespace json = reckless::core::json;

class InterfaceSettingsFile : public ::testing::Test
{
  protected:
    void SetUp() override
    {
        file_ = reckless::test::ScratchPath("reckless-interface-settings") /
                (std::string(::testing::UnitTest::GetInstance()->current_test_info()->name()) + ".json");
        std::filesystem::remove(file_);
        InterfaceSettings::SetFileForTests(file_);
    }

    void TearDown() override
    {
        InterfaceSettings::SetFileForTests({});
        std::filesystem::remove(file_);
    }

    void Write(const std::string &text) const
    {
        std::filesystem::create_directories(file_.parent_path());
        std::ofstream(file_, std::ios::binary) << text;
    }

    std::filesystem::path file_;
};
} // namespace

TEST_F(InterfaceSettingsFile, AMissingFileIsScale)
{
    EXPECT_EQ(InterfaceSettings::Mode(), Mode::kScale);
}

TEST_F(InterfaceSettingsFile, TheModeIsWrittenAndReadBack)
{
    ASSERT_TRUE(InterfaceSettings::SetMode(Mode::kScroll));
    EXPECT_EQ(InterfaceSettings::Mode(), Mode::kScroll);
    EXPECT_EQ(reckless::core::ReadBytes(file_).value_or(""), "{\n  \"window_mode\": \"scroll\"\n}\n");
    ASSERT_TRUE(InterfaceSettings::SetMode(Mode::kScale));
    EXPECT_EQ(InterfaceSettings::Mode(), Mode::kScale);
}

TEST_F(InterfaceSettingsFile, TheFilesOtherMembersAreKept)
{
    Write(R"({"theme": "dark", "window_mode": "scale", "count": 3})");
    ASSERT_TRUE(InterfaceSettings::SetMode(Mode::kScroll));
    const auto settings = reckless::core::ReadJsonFile(file_);
    ASSERT_TRUE(settings.has_value());
    EXPECT_EQ((*settings)["theme"].AsString(), "dark");
    EXPECT_EQ((*settings)["count"].AsInt(), 3);
    EXPECT_EQ((*settings)["window_mode"].AsString(), "scroll");
}

TEST_F(InterfaceSettingsFile, AFileThatIsNotTheSettingsIsScaleAndIsWrittenOver)
{
    for (const char *text : {"not json", "[\"scroll\"]", R"({"window_mode": 1})", R"({"window_mode": "SCROLL"})"})
    {
        Write(text);
        EXPECT_EQ(InterfaceSettings::Mode(), Mode::kScale) << text;
    }
    Write("not json");
    ASSERT_TRUE(InterfaceSettings::SetMode(Mode::kScroll));
    EXPECT_EQ(InterfaceSettings::Mode(), Mode::kScroll);
}

TEST(InterfaceSettings, TheFileIsInTheUsersSettingsFolder)
{
    const std::filesystem::path file = InterfaceSettings::File();
    EXPECT_EQ(file.filename(), "interface.json");
    EXPECT_EQ(file.parent_path(), reckless::core::UserSettingsFolder(reckless::core::kSuiteFolder));
    EXPECT_TRUE(file.is_absolute());
}
