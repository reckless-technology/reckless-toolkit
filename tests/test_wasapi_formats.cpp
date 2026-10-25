// The arithmetic of the suite's WASAPI back end (audio/WasapiFormats.h, and the encodings it shares with ALSA's,
// audio/SampleEncodings.h), held on every platform: the modes' names, every encoding's round trip and its full scale,
// the callback's channels landing on the device channels they name with the rest silent and the device's landing on
// the callback's that name them, the period each mode opens at, the 100 ns arithmetic, and the capture ring - written
// and read, silent before its first packet, dry said once it has been written, and full dropping its oldest.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "WasapiFormats.h"

#include <gtest/gtest.h>

namespace
{
namespace wasapi = reckless::audio::wasapi;
using wasapi::Encoding;
} // namespace

TEST(WasapiFormats, EachModeHasTheNameASettingsFileKeeps)
{
    for (const wasapi::Mode mode : {wasapi::Mode::kShared, wasapi::Mode::kExclusive})
    {
        EXPECT_EQ(wasapi::ModeNamed(wasapi::NameOf(mode)), mode);
    }
    EXPECT_EQ(wasapi::NameOf(wasapi::Mode::kShared), "Windows Audio") << "the name the settings kept before";
    EXPECT_EQ(wasapi::NameOf(wasapi::Mode::kExclusive), "Windows Audio (Exclusive Mode)");
    EXPECT_EQ(wasapi::ModeNamed("Windows Audio (Low Latency Mode)"), wasapi::Mode::kShared)
        << "a mode of its own until 2026-10-25, what Windows Audio does now";
    EXPECT_FALSE(wasapi::ModeNamed("ASIO").has_value());
    EXPECT_FALSE(wasapi::ModeNamed("Windows Audio (Exclusive)").has_value());
}

TEST(WasapiFormats, EveryEncodingCarriesASampleThereAndBack)
{
    // Each encoding's container and valid bits, and a ramp of samples through it and back within its step.
    struct Row
    {
        Encoding encoding;
        int      bytes;
        int      valid;
        double   step;
    };

    const Row rows[] = {{Encoding::kFloat32, 4, 32, 0.0},
                        {Encoding::kInt32, 4, 32, 1.0 / 2147483648.0},
                        {Encoding::kInt24In32, 4, 24, 1.0 / 8388608.0},
                        {Encoding::kInt24, 3, 24, 1.0 / 8388608.0},
                        {Encoding::kInt16, 2, 16, 1.0 / 32768.0}};
    std::printf("  %-16s %5s %5s %12s\n", "encoding", "bytes", "valid", "worst error");
    for (const Row &row : rows)
    {
        EXPECT_EQ(wasapi::BytesOf(row.encoding), row.bytes);
        EXPECT_EQ(wasapi::ValidBitsOf(row.encoding), row.valid);
        std::vector<float> ramp;
        for (int i = -1000; i <= 1000; ++i)
        {
            ramp.push_back(static_cast<float>(i) / 1000.0f * 0.999f);
        }
        const int                 frames = static_cast<int>(ramp.size());
        std::vector<std::uint8_t> device(static_cast<std::size_t>(frames * row.bytes));
        const float              *channel = ramp.data();
        wasapi::Encode(&channel, 1, 0, 1, frames, row.encoding, device.data());
        double worst = 0.0;
        for (int f = 0; f < frames; ++f)
        {
            const float back = wasapi::DecodeOne(device.data() + f * row.bytes, row.encoding);
            worst = std::max(worst, std::fabs(static_cast<double>(back) - ramp[static_cast<std::size_t>(f)]));
        }
        std::printf("  %-16s %5d %5d %12.3g\n", std::string(wasapi::NameOf(row.encoding)).c_str(), row.bytes, row.valid,
                    worst);
        EXPECT_LE(worst, row.step * 0.5 + 1e-9) << wasapi::NameOf(row.encoding) << ": within half a step";
    }
}

TEST(WasapiFormats, AnIntegerHoldsFullScaleAndClipsPastIt)
{
    EXPECT_EQ(wasapi::ToInteger(1.0f, 16), 32767) << "full scale, held to the top";
    EXPECT_EQ(wasapi::ToInteger(-1.0f, 16), -32768);
    EXPECT_EQ(wasapi::ToInteger(4.0f, 24), 8388607) << "past full scale: clipped, never wrapped";
    EXPECT_EQ(wasapi::ToInteger(-4.0f, 24), -8388608);
    EXPECT_EQ(wasapi::ToInteger(0.0f, 32), 0);
    EXPECT_EQ(wasapi::ToInteger(2.0f, 32), 2147483647);

    // The 24-bit-in-32 container carries the sample in its top 24 bits, its low byte zero; the packed one in three
    // little-endian bytes, the sign in the third.
    std::array<std::uint8_t, 4> in32{};
    std::array<std::uint8_t, 3> packed{};
    const float                 x = -0.5f;
    const float                *c = &x;
    wasapi::Encode(&c, 1, 0, 1, 1, Encoding::kInt24In32, in32.data());
    wasapi::Encode(&c, 1, 0, 1, 1, Encoding::kInt24, packed.data());
    EXPECT_EQ(in32[0], 0u);
    EXPECT_EQ(in32[3], 0xC0u);
    EXPECT_EQ(packed[2], 0xC0u);
    EXPECT_EQ(wasapi::DecodeOne(in32.data(), Encoding::kInt24In32), -0.5f);
    EXPECT_EQ(wasapi::DecodeOne(packed.data(), Encoding::kInt24), -0.5f);
}

TEST(WasapiFormats, TheCallbacksChannelsLandOnTheDeviceChannelsTheyNameTheRestSilent)
{
    // A device of four channels, the callback's two on its channels 2 and 3 (counted from 0): every frame's first two
    // silent, the rest the callback's, interleaved.
    const std::array<float, 3> a           = {0.1f, 0.2f, 0.3f};
    const std::array<float, 3> b           = {-0.1f, -0.2f, -0.3f};
    const float               *channels[2] = {a.data(), b.data()};
    std::array<float, 12>      device{};
    device.fill(9.0f);
    wasapi::Encode(channels, 2, 2, 4, 3, Encoding::kFloat32, reinterpret_cast<std::uint8_t *>(device.data()));
    const std::array<float, 12> want = {0, 0, 0.1f, -0.1f, 0, 0, 0.2f, -0.2f, 0, 0, 0.3f, -0.3f};
    EXPECT_EQ(device, want);

    // A channel past the device's own is dropped, never written past the frame.
    device.fill(9.0f);
    wasapi::Encode(channels, 2, 3, 4, 3, Encoding::kFloat32, reinterpret_cast<std::uint8_t *>(device.data()));
    const std::array<float, 12> clipped = {0, 0, 0, 0.1f, 0, 0, 0, 0.2f, 0, 0, 0, 0.3f};
    EXPECT_EQ(device, clipped);
}

TEST(WasapiFormats, TheDevicesChannelsLandOnTheCallbackChannelsThatNameThem)
{
    // The way back (SampleEncodings.h's Decode, ALSA's capture): a device of four 16-bit channels, the callback's two
    // reading its channels 2 and 3, and a third past the device's own reading silence.
    const std::array<std::int16_t, 12> device = {1, 2, 16384, -16384, 3, 4, 8192, -8192, 5, 6, -32768, 32767};
    std::array<float, 3>               a{}, b{}, c{};
    c.fill(9.0f);
    float *const channels[3] = {a.data(), b.data(), c.data()};
    reckless::audio::Decode(reinterpret_cast<const std::uint8_t *>(device.data()), 4, 3, Encoding::kInt16, 2, 3,
                            channels);
    EXPECT_EQ(a, (std::array<float, 3>{0.5f, 0.25f, -1.0f}));
    EXPECT_EQ(b, (std::array<float, 3>{-0.5f, -0.25f, 32767.0f / 32768.0f}));
    EXPECT_EQ(c, (std::array<float, 3>{0.0f, 0.0f, 0.0f}));
}

TEST(WasapiFormats, ASharedStreamTakesTheDriversPeriodAtTheEndpointsOwnRate)
{
    // At the endpoint's own rate nothing is converted, and a small period can be had; at another the engine converts,
    // which it does at its own period.
    EXPECT_EQ(wasapi::SharedRouteOf(48000, 48000), wasapi::SharedRoute::kDriversPeriod);
    EXPECT_EQ(wasapi::SharedRouteOf(44100, 48000), wasapi::SharedRoute::kEnginesPeriod);
    EXPECT_EQ(wasapi::SharedRouteOf(96000, 48000), wasapi::SharedRoute::kEnginesPeriod);
    EXPECT_EQ(wasapi::SharedRouteOf(0, 48000), wasapi::SharedRoute::kEnginesPeriod) << "no rate: none to match";
}

TEST(WasapiFormats, ALowLatencyPeriodIsTheDriversSmallestThatHoldsTheBlock)
{
    // A driver of 2.67 ms steps at 48 kHz: 128 frames the smallest and the step, 480 the largest.
    EXPECT_EQ(wasapi::LowLatencyPeriod(0, 128, 128, 480), 128) << "no block asked: the smallest";
    EXPECT_EQ(wasapi::LowLatencyPeriod(32, 128, 128, 480), 128) << "less than the smallest: the smallest";
    EXPECT_EQ(wasapi::LowLatencyPeriod(128, 128, 128, 480), 128);
    EXPECT_EQ(wasapi::LowLatencyPeriod(129, 128, 128, 480), 256) << "the next step that holds it";
    EXPECT_EQ(wasapi::LowLatencyPeriod(256, 128, 128, 480), 256);
    EXPECT_EQ(wasapi::LowLatencyPeriod(512, 128, 128, 480), 384) << "past the largest: the largest whole step";
    EXPECT_EQ(wasapi::LowLatencyPeriod(2048, 32, 96, 480), 480) << "a step that lands on the largest";
    EXPECT_EQ(wasapi::LowLatencyPeriod(100, 32, 96, 480), 128);
    EXPECT_EQ(wasapi::LowLatencyPeriod(256, 0, 480, 480), 480) << "a driver of one period: that period";
}

TEST(WasapiFormats, AnExclusivePeriodIsTheBlockNoShorterThanTheDevicesMinimum)
{
    EXPECT_EQ(wasapi::ExclusivePeriod(256, 144, 480), 256);
    EXPECT_EQ(wasapi::ExclusivePeriod(64, 144, 480), 144) << "under the minimum: the minimum";
    EXPECT_EQ(wasapi::ExclusivePeriod(0, 144, 480), 480) << "no block asked: the default";
    EXPECT_EQ(wasapi::ExclusivePeriod(0, 600, 480), 600) << "a default under the minimum: the minimum";
}

TEST(WasapiFormats, DurationsGoToHundredsOfNanosecondsAndBack)
{
    EXPECT_EQ(wasapi::HundredsOfNanoseconds(480, 48000), 100000) << "10 ms";
    EXPECT_EQ(wasapi::HundredsOfNanoseconds(128, 48000), 26667) << "2.67 ms, rounded";
    EXPECT_EQ(wasapi::HundredsOfNanoseconds(441, 44100), 100000);
    EXPECT_EQ(wasapi::HundredsOfNanoseconds(64, 0), 0);
    for (const int rate : wasapi::kRates)
    {
        for (const int frames : {32, 64, 96, 128, 144, 256, 441, 480, 512, 1024})
        {
            EXPECT_EQ(wasapi::FramesOf(wasapi::HundredsOfNanoseconds(frames, rate), rate), frames)
                << frames << " frames at " << rate << " Hz, there and back";
        }
    }
}

TEST(WasapiFormats, TheCaptureRingCarriesFramesToTheCallback)
{
    wasapi::CaptureRing ring;
    ring.Prepare(2, 8);
    EXPECT_EQ(ring.Capacity(), 8);
    std::array<float, 4> left{};
    std::array<float, 4> right{};
    float               *channels[2] = {left.data(), right.data()};

    // Before its first packet the ring reads silence and says nothing of it: the stream is starting.
    left.fill(5.0f);
    EXPECT_TRUE(ring.Read(channels, 2, 0, 4));
    EXPECT_EQ(left, (std::array<float, 4>{0, 0, 0, 0}));

    // Frames in, frames out, de-interleaved.
    const std::array<float, 6> packet = {1, -1, 2, -2, 3, -3};
    EXPECT_TRUE(ring.Write(packet.data(), 3));
    EXPECT_EQ(ring.Count(), 3);
    EXPECT_TRUE(ring.Read(channels, 2, 0, 2));
    EXPECT_EQ(left[0], 1.0f);
    EXPECT_EQ(right[1], -2.0f);
    EXPECT_EQ(ring.Count(), 1);

    // Dry once it has been written: what it has, then silence, and said.
    EXPECT_FALSE(ring.Read(channels, 2, 0, 3));
    EXPECT_EQ(left, (std::array<float, 4>{3, 0, 0, 0}));
    EXPECT_EQ(ring.Count(), 0);

    // One callback channel reading the ring's second.
    EXPECT_TRUE(ring.Write(packet.data(), 1));
    float *one[1] = {left.data()};
    EXPECT_TRUE(ring.Read(one, 1, 1, 1));
    EXPECT_EQ(left[0], -1.0f);

    // A silent packet is silence; a full ring drops its oldest and says so.
    EXPECT_TRUE(ring.Write(nullptr, 2));
    EXPECT_EQ(ring.Count(), 2);
    const std::array<float, 14> lots = {1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7};
    EXPECT_FALSE(ring.Write(lots.data(), 7)) << "nine frames into eight";
    EXPECT_EQ(ring.Count(), 8);
    EXPECT_TRUE(ring.Read(channels, 2, 0, 2));
    EXPECT_EQ(left[0], 0.0f) << "the second silent frame, the first dropped";
    EXPECT_EQ(left[1], 1.0f);

    // Cleared: empty, and silent again without a word until its next packet.
    ring.Clear();
    EXPECT_EQ(ring.Count(), 0);
    EXPECT_TRUE(ring.Read(channels, 2, 0, 4));
}

TEST(WasapiFormats, TheCaptureRingGathersItsMarginBeforeItIsRead)
{
    // Primed at six frames: read as silence, unsaid, until six have gathered - a callback's and a capture period's -
    // then a callback's at a time, the margin kept; run dry, it says so once and gathers the margin again.
    wasapi::CaptureRing ring;
    ring.Prepare(1, 32, 6);
    std::array<float, 4> out{};
    float               *channel[1] = {out.data()};
    std::vector<float>   frames;
    for (int i = 1; i <= 32; ++i)
    {
        frames.push_back(static_cast<float>(i));
    }

    ring.Write(frames.data(), 4);
    out.fill(9.0f);
    EXPECT_TRUE(ring.Read(channel, 1, 0, 4)) << "four of six: still gathering, and nothing said";
    EXPECT_EQ(out, (std::array<float, 4>{0, 0, 0, 0}));
    EXPECT_EQ(ring.Count(), 4) << "nothing taken while gathering";

    ring.Write(frames.data() + 4, 2);
    EXPECT_TRUE(ring.Read(channel, 1, 0, 4)) << "six gathered: read";
    EXPECT_EQ(out, (std::array<float, 4>{1, 2, 3, 4}));
    EXPECT_EQ(ring.Count(), 2) << "the margin left";

    // The steady state: a period in, a callback out, the margin kept.
    for (int k = 0; k < 4; ++k)
    {
        ring.Write(frames.data() + 6 + 4 * k, 4);
        EXPECT_TRUE(ring.Read(channel, 1, 0, 4));
        EXPECT_EQ(ring.Count(), 2);
    }
    EXPECT_EQ(out[3], 20.0f);

    // A period lost: the margin and a little, then dry - said once - and silence while it gathers again.
    EXPECT_FALSE(ring.Read(channel, 1, 0, 4)) << "two of four: dry";
    EXPECT_EQ(out, (std::array<float, 4>{21, 22, 0, 0}));
    ring.Write(frames.data(), 4);
    EXPECT_TRUE(ring.Read(channel, 1, 0, 4)) << "gathering again, unsaid";
    EXPECT_EQ(out, (std::array<float, 4>{0, 0, 0, 0}));
    ring.Write(frames.data(), 4);
    EXPECT_TRUE(ring.Read(channel, 1, 0, 4)) << "eight gathered: read again";
    EXPECT_EQ(out, (std::array<float, 4>{1, 2, 3, 4}));
}
