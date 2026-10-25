#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace reckless::audio::coremidi
{
/**
    The arithmetic of the suite's CoreMIDI back end (mac/CoreMidi.cpp) that needs no Mac to hold: a source's name as
    RtMidi composed it from its endpoint, entity and device, and the MIDI 1.0 messages in the Universal MIDI Packets
    CoreMIDI hands a port over. Header only and platform-neutral, so the audio suite holds it on every platform
    (tests/test_core_midi_formats.cpp).
*/

/** What an endpoint's name is composed of: its own name, and its entity's and device's where it has them. */
struct EndpointParts
{
    std::string endpoint;            ///< the endpoint's own name
    bool        has_entity{false};   ///< whether the endpoint belongs to an entity (a virtual one does not)
    std::string entity;              ///< the entity's name
    bool        has_device{false};   ///< whether the entity belongs to a device
    bool        device_named{false}; ///< whether the device has a name at all
    std::string device;              ///< the device's name
    int         device_entities{0};  ///< how many entities the device has
};

/** An endpoint's name as RtMidi composed it (its CreateEndpointName): the endpoint's name trimmed of spaces; for one
    with no entity - a virtual source - that alone; else the entity's name where the endpoint's is empty; then, where
   the entity belongs to a device, the device's name alone where nothing else was found or where an external device has
    fewer than two entities, and otherwise the device's name before it, unless it already begins so.
    @param parts        what the endpoint's name is composed of
    @param is_external  whether the endpoint is an external device's (one a source is connected to)
    @return             the name */
inline std::string EndpointName(const EndpointParts &parts, bool is_external)
{
    std::string result = parts.endpoint;
    const auto  first  = result.find_first_not_of(' ');
    result             = first == std::string::npos ? std::string() : result.substr(first);
    const auto last    = result.find_last_not_of(' ');
    result             = last == std::string::npos ? std::string() : result.substr(0, last + 1);
    if (!parts.has_entity)
    {
        return result;
    }
    if (result.empty())
    {
        result = parts.entity;
    }
    if (!parts.has_device)
    {
        return result;
    }
    if (result.empty())
    {
        return parts.device;
    }
    if (!parts.device_named)
    {
        return result;
    }
    if (is_external && parts.device_entities < 2)
    {
        return parts.device;
    }
    if (parts.device.empty())
    {
        return result;
    }
    if (result.compare(0, parts.device.size(), parts.device) != 0)
    {
        result = parts.device + " " + result;
    }
    return result;
}

/** The names of the external endpoints a source is connected to, as RtMidi joined them (its
    CreateConnectedEndpointName): ", " between; empty where there are none, and the source is then named by its own
    endpoint (EndpointName).
    @param names  the connected endpoints' names, empty ones skipped
    @return       the joined name */
inline std::string JoinConnected(const std::vector<std::string> &names)
{
    std::string joined;
    for (const std::string &name : names)
    {
        if (name.empty())
        {
            continue;
        }
        joined += joined.empty() ? name : ", " + name;
    }
    return joined;
}

/** The words a Universal MIDI Packet takes, from its first word's message type.
    @param first  the packet's first word
    @return       1 to 4 */
constexpr int UmpWords(std::uint32_t first) noexcept
{
    constexpr int kWords[16] = {1, 1, 1, 2, 2, 4, 1, 1, 2, 2, 2, 3, 3, 4, 4, 4};
    return kWords[first >> 28];
}

/** The MIDI 1.0 message one Universal MIDI Packet carries, as its bytes: a channel voice message (type 2) or a system
    common or real-time one (type 1); none for what the cores do not read - timing clock, MIDI time code and active
    sensing, which RtMidi was told to leave out, and system exclusive (type 3) - nor for any other type.
    @param first  the packet's first word
    @param bytes  set to the message's bytes, the status first
    @return       the message's length, 1 to 3; 0 for a packet left out */
inline int Midi1Message(std::uint32_t first, std::uint8_t bytes[3]) noexcept
{
    const unsigned type   = first >> 28;
    const auto     status = static_cast<std::uint8_t>((first >> 16) & 0xFF);
    int            length = 0;
    if (type == 2 && status >= 0x80 && status < 0xF0)
    {
        length = status < 0xC0 ? 3 : (status < 0xE0 ? 2 : 3);
    }
    else if (type == 1 && status >= 0xF0)
    {
        if (status == 0xF1 || status == 0xF8 || status == 0xFE || status == 0xF0 || status == 0xF7)
        {
            return 0;
        }
        length = status == 0xF2 ? 3 : (status == 0xF3 ? 2 : 1);
    }
    else
    {
        return 0;
    }
    bytes[0] = status;
    bytes[1] = static_cast<std::uint8_t>((first >> 8) & 0x7F);
    bytes[2] = static_cast<std::uint8_t>(first & 0x7F);
    return length;
}
} // namespace reckless::audio::coremidi
