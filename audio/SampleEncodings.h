#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace reckless::audio
{
/**
    How a device stores its samples, and the conversion between a device's interleaved frames and the float channels a
    callback is handed: the encodings the native back ends meet - WASAPI's (win/WasapiDevice.cpp) and ALSA's
    (linux/AlsaDevice.cpp) - in one place, so the two convert alike. Header only and platform-neutral, so the audio
    suite holds it on every platform (tests/test_wasapi_formats.cpp).
*/

/** How a device frame's samples are stored: 32-bit float, a 32-bit integer whose top 32 or 24 bits are the sample, a
    24-bit integer in three bytes, or a 16-bit integer, every integer little-endian and signed. */
enum class Encoding
{
    kFloat32,   ///< 32-bit IEEE float
    kInt32,     ///< a 32-bit integer
    kInt24In32, ///< a 24-bit integer in the top three bytes of four
    kInt24,     ///< a 24-bit integer in three bytes
    kInt16      ///< a 16-bit integer
};

/** The bytes one sample of an encoding takes.
    @param encoding  the encoding
    @return          4, 3 or 2 */
constexpr int BytesOf(Encoding encoding) noexcept
{
    return encoding == Encoding::kInt24 ? 3 : (encoding == Encoding::kInt16 ? 2 : 4);
}

/** The bits of an encoding's container that carry the sample (WASAPI's wValidBitsPerSample, ALSA's
    significant bits).
    @param encoding  the encoding
    @return          32, 24 or 16 */
constexpr int ValidBitsOf(Encoding encoding) noexcept
{
    return encoding == Encoding::kInt24In32 || encoding == Encoding::kInt24 ? 24
                                                                            : (encoding == Encoding::kInt16 ? 16 : 32);
}

/** An encoding's name, for a stream's description.
    @param encoding  the encoding
    @return          a short name: "32-bit float", "24-bit in 32" and so on */
constexpr std::string_view NameOf(Encoding encoding) noexcept
{
    switch (encoding)
    {
    case Encoding::kFloat32:
        return "32-bit float";
    case Encoding::kInt32:
        return "32-bit integer";
    case Encoding::kInt24In32:
        return "24-bit in 32";
    case Encoding::kInt24:
        return "24-bit integer";
    case Encoding::kInt16:
        return "16-bit integer";
    }
    return "";
}

/** One sample as an integer of `bits` bits, rounded to the nearest and held to the range, full scale 1.0.
    @param x     the sample
    @param bits  the integer's width, 16, 24 or 32
    @return      the integer */
inline std::int32_t ToInteger(float x, int bits) noexcept
{
    const double scale = static_cast<double>(std::int64_t{1} << (bits - 1));
    const double v     = std::clamp(static_cast<double>(x) * scale, -scale, scale - 1.0);
    return static_cast<std::int32_t>(v < 0.0 ? v - 0.5 : v + 0.5);
}

/** The callback's float channels into a device's interleaved frames: device channel `first + c` from callback channel
    c for the `count` opened, every other channel silent - the device always opens with all its channels.
    @param channels         the callback's channels, `count` pointers to `frames` samples
    @param count            the callback's channels
    @param first            the device channel callback channel 0 lands on
    @param device_channels  the device frame's channels
    @param frames           the frames to write
    @param encoding         the device's encoding
    @param out              the device's buffer, `frames` frames of `device_channels` samples */
inline void Encode(const float *const *channels, int count, int first, int device_channels, int frames,
                   Encoding encoding, std::uint8_t *out) noexcept
{
    const int bytes = BytesOf(encoding);
    std::memset(out, 0, static_cast<std::size_t>(frames) * static_cast<std::size_t>(device_channels * bytes));
    for (int c = 0; c < count; ++c)
    {
        const int d = first + c;
        if (d < 0 || d >= device_channels)
        {
            continue;
        }
        const float  *in   = channels[c];
        std::uint8_t *at   = out + static_cast<std::size_t>(d * bytes);
        const auto    step = static_cast<std::size_t>(device_channels * bytes);
        for (int f = 0; f < frames; ++f, at += step)
        {
            const float x = in[f];
            switch (encoding)
            {
            case Encoding::kFloat32:
                std::memcpy(at, &x, 4);
                break;
            case Encoding::kInt32: {
                const std::int32_t v = ToInteger(x, 32);
                std::memcpy(at, &v, 4);
                break;
            }
            case Encoding::kInt24In32: {
                const std::int32_t v = static_cast<std::int32_t>(static_cast<std::uint32_t>(ToInteger(x, 24)) << 8);
                std::memcpy(at, &v, 4);
                break;
            }
            case Encoding::kInt24: {
                const auto v = static_cast<std::uint32_t>(ToInteger(x, 24));
                at[0]        = static_cast<std::uint8_t>(v);
                at[1]        = static_cast<std::uint8_t>(v >> 8);
                at[2]        = static_cast<std::uint8_t>(v >> 16);
                break;
            }
            case Encoding::kInt16: {
                const auto v = static_cast<std::int16_t>(ToInteger(x, 16));
                std::memcpy(at, &v, 2);
                break;
            }
            }
        }
    }
}

/** One device sample as a float, full scale 1.0.
    @param at        the sample's first byte
    @param encoding  its encoding
    @return          the sample */
inline float DecodeOne(const std::uint8_t *at, Encoding encoding) noexcept
{
    switch (encoding)
    {
    case Encoding::kFloat32: {
        float x = 0.0f;
        std::memcpy(&x, at, 4);
        return x;
    }
    case Encoding::kInt32:
    case Encoding::kInt24In32: {
        std::int32_t v = 0;
        std::memcpy(&v, at, 4);
        return static_cast<float>(static_cast<double>(v) / 2147483648.0);
    }
    case Encoding::kInt24: {
        const std::uint32_t u = static_cast<std::uint32_t>(at[0]) | (static_cast<std::uint32_t>(at[1]) << 8) |
                                (static_cast<std::uint32_t>(at[2]) << 16);
        const auto          v = static_cast<std::int32_t>(u << 8); // the sign in the top bit
        return static_cast<float>(static_cast<double>(v) / 2147483648.0);
    }
    case Encoding::kInt16: {
        std::int16_t v = 0;
        std::memcpy(&v, at, 2);
        return static_cast<float>(v) / 32768.0f;
    }
    }
    return 0.0f;
}

/** A device's interleaved frames into callback channels: callback channel c from device channel `first + c` for the
    `count` asked, silence for a channel past the device's.
    @param in               the device's buffer, `frames` frames of `device_channels` samples
    @param device_channels  the device frame's channels
    @param frames           the frames to read
    @param encoding         the device's encoding
    @param first            the device channel callback channel 0 reads
    @param count            the callback's channels
    @param channels         the callback's channels, `count` pointers to `frames` samples */
inline void Decode(const std::uint8_t *in, int device_channels, int frames, Encoding encoding, int first, int count,
                   float *const *channels) noexcept
{
    const int  bytes = BytesOf(encoding);
    const auto step  = static_cast<std::size_t>(device_channels * bytes);
    for (int c = 0; c < count; ++c)
    {
        float    *out = channels[c];
        const int d   = first + c;
        if (d < 0 || d >= device_channels)
        {
            std::fill(out, out + frames, 0.0f);
            continue;
        }
        const std::uint8_t *at = in + static_cast<std::size_t>(d * bytes);
        for (int f = 0; f < frames; ++f, at += step)
        {
            out[f] = DecodeOne(at, encoding);
        }
    }
}
} // namespace reckless::audio
