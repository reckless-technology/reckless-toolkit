#pragma once

// Test-only, macOS: the libraries this program was linked against, as `otool -L` reads its load commands - what a
// suite asks to hold that a binary does or does not link a framework. Not the libraries loaded: AppKit loads Metal
// into every process that makes a window, so a binary that never names Metal has it loaded all the same, and a check
// of what is loaded would prove nothing of what the binary carries.
#include <mach-o/dyld.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace reckless::test
{
/** `otool -L` of this program's own executable: its path, then each library it names, one a line.
    @return  the listing; empty where otool could not be run or read nothing */
inline std::string LinkedLibrariesOfThisProgram()
{
    std::uint32_t size = 0;
    _NSGetExecutablePath(nullptr, &size);
    std::vector<char> path(size + 1, '\0');
    if (_NSGetExecutablePath(path.data(), &size) != 0)
    {
        return {};
    }
    const std::string command = std::string("/usr/bin/otool -L '") + path.data() + "' 2>/dev/null";
    FILE             *otool   = popen(command.c_str(), "r");
    if (otool == nullptr)
    {
        return {};
    }
    std::string listing;
    char        line[1024];
    while (std::fgets(line, sizeof(line), otool) != nullptr)
    {
        listing += line;
    }
    return pclose(otool) == 0 ? listing : std::string();
}
} // namespace reckless::test
