#pragma once

// What a Windows HID device interface's path says, read with none of Windows' headers so that the suites hold it on
// every platform. A path is "\\?\hid#vid_045e&pid_028e&ig_00#7&2f3a1b2&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}":
// the device's hardware id, its instance - which Windows makes from the port the device is plugged into, or from a
// wireless device's own address, and keeps across launches - and the interface class.

#include <cstddef>
#include <string>
#include <string_view>

namespace reckless::input::hidpath
{
/** A character in lower case, where it is an ASCII capital; any other as it is.
    @tparam Character  char or wchar_t
    @param character   the character
    @return            it in lower case */
template <typename Character> constexpr Character LowerAscii(Character character) noexcept
{
    return character >= Character('A') && character <= Character('Z')
               ? static_cast<Character>(character - Character('A') + Character('a'))
               : character;
}

/** Whether a HID path is one of an XInput controller's - "IG_" in its hardware id, in either case - which the XInput
    back end reads, so the HID back end passes it over rather than giving a person the one pad twice.
    @tparam Character  char or wchar_t
    @param path        the path
    @return            true for an XInput controller's */
template <typename Character> constexpr bool IsXInputPath(std::basic_string_view<Character> path) noexcept
{
    for (std::size_t at = 0; at + 3 <= path.size(); ++at)
    {
        if (LowerAscii(path[at]) == Character('i') && LowerAscii(path[at + 1]) == Character('g') &&
            path[at + 2] == Character('_'))
        {
            return true;
        }
    }
    return false;
}

/** A HID path's instance: what lies between its second and third '#', in lower case. Two devices of one model differ
    in it, and one device keeps it while it is plugged into the same port.
    @tparam Character  char or wchar_t
    @param path        the path
    @return            the instance as ASCII; empty for a path without one */
template <typename Character> std::string InstanceOf(std::basic_string_view<Character> path)
{
    const std::size_t first = path.find(Character('#'));
    if (first == std::basic_string_view<Character>::npos)
    {
        return {};
    }
    const std::size_t second = path.find(Character('#'), first + 1);
    if (second == std::basic_string_view<Character>::npos)
    {
        return {};
    }
    const std::size_t third = path.find(Character('#'), second + 1);
    const std::size_t end   = third == std::basic_string_view<Character>::npos ? path.size() : third;
    std::string       instance;
    for (std::size_t at = second + 1; at < end; ++at)
    {
        const Character character = LowerAscii(path[at]);
        instance += character >= Character(0x20) && character < Character(0x7F) ? static_cast<char>(character) : '?';
    }
    return instance;
}
} // namespace reckless::input::hidpath
