// The preset files' platform part (PresetFiles.h): where the user's documents, music and settings are, and the factory
// presets an installer put on the machine, each found by the platform's own call, which is why it is a source of its
// own: the header stays free of the platform's headers.
#include "PresetFiles.h"

#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

#if defined(__APPLE__)
#include <pwd.h>
#include <sysdir.h>
#include <unistd.h>
#elif defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// The shell's folders, after windows.h, which it needs.
#include <knownfolders.h>
#include <shlobj.h>
#else
#include <pwd.h>
#include <unistd.h>
#endif

namespace reckless::core
{
namespace
{
#if !defined(_WIN32)
/** The home directory: HOME, else the user's entry in the password database.
    @return  the home directory, or an empty path when neither names one */
std::filesystem::path Home()
{
    if (const char *home = std::getenv("HOME"); home != nullptr && *home != '\0')
    {
        return home;
    }
    if (const passwd *entry = getpwuid(getuid()); entry != nullptr && entry->pw_dir != nullptr)
    {
        return entry->pw_dir;
    }
    return {};
}
#endif

#if defined(__APPLE__)
/** The system's directory of `kind` in `domain`, its leading "~" the home directory.
    @param kind    which directory to ask the system for
    @param domain  whose: the user's, or the machine's (SYSDIR_DOMAIN_MASK_LOCAL)
    @return        the directory */
std::filesystem::path SystemDirectory(sysdir_search_path_directory_t   kind,
                                      sysdir_search_path_domain_mask_t domain = SYSDIR_DOMAIN_MASK_USER)
{
    char path[PATH_MAX] = {};
    sysdir_get_next_search_path_enumeration(sysdir_start_search_path_enumeration(kind, domain), path);
    const std::string_view text(path);
    if (text.starts_with("~/"))
    {
        return Home() / std::string(text.substr(2));
    }
    return text == "~" ? Home() : std::filesystem::path(std::string(text));
}
#elif defined(_WIN32)
/** The known folder `id`, or nothing.
    @param id  the shell's known-folder id
    @return    the folder, or an empty path when the shell has none */
std::filesystem::path KnownFolder(REFKNOWNFOLDERID id)
{
    PWSTR                 path = nullptr;
    std::filesystem::path folder;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) && path != nullptr)
    {
        folder = std::filesystem::path(std::wstring(path));
    }
    CoTaskMemFree(path);
    return folder;
}
#else
/** The directory the `type` line (XDG_DOCUMENTS_DIR, XDG_MUSIC_DIR) of ~/.config/user-dirs.dirs names, its "$HOME"
    the home directory and its quotes off, where it is a directory; else `fallback` under the home directory.
    @param type      the line's name, such as "XDG_DOCUMENTS_DIR"
    @param fallback  the directory's name under the home directory when the file names none
    @return          the directory */
std::filesystem::path XdgDirectory(std::string_view type, std::string_view fallback)
{
    const std::filesystem::path home  = Home();
    const std::string           text  = ReadBytes(home / ".config" / "user-dirs.dirs").value_or(std::string());
    std::size_t                 start = 0;
    while (start < text.size())
    {
        std::size_t end  = text.find('\n', start);
        end              = end == std::string::npos ? text.size() : end;
        std::string line = presets::TrimmedText(std::string_view(text).substr(start, end - start));
        start            = end + 1;
        if (!line.starts_with(type) || line.size() <= type.size() || line[type.size()] != '=')
        {
            continue;
        }
        std::string value = line.substr(type.size() + 1);
        if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
        {
            value = value.substr(1, value.size() - 2);
        }
        if (value.starts_with("$HOME"))
        {
            value = home.native() + value.substr(5);
        }
        std::error_code error;
        if (!value.empty() && std::filesystem::is_directory(value, error))
        {
            return value;
        }
    }
    return home / std::string(fallback);
}
#endif
} // namespace

std::filesystem::path presets::DocumentsDirectory()
{
#if defined(__APPLE__)
    return SystemDirectory(SYSDIR_DIRECTORY_DOCUMENT);
#elif defined(_WIN32)
    return KnownFolder(FOLDERID_Documents);
#else
    return XdgDirectory("XDG_DOCUMENTS_DIR", "Documents");
#endif
}

std::filesystem::path presets::MusicDirectory()
{
#if defined(__APPLE__)
    return SystemDirectory(SYSDIR_DIRECTORY_MUSIC);
#elif defined(_WIN32)
    return KnownFolder(FOLDERID_Music);
#else
    return XdgDirectory("XDG_MUSIC_DIR", "Music");
#endif
}

std::filesystem::path UserSettingsFolder(std::string_view folder_name)
{
    const std::filesystem::path folder = presets::PathOf(folder_name);
#if defined(__APPLE__)
    return SystemDirectory(SYSDIR_DIRECTORY_APPLICATION_SUPPORT) / folder;
#elif defined(_WIN32)
    const std::filesystem::path roaming = KnownFolder(FOLDERID_RoamingAppData);
    return roaming.empty() ? roaming : roaming / folder;
#else
    if (const char *config = std::getenv("XDG_CONFIG_HOME"); config != nullptr && *config == '/')
    {
        return std::filesystem::path(config) / folder;
    }
    return Home() / ".config" / folder;
#endif
}

std::filesystem::path MachineSettingsFolder(std::string_view folder_name)
{
    const std::filesystem::path folder = presets::PathOf(folder_name);
#if defined(__APPLE__)
    const std::filesystem::path support =
        SystemDirectory(SYSDIR_DIRECTORY_APPLICATION_SUPPORT, SYSDIR_DOMAIN_MASK_LOCAL);
    return support.empty() ? support : support / folder;
#elif defined(_WIN32)
    const std::filesystem::path data = KnownFolder(FOLDERID_ProgramData);
    return data.empty() ? data : data / folder;
#else
    if (const char *dirs = std::getenv("XDG_CONFIG_DIRS"); dirs != nullptr)
    {
        const std::string_view list(dirs);
        const std::string_view first = list.substr(0, list.find(':'));
        if (first.starts_with('/'))
        {
            return std::filesystem::path(std::string(first)) / folder;
        }
    }
    return std::filesystem::path("/etc/xdg") / folder;
#endif
}

std::filesystem::path presets::InstalledFactoryRoot()
{
#if defined(__APPLE__) || defined(_WIN32)
    const std::filesystem::path machine = MachineSettingsFolder(kSuiteFolder);
    return machine.empty() ? machine : machine / "presets";
#else
    const std::filesystem::path tail = std::filesystem::path(std::string(kSuiteFolder)) / "presets";
    const char                 *dirs = std::getenv("XDG_DATA_DIRS");
    const std::string_view      list = dirs != nullptr && *dirs != '\0' ? dirs : "/usr/local/share:/usr/share";
    for (std::size_t start = 0; start <= list.size();)
    {
        const std::size_t      end = std::min(list.find(':', start), list.size());
        const std::string_view dir = list.substr(start, end - start);
        start                      = end + 1;
        std::error_code error;
        if (dir.starts_with('/') &&
            std::filesystem::is_directory(std::filesystem::path(std::string(dir)) / tail, error))
        {
            return std::filesystem::path(std::string(dir)) / tail;
        }
    }
    return std::filesystem::path("/usr/share") / tail;
#endif
}

std::string presets::ErrorText(const std::error_code &error)
{
#if defined(_WIN32)
    if (error.category() == std::system_category())
    {
        LPWSTR      buffer = nullptr;
        const DWORD length = FormatMessageW(
            FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
            static_cast<DWORD>(error.value()), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
        std::wstring text = length > 0 && buffer != nullptr ? std::wstring(buffer, length) : std::wstring();
        if (buffer != nullptr)
        {
            LocalFree(buffer);
        }
        while (!text.empty() && (text.back() == L'\r' || text.back() == L'\n' || text.back() == L' '))
        {
            text.pop_back(); // the line break the system ends its messages with
        }
        if (!text.empty())
        {
            return TextOf(std::filesystem::path(text));
        }
        return "Windows error " + std::to_string(error.value());
    }
#endif
    return error.message();
}
} // namespace reckless::core
