// The preset files (core/PresetFiles.h) and the JSON files under them (JsonFile.h): a name trimmed and refused
// where a file system cannot hold it, the files - saved, listed by name ignoring case, loaded, refused and deleted - in
// a scratch folder, written whole, and the platform's documents and music folders.
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "JsonFile.h"
#include "PresetFiles.h"
#include "ScratchPath.h"

#include <gtest/gtest.h>

namespace
{
namespace core    = reckless::core;
namespace json    = reckless::core::json;
namespace presets = reckless::core::presets;

/** A scratch presets root for the test's life, so nothing touches the user's documents; in the process's own
    scratch folder, since CTest runs the suites at once and two runs of this one may meet. */
class ScratchRoot
{
  public:
    ScratchRoot() : folder_(reckless::test::ScratchPath("reckless-core-preset-files-test"))
    {
        std::error_code error;
        std::filesystem::remove_all(folder_, error);
        std::filesystem::create_directories(folder_, error);
        presets::SetRootForTests(folder_);
    }

    ~ScratchRoot()
    {
        presets::SetRootForTests({});
        std::error_code error;
        std::filesystem::remove_all(folder_, error);
    }

    const std::filesystem::path &folder() const
    {
        return folder_;
    }

  private:
    std::filesystem::path folder_;
};

void WriteFile(const std::filesystem::path &file, const std::string &bytes)
{
    std::filesystem::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

/** A plugin's state document: its format, its plugin, two parameters and its own state. */
json::Value SampleDocument()
{
    json::Value document = json::Value::MakeObject();
    document.Set("format", 1);
    document.Set("plugin", "Reckless Toy");
    json::Value &parameters = document.Set("parameters", json::Value::MakeObject());
    parameters.Set("gain", 1.5);
    parameters.Set("mode", 2);
    document.Set("state", json::Value::MakeObject()).Set("note", "kept");
    return document;
}
} // namespace

TEST(CorePresetFiles, ANameIsTrimmedAndRefusedWhereAFileCannotHoldIt)
{
    EXPECT_EQ(presets::TrimmedText("  \t Crunch Lead \r\n"), "Crunch Lead");
    EXPECT_EQ(presets::TrimmedText(" \n "), "");
    for (const char *name : {"Crunch",
                             "Br\xC3\xBC"
                             "cke 2",
                             "a.b", "COM0", "CONSOLE", "Aux2", "~home"})
    {
        EXPECT_EQ(presets::NameRefusal(name), "") << name;
    }
    for (const char *name :
         {"",     "a/b", "a\\b",    "a:b", "a*b", "a?b", "a\"b",     "a<b",  "a>b",      "a|b",   "tab\there",
          "dot.", "..",  ".hidden", "CON", "con", "Aux", "nul.json", "COM1", "lpt9.txt", "PRN .x"})
    {
        EXPECT_NE(presets::NameRefusal(name), "") << name;
    }
    EXPECT_NE(presets::NameRefusal(std::string(201, 'x')), "") << "too long for a file name and its extension";
    EXPECT_EQ(presets::NameRefusal(std::string(200, 'x')), "");
    EXPECT_EQ(presets::LegalFileName("Capture 2026/10/04 22:48"), "Capture 20261004 2248")
        << "a name a save dialog suggests, the characters a file cannot hold taken out";
}

TEST(CorePresetFiles, SaveListLoadAndDeleteInTheFolderPerPlugin)
{
    ScratchRoot                 scratch;
    const std::filesystem::path folder = presets::FolderFor("Reckless Toy");
    EXPECT_EQ(folder, scratch.folder() / "toy") << "the plugin's folder name: lower case, no Reckless, no space";
    EXPECT_TRUE(presets::List("Reckless Toy").empty()) << "no folder yet";

    std::filesystem::path saved;
    ASSERT_TRUE(presets::Save("Reckless Toy", "  Squash ", SampleDocument(), &saved).IsOk());
    EXPECT_EQ(saved, folder / "Squash.json") << "the name trimmed";
    EXPECT_EQ(core::ReadBytes(saved).value_or(""), json::Write(SampleDocument()) + "\n")
        << "the document as JSON text and a line feed";
    ASSERT_TRUE(presets::Save("Reckless Toy", "apple", SampleDocument()).IsOk());
    ASSERT_TRUE(presets::Save("Reckless Toy", "Banana", SampleDocument()).IsOk());
    WriteFile(folder / "Cherry.JSON", json::Write(SampleDocument()));
    WriteFile(folder / "notes.txt", "not a preset");
    WriteFile(folder / ".Squash.json.0123.tmp", "a temporary a crash left");
    WriteFile(folder / ".hidden.json", "{}");
    std::filesystem::create_directories(folder / "Folder.json");

    std::vector<std::string> names;
    for (const auto &file : presets::List("Reckless Toy"))
    {
        names.push_back(presets::NameOf(file));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"apple", "Banana", "Cherry", "Squash"}))
        << "every .json file, in any case, by name ignoring case; no other file, no hidden one, no folder";

    const presets::Loaded loaded = presets::LoadFrom(saved);
    ASSERT_TRUE(loaded.IsOk()) << loaded.error;
    EXPECT_EQ(loaded.document, SampleDocument());
    EXPECT_EQ(loaded.name, "Squash");

    ASSERT_TRUE(presets::Save("Reckless Toy", "Squash", json::Value::MakeObject()).IsOk()) << "a save over one";
    EXPECT_EQ(presets::LoadFrom(saved).document, json::Value::MakeObject());

    EXPECT_TRUE(presets::Delete(saved));
    EXPECT_FALSE(std::filesystem::exists(saved));
    EXPECT_TRUE(presets::Delete(saved)) << "a file that is not there is gone";
}

TEST(CorePresetFiles, TheFactoryPresetsAreListedBesideTheUsersOwn)
{
    ScratchRoot scratch;
    EXPECT_TRUE(presets::FactoryRoot().empty())
        << "a test's root set and no factory root: none, whatever an installer put on the machine";
    EXPECT_TRUE(presets::FactoryFolderFor("Reckless Toy").empty());

    const std::filesystem::path factory_root = scratch.folder() / "factory";
    presets::SetFactoryRootForTests(factory_root);
    const std::filesystem::path factory = presets::FactoryFolderFor("Reckless Toy");
    EXPECT_EQ(factory, factory_root / "toy") << "the plugin's folder name, as the user's";
    WriteFile(factory / "Lead.json", json::Write(SampleDocument()));
    WriteFile(factory / "clean.json", json::Write(SampleDocument()));
    WriteFile(factory / "Ambient.json", json::Write(SampleDocument()));
    WriteFile(factory / ".hidden.json", "{}");
    ASSERT_TRUE(presets::Save("Reckless Toy", "Banana", SampleDocument()).IsOk());
    ASSERT_TRUE(presets::Save("Reckless Toy", "LEAD", json::Value::MakeObject()).IsOk());

    std::vector<std::string> names;
    std::vector<std::string> folders;
    for (const auto &file : presets::List("Reckless Toy"))
    {
        names.push_back(presets::NameOf(file));
        folders.push_back(file.parent_path() == factory ? "factory" : "user");
    }
    EXPECT_EQ(names, (std::vector<std::string>{"Ambient", "Banana", "clean", "LEAD"}))
        << "both folders by name ignoring case, the user's LEAD in the factory Lead's place";
    EXPECT_EQ(folders, (std::vector<std::string>{"factory", "user", "factory", "user"}));

    EXPECT_TRUE(presets::IsFactoryOnly("Reckless Toy", "Ambient"));
    EXPECT_TRUE(presets::IsFactoryOnly("Reckless Toy", " clean "));
    EXPECT_FALSE(presets::IsFactoryOnly("Reckless Toy", "Lead")) << "the user's copy of it is the preset now";
    EXPECT_FALSE(presets::IsFactoryOnly("Reckless Toy", "Banana"));
    EXPECT_FALSE(presets::IsFactoryOnly("Reckless Toy", "Missing"));
    EXPECT_TRUE(presets::List("Reckless Other").empty()) << "another plugin's factory folder is its own";

    presets::SetFactoryRootForTests({});
    EXPECT_TRUE(presets::FactoryRoot().empty());
}

TEST(CorePresetFiles, TheInstalledFactoryPresetsAreInTheMachinesFolder)
{
    // Where the installers put the factory presets: the machine's settings on macOS and Windows, the shared data on
    // Linux, where the Debian package puts them (/usr/share).
    const std::filesystem::path root = presets::InstalledFactoryRoot();
    std::printf("  the factory presets: %s\n", presets::TextOf(root).c_str());
    EXPECT_TRUE(root.is_absolute());
    EXPECT_EQ(root.filename(), "presets");
#if defined(__APPLE__) || defined(_WIN32)
    EXPECT_EQ(root, core::MachineSettingsFolder(core::kSuiteFolder) / "presets");
#elif defined(__linux__)
    const char                 *was   = std::getenv("XDG_DATA_DIRS");
    const std::string           kept  = was != nullptr ? was : "";
    const std::filesystem::path share = reckless::test::ScratchPath("reckless-core-factory-share");
    std::error_code             error;
    std::filesystem::remove_all(share, error);
    setenv("XDG_DATA_DIRS", ("relative:" + share.string() + ":/usr/share").c_str(), 1);
    EXPECT_EQ(presets::InstalledFactoryRoot(), std::filesystem::path("/usr/share/reckless-technology/presets"))
        << "no directory holding the presets: where the package puts them";
    std::filesystem::create_directories(share / "reckless-technology" / "presets", error);
    EXPECT_EQ(presets::InstalledFactoryRoot(), share / "reckless-technology" / "presets")
        << "the first directory that holds them, a relative one passed over";
    if (was != nullptr)
    {
        setenv("XDG_DATA_DIRS", kept.c_str(), 1);
    }
    else
    {
        unsetenv("XDG_DATA_DIRS");
    }
    std::filesystem::remove_all(share, error);
#endif
}

TEST(CorePresetFiles, WhatCannotBeSavedOrReadIsSaidInPlainWords)
{
    ScratchRoot scratch;
    EXPECT_EQ(presets::Save("Reckless Toy", "  ", SampleDocument()).error, "A preset needs a name.");
    EXPECT_NE(presets::Save("Reckless Toy", "a/b", SampleDocument()).error, "");
    EXPECT_TRUE(presets::List("Reckless Toy").empty()) << "nothing written for a refused name";

    const std::filesystem::path folder = presets::FolderFor("Reckless Toy");
    EXPECT_NE(presets::LoadFrom(folder / "Missing.json").error.find("There is no file"), std::string::npos);
    WriteFile(folder / "Broken.json", "{\"format\": 1,");
    const presets::Loaded broken = presets::LoadFrom(folder / "Broken.json");
    EXPECT_FALSE(broken.IsOk());
    EXPECT_NE(broken.error.find("Broken.json is not a preset: it is not JSON"), std::string::npos) << broken.error;
    EXPECT_TRUE(broken.document.IsNull());

    // A folder where the plugin's folder should be is a file: the save says why, and writes nothing.
    WriteFile(scratch.folder() / "blocked", "a file in the folder's place");
    const presets::Result blocked = presets::Save("Reckless Blocked", "Lead", SampleDocument());
    EXPECT_FALSE(blocked.IsOk());
    std::printf("  a folder the system will not make: %s\n", blocked.error.c_str());
}

TEST(CoreJsonFile, AFileIsWrittenWholeAndReadBackOrNothing)
{
    ScratchRoot                 scratch;
    const std::filesystem::path file = scratch.folder() / "deeper" / "settings.json";
    ASSERT_TRUE(core::WriteJsonFile(file, SampleDocument())) << "the folder made first";
    EXPECT_EQ(core::ReadJsonFile(file), SampleDocument());
    std::vector<std::string> left;
    for (const auto &entry : std::filesystem::directory_iterator(file.parent_path()))
    {
        left.push_back(entry.path().filename().string());
    }
    EXPECT_EQ(left, (std::vector<std::string>{"settings.json"})) << "no temporary left beside it";

    std::string why;
    EXPECT_FALSE(core::ReadJsonFile(scratch.folder() / "missing.json", &why).has_value());
    EXPECT_EQ(why, "it cannot be read");
    WriteFile(file, "[1, 2");
    EXPECT_FALSE(core::ReadJsonFile(file, &why).has_value());
    EXPECT_EQ(why.rfind("it is not JSON", 0), 0u) << why;
    EXPECT_FALSE(core::WriteJsonFile({}, SampleDocument())) << "no file to write";
}

TEST(CoreJsonFile, AWriteThatStopsSaysWhereAndLeavesNothingBehind)
{
    // WriteWhole, which every saved file goes through - the documents and the responses' WAVs alike: bytes that are
    // not text written as they are; a folder that is not there stops it at the temporary, a destination nothing can be
    // renamed over (a folder holding a file) at the rename, five tries in; neither leaves a temporary beside it.
    ScratchRoot        scratch;
    core::WriteFailure failure = core::WriteFailure::kReplace;
    const std::string  binary("RIFF\0\x01\xff\x7f", 8);
    const auto         written = scratch.folder() / "bytes.wav";
    ASSERT_TRUE(core::WriteWhole(written, binary, &failure));
    EXPECT_EQ(failure, core::WriteFailure::kNone);
    EXPECT_EQ(core::ReadBytes(written), binary);

    EXPECT_FALSE(core::WriteWhole(scratch.folder() / "missing" / "x.wav", binary, &failure));
    EXPECT_EQ(failure, core::WriteFailure::kOpen);

    const auto taken = scratch.folder() / "taken.wav";
    WriteFile(taken / "inside", "a folder in the file's place");
    EXPECT_FALSE(core::WriteWhole(taken, binary, &failure));
    EXPECT_EQ(failure, core::WriteFailure::kReplace);
    EXPECT_FALSE(core::WriteWhole(taken, binary)) << "and with nobody asking why";

    std::vector<std::string> left;
    for (const auto &entry : std::filesystem::directory_iterator(scratch.folder()))
    {
        left.push_back(entry.path().filename().string());
    }
    std::sort(left.begin(), left.end());
    EXPECT_EQ(left, (std::vector<std::string>{"bytes.wav", "taken.wav"})) << "no temporary left beside them";
}

TEST(CorePresetFiles, TheDocumentsFolderIsThePlatforms)
{
    const std::filesystem::path documents = presets::DocumentsDirectory();
    std::printf("  the user's documents: %s\n", presets::TextOf(documents).c_str());
    EXPECT_FALSE(documents.empty());
    EXPECT_TRUE(documents.is_absolute());
    EXPECT_EQ(presets::Root(), documents / "reckless-technology" / "presets");
}

TEST(CorePresetFiles, TheMachinesSettingsFolderIsThePlatforms)
{
    // Where a licence every user of the machine shares is read (licence/'s Store): the machine's own settings,
    // which an administrator writes and every user reads, never under the user's home.
    const std::filesystem::path folder = core::MachineSettingsFolder(core::kSuiteFolder);
    std::printf("  the machine's settings: %s\n", presets::TextOf(folder).c_str());
    EXPECT_TRUE(folder.is_absolute());
    EXPECT_EQ(folder.filename(), "reckless-technology");
    EXPECT_NE(folder, core::UserSettingsFolder(core::kSuiteFolder));
#if defined(__APPLE__)
    EXPECT_EQ(folder, std::filesystem::path("/Library/Application Support/reckless-technology"));
#elif defined(__linux__)
    if (std::getenv("XDG_CONFIG_DIRS") == nullptr)
    {
        EXPECT_EQ(folder, std::filesystem::path("/etc/xdg/reckless-technology"));
    }
#endif
}

TEST(CorePresetFiles, TheMusicFolderIsThePlatforms)
{
    // Where a chooser for a response opens when nothing better is known (the Cab's and the Space's).
    const std::filesystem::path music = presets::MusicDirectory();
    std::printf("  the user's music: %s\n", presets::TextOf(music).c_str());
    EXPECT_FALSE(music.empty());
    EXPECT_TRUE(music.is_absolute());
#if defined(__APPLE__)
    EXPECT_EQ(music, presets::DocumentsDirectory().parent_path() / "Music");
#endif
#if defined(__linux__)
    // On Linux the XDG_MUSIC_DIR line of the home directory's user-dirs.dirs when it names a directory, "$HOME" read
    // as the home directory, else the home directory's Music: a scratch home for the run, the person's put back.
    const char                 *was  = std::getenv("HOME");
    const std::string           kept = was != nullptr ? was : "";
    const std::filesystem::path home = reckless::test::ScratchPath("reckless-core-music-home");
    std::error_code             error;
    std::filesystem::remove_all(home, error);
    std::filesystem::create_directories(home / ".config", error);
    setenv("HOME", home.c_str(), 1);
    EXPECT_EQ(presets::MusicDirectory(), home / "Music") << "no user-dirs.dirs";
    std::ofstream(home / ".config" / "user-dirs.dirs") << "# written by the suite\n"
                                                       << "XDG_DOCUMENTS_DIR=\"$HOME/Papers\"\n"
                                                       << "XDG_MUSIC_DIR=\"$HOME/Tunes\"\n";
    EXPECT_EQ(presets::MusicDirectory(), home / "Music") << "a folder that is not there is passed over";
    std::filesystem::create_directories(home / "Tunes", error);
    EXPECT_EQ(presets::MusicDirectory(), home / "Tunes") << "the folder the line names";
    if (was != nullptr)
    {
        setenv("HOME", kept.c_str(), 1);
    }
    else
    {
        unsetenv("HOME");
    }
    std::filesystem::remove_all(home, error);
#endif
}

TEST(CorePresetFiles, APluginsFolderIsItsNameInLowerCaseWithNoSpace)
{
    // Every folder the suite keeps is one word in a shell: the plugin's own is its directory in the source tree.
    EXPECT_EQ(core::PluginFolderName("Reckless Amp"), "amp");
    EXPECT_EQ(core::PluginFolderName("Reckless EQ"), "eq");
    EXPECT_EQ(core::PluginFolderName("Reckless NAM"), "nam");
    EXPECT_EQ(core::PluginFolderName("Reckless Pedalboard"), "pedalboard");
    EXPECT_EQ(core::PluginFolderName("Midi Toy"), "midi-toy") << "no prefix to drop: the spaces become hyphens";
    EXPECT_EQ(core::PluginFolderName("Reckless  Two   Words "), "two-words") << "a run of spaces one hyphen";
    EXPECT_EQ(core::kSuiteFolder, "reckless-technology");
}
