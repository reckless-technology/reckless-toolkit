#pragma once

#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Json.h"
#include "JsonFile.h"
#include "PresetFiles.h"

namespace reckless::audio
{
/**
    The audio settings a Standalone keeps between runs: the API, the two devices, the rate, the block, the channels each
    bus takes and the MIDI inputs. Whatever is unset is the default the engine works out on the devices in front of it
    (StandaloneEngine::Open): the API's default devices, the output device's own rate (the input's, where only that is
    a rate both list), a block of 512 (an ASIO driver's own), the first two channels each way and, for a core with a
    key bus, the two inputs after the main input's. An input device set to the empty name is none - a silent input; a
    channel list set empty is no channels. Whether the input is muted against a feedback loop is a Standalone window's
    to apply (StandaloneEngine::SetInputMuted); unset, it is muted.
*/
struct AudioSettings
{
    std::string                     api;           ///< the audio API's name; empty for the platform's default
    std::optional<std::string>      output_device; ///< the output device's name; unset for the API's default
    std::optional<std::string>      input_device;  ///< the input device's name; unset for the default, empty for none
    std::optional<int>              sample_rate;   ///< the rate, in hertz; unset for the device's own
    std::optional<int>              block_size;    ///< the block, in frames; unset for 512, or an ASIO driver's own
    std::optional<std::vector<int>> outputs;       ///< the device output channels, from 0; unset for the first two
    std::optional<std::vector<int>> inputs;        ///< the device input channels, from 0; unset for the first two
    std::optional<std::vector<int>> key;           ///< the key bus's inputs, from 0; unset for the two after the main's
    std::vector<std::string>        midi_inputs;   ///< the names of the MIDI inputs to open
    std::optional<bool>             mute_input;    ///< whether the input is muted; unset is muted

    /** Whether every setting is the same, field by field. */
    friend bool operator==(const AudioSettings &, const AudioSettings &) = default;
};

/** The settings as the suite's file holds them: a JSON object, a member a setting - "api", "output_device",
    "input_device", "sample_rate", "block_size", "outputs", "inputs" and "key" (arrays of channel indices),
   "midi_inputs" (an array of names) and "mute_input" - an unset setting no member.
    @param s  the settings
    @return   the document */
inline core::json::Value SettingsDocument(const AudioSettings &s)
{
    namespace json      = core::json;
    const auto channels = [](const std::vector<int> &list) {
        json::Value array = json::Value::MakeArray();
        for (const int c : list)
        {
            array.Append(c);
        }
        return array;
    };
    json::Value document = json::Value::MakeObject();
    if (!s.api.empty())
    {
        document.Set("api", s.api);
    }
    if (s.output_device)
    {
        document.Set("output_device", *s.output_device);
    }
    if (s.input_device)
    {
        document.Set("input_device", *s.input_device);
    }
    if (s.sample_rate)
    {
        document.Set("sample_rate", *s.sample_rate);
    }
    if (s.block_size)
    {
        document.Set("block_size", *s.block_size);
    }
    if (s.outputs)
    {
        document.Set("outputs", channels(*s.outputs));
    }
    if (s.inputs)
    {
        document.Set("inputs", channels(*s.inputs));
    }
    if (s.key)
    {
        document.Set("key", channels(*s.key));
    }
    if (!s.midi_inputs.empty())
    {
        json::Value names = json::Value::MakeArray();
        for (const std::string &name : s.midi_inputs)
        {
            names.Append(name);
        }
        document.Set("midi_inputs", std::move(names));
    }
    if (s.mute_input)
    {
        document.Set("mute_input", *s.mute_input);
    }
    return document;
}

/** The file's document read back; nothing for one that is not an object. A member of the wrong kind is unset; a rate
    or a block that is not a positive whole number is unset; a channel that is not a whole number from 0 is skipped.
    @param document  the file's parsed document
    @return          the settings, or nothing when `document` is not an object */
inline std::optional<AudioSettings> SettingsOfDocument(const core::json::Value &document)
{
    namespace json = core::json;
    if (!document.IsObject())
    {
        return std::nullopt;
    }
    const auto text = [&document](std::string_view name) -> std::optional<std::string> {
        const json::Value *value = document.Find(name);
        return value != nullptr && value->IsString() ? std::optional<std::string>(value->AsString()) : std::nullopt;
    };
    const auto whole = [&document](std::string_view name) -> std::optional<int> {
        const json::Value *value = document.Find(name);
        return value != nullptr && value->IsInt() && value->AsInt() > 0 && value->AsInt() < (1 << 30)
                   ? std::optional<int>(static_cast<int>(value->AsInt()))
                   : std::nullopt;
    };
    const auto list = [&document](std::string_view name) -> std::optional<std::vector<int>> {
        const json::Array *array = document[name].AsArray();
        if (array == nullptr)
        {
            return std::nullopt;
        }
        std::vector<int> channels;
        for (const json::Value &c : *array)
        {
            if (c.IsInt() && c.AsInt() >= 0 && c.AsInt() < (1 << 16))
            {
                channels.push_back(static_cast<int>(c.AsInt()));
            }
        }
        return channels;
    };
    AudioSettings s;
    s.api           = text("api").value_or("");
    s.output_device = text("output_device");
    s.input_device  = text("input_device");
    s.sample_rate   = whole("sample_rate");
    s.block_size    = whole("block_size");
    s.outputs       = list("outputs");
    s.inputs        = list("inputs");
    s.key           = list("key");
    if (const json::Array *names = document["midi_inputs"].AsArray())
    {
        for (const json::Value &name : *names)
        {
            if (name.IsString())
            {
                s.midi_inputs.push_back(name.AsString());
            }
        }
    }
    if (const json::Value *mute = document.Find("mute_input"); mute != nullptr && mute->IsBool())
    {
        s.mute_input = mute->AsBool();
    }
    return s;
}

/** The folder a Standalone's files live in, from the environment, so a run pointed at a scratch folder keeps off a
    person's own: the suite's folder and the plugin's own under it (core::kSuiteFolder, core::PluginFolderName:
    `reckless-technology/amp`) in `~/Library/Application Support` on macOS, `%APPDATA%` on Windows and `~/.config` on
    Linux, no space anywhere in it. Empty when the environment names no home. Not core::UserSettingsFolder, where the
   suite's shared settings go (the window mode, the response folders): that asks the system for the folder on macOS and
   Windows and honours XDG_CONFIG_HOME on Linux, any of which would take a run with a scratch HOME or APPDATA back to
   the person's own files - the one variable that names the folder here is what the suites and a launch's smoke run rely
   on.
    @param plugin_name  the plugin's display name, whose folder name the plugin's own folder is
    @return             the folder, empty when the environment names no home */
inline std::filesystem::path SettingsFolder(std::string_view plugin_name)
{
    const std::filesystem::path own = std::filesystem::path(std::string(core::kSuiteFolder)) /
                                      std::filesystem::path(core::PluginFolderName(plugin_name));
#if defined(_WIN32)
    wchar_t    *appdata = nullptr;
    std::size_t length  = 0;
    if (_wdupenv_s(&appdata, &length, L"APPDATA") != 0 || appdata == nullptr)
    {
        return {};
    }
    std::filesystem::path folder(appdata);
    std::free(appdata);
    return folder / own;
#else
    const char *home = std::getenv("HOME");
    if (home == nullptr || *home == '\0')
    {
        return {};
    }
#if defined(__APPLE__)
    return std::filesystem::path(home) / "Library" / "Application Support" / own;
#else
    return std::filesystem::path(home) / ".config" / own;
#endif
#endif
}

/** The Standalone's settings file: audio.json in its folder.
    @param plugin_name  the plugin's display name
    @return             the file's path, empty when SettingsFolder() is */
inline std::filesystem::path SettingsFile(std::string_view plugin_name)
{
    const std::filesystem::path folder = SettingsFolder(plugin_name);
    return folder.empty() ? folder : folder / "audio.json";
}

/** The settings file at `file`, or nothing when it is missing or unreadable.
    @param file  the settings file
    @return      the settings read, or nothing */
inline std::optional<AudioSettings> ReadSettings(const std::filesystem::path &file)
{
    const std::optional<core::json::Value> document = core::ReadJsonFile(file);
    return document ? SettingsOfDocument(*document) : std::nullopt;
}

/** The settings written to the file at `file`, whole, its folder made where it is missing. False, with nothing changed,
    when it could not be.
    @param file  the settings file to write
    @param s     the settings
    @return      true when the file was written */
inline bool WriteSettings(const std::filesystem::path &file, const AudioSettings &s)
{
    return core::WriteJsonFile(file, SettingsDocument(s));
}

/** What a Standalone starts on: its file at `file` if there is one, else the defaults.
    @param file  the settings file
    @return      the settings read, or the defaults */
inline AudioSettings LoadSettings(const std::filesystem::path &file)
{
    return ReadSettings(file).value_or(AudioSettings{});
}
} // namespace reckless::audio
