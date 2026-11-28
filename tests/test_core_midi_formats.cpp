// The arithmetic of the suite's CoreMIDI back end (audio/CoreMidiFormats.h), held on every platform: a source's name as
// RtMidi composed it from its endpoint, entity and device and the external endpoints it is connected to, and the MIDI
// 1.0 messages among the Universal MIDI Packets CoreMIDI hands a port, with what the cores do not read left out; and
// for output, the packet a message goes out as and the host time it is stamped with.
#include <cstdint>
#include <string>
#include <vector>

#include "CoreMidiFormats.h"

#include <gtest/gtest.h>

namespace
{
namespace coremidi = reckless::audio::coremidi;

coremidi::EndpointParts Parts(std::string endpoint, std::string entity, std::string device, int entities)
{
    coremidi::EndpointParts parts;
    parts.endpoint        = std::move(endpoint);
    parts.has_entity      = true;
    parts.entity          = std::move(entity);
    parts.has_device      = true;
    parts.device_named    = true;
    parts.device          = std::move(device);
    parts.device_entities = entities;
    return parts;
}
} // namespace

TEST(CoreMidiFormats, ASourceIsNamedAsRtMidiNamedIt)
{
    // A virtual source: its endpoint's name alone, trimmed of spaces.
    coremidi::EndpointParts virtual_source;
    virtual_source.endpoint = "  Reckless test source ";
    EXPECT_EQ(coremidi::EndpointName(virtual_source, false), "Reckless test source");

    // A USB interface's port: the device's name before the port's.
    EXPECT_EQ(coremidi::EndpointName(Parts("MIDI 1", "UMC1820", "UMC1820", 1), false), "UMC1820 MIDI 1");
    // A port already named after its device is not named twice.
    EXPECT_EQ(coremidi::EndpointName(Parts("Impact LX88+ MIDI1", "Impact LX88+", "Impact LX88+", 2), false),
              "Impact LX88+ MIDI1");
    // A port with no name of its own: its entity's, with the device's before it.
    EXPECT_EQ(coremidi::EndpointName(Parts("", "Port 2", "Hub", 4), false), "Hub Port 2");
    // Nothing but the device's: the device's alone.
    EXPECT_EQ(coremidi::EndpointName(Parts("", "", "Keys", 1), false), "Keys");
    // An external device of one entity, as a connection names it: the device alone.
    EXPECT_EQ(coremidi::EndpointName(Parts("Out", "Out", "Synth", 1), true), "Synth");
    EXPECT_EQ(coremidi::EndpointName(Parts("Out", "Out", "Synth", 2), true), "Synth Out") << "two entities: both";

    // Connected sources are named by what they are connected to, joined; none, and the endpoint names itself.
    EXPECT_EQ(coremidi::JoinConnected({"Synth", "", "Drum Machine"}), "Synth, Drum Machine");
    EXPECT_EQ(coremidi::JoinConnected({}), "");
}

TEST(CoreMidiFormats, AUniversalPacketCarriesItsMidi1Message)
{
    std::uint8_t bytes[3] = {};
    ASSERT_EQ(coremidi::Midi1Message(0x20913C64u, bytes), 3) << "a note on, channel 2";
    EXPECT_EQ(bytes[0], 0x91);
    EXPECT_EQ(bytes[1], 0x3C);
    EXPECT_EQ(bytes[2], 0x64);
    EXPECT_EQ(coremidi::Midi1Message(0x23C50500u, bytes), 2) << "a program change, any group";
    EXPECT_EQ(bytes[1], 0x05);
    EXPECT_EQ(coremidi::Midi1Message(0x20EF0040u, bytes), 3) << "a pitch bend";
    EXPECT_EQ(coremidi::Midi1Message(0x10F22030u, bytes), 3) << "song position";
    EXPECT_EQ(coremidi::Midi1Message(0x10FA0000u, bytes), 1) << "start";

    // Left out: timing clock, MIDI time code, active sensing, system exclusive, and MIDI 2.0's messages.
    for (const std::uint32_t first : {0x10F80000u, 0x10F11200u, 0x10FE0000u, 0x30037D01u, 0x40903C00u})
    {
        EXPECT_EQ(coremidi::Midi1Message(first, bytes), 0) << std::hex << first;
    }

    // Each packet's length, from its type: one word for types 0 to 2, two for system exclusive, four for type 5.
    EXPECT_EQ(coremidi::UmpWords(0x20913C64u), 1);
    EXPECT_EQ(coremidi::UmpWords(0x10F80000u), 1);
    EXPECT_EQ(coremidi::UmpWords(0x30037D01u), 2);
    EXPECT_EQ(coremidi::UmpWords(0x40903C00u), 2);
    EXPECT_EQ(coremidi::UmpWords(0x50000000u), 4);
}

TEST(CoreMidiFormats, AMessageSentGoesOutAsTheUniversalPacketThatCarriesIt)
{
    const std::uint8_t note_on[] = {0x93, 60, 100};
    const std::uint8_t program[] = {0xC1, 5};
    const std::uint8_t bend[]    = {0xEF, 0x00, 0x40};
    const std::uint8_t clock[]   = {0xF8};
    const std::uint8_t stop[]    = {0xFC};
    EXPECT_EQ(coremidi::Midi1Packet(note_on, 3), 0x20933C64U);
    EXPECT_EQ(coremidi::Midi1Packet(program, 2), 0x20C10500U);
    EXPECT_EQ(coremidi::Midi1Packet(bend, 3), 0x20EF0040U);
    EXPECT_EQ(coremidi::Midi1Packet(clock, 1), 0x10F80000U) << "a system message is type 1";
    EXPECT_EQ(coremidi::Midi1Packet(stop, 1), 0x10FC0000U);
    // And read back as the input side reads a packet, the same bytes.
    std::uint8_t bytes[3] = {};
    ASSERT_EQ(coremidi::Midi1Message(coremidi::Midi1Packet(note_on, 3), bytes), 3);
    EXPECT_EQ(bytes[0], 0x93);
    EXPECT_EQ(bytes[1], 60);
    EXPECT_EQ(bytes[2], 100);
    ASSERT_EQ(coremidi::Midi1Message(coremidi::Midi1Packet(program, 2), bytes), 2);
    EXPECT_EQ(bytes[0], 0xC1);
    EXPECT_EQ(bytes[1], 5);
}

TEST(CoreMidiFormats, AMessagesTimeIsStampedInHostTicks)
{
    // An Intel Mac's timebase is 1/1 (a tick a nanosecond); Apple silicon's 125/3 (24 MHz ticks).
    EXPECT_EQ(coremidi::HostTimeAt(1'005'000, 1'000'000, 777, 1, 1), 777U + 5'000U);
    EXPECT_EQ(coremidi::HostTimeAt(1'000'000 + 20'000'000, 1'000'000, 1'000'000'000, 125, 3), 1'000'000'000U + 480'000U)
        << "20 ms ahead is 480,000 ticks at 24 MHz";
    EXPECT_EQ(coremidi::HostTimeAt(1'000'000 + 1'000, 1'000'000, 50, 125, 3), 50U + 24U) << "1 us, rounded down";
    EXPECT_EQ(coremidi::HostTimeAt(999'999, 1'000'000, 4242, 125, 3), 4242U) << "a time past: at once";
    EXPECT_EQ(coremidi::HostTimeAt(1'000'000, 1'000'000, 4242, 125, 3), 4242U) << "now: at once";
    EXPECT_EQ(coremidi::HostTimeAt(0x7FFFFFFFFFFF, 0, 0, 125, 3),
              0x7FFFFFFFFFFFULL / 125 * 3 + 0x7FFFFFFFFFFFULL % 125 * 3 / 125)
        << "far ahead without overflowing";
}
