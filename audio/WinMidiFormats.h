#pragma once

#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

namespace reckless::audio::winmm
{
/**
    The arithmetic of the suite's Windows MIDI back end (win/WinMidi.cpp) that needs no Windows to hold: an input's name
    as RtMidi listed it, finding the input a settings file's name means, and the bytes of a short message WinMM hands
    over packed in one word. Header only and platform-neutral, so the audio suite holds it on every platform
    (tests/test_win_midi_formats.cpp).
*/

/** An input's name as RtMidi listed it, so a settings file written then still finds it: the device's own name with its
    index after it - "UMC1820 MIDI In 0" - which keeps two devices of one name apart.
    @param device  the device's name, in UTF-8
    @param index   its index among WinMM's inputs
    @return        the listed name */
inline std::string ListedName(const std::string &device, unsigned int index)
{
    return device + " " + std::to_string(index);
}

/** A listed name with its index set aside: "UMC1820 MIDI In 0" as "UMC1820 MIDI In".
    @param name  a listed name
    @return      the name without its trailing " <index>"; the name itself where it has none */
inline std::string WithoutIndex(const std::string &name)
{
    const std::size_t space = name.rfind(' ');
    if (space == std::string::npos || space + 1 == name.size())
    {
        return name;
    }
    for (std::size_t i = space + 1; i < name.size(); ++i)
    {
        if (std::isdigit(static_cast<unsigned char>(name[i])) == 0)
        {
            return name;
        }
    }
    return name.substr(0, space);
}

/** The input a settings file's `name` means: that name exactly; else the first whose name is the same with the index
   set aside, a device whose index moved when another came or went; else the first whose name holds it, as RtMidi found
   a shorter name.
    @param listed  the inputs' listed names, in WinMM's order
    @param name    the name kept
    @return        the input's place in `listed`, or -1 for none */
inline int Find(const std::vector<std::string> &listed, const std::string &name)
{
    for (std::size_t i = 0; i < listed.size(); ++i)
    {
        if (listed[i] == name)
        {
            return static_cast<int>(i);
        }
    }
    const std::string bare = WithoutIndex(name);
    for (std::size_t i = 0; i < listed.size(); ++i)
    {
        if (WithoutIndex(listed[i]) == bare)
        {
            return static_cast<int>(i);
        }
    }
    for (std::size_t i = 0; i < listed.size(); ++i)
    {
        if (!name.empty() && listed[i].find(name) != std::string::npos)
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/** The bytes of the short message a WinMM input packs into one word, its status in the low byte: as many as its status
    asks, or none for what the cores do not read - timing clock, MIDI time code and active sensing, which RtMidi was
   told to leave out, and a system exclusive's bytes, which come whole by another message - and for a word that starts
   with no status.
    @param packed  the message, as WinMM's MIM_DATA hands it over
    @param bytes   set to the message's bytes, the status first
    @return        the message's length, 1 to 3; 0 for a message left out */
inline int ShortMessage(std::uint32_t packed, std::uint8_t bytes[3]) noexcept
{
    const auto status = static_cast<std::uint8_t>(packed & 0xFF);
    if ((status & 0x80) == 0 || status == 0xF0 || status == 0xF7 || status == 0xF1 || status == 0xF8 || status == 0xFE)
    {
        return 0;
    }
    int length = 1;
    if (status < 0xC0)
    {
        length = 3;
    }
    else if (status < 0xE0)
    {
        length = 2;
    }
    else if (status < 0xF0)
    {
        length = 3;
    }
    else if (status == 0xF2)
    {
        length = 3;
    }
    else if (status == 0xF3)
    {
        length = 2;
    }
    for (int i = 0; i < length; ++i)
    {
        bytes[i] = static_cast<std::uint8_t>((packed >> (8 * i)) & 0xFF);
    }
    return length;
}
} // namespace reckless::audio::winmm
