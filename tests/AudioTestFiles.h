#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

// Test-only: audio files built a field at a time, for the suite that holds the reader (core/AudioFiles.h) to
// the formats' definitions - WAV, RF64, AIFF, AIFF-C and `.wir` at every width and their edges - and the table of the
// FLAC and Ogg Vorbis files tests/audio carries beside the reference decoders' samples for them.
namespace reckless::test
{
/** The bytes of a file. */
using AudioBytes = std::vector<std::uint8_t>;

/** A file built a field at a time. */
class AudioFileWriter
{
  public:
    /** Append text as bytes.
        @param text  a null-terminated string; its characters are written, not its terminator
        @return      this writer */
    AudioFileWriter &Raw(const char *text)
    {
        bytes_.insert(bytes_.end(), text, text + std::strlen(text));
        return *this;
    }

    /** Append one byte.
        @param value  the byte; only its low eight bits are written
        @return       this writer */
    AudioFileWriter &Byte(int value)
    {
        bytes_.push_back(static_cast<std::uint8_t>(value & 0xff));
        return *this;
    }

    /** Append an unsigned integer, least significant byte first.
        @param value  the integer
        @param width  the bytes to write, 1 to 8
        @return       this writer */
    AudioFileWriter &Le(std::uint64_t value, int width)
    {
        for (int i = 0; i < width; ++i)
        {
            Byte(static_cast<int>((value >> (8 * i)) & 0xff));
        }
        return *this;
    }

    /** Append an unsigned integer, most significant byte first.
        @param value  the integer
        @param width  the bytes to write, 1 to 8
        @return       this writer */
    AudioFileWriter &Be(std::uint64_t value, int width)
    {
        for (int i = width - 1; i >= 0; --i)
        {
            Byte(static_cast<int>((value >> (8 * i)) & 0xff));
        }
        return *this;
    }

    /** An 80-bit extended float, AIFF's rate: the biased exponent, then the 64-bit mantissa with its leading one.
        @param value  the number, positive and finite
        @return       this writer */
    AudioFileWriter &Extended(double value)
    {
        int        exponent = 0;
        const auto fraction = std::frexp(value, &exponent); // value = fraction * 2^exponent, fraction in [0.5, 1)
        Be(static_cast<std::uint64_t>(16383 + exponent - 1), 2);
        return Be(static_cast<std::uint64_t>(std::ldexp(fraction, 64)), 8);
    }

    /** The bytes written so far.
        @return  the length of the file so far, in bytes */
    std::size_t Size() const noexcept
    {
        return bytes_.size();
    }

    /** Overwrite four bytes already written with a 32-bit integer, least significant byte first.
        @param at     the offset of the first byte, in bytes; at least four before the end
        @param value  the integer */
    void PatchLe32(std::size_t at, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
        {
            bytes_[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (8 * i)) & 0xff);
        }
    }

    /** Overwrite four bytes already written with a 32-bit integer, most significant byte first.
        @param at     the offset of the first byte, in bytes; at least four before the end
        @param value  the integer */
    void PatchBe32(std::size_t at, std::uint32_t value)
    {
        for (int i = 0; i < 4; ++i)
        {
            bytes_[at + static_cast<std::size_t>(i)] = static_cast<std::uint8_t>((value >> (24 - 8 * i)) & 0xff);
        }
    }

    /** The finished file.
        @return  the bytes; the writer is left empty */
    AudioBytes Take()
    {
        return std::move(bytes_);
    }

  private:
    AudioBytes bytes_; ///< the file so far
};

/** The samples a test writes: raw integer codes or float bits, interleaved, as the file stores them. */
struct PcmCodes
{
    int                        tag{1};      ///< WAV format tag: 1 PCM, 3 float, 0xfffe extensible
    int                        width{2};    ///< bytes a sample
    int                        channels{1}; ///< interleaved channels
    int                        rate{48000}; ///< the sample rate, in hertz
    std::vector<std::uint64_t> codes;       ///< interleaved, each `width` bytes
};

/** A WAV file: RIFF, `fmt ` (extensible for tag 0xfffe, its sub-format float or PCM), an optional odd-length
    metadata chunk, `data`.
    @param pcm                the samples and their format
    @param extensible_float   for tag 0xfffe, true for a float sub-format, false for PCM
    @param extra_chunk        the four-character id of an odd-length chunk to put before `data`, or nullptr for none
    @return                   the file's bytes */
inline AudioBytes WavBytes(const PcmCodes &pcm, bool extensible_float = false, const char *extra_chunk = nullptr)
{
    const int       block = pcm.width * pcm.channels;
    AudioFileWriter w;
    w.Raw("RIFF").Le(0, 4).Raw("WAVE");
    if (pcm.tag == 0xfffe)
    {
        w.Raw("fmt ").Le(40, 4).Le(0xfffe, 2).Le(static_cast<std::uint64_t>(pcm.channels), 2);
        w.Le(static_cast<std::uint64_t>(pcm.rate), 4).Le(static_cast<std::uint64_t>(pcm.rate * block), 4);
        w.Le(static_cast<std::uint64_t>(block), 2).Le(static_cast<std::uint64_t>(pcm.width * 8), 2);
        w.Le(22, 2).Le(static_cast<std::uint64_t>(pcm.width * 8), 2).Le(3, 4);
        w.Le(extensible_float ? 3 : 1, 4).Le(0, 2).Le(0x10, 2);
        for (const int b : {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71})
        {
            w.Byte(b);
        }
    }
    else
    {
        w.Raw("fmt ").Le(16, 4).Le(static_cast<std::uint64_t>(pcm.tag), 2);
        w.Le(static_cast<std::uint64_t>(pcm.channels), 2).Le(static_cast<std::uint64_t>(pcm.rate), 4);
        w.Le(static_cast<std::uint64_t>(pcm.rate * block), 4).Le(static_cast<std::uint64_t>(block), 2);
        w.Le(static_cast<std::uint64_t>(pcm.width * 8), 2);
    }
    if (extra_chunk != nullptr)
    {
        w.Raw(extra_chunk).Le(3, 4).Raw("abc").Byte(0); // an odd-length chunk and its pad byte
    }
    w.Raw("data").Le(pcm.codes.size() * static_cast<std::uint64_t>(pcm.width), 4);
    for (const std::uint64_t code : pcm.codes)
    {
        w.Le(code, pcm.width);
    }
    const std::size_t size = w.Size();
    w.PatchLe32(4, static_cast<std::uint32_t>(size - 8));
    return w.Take();
}

/** An RF64 file: the RIFF sizes 0xffffffff and the real ones in a ds64 chunk, as a writer of files past 4 GB has it.
    @param pcm  the samples and their format
    @return     the file's bytes */
inline AudioBytes Rf64Bytes(const PcmCodes &pcm)
{
    const int           block = pcm.width * pcm.channels;
    const std::uint64_t data  = pcm.codes.size() * static_cast<std::uint64_t>(pcm.width);
    AudioFileWriter     w;
    w.Raw("RF64").Le(0xffffffff, 4).Raw("WAVE");
    w.Raw("ds64").Le(28, 4).Le(4 + (8 + 28) + (8 + 16) + 8 + data, 8).Le(data, 8);
    w.Le(pcm.codes.size() / static_cast<std::size_t>(pcm.channels), 8).Le(0, 4);
    w.Raw("fmt ").Le(16, 4).Le(static_cast<std::uint64_t>(pcm.tag), 2).Le(static_cast<std::uint64_t>(pcm.channels), 2);
    w.Le(static_cast<std::uint64_t>(pcm.rate), 4).Le(static_cast<std::uint64_t>(pcm.rate * block), 4);
    w.Le(static_cast<std::uint64_t>(block), 2).Le(static_cast<std::uint64_t>(pcm.width * 8), 2);
    w.Raw("data").Le(0xffffffff, 4);
    for (const std::uint64_t code : pcm.codes)
    {
        w.Le(code, pcm.width);
    }
    return w.Take();
}

/** An AIFF file, or AIFF-C with a compression id, big-endian samples unless the compression says `sowt`.
    @param pcm          the samples and their format
    @param compression  the AIFF-C compression id, four characters, or nullptr for plain AIFF
    @param rate         the rate written in the header, in hertz; zero for `pcm.rate`
    @return             the file's bytes */
inline AudioBytes AiffBytes(const PcmCodes &pcm, const char *compression = nullptr, double rate = 0.0)
{
    const bool      little = compression != nullptr && std::strcmp(compression, "sowt") == 0;
    const auto      frames = pcm.codes.size() / static_cast<std::size_t>(pcm.channels);
    AudioFileWriter w;
    w.Raw("FORM").Be(0, 4).Raw(compression != nullptr ? "AIFC" : "AIFF");
    if (compression != nullptr)
    {
        w.Raw("FVER").Be(4, 4).Be(0xa2805140, 4);
    }
    w.Raw("COMM").Be(compression != nullptr ? 24 : 18, 4).Be(static_cast<std::uint64_t>(pcm.channels), 2);
    w.Be(frames, 4).Be(static_cast<std::uint64_t>(pcm.width * 8), 2).Extended(rate > 0.0 ? rate : pcm.rate);
    if (compression != nullptr)
    {
        w.Raw(compression).Byte(0).Byte(0); // the compression's id and an empty name
    }
    w.Raw("SSND").Be(8 + pcm.codes.size() * static_cast<std::uint64_t>(pcm.width), 4).Be(0, 4).Be(0, 4);
    for (const std::uint64_t code : pcm.codes)
    {
        little ? w.Le(code, pcm.width) : w.Be(code, pcm.width);
    }
    const std::size_t size = w.Size();
    w.PatchBe32(4, static_cast<std::uint32_t>(size - 8));
    return w.Take();
}

/** A `.wir`: `wvIR <size> ver1` - or another head of four characters, a size and four more, an ordinary
    `RIFF <size> WAVE` among them - `fmt ` with the bits field as given - the format's float files say 23 - and
    `data`.
    @param pcm        the samples and their format
    @param bits_field the bits field written in `fmt `
    @param magic      the four characters that open the file
    @param form       the four characters after the size
    @return           the file's bytes */
inline AudioBytes WirBytes(const PcmCodes &pcm, int bits_field, const char *magic = "wvIR", const char *form = "ver1")
{
    const int       block = pcm.width * pcm.channels;
    AudioFileWriter w;
    w.Raw(magic).Le(0, 4).Raw(form);
    w.Raw("fmt ").Le(16, 4).Le(static_cast<std::uint64_t>(pcm.tag), 2).Le(static_cast<std::uint64_t>(pcm.channels), 2);
    w.Le(static_cast<std::uint64_t>(pcm.rate), 4).Le(static_cast<std::uint64_t>(pcm.rate * block), 4);
    w.Le(static_cast<std::uint64_t>(block), 2).Le(static_cast<std::uint64_t>(bits_field), 2);
    w.Raw("data").Le(pcm.codes.size() * static_cast<std::uint64_t>(pcm.width), 4);
    for (const std::uint64_t code : pcm.codes)
    {
        w.Le(code, pcm.width);
    }
    return w.Take();
}

/**
    The FLAC and Ogg Vorbis files tests/audio carries, which the suite has no encoder to make: each encoded from
   a test signal - a decaying noise burst with a few resonances, or chords - beside the samples the reference decoders,
    libFLAC and libvorbis, gave for it (`<name>.f32`, kDecodedSuffix: little-endian 32-bit floats, the channels
    interleaved), recorded once by the JUCE build, whose readers ran those decoders. The core's suite holds the reader
    to them: FLAC, which is lossless, to the bit, Ogg Vorbis - whose two decoders compute in floating point - within
    kCarriedVorbisTolerance.
*/
struct CarriedFile
{
    const char *name;     ///< the file's name in tests/audio
    bool        vorbis;   ///< Ogg Vorbis; FLAC otherwise
    double      rate;     ///< the sample rate, in hertz
    int         channels; ///< the channels
    int         frames;   ///< the length, in sample frames
};

/// The FLAC files' frames: level 0 codes 1152 samples a frame with fixed predictors, so the first is three frames,
/// the last short; level 8 codes 4096 a frame by linear prediction, so the second is two, its stereo pair decorrelated.
inline constexpr CarriedFile kCarriedFiles[] = {
    {"response_stereo_44k1_16bit_level0.flac", false, 44100.0, 2, 3000},
    {"response_mono_96k_24bit_level8.flac", false, 96000.0, 1, 4500},
    {"response_mono_44k1_q0.ogg", true, 44100.0, 1, 3000},
    {"chords_stereo_48k_q5.ogg", true, 48000.0, 2, 3000},
};

inline constexpr const char *kDecodedSuffix = ".f32"; ///< what follows a carried file's name for its decoded samples

/** The largest difference allowed between a carried Ogg Vorbis file's samples as the reader decodes them and as the
    reference decoder did: 120 dB under full scale, a few units in the last place of a float near it. The reference
    decoded them on arm64, with fused multiply-adds; the reader lands within 2.4e-7 of them built so and within 3.0e-7
    built without them, as an x86-64 build is, where another platform's libm, which builds the decoder's tables, may
    move a sample a unit further. */
inline constexpr double kCarriedVorbisTolerance = 1.0e-6;

/** A FLAC file with STREAMINFO's count of samples cleared, as an encoder that cannot seek back to write it leaves
    it: the 36 bits from the low half of byte 21 to byte 25, past the "fLaC" mark, the block's header and the sizes, the
    rate, the channels and the width. The reader, finding no count, decodes the whole stream to count its samples.
    @param flac  a FLAC file
    @return      the file with the count cleared; unchanged when too short to hold one */
inline AudioBytes WithoutFlacCount(AudioBytes flac)
{
    if (flac.size() > 25)
    {
        flac[21] = static_cast<std::uint8_t>(flac[21] & 0xf0);
        flac[22] = flac[23] = flac[24] = flac[25] = 0;
    }
    return flac;
}

/** The floats a decoded file holds.
    @param bytes  the contents of a `.f32` file
    @return       its little-endian 32-bit floats, the channels interleaved */
inline std::vector<float> FloatsOf(const AudioBytes &bytes)
{
    std::vector<float> samples(bytes.size() / 4);
    for (std::size_t s = 0; s < samples.size(); ++s)
    {
        std::uint32_t bits = 0;
        for (int i = 0; i < 4; ++i)
        {
            bits |= static_cast<std::uint32_t>(bytes[4 * s + static_cast<std::size_t>(i)]) << (8 * i);
        }
        std::memcpy(&samples[s], &bits, sizeof(bits));
    }
    return samples;
}
} // namespace reckless::test
