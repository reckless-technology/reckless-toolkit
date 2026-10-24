#pragma once

#include <filesystem>
#include <optional>
#include <string_view>

#include "Json.h"
#include "JsonFile.h"
#include "PresetFiles.h"

namespace reckless::core
{
/**
    The one setting the whole suite's windows share, per user: whether a window dragged larger or smaller scales its
    panel to fit or keeps it at its design size behind scroll bars. It lives in reckless-technology/interface.json in
    the user's settings folder (UserSettingsFolder), a JSON object whose "window_mode" is "scroll" for scroll bars and
    anything else - or nothing - for scale; the file's other members are kept as they are when the mode is written. The
    file is read afresh for every question, so a window opening sees what another wrote, and written whole.

    Tests point the file at a scratch path with SetFileForTests.
*/
class InterfaceSettings
{
  public:
    /** How a window treats its panel when the person resizes it. */
    enum class WindowMode
    {
        kScale = 0, ///< the panel scaled to the window, its aspect kept
        kScroll     ///< the panel at its design size, scroll bars where the window is smaller
    };

    /** The member of the settings object that holds the mode. */
    static constexpr std::string_view kWindowModeKey = "window_mode";

    /** The mode the file holds; scale when the file or the member is missing or unreadable.
        @return  the window mode in force */
    static WindowMode Mode()
    {
        const std::optional<json::Value> settings = ReadJsonFile(File());
        return settings && settings->StringOr(kWindowModeKey, {}) == "scroll" ? WindowMode::kScroll
                                                                              : WindowMode::kScale;
    }

    /** The mode written, the file's other members kept.
        @param mode  the window mode to store
        @return      false when the file could not be written */
    static bool SetMode(WindowMode mode)
    {
        const std::filesystem::path file     = File();
        std::optional<json::Value>  settings = ReadJsonFile(file);
        json::Value document = settings && settings->IsObject() ? std::move(*settings) : json::Value::MakeObject();
        document.Set(kWindowModeKey, mode == WindowMode::kScroll ? "scroll" : "scale");
        return WriteJsonFile(file, document);
    }

    /** The file the setting lives in: the scratch file a test set, or reckless-technology/interface.json in the user's
        settings folder.
        @return  the path of the settings file */
    static std::filesystem::path File()
    {
        const std::filesystem::path &scratch = Override();
        return !scratch.empty() ? scratch : UserSettingsFolder(kSuiteFolder) / "interface.json";
    }

    /** Point the setting at `file` for the rest of the process (a test's scratch file); an empty path puts the default
        back.
        @param file  the scratch file to use, or an empty path for the default */
    static void SetFileForTests(const std::filesystem::path &file)
    {
        Override() = file;
    }

  private:
    /** The test's scratch path, empty when none is set.
        @return  a reference to the process-wide override, which SetFileForTests writes */
    static std::filesystem::path &Override()
    {
        static std::filesystem::path file;
        return file;
    }
};
} // namespace reckless::core
