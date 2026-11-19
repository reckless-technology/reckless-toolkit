#pragma once

#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

#include "Utf8.h"

namespace reckless::audio
{
/**
    The names a device's channels carry (DeviceInfo::input_channel_names, output_channel_names), and the arithmetic that
    makes them from what a back end reads, held on every platform so the audio suite checks it where no device is
    (tests/test_channel_names.cpp). A name is only ever what the API or the driver said; where it said nothing the name
    is empty, and a back end never writes "Input 3" for it - a product does that itself.
*/

/** A name read from an API, cleaned: control characters and surrounding white space removed, and nothing at all where
    what was read is not well-formed UTF-8 (a driver's text in a code page this layer was not told is not guessed at).
    @param raw  the text as the API gave it
    @return     the name, UTF-8; empty where there is none worth showing */
inline std::string CleanChannelName(std::string_view raw)
{
    std::string text;
    for (const char c : raw)
    {
        if (c == '\0')
        {
            break; // a fixed-size buffer's text ends at its first NUL
        }
        text.push_back(static_cast<unsigned char>(c) < 0x20 || c == 0x7f ? ' ' : c);
    }
    const std::size_t first = text.find_first_not_of(' ');
    if (first == std::string::npos)
    {
        return {};
    }
    const std::size_t last = text.find_last_not_of(' ');
    text                   = text.substr(first, last - first + 1);
    return core::utf8::IsWellFormed(text) ? text : std::string();
}

/** A back end's names for one way's channels, as DeviceInfo holds them: exactly `channels` of them, one per channel in
    the device's order, each cleaned (CleanChannelName), and an empty list where no channel has a name at all.
    @param names     what the back end read, in channel order; fewer than `channels` is padded with empty names, more
                     is cut
    @param channels  the way's channel count
    @return          the names, empty or `channels` long */
inline std::vector<std::string> ChannelNamesFor(const std::vector<std::string> &names, int channels)
{
    std::vector<std::string> result;
    bool                     is_any_named = false;
    for (int c = 0; c < channels; ++c)
    {
        const auto  i    = static_cast<std::size_t>(c);
        std::string name = i < names.size() ? CleanChannelName(names[i]) : std::string();
        is_any_named     = is_any_named || !name.empty();
        result.push_back(std::move(name));
    }
    return is_any_named ? result : std::vector<std::string>();
}

/** A JACK port's own name, without its client: "capture_1" for "system:capture_1".
    @param port  the port's full name, "client:port"
    @return      what follows the first colon; empty where there is no colon or nothing after it */
inline std::string PortNameWithoutClient(std::string_view port)
{
    const std::size_t colon = port.find(':');
    return colon == std::string_view::npos ? std::string() : std::string(port.substr(colon + 1));
}

/** The names of channels that are JACK ports: each port's own name, in the order the device layer connects them.
    @param ports  the ports' full names, "client:port", in channel order
    @return       the names as DeviceInfo holds them (ChannelNamesFor) */
inline std::vector<std::string> ChannelNamesOfPorts(const std::vector<std::string> &ports)
{
    std::vector<std::string> names;
    names.reserve(ports.size());
    for (const std::string &port : ports)
    {
        names.push_back(PortNameWithoutClient(port));
    }
    return ChannelNamesFor(names, static_cast<int>(ports.size()));
}
} // namespace reckless::audio
