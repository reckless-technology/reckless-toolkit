// The names a device's channels carry (audio/ChannelNames.h, DeviceInfo::input_channel_names): what is made of the text
// a back end reads, held on every platform, and what the device layer's simulated device does with them. What the real
// APIs name is the device suite's (test_audio_devices.cpp), which needs the devices.
#include <string>
#include <vector>

#include "AudioDevice.h"
#include "ChannelNames.h"
#include "OfflineDevice.h"

#include <gtest/gtest.h>

namespace
{
using reckless::audio::ChannelNamesFor;
using reckless::audio::ChannelNamesOfPorts;
using reckless::audio::CleanChannelName;
using reckless::audio::DeviceInfo;
using reckless::audio::PortNameWithoutClient;
} // namespace

TEST(ChannelNames, ATextIsTrimmedAndStopsAtItsFirstNul)
{
    EXPECT_EQ(CleanChannelName("Mic/Line 3"), "Mic/Line 3");
    EXPECT_EQ(CleanChannelName("  Analog 1 \t"), "Analog 1");
    EXPECT_EQ(CleanChannelName(std::string("In 1\0garbage", 12)), "In 1") << "a fixed buffer's text ends at its NUL";
    EXPECT_EQ(CleanChannelName("Line\n3"), "Line 3") << "a control character is a space, not part of a name";
    EXPECT_EQ(CleanChannelName(""), "");
    EXPECT_EQ(CleanChannelName("   "), "");
    EXPECT_EQ(CleanChannelName("Entr\xc3\xa9"
                               "e 1"),
              "Entr\xc3\xa9"
              "e 1")
        << "UTF-8 is kept as it is";
}

TEST(ChannelNames, TextThatIsNotUtf8IsDroppedNotGuessedAt)
{
    EXPECT_EQ(CleanChannelName("Entr\xe9"
                               "e 1"),
              "")
        << "Latin-1 read as UTF-8 is not a name";
    EXPECT_EQ(CleanChannelName("\xff\xfe"), "");
}

TEST(ChannelNames, AListIsEmptyOrOneNamePerChannel)
{
    EXPECT_TRUE(ChannelNamesFor({}, 4).empty()) << "nothing read: nothing said";
    EXPECT_TRUE(ChannelNamesFor({"", " ", ""}, 3).empty()) << "every name empty: no list at all";
    EXPECT_TRUE(ChannelNamesFor({"Mic 1"}, 0).empty()) << "no channels, no names";

    const std::vector<std::string> some = ChannelNamesFor({"Mic 1", "", "Line 3"}, 3);
    ASSERT_EQ(some.size(), 3u);
    EXPECT_EQ(some[0], "Mic 1");
    EXPECT_EQ(some[1], "") << "a channel with no name stays unnamed, and is not given one";
    EXPECT_EQ(some[2], "Line 3");

    const std::vector<std::string> padded = ChannelNamesFor({"A"}, 3);
    EXPECT_EQ(padded, (std::vector<std::string>{"A", "", ""})) << "fewer names than channels: the rest unnamed";
    const std::vector<std::string> cut = ChannelNamesFor({"A", "B", "C"}, 2);
    EXPECT_EQ(cut, (std::vector<std::string>{"A", "B"})) << "more names than channels: cut to the channels";
}

TEST(ChannelNames, AJackPortIsNamedByItsOwnNameWithoutItsClient)
{
    EXPECT_EQ(PortNameWithoutClient("system:capture_1"), "capture_1");
    EXPECT_EQ(PortNameWithoutClient("UMC1820 Pro:capture_AUX0"), "capture_AUX0");
    EXPECT_EQ(PortNameWithoutClient("a:b:c"), "b:c") << "the first colon ends the client";
    EXPECT_EQ(PortNameWithoutClient("nocolon"), "");
    EXPECT_EQ(PortNameWithoutClient("client:"), "");

    EXPECT_EQ(ChannelNamesOfPorts({"system:capture_1", "system:capture_2"}),
              (std::vector<std::string>{"capture_1", "capture_2"}));
    EXPECT_TRUE(ChannelNamesOfPorts({}).empty());
    EXPECT_TRUE(ChannelNamesOfPorts({"nocolon"}).empty());
}

TEST(ChannelNames, ADeviceAnswersPerChannelAndNeverInventsOne)
{
    DeviceInfo device;
    device.input_channels      = 3;
    device.output_channels     = 2;
    device.input_channel_names = {"Mic/Line 1", "", "Mic/Line 3"};
    EXPECT_EQ(device.InputChannelName(0), "Mic/Line 1");
    EXPECT_EQ(device.InputChannelName(1), "");
    EXPECT_EQ(device.InputChannelName(2), "Mic/Line 3");
    EXPECT_EQ(device.InputChannelName(3), "") << "past the channels";
    EXPECT_EQ(device.InputChannelName(-1), "");
    EXPECT_EQ(device.OutputChannelName(0), "") << "a back end that cannot tell leaves the list empty";
    EXPECT_EQ(device.OutputChannelName(1), "");
}

TEST(ChannelNames, ADeviceMadeWithoutNamesHasNone)
{
    const DeviceInfo device;
    EXPECT_TRUE(device.input_channel_names.empty());
    EXPECT_TRUE(device.output_channel_names.empty());
}

TEST(ChannelNames, TheOfflineSystemOffersADevicesNamesAsTheyAre)
{
    DeviceInfo named;
    named.name                 = "Interface";
    named.input_channels       = 2;
    named.output_channels      = 2;
    named.input_channel_names  = {"Mic 1", "Mic 2"};
    named.output_channel_names = {};
    DeviceInfo plain;
    plain.name            = "Plain";
    plain.input_channels  = 2;
    plain.output_channels = 2;

    reckless::audio::OfflineAudioSystem system({named, plain});
    const std::vector<DeviceInfo>       devices = system.Devices();
    ASSERT_EQ(devices.size(), 2u);
    EXPECT_EQ(devices[0].input_channel_names, named.input_channel_names);
    EXPECT_TRUE(devices[0].output_channel_names.empty());
    EXPECT_TRUE(devices[1].input_channel_names.empty());

    // The device made from counts alone names nothing.
    reckless::audio::OfflineAudioSystem counted(2, 2);
    EXPECT_TRUE(counted.Devices()[0].input_channel_names.empty());
}
