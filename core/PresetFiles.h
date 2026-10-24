#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "Json.h"
#include "JsonFile.h"
#include "ParameterText.h"
#include "Utf8.h"

namespace reckless::core
{
/** The one folder every file the suite keeps on a machine sits in, under the user's settings, the machine's settings
    and the user's documents: lower case and hyphenated, with no space in it, so a path to any of them is one word in a
    shell. */
inline constexpr std::string_view kSuiteFolder = "reckless-technology";

/** A plugin's own folder among the suite's files - its presets', its Standalone's settings' - from its product name:
    lower case, a leading "Reckless " dropped and every other run of spaces a hyphen, so "Reckless Amp" is "amp" and
    "Reckless EQ" is "eq", the plugin's directory in the source tree.
    @param product  the plugin's product name, UTF-8
    @return         the folder's name */
inline std::string PluginFolderName(std::string_view product)
{
    constexpr std::string_view kPrefix = "Reckless ";
    if (product.starts_with(kPrefix))
    {
        product.remove_prefix(kPrefix.size());
    }
    std::string name;
    bool        space = false;
    for (const char c : product)
    {
        if (c == ' ')
        {
            space = true;
            continue;
        }
        if (space && !name.empty())
        {
            name += '-';
        }
        space = false;
        name += (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    }
    return name;
}

/** The folder a per-user settings file of `folder_name` lives in: on macOS `~/Library/Application Support/folder`, on
    Windows the roaming application data folder with `folder` under it, on Linux `$XDG_CONFIG_HOME/folder`
    (`~/.config/folder` where it is unset). PresetFiles.cpp.
    @param folder_name  the folder's name, UTF-8, such as kSuiteFolder
    @return             the folder; an empty path where the platform names no settings location */
std::filesystem::path UserSettingsFolder(std::string_view folder_name);

/** The folder a settings file every user of the machine shares lives in, which an administrator writes and every user
    reads: on macOS `/Library/Application Support/folder`, on Windows the program data folder (`C:\ProgramData`) with
    `folder` under it, on Linux the first absolute directory of `$XDG_CONFIG_DIRS` with `folder` under it
    (`/etc/xdg/folder` where it names none). PresetFiles.cpp.
    @param folder_name  the folder's name, UTF-8, such as kSuiteFolder
    @return             the folder; an empty path where the platform names no such location */
std::filesystem::path MachineSettingsFolder(std::string_view folder_name);
} // namespace reckless::core

namespace reckless::core::presets
{
/**
    The preset files: where they live, what they may be called, how they are listed, written, read and deleted.
    presets.md (docs/core/) has the design; tests/test_preset_files.cpp holds it.

    The folder: Documents/reckless-technology/presets/`<plugin>`/ (PluginFolderName: `amp` for Reckless Amp), the
    user's documents found by the platform's own call (DocumentsDirectory). A preset is `<name>.json` there, its name
   trimmed of white space at either end and refused where a file system cannot hold it (NameRefusal); the listing is
   every file in the folder whose name ends in ".json" in any case, with the factory presets an installer put in the
   machine's folder for the plugin (FactoryFolderFor) beside them, read only, a user's preset of the same name taking
   a factory one's place, sorted by name ignoring case. A file holds a
   plugin's state document (HostSession.h's PresetDocument), written whole or not at all (WriteJsonFile); reading one
   hands back the document and the name, and what the document must be is the session's to say (HostSession::Refusal).

    Paths and names are UTF-8 text in the code: PathOf makes a path of it, TextOf text of a path.
*/

/** The extension of a preset file, with its dot. */
inline constexpr std::string_view kExtension = ".json";

/** The user's documents, by the platform's own call: on macOS the system's documents directory for the user
    (sysdir), on Windows the Documents known folder, on Linux the XDG_DOCUMENTS_DIR line of ~/.config/user-dirs.dirs
    when it names a directory, else ~/Documents. PresetFiles.cpp, the one part on the platform's own interfaces.
    @return  the documents directory */
std::filesystem::path DocumentsDirectory();

/** Where the installers put the factory presets every user of the machine shares, the plugins' folders under it: on
    macOS and Windows the machine's settings (MachineSettingsFolder: `/Library/Application Support` or `C:\ProgramData`)
    with `reckless-technology/presets` under it; on Linux the first directory of `$XDG_DATA_DIRS` (`/usr/local/share`
   and
    `/usr/share` where it is unset) holding `reckless-technology/presets`, else
   `/usr/share/reckless-technology/presets`, where the Debian package installs them. PresetFiles.cpp.
    @return  the folder; it may not exist, and is empty where the platform names no such location */
std::filesystem::path InstalledFactoryRoot();

/** The user's music, where a chooser for an audio file opens when nothing better is known (the Cab's and the Space's
    responses), found as DocumentsDirectory finds the documents: the music directory, the Music known folder, or
    XDG_MUSIC_DIR else ~/Music. PresetFiles.cpp.
    @return  the music directory */
std::filesystem::path MusicDirectory();

/** What went wrong, in words a window can show: on Windows a system error's message in the system's language from the
    wide interface, as UTF-8 (std::error_code::message() gives it in the ANSI code page); elsewhere the error's own
    message. PresetFiles.cpp.
    @param error  the error to describe
    @return       the message, UTF-8 */
std::string ErrorText(const std::error_code &error);

/** UTF-8 `text` as a path: its bytes on macOS and Linux, its UTF-16 on Windows (a malformed byte read as itself).
    @param text  the UTF-8 text
    @return      the path */
inline std::filesystem::path PathOf(std::string_view text)
{
#if defined(_WIN32)
    std::wstring wide;
    for (std::size_t i = 0; i < text.size();)
    {
        const std::uint32_t c = utf8::NextCodePoint(text, i);
        if (c >= 0x10000)
        {
            wide.push_back(static_cast<wchar_t>(0xd800 + ((c - 0x10000) >> 10)));
            wide.push_back(static_cast<wchar_t>(0xdc00 + ((c - 0x10000) & 0x3ff)));
        }
        else
        {
            wide.push_back(static_cast<wchar_t>(c));
        }
    }
    return std::filesystem::path(wide);
#else
    return std::filesystem::path(std::string(text));
#endif
}

/** A path as UTF-8 text: its bytes on macOS and Linux; on Windows its UTF-16, a surrogate pair one character and a lone
    surrogate U+FFFD.
    @param path  the path
    @return      the UTF-8 text */
inline std::string TextOf(const std::filesystem::path &path)
{
#if defined(_WIN32)
    const std::wstring &wide = path.native();
    std::string         out;
    for (std::size_t i = 0; i < wide.size(); ++i)
    {
        auto unit = static_cast<std::uint32_t>(static_cast<std::uint16_t>(wide[i]));
        if (unit >= 0xd800 && unit <= 0xdbff && i + 1 < wide.size() &&
            static_cast<std::uint16_t>(wide[i + 1]) >= 0xdc00 && static_cast<std::uint16_t>(wide[i + 1]) <= 0xdfff)
        {
            unit = 0x10000 + ((unit - 0xd800) << 10) + (static_cast<std::uint16_t>(wide[++i]) - 0xdc00u);
        }
        else if (unit >= 0xd800 && unit <= 0xdfff)
        {
            unit = 0xfffd;
        }
        utf8::AppendUtf8(out, unit);
    }
    return out;
#else
    return path.native();
#endif
}

/** `text` with white space - space, tab, line feed, carriage return, vertical tab and form feed - taken off both ends
    (ParameterText.h's Trim).
    @param text  the text to trim
    @return      the trimmed text, a copy */
inline std::string TrimmedText(std::string_view text)
{
    return std::string(Trim(text));
}

/** The characters no file name may hold on one platform or another: the separators, the characters Windows keeps, and
    the controls.
    @param c  one byte of a UTF-8 name
    @return   true when a file name may not hold it */
inline bool IsIllegalInFileName(char c) noexcept
{
    constexpr std::string_view kIllegal = "/\\:*?\"<>|";
    const auto                 byte     = static_cast<unsigned char>(c);
    return byte < 0x20 || byte == 0x7f || kIllegal.find(c) != std::string_view::npos;
}

/** `name` with every character IsIllegalInFileName names taken out: a name a save dialog suggests (the Cab's
    capture).
    @param name  the name
    @return      the name without those characters */
inline std::string LegalFileName(std::string_view name)
{
    std::string out;
    for (const char c : name)
    {
        if (!IsIllegalInFileName(c))
        {
            out.push_back(c);
        }
    }
    return out;
}

/** The longest preset name in bytes: a file name of it and ".json" stays under every file system's 255. */
inline constexpr std::size_t kMaxNameBytes = 200;

/** Why `name`, already trimmed, cannot name a preset's file, or empty when it can: nothing at all, a character a file
    name cannot hold (IsIllegalInFileName), a dot first (a hidden file, which the listing leaves out) or last (which
    Windows drops), a name longer than kMaxNameBytes, or one Windows keeps for a device - CON, PRN, AUX, NUL, COM1 to
    COM9 and LPT1 to LPT9, in any case, alone or before a dot.
    @param name  the preset's name, already trimmed
    @return      the reason, in a sentence a window can show, or an empty string when the name will do */
inline std::string NameRefusal(std::string_view name)
{
    if (name.empty())
    {
        return "A preset needs a name.";
    }
    if (std::any_of(name.begin(), name.end(), IsIllegalInFileName))
    {
        return "A preset's name cannot hold / \\ : * ? \" < > | or a control character.";
    }
    if (name.front() == '.' || name.back() == '.')
    {
        return "A preset's name cannot start or end with a dot.";
    }
    if (name.size() > kMaxNameBytes)
    {
        return "A preset's name can be at most " + std::to_string(kMaxNameBytes) + " bytes long.";
    }
    std::string_view stem = name.substr(0, name.find('.'));
    while (!stem.empty() && stem.back() == ' ')
    {
        stem.remove_suffix(1);
    }
    const std::string device   = ToLower(stem);
    const bool        numbered = device.size() == 4 && (device.starts_with("com") || device.starts_with("lpt")) &&
                                 device[3] >= '1' && device[3] <= '9';
    if (device == "con" || device == "prn" || device == "aux" || device == "nul" || numbered)
    {
        return "\"" + std::string(name) + "\" is a name Windows keeps for a device.";
    }
    return {};
}

/** `a` against `b` ignoring the case of ASCII letters, byte by byte: negative, zero or positive.
    @param a  the first text
    @param b  the second text
    @return   negative when `a` sorts first, zero when they are the same ignoring case, positive when `b` sorts first */
inline int CompareIgnoringCase(std::string_view a, std::string_view b) noexcept
{
    for (std::size_t i = 0; i < a.size() && i < b.size(); ++i)
    {
        const auto x = static_cast<unsigned char>(LowerAscii(a[i]));
        const auto y = static_cast<unsigned char>(LowerAscii(b[i]));
        if (x != y)
        {
            return x < y ? -1 : 1;
        }
    }
    return a.size() == b.size() ? 0 : (a.size() < b.size() ? -1 : 1);
}

/** The root the plugins' folders sit under - the user's documents by default, a scratch folder in a test.
    @return  a reference to the process-wide override; empty for the default */
inline std::filesystem::path &RootStorage()
{
    static std::filesystem::path root;
    return root;
}

/** Put the plugins' folders under a scratch folder for the rest of the process.
    @param root  the scratch folder, or an empty path for the default */
inline void SetRootForTests(const std::filesystem::path &root)
{
    RootStorage() = root;
}

/** Documents/reckless-technology/presets, or the folder a test set.
    @return  the folder the plugins' preset folders sit in */
inline std::filesystem::path Root()
{
    if (!RootStorage().empty())
    {
        return RootStorage();
    }
    return DocumentsDirectory() / presets::PathOf(kSuiteFolder) / "presets";
}

/** The folder a plugin's presets live in: Root() with the plugin's folder name under it (PluginFolderName: "amp" for
    "Reckless Amp"). Created by the first save.
    @param plugin  the plugin's product name, UTF-8
    @return        the folder */
inline std::filesystem::path FolderFor(std::string_view plugin)
{
    return Root() / PathOf(PluginFolderName(plugin));
}

/** A preset's name as its file spells it: the file name without its extension.
    @param file  the preset's file
    @return      the name, UTF-8 */
inline std::string NameOf(const std::filesystem::path &file)
{
    return TextOf(file.stem());
}

/** The file a preset of `name` is saved as in `folder`: the name trimmed, and ".json".
    @param folder  the plugin's preset folder
    @param name    the preset's name
    @return        the file's path */
inline std::filesystem::path FileFor(const std::filesystem::path &folder, std::string_view name)
{
    return folder / PathOf(TrimmedText(name) + std::string(kExtension));
}

/** Whether `name` ends in ".json" in any case.
    @param name  a file's name, UTF-8
    @return      true when it has the extension and something before it */
inline bool HasPresetExtension(std::string_view name) noexcept
{
    return name.size() > kExtension.size() &&
           CompareIgnoringCase(name.substr(name.size() - kExtension.size()), kExtension) == 0;
}

/** The factory presets' root a test set, or a path naming none.
    @return  a reference to the process-wide override; empty for the default */
inline std::filesystem::path &FactoryRootStorage()
{
    static std::filesystem::path root;
    return root;
}

/** Put the factory presets' folders under a scratch folder for the rest of the process, or, with an empty path, back
    to the default - which, while SetRootForTests has a root set, is none at all, so a suite never lists what an
    installer put on the machine it runs on.
    @param root  the scratch folder, or an empty path for the default */
inline void SetFactoryRootForTests(const std::filesystem::path &root)
{
    FactoryRootStorage() = root;
}

/** Where the factory presets' folders sit: the folder a test set, else none while a test has the user's root set, else
    the machine's (InstalledFactoryRoot).
    @return  the folder; empty for none */
inline std::filesystem::path FactoryRoot()
{
    if (!FactoryRootStorage().empty())
    {
        return FactoryRootStorage();
    }
    if (!RootStorage().empty())
    {
        return {};
    }
    return InstalledFactoryRoot();
}

/** The folder a plugin's factory presets live in, read only: FactoryRoot() with the plugin's folder name under it, as
    the user's are under Root().
    @param plugin  the plugin's product name, UTF-8
    @return        the folder; empty where there is no factory root */
inline std::filesystem::path FactoryFolderFor(std::string_view plugin)
{
    const std::filesystem::path root = FactoryRoot();
    return root.empty() ? root : root / PathOf(PluginFolderName(plugin));
}

/** Every preset file of one folder - not a directory, and not a hidden one - whose name ends in ".json", in the order
    the folder gives them.
    @param folder  the folder to read
    @return        the files' paths; empty when the folder is missing, unreadable or empty */
inline std::vector<std::filesystem::path> FilesIn(const std::filesystem::path &folder)
{
    std::vector<std::filesystem::path> files;
    if (folder.empty())
    {
        return files;
    }
    std::error_code                     error;
    std::filesystem::directory_iterator entry(folder, error);
    for (; !error && entry != std::filesystem::directory_iterator(); entry.increment(error))
    {
        const std::string name = TextOf(entry->path().filename());
        std::error_code   status;
        if (name.starts_with('.') || !HasPresetExtension(name) || entry->is_directory(status))
        {
            continue;
        }
        files.push_back(entry->path());
    }
    return files;
}

/** A plugin's presets: the preset files of the plugin's own folder (FilesIn) and of its factory folder
    (FactoryFolderFor) but where one of the user's has its name ignoring case, which takes the factory preset's place -
    so a factory preset saved under its own name is the user's copy from then on - all sorted by name ignoring case
    (CompareIgnoringCase), two the same that way by their bytes.
    @param plugin  the plugin's product name
    @return        the files' paths in order; empty when neither folder holds any */
inline std::vector<std::filesystem::path> List(std::string_view plugin)
{
    std::vector<std::filesystem::path> files = FilesIn(FolderFor(plugin));
    const std::size_t                  own   = files.size();
    for (const std::filesystem::path &factory : FilesIn(FactoryFolderFor(plugin)))
    {
        const std::string name = NameOf(factory);
        const auto        shadowed =
            std::any_of(files.begin(), files.begin() + static_cast<std::ptrdiff_t>(own),
                        [&](const auto &file) { return CompareIgnoringCase(NameOf(file), name) == 0; });
        if (!shadowed)
        {
            files.push_back(factory);
        }
    }
    std::sort(files.begin(), files.end(), [](const std::filesystem::path &a, const std::filesystem::path &b) {
        const std::string x     = NameOf(a);
        const std::string y     = NameOf(b);
        const int         order = CompareIgnoringCase(x, y);
        return order != 0 ? order < 0 : x < y;
    });
    return files;
}

/** What a save or a load came to: nothing, or why not. */
struct Result
{
    std::string error; ///< why not, in words a window can show; empty on success

    /** Whether it came to nothing wrong.
        @return  true when `error` is empty */
    bool IsOk() const noexcept
    {
        return error.empty();
    }
};

/** What a load read: the document and the preset's name (the file's), or why it read nothing. */
struct Loaded
{
    std::string error;    ///< why nothing was read, in words a window can show; empty on success
    json::Value document; ///< the JSON document the file holds
    std::string name;     ///< the preset's name, the file's (NameOf)

    /** Whether the file was read.
        @return  true when `error` is empty */
    bool IsOk() const noexcept
    {
        return error.empty();
    }
};

/** Writes `document` to `file` (WriteJsonFile), its folder made first; why not, where it could not be.
    @param file      the file to write
    @param document  the document to write
    @return          the result; its error says why the write failed */
inline Result SaveTo(const std::filesystem::path &file, const json::Value &document)
{
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
    {
        return {ErrorText(error)};
    }
    if (!WriteJsonFile(file, document))
    {
        return {"Could not write " + TextOf(file) + "."};
    }
    return {};
}

/** Saves `document` as the preset `name` of `plugin` in the plugin's own folder: the name trimmed and refused where a
    file cannot hold it (NameRefusal); `saved`, when given, receives the file.
    @param plugin    the plugin's product name
    @param name      the preset's name
    @param document  the document to write
    @param saved     where to put the file's path, or null; set once the name is accepted, whether or not the write
   works
    @return          the result; its error says why the save failed */
inline Result Save(std::string_view plugin, std::string_view name, const json::Value &document,
                   std::filesystem::path *saved = nullptr)
{
    const std::string trimmed = TrimmedText(name);
    if (std::string why = NameRefusal(trimmed); !why.empty())
    {
        return {std::move(why)};
    }
    const std::filesystem::path file = FileFor(FolderFor(plugin), trimmed);
    if (saved != nullptr)
    {
        *saved = file;
    }
    return SaveTo(file, document);
}

/** Reads `file` as a preset: the JSON document it holds and its name (NameOf), or why not - a file that is not there,
    or not JSON. Whether the document is a state of the plugin is the caller's to ask (HostSession::Refusal).
    @param file  the preset's file
    @return      what was read, or why not */
inline Loaded LoadFrom(const std::filesystem::path &file)
{
    std::error_code error;
    if (!std::filesystem::exists(file, error) || std::filesystem::is_directory(file, error))
    {
        return {"There is no file " + TextOf(file) + ".", {}, {}};
    }
    std::string                why;
    std::optional<json::Value> document = ReadJsonFile(file, &why);
    if (!document)
    {
        return {TextOf(file.filename()) + " is not a preset: " + why + ".", {}, {}};
    }
    return {{}, std::move(*document), NameOf(file)};
}

/** Whether the preset `name` of `plugin` is a factory preset alone: none of the user's files has its name ignoring case
    - which would take its place in the listing (List) - and the factory folder has one that does.
    @param plugin  the plugin's product name
    @param name    the preset's name
    @return        true when the only preset of that name is the factory's */
inline bool IsFactoryOnly(std::string_view plugin, std::string_view name)
{
    const std::string trimmed = TrimmedText(name);
    const auto        named   = [&](const std::filesystem::path &file) {
        return CompareIgnoringCase(NameOf(file), trimmed) == 0;
    };
    const std::vector<std::filesystem::path> own     = FilesIn(FolderFor(plugin));
    const std::vector<std::filesystem::path> factory = FilesIn(FactoryFolderFor(plugin));
    return std::none_of(own.begin(), own.end(), named) && std::any_of(factory.begin(), factory.end(), named);
}

/** Deletes `file`, as the plate's Delete does: true when it is gone, a file that was never there included.
    @param file  the file to delete
    @return      true when the file is gone */
inline bool Delete(const std::filesystem::path &file)
{
    std::error_code error;
    std::filesystem::remove(file, error);
    return !error;
}
} // namespace reckless::core::presets
