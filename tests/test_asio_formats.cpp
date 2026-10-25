// The arithmetic of the suite's ASIO back end (audio/AsioFormats.h), held on every platform: the block a stream opens
// at from what a driver says it takes - its preferred size where none is asked, held to its smallest and largest, the
// nearest power of two where it takes only those, a whole number of its granularity otherwise - and the sample types
// SampleEncodings.h does not carry, a 32-bit integer aligned low and a 64-bit float, there and back.
#include <array>
#include <cstdint>
#include <cstring>

#include "AsioFormats.h"

#include <gtest/gtest.h>

namespace
{
namespace asio = reckless::audio::asio;
} // namespace

TEST(AsioFormats, TheBlockIsTheAskedOneHeldToWhatTheDriverTakes)
{
    // The SDK's sample driver: one size, 256 frames, whatever is asked.
    EXPECT_EQ(asio::ChooseBlock(128, 256, 256, 256, 0), 256);
    EXPECT_EQ(asio::ChooseBlock(0, 256, 256, 256, 0), 256);

    // A driver of 32 to 2048 frames, 512 preferred, any size between.
    EXPECT_EQ(asio::ChooseBlock(0, 32, 2048, 512, 0), 512) << "none asked: the preferred size";
    EXPECT_EQ(asio::ChooseBlock(16, 32, 2048, 512, 0), 32) << "under the smallest: the smallest";
    EXPECT_EQ(asio::ChooseBlock(4096, 32, 2048, 512, 0), 2048) << "over the largest: the largest";
    EXPECT_EQ(asio::ChooseBlock(100, 32, 2048, 512, 0), 100) << "any size: the size asked";

    // Powers of two alone: the nearest, the lower on a tie.
    EXPECT_EQ(asio::ChooseBlock(100, 32, 2048, 512, -1), 128);
    EXPECT_EQ(asio::ChooseBlock(96, 32, 2048, 512, -1), 64) << "a tie between 64 and 128: the lower";
    EXPECT_EQ(asio::ChooseBlock(64, 32, 2048, 512, -1), 64);
    EXPECT_EQ(asio::ChooseBlock(50, 48, 2048, 512, -1), 64) << "no power under the smallest";

    // A granularity: rounded up to a whole number of it, held to the largest.
    EXPECT_EQ(asio::ChooseBlock(100, 32, 2048, 512, 48), 144);
    EXPECT_EQ(asio::ChooseBlock(144, 32, 2048, 512, 48), 144);
    EXPECT_EQ(asio::ChooseBlock(2040, 32, 2047, 512, 48), 2047) << "a step past the largest: the largest";
}

TEST(AsioFormats, ALowAlignedIntegerCarriesASampleThereAndBack)
{
    // 24 bits aligned low: full scale is 2^23, the sign extended through the top byte, and held to the range.
    const std::array<float, 4>   in = {0.5f, -0.5f, 1.0f, -1.0f};
    std::array<std::uint8_t, 16> driver{};
    asio::EncodeLowAligned(in.data(), driver.data(), 4, 24);
    std::array<std::int32_t, 4> raw{};
    std::memcpy(raw.data(), driver.data(), 16);
    EXPECT_EQ(raw[0], 1 << 22);
    EXPECT_EQ(raw[1], -(1 << 22));
    EXPECT_EQ(raw[2], (1 << 23) - 1) << "full scale held to the largest integer";
    EXPECT_EQ(raw[3], -(1 << 23));
    std::array<float, 4> back{};
    asio::DecodeLowAligned(driver.data(), back.data(), 4, 24);
    EXPECT_EQ(back[0], 0.5f);
    EXPECT_EQ(back[1], -0.5f);
    EXPECT_EQ(back[3], -1.0f);

    // 16 bits aligned low: the same, at 2^15.
    asio::EncodeLowAligned(in.data(), driver.data(), 2, 16);
    std::memcpy(raw.data(), driver.data(), 8);
    EXPECT_EQ(raw[0], 1 << 14);
    EXPECT_EQ(raw[1], -(1 << 14));
}

TEST(AsioFormats, ADoubleCarriesASampleThereAndBack)
{
    const std::array<float, 3>   in = {0.25f, -0.75f, 0.0f};
    std::array<std::uint8_t, 24> driver{};
    asio::EncodeFloat64(in.data(), driver.data(), 3);
    std::array<double, 3> raw{};
    std::memcpy(raw.data(), driver.data(), 24);
    EXPECT_EQ(raw[0], 0.25);
    EXPECT_EQ(raw[1], -0.75);
    std::array<float, 3> back{};
    asio::DecodeFloat64(driver.data(), back.data(), 3);
    EXPECT_EQ(back, in);
}
