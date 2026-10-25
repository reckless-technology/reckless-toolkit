// The arithmetic of the suite's Windows MIDI back end (audio/WinMidiFormats.h), held on every platform: an input listed
// under RtMidi's name for it, the input a settings file's name means - exactly, with its index moved, or by a shorter
// name - and the bytes of a short message WinMM packs into one word, with what the cores do not read left out.
#include <cstdint>
#include <string>
#include <vector>

#include "WinMidiFormats.h"

#include <gtest/gtest.h>

namespace
{
namespace winmm = reckless::audio::winmm;
} // namespace

TEST(WinMidiFormats, AnInputIsListedUnderRtMidisNameForIt)
{
    EXPECT_EQ(winmm::ListedName("UMC1820 MIDI In", 0), "UMC1820 MIDI In 0");
    EXPECT_EQ(winmm::ListedName("Impact LX88+ MIDI1", 3), "Impact LX88+ MIDI1 3");
    EXPECT_EQ(winmm::WithoutIndex("UMC1820 MIDI In 0"), "UMC1820 MIDI In");
    EXPECT_EQ(winmm::WithoutIndex("MIDI 1 12"), "MIDI 1") << "the index alone, a number in the name kept";
    EXPECT_EQ(winmm::WithoutIndex("Keys"), "Keys") << "no index: the name itself";
    EXPECT_EQ(winmm::WithoutIndex("Keys A"), "Keys A");
}

TEST(WinMidiFormats, ASettingsFilesNameFindsItsInputThoughItsIndexMoved)
{
    const std::vector<std::string> listed = {"Pedal 0", "UMC1820 MIDI In 1", "Impact LX88+ MIDI1 2",
                                             "Impact LX88+ MIDI2 3"};
    EXPECT_EQ(winmm::Find(listed, "UMC1820 MIDI In 1"), 1) << "exactly";
    EXPECT_EQ(winmm::Find(listed, "UMC1820 MIDI In 0"), 1) << "the index moved: another device came first";
    EXPECT_EQ(winmm::Find(listed, "Impact LX88+ MIDI2 0"), 3);
    EXPECT_EQ(winmm::Find(listed, "Impact LX88+"), 2) << "a shorter name: the first that holds it, as RtMidi found it";
    EXPECT_EQ(winmm::Find(listed, "Gone Keys 4"), -1);
    EXPECT_EQ(winmm::Find(listed, ""), -1);
}

TEST(WinMidiFormats, AShortMessageCarriesTheBytesItsStatusAsks)
{
    std::uint8_t bytes[3] = {};
    // A note on, channel 2: status in the low byte, then the note and the velocity.
    ASSERT_EQ(winmm::ShortMessage(0x00643C91u, bytes), 3);
    EXPECT_EQ(bytes[0], 0x91);
    EXPECT_EQ(bytes[1], 0x3C);
    EXPECT_EQ(bytes[2], 0x64);
    EXPECT_EQ(winmm::ShortMessage(0x000005C0u, bytes), 2) << "a program change: two bytes";
    EXPECT_EQ(bytes[1], 0x05);
    EXPECT_EQ(winmm::ShortMessage(0x00007FD3u, bytes), 2) << "channel pressure: two bytes";
    EXPECT_EQ(winmm::ShortMessage(0x004000E0u, bytes), 3) << "a pitch bend: three";
    EXPECT_EQ(winmm::ShortMessage(0x002030F2u, bytes), 3) << "song position: three";
    EXPECT_EQ(winmm::ShortMessage(0x000003F3u, bytes), 2) << "song select: two";
    EXPECT_EQ(winmm::ShortMessage(0x000000FAu, bytes), 1) << "start: one";

    // Left out: timing clock, MIDI time code, active sensing, a system exclusive's bytes, and no status at all.
    for (const std::uint32_t packed : {0x000000F8u, 0x000012F1u, 0x000000FEu, 0x000000F0u, 0x000000F7u, 0x00003C40u})
    {
        EXPECT_EQ(winmm::ShortMessage(packed, bytes), 0) << std::hex << packed;
    }
}
