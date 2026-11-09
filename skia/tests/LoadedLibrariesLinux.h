#pragma once

// Test-only, Linux: the shared libraries loaded in this process, from the dynamic linker's own list (dl_iterate_phdr),
// for a suite to hold that a window which links no GPU back end loads no EGL or GL - and, as the control that the read
// can see one, that the GPU's own suite does. Every library a program is linked against is loaded as it starts, so
// what is not in the list is not linked either.
#include <link.h>

#include <cstddef>
#include <string>

namespace reckless::test
{
/** The path of every shared object loaded in this process, one a line: the program's own (an empty name), the
    libraries it was linked against and any loaded since.
    @return  the paths, each followed by a newline */
inline std::string LoadedLibrariesOfThisProcess()
{
    std::string paths;
    dl_iterate_phdr(
        [](dl_phdr_info *info, std::size_t, void *data) {
            auto &into = *static_cast<std::string *>(data);
            into += info->dlpi_name != nullptr ? info->dlpi_name : "";
            into += '\n';
            return 0;
        },
        &paths);
    return paths;
}

/** Whether a library named `file_name` (a file name such as "libEGL.so", matched whole, before any version suffix) is
    among `paths`.
    @param paths      the list LoadedLibrariesOfThisProcess gave
    @param file_name  the library's file name, without directory or version
    @return           true when it is loaded */
inline bool NamesLibrary(const std::string &paths, const std::string &file_name)
{
    for (std::size_t at = paths.find(file_name); at != std::string::npos; at = paths.find(file_name, at + 1))
    {
        const bool is_start = at == 0 || paths[at - 1] == '/' || paths[at - 1] == '\n';
        const char after    = at + file_name.size() < paths.size() ? paths[at + file_name.size()] : '\n';
        if (is_start && (after == '\n' || after == '.'))
        {
            return true;
        }
    }
    return false;
}
} // namespace reckless::test
