#pragma once

#include <string>
#include <vector>

namespace reckless::window
{
/** A hexadecimal digit's value, or -1 for a character that is none.
    @param c  the character, 0-9, a-f or A-F for a digit
    @return   0 to 15, or -1 */
inline int HexDigitValue(char c) noexcept
{
    if (c >= '0' && c <= '9')
    {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f')
    {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F')
    {
        return c - 'A' + 10;
    }
    return -1;
}

/** The file paths in a text/uri-list - what an X11 drag source hands a drop target (XDND) - one URI a line, comments
    starting '#', a file URI's path percent-decoded. A '%' not followed by two hexadecimal digits is taken as itself, as
    a browser takes one: some drag sources send a path whose '%' they never escaped ("100%_wet.wav"), and the text is
    another program's, so nothing in it may stop the window that reads it.
    @param list  the drag source's text/uri-list, lines ended by LF or CRLF
    @return      the decoded paths of the file URIs, in order; a line that is no file URI is skipped */
inline std::vector<std::string> PathsOfUriList(const std::string &list)
{
    std::vector<std::string> paths;
    std::size_t              start = 0;
    while (start < list.size())
    {
        std::size_t end = list.find('\n', start);
        if (end == std::string::npos)
        {
            end = list.size();
        }
        std::string line = list.substr(start, end - start);
        start            = end + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
        {
            line.pop_back();
        }
        if (line.empty() || line[0] == '#' || line.rfind("file://", 0) != 0)
        {
            continue;
        }
        const std::size_t slash = line.find('/', 7); // past the host, which is empty or this machine
        if (slash == std::string::npos)
        {
            continue;
        }
        std::string path;
        for (std::size_t i = slash; i < line.size(); ++i)
        {
            const int high = line[i] == '%' && i + 2 < line.size() ? HexDigitValue(line[i + 1]) : -1;
            const int low  = high >= 0 ? HexDigitValue(line[i + 2]) : -1;
            if (low >= 0)
            {
                path.push_back(static_cast<char>(high * 16 + low));
                i += 2;
            }
            else
            {
                path.push_back(line[i]);
            }
        }
        paths.push_back(path);
    }
    return paths;
}
} // namespace reckless::window
