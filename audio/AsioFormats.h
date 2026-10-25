#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>

#include "SampleEncodings.h"

namespace reckless::audio::asio
{
/**
    The arithmetic of the suite's ASIO back end (win/AsioDevice.cpp) that needs no Windows to hold: the block a stream
    opens at from what a driver says it takes, and the conversion of the sample types SampleEncodings.h does not carry -
    a 32-bit integer aligned low at fewer bits, and a 64-bit float - between a driver's channel and the callback's.
   Header only and platform-neutral, so the audio suite holds it on every platform (tests/test_asio_formats.cpp).
*/

/** The block a stream opens at, by RtAudio's rule, so a setting carries over: the driver's preferred size where none is
    asked; else the size asked held to the driver's smallest and largest; and within them the nearest power of two where
    the driver's granularity is -1 (powers of two alone), or the size rounded up to a whole number of a positive
    granularity, held to the largest.
    @param asked        the block asked for, in frames; 0 or less for the driver's preferred size
    @param smallest     the driver's smallest buffer, in frames
    @param largest      its largest, in frames
    @param preferred    its preferred, in frames
    @param granularity  its step: 0 for any size, -1 for powers of two, else the step in frames
    @return             the block, in frames */
constexpr long ChooseBlock(long asked, long smallest, long largest, long preferred, long granularity) noexcept
{
    if (asked <= 0)
    {
        return preferred;
    }
    if (smallest >= largest)
    {
        return smallest;
    }
    if (asked <= smallest)
    {
        return smallest;
    }
    if (asked >= largest)
    {
        return largest;
    }
    if (granularity == -1)
    {
        long best = 0;
        for (long power = 1; power <= largest && power > 0; power *= 2)
        {
            if (power < smallest)
            {
                continue;
            }
            const long distance = power > asked ? power - asked : asked - power;
            const long kept     = best > asked ? best - asked : asked - best;
            if (best == 0 || distance < kept)
            {
                best = power;
            }
        }
        return best > 0 ? best : smallest;
    }
    if (granularity > 0)
    {
        return std::min(largest, (asked + granularity - 1) / granularity * granularity);
    }
    return asked;
}

/** The callback's samples into a driver's channel of 32-bit integers aligned low at `bits` - the sample in the low
    `bits`, sign-extended above them - full scale 1.0, held to the range.
    @param in      the callback's channel, `frames` samples
    @param out     the driver's channel, `frames` 32-bit integers
    @param frames  the frames
    @param bits    16, 18, 20 or 24 */
inline void EncodeLowAligned(const float *in, std::uint8_t *out, int frames, int bits) noexcept
{
    for (int f = 0; f < frames; ++f)
    {
        const std::int32_t v = ToInteger(in[f], bits);
        std::memcpy(out + static_cast<std::size_t>(f) * 4, &v, 4);
    }
}

/** A driver's channel of 32-bit integers aligned low at `bits` into the callback's samples, full scale 1.0.
    @param in      the driver's channel, `frames` 32-bit integers
    @param out     the callback's channel, `frames` samples
    @param frames  the frames
    @param bits    16, 18, 20 or 24 */
inline void DecodeLowAligned(const std::uint8_t *in, float *out, int frames, int bits) noexcept
{
    const double scale = 1.0 / static_cast<double>(std::int64_t{1} << (bits - 1));
    for (int f = 0; f < frames; ++f)
    {
        std::int32_t v = 0;
        std::memcpy(&v, in + static_cast<std::size_t>(f) * 4, 4);
        out[f] = static_cast<float>(static_cast<double>(v) * scale);
    }
}

/** The callback's samples into a driver's channel of 64-bit floats.
    @param in      the callback's channel, `frames` samples
    @param out     the driver's channel, `frames` doubles
    @param frames  the frames */
inline void EncodeFloat64(const float *in, std::uint8_t *out, int frames) noexcept
{
    for (int f = 0; f < frames; ++f)
    {
        const double v = in[f];
        std::memcpy(out + static_cast<std::size_t>(f) * 8, &v, 8);
    }
}

/** A driver's channel of 64-bit floats into the callback's samples.
    @param in      the driver's channel, `frames` doubles
    @param out     the callback's channel, `frames` samples
    @param frames  the frames */
inline void DecodeFloat64(const std::uint8_t *in, float *out, int frames) noexcept
{
    for (int f = 0; f < frames; ++f)
    {
        double v = 0.0;
        std::memcpy(&v, in + static_cast<std::size_t>(f) * 8, 8);
        out[f] = static_cast<float>(v);
    }
}
} // namespace reckless::audio::asio
