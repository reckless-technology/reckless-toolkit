// The settings a Standalone keeps (audio/AudioSettings.h): reckless-technology/<plugin>/audio.json, every setting set
// or left to its default and read back as written, a member of the wrong kind unset, and a file that is not the
// settings read as the defaults.
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

#include "AudioSettings.h"
#include "Json.h"
#include "ScratchPath.h"

#include <gtest/gtest.h>

namespace
{
using reckless::audio::AudioSettings;
namespace fs   = std::filesystem;
namespace json = reckless::core::json;

/** A folder of the test's own, removed after. */
class Folder
{
  public:
    Folder()
        : path_(
              reckless::test::ScratchPath("reckless-audio-settings-" +
                                          std::string(::testing::UnitTest::GetInstance()->current_test_info()->name())))
    {
        std::error_code error;
        fs::remove_all(path_, error);
        fs::create_directories(path_, error);
    }

    ~Folder()
    {
        std::error_code error;
        fs::remove_all(path_, error);
    }

    const fs::path &path() const noexcept
    {
        return path_;
    }

  private:
    fs::path path_;
};

void Write(const fs::path &file, const std::string &text)
{
    std::ofstream(file, std::ios::binary | std::ios::trunc) << text;
}

AudioSettings Full()
{
    AudioSettings full;
    full.api           = "ASIO";
    full.output_device = "Interface \"Pro\" <USB> & co";
    full.input_device  = "Interface \"Pro\" <USB> & co";
    full.sample_rate   = 96000;
    full.block_size    = 64;
    full.outputs       = std::vector<int>{2, 3};
    full.inputs        = std::vector<int>{0};
    full.key           = std::vector<int>{4, 5};
    full.midi_inputs   = {"Pedal", "\xce\xa9 Keys"}; // an omega, in UTF-8
    full.mute_input    = false;
    return full;
}
} // namespace

TEST(AudioSettings, TheFileReadsBackWhatItWrote)
{
    Folder         folder;
    const fs::path file = folder.path() / "audio.json";
    ASSERT_TRUE(reckless::audio::WriteSettings(file, Full()));
    EXPECT_EQ(reckless::audio::ReadSettings(file), std::optional<AudioSettings>(Full()));
    const auto document = reckless::core::ReadJsonFile(file);
    ASSERT_TRUE(document.has_value());
    EXPECT_EQ(json::Write((*document)["outputs"]), "[\n  2,\n  3\n]") << "a channel list an array of indices";
    EXPECT_EQ((*document)["midi_inputs"].AsArray()->size(), 2u);

    // Unset is unset, an empty input device is none, an empty channel list is no channels: three different things.
    AudioSettings sparse;
    sparse.input_device = "";
    sparse.inputs       = std::vector<int>{};
    ASSERT_TRUE(reckless::audio::WriteSettings(file, sparse));
    const auto back = reckless::audio::ReadSettings(file);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ(*back, sparse);
    EXPECT_FALSE(back->output_device.has_value());
    EXPECT_EQ(back->input_device, std::optional<std::string>(""));
    EXPECT_EQ(back->inputs, std::optional<std::vector<int>>(std::vector<int>{}));
    EXPECT_FALSE(back->key.has_value());
    EXPECT_FALSE(back->mute_input.has_value()) << "muted, by the window's default";

    ASSERT_TRUE(reckless::audio::WriteSettings(file, AudioSettings{}));
    EXPECT_EQ(reckless::core::ReadBytes(file).value_or(""), "{}\n") << "nothing set, nothing written";
    EXPECT_EQ(reckless::audio::ReadSettings(file), std::optional<AudioSettings>(AudioSettings{}));
}

TEST(AudioSettings, AFileThatIsNotTheSettingsIsTheDefaultsAndAWrongMemberIsUnset)
{
    Folder         folder;
    const fs::path file = folder.path() / "settings.json";
    EXPECT_FALSE(reckless::audio::ReadSettings(file).has_value()) << "no file";
    EXPECT_EQ(reckless::audio::LoadSettings(file), AudioSettings{});
    for (const char *text : {"not json", "[1, 2]", "\"text\""})
    {
        Write(file, text);
        EXPECT_FALSE(reckless::audio::ReadSettings(file).has_value()) << text;
        EXPECT_EQ(reckless::audio::LoadSettings(file), AudioSettings{}) << text;
    }
    Write(file, R"({"api": 3, "output_device": ["x"], "sample_rate": -48000, "block_size": 64.5,
                    "outputs": [0, -1, "2", 3], "midi_inputs": ["Pedal", 7], "mute_input": "no"})");
    const auto read = reckless::audio::ReadSettings(file);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(read->api, "");
    EXPECT_FALSE(read->output_device.has_value());
    EXPECT_FALSE(read->sample_rate.has_value()) << "a rate that is not a positive whole number";
    EXPECT_FALSE(read->block_size.has_value());
    EXPECT_EQ(read->outputs, std::optional<std::vector<int>>(std::vector<int>{0, 3})) << "the channels that are some";
    EXPECT_EQ(read->midi_inputs, (std::vector<std::string>{"Pedal"}));
    EXPECT_FALSE(read->mute_input.has_value());
}

TEST(AudioSettings, TheFilesAreTheStandalonesOwnInItsFolder)
{
    const fs::path settings = reckless::audio::SettingsFile("Reckless Octave");
    EXPECT_EQ(settings.filename().string(), "audio.json");
    EXPECT_EQ(settings.parent_path(), reckless::audio::SettingsFolder("Reckless Octave"));
    const fs::path folder = reckless::audio::SettingsFolder("Reckless Octave");
    EXPECT_EQ(folder.filename().string(), "octave") << "the plugin's own folder, no space";
    EXPECT_EQ(folder.parent_path().filename().string(), "reckless-technology") << "in the suite's";
}
