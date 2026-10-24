// Audio files decoded by the suite's own reader (core/AudioFiles.h): files written byte by byte, every sample's
// value worked out from the formats' definitions - an integer of b bits over 2^(b-1), a float its value - what is
// refused, the carried MP3s trimmed of the encoder's delay and padding to the samples that were encoded, the carried
// FLAC and Ogg Vorbis files read to the reference decoders' samples, and every file cut short or damaged decoded
// without a fault; then the WAV writer, its header field by field and its samples read back.
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <tuple>
#include <vector>

#include "AudioFiles.h"
#include "AudioTestFiles.h"

#include <gtest/gtest.h>

namespace
{
using reckless::core::AudioFileFormat;
using reckless::core::AudioFileFormatFor;
using reckless::core::AudioFileReader;
using Bytes    = reckless::test::AudioBytes;
using Writer   = reckless::test::AudioFileWriter;
using Pcm      = reckless::test::PcmCodes;
using Channels = std::vector<std::vector<double>>;

/** An integer sample of `bits` bits as it reads: its value over 2^(bits - 1). */
double Scaled(std::int64_t sample, int bits)
{
    return std::ldexp(static_cast<double>(sample), 1 - bits);
}

struct Decoded
{
    std::string  why;
    double       sample_rate{0.0};
    int          channels{0};
    std::int64_t length{0};
    Channels     samples;
};

/** Open and read the whole file, every channel, as the loaders would: a file with no samples refused as having no
    audio, as DecodeImpulseFile refuses it. */
Decoded Decode(Bytes bytes, AudioFileFormat format)
{
    Decoded     out;
    std::string why;
    auto        reader = AudioFileReader::Open(std::move(bytes), format, why);
    if (reader == nullptr)
    {
        out.why = why.empty() ? "refused with no reason" : why;
        return out;
    }
    out.sample_rate = reader->Info().sample_rate;
    out.channels    = reader->Info().channels;
    out.length      = reader->Info().length;
    out.why         = out.length > 0 ? reader->Read(out.length, out.channels, out.samples) : "it has no audio in it";
    return out;
}

/** Each sample against what the definitions give, exactly, channel by channel; a NaN against a NaN. */
void ExpectSamples(const Decoded &got, const Channels &want, const std::string &what)
{
    ASSERT_TRUE(got.why.empty()) << what << ": " << got.why;
    ASSERT_EQ(got.samples.size(), want.size()) << what;
    for (std::size_t c = 0; c < want.size(); ++c)
    {
        ASSERT_EQ(got.samples[c].size(), want[c].size()) << what;
        for (std::size_t i = 0; i < want[c].size(); ++i)
        {
            if (std::isnan(want[c][i]))
            {
                ASSERT_TRUE(std::isnan(got.samples[c][i])) << what << ", channel " << c << " sample " << i;
                continue;
            }
            ASSERT_EQ(got.samples[c][i], want[c][i]) << what << ", channel " << c << " sample " << i;
        }
    }
}

Bytes ReadFixture(const std::string &name)
{
    std::ifstream in(std::string(RECKLESS_AUDIO_FIXTURES) + "/" + name, std::ios::binary);
    return Bytes(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}
} // namespace

TEST(AudioFiles, TheExtensionPicksTheFormatAndNothingElseDoes)
{
    EXPECT_EQ(AudioFileFormatFor("hall.wav"), AudioFileFormat::kWav);
    EXPECT_EQ(AudioFileFormatFor("/responses/Hall.WAV"), AudioFileFormat::kWav);
    EXPECT_EQ(AudioFileFormatFor("take.bwf"), AudioFileFormat::kWav);
    EXPECT_EQ(AudioFileFormatFor("plate.aif"), AudioFileFormat::kAiff);
    EXPECT_EQ(AudioFileFormatFor("plate.Aiff"), AudioFileFormat::kAiff);
    EXPECT_EQ(AudioFileFormatFor("room.flac"), AudioFileFormat::kFlac);
    EXPECT_EQ(AudioFileFormatFor("wash.ogg"), AudioFileFormat::kOggVorbis);
    EXPECT_EQ(AudioFileFormatFor("cab.mp3"), AudioFileFormat::kMp3);
    EXPECT_EQ(AudioFileFormatFor("church.wir"), AudioFileFormat::kWir);
    EXPECT_EQ(AudioFileFormatFor("loop.caf"), std::nullopt);
    EXPECT_EQ(AudioFileFormatFor("noextension"), std::nullopt);
    EXPECT_EQ(AudioFileFormatFor("folder.wav/response"), std::nullopt) << "a dot in a folder's name is no extension";
}

TEST(AudioFiles, WavIntegersAreScaledByTheirWidth)
{
    // The codes at each width that test the scaling's edges: the two extremes, zero and one either side of it, at
    // every width, plain, extensible, with a chunk before the data and as RF64, mono and stereo. Every sample is its
    // code over 2^(bits - 1), exactly - 32 bits too, which a double holds whole; the 8-bit sample is offset binary.
    struct Case
    {
        int                       bits;
        std::vector<std::int64_t> samples;
    };

    const Case cases[] = {
        {8, {-128, -127, -1, 0, 1, 64, 127}},
        {16, {-32768, -32767, -1, 0, 1, 12345, 32767}},
        {24, {-8388608, -8388607, -1, 0, 1, 4194304, 8388607}},
        {32,
         {std::numeric_limits<std::int32_t>::min(), -16777217, -1, 0, 1, 16777217, 16777219,
          std::numeric_limits<std::int32_t>::max()}},
    };
    for (const Case &c : cases)
    {
        for (const int channels : {1, 2})
        {
            Pcm pcm;
            pcm.width    = c.bits / 8;
            pcm.channels = channels;
            Channels want(static_cast<std::size_t>(channels));
            for (std::size_t i = 0; i < c.samples.size(); ++i)
            {
                for (int ch = 0; ch < channels; ++ch)
                {
                    // The second channel carries the first's samples negated, where that stays in range.
                    std::int64_t sample = c.samples[i];
                    if (ch == 1 && -sample <= (std::int64_t{1} << (c.bits - 1)) - 1)
                    {
                        sample = -sample;
                    }
                    const std::int64_t code = c.bits == 8 ? sample + 128 : sample;
                    pcm.codes.push_back(static_cast<std::uint64_t>(code) & ((std::uint64_t{1} << c.bits) - 1));
                    want[static_cast<std::size_t>(ch)].push_back(Scaled(sample, c.bits));
                }
            }
            const std::string what  = std::to_string(c.bits) + " bits, " + std::to_string(channels) + " channel(s)";
            const auto        plain = Decode(reckless::test::WavBytes(pcm), AudioFileFormat::kWav);
            EXPECT_EQ(plain.sample_rate, 48000.0);
            EXPECT_EQ(plain.channels, channels);
            EXPECT_EQ(plain.length, static_cast<std::int64_t>(c.samples.size()));
            ExpectSamples(plain, want, "plain WAV, " + what);

            Pcm wide = pcm;
            wide.tag = 0xfffe;
            ExpectSamples(Decode(reckless::test::WavBytes(wide), AudioFileFormat::kWav), want, "extensible, " + what);
            ExpectSamples(Decode(reckless::test::WavBytes(pcm, false, "LIST"), AudioFileFormat::kWav), want,
                          "a chunk before the data, " + what);
            const auto rf64 = Decode(reckless::test::Rf64Bytes(pcm), AudioFileFormat::kWav);
            EXPECT_EQ(rf64.length, static_cast<std::int64_t>(c.samples.size())) << "RF64: the length from ds64";
            ExpectSamples(rf64, want, "RF64, " + what);
        }
    }
}

TEST(AudioFiles, FloatSamplesAreTheirValues)
{
    // 32 and 64-bit floats, plain and extensible: every finite value exactly, signs and subnormals included; a NaN or
    // an infinity comes through as one, for the loader to refuse.
    const std::vector<double> values = {1.0,
                                        -1.0,
                                        0.0,
                                        -0.0,
                                        0x1p-149,
                                        -0x1p-140,
                                        0.3333333333333333,
                                        2.5e9,
                                        -7.0,
                                        1e-30,
                                        0.125,
                                        std::numeric_limits<double>::infinity(),
                                        std::numeric_limits<double>::quiet_NaN()};
    for (const int width : {4, 8})
    {
        Pcm pcm;
        pcm.tag   = 3;
        pcm.width = width;
        Channels want(1);
        for (const double value : values)
        {
            if (width == 4)
            {
                pcm.codes.push_back(std::bit_cast<std::uint32_t>(static_cast<float>(value)));
                want[0].push_back(static_cast<double>(static_cast<float>(value)));
            }
            else
            {
                pcm.codes.push_back(std::bit_cast<std::uint64_t>(value));
                want[0].push_back(value);
            }
        }
        const std::string what = std::to_string(width * 8) + "-bit float WAV";
        ExpectSamples(Decode(reckless::test::WavBytes(pcm), AudioFileFormat::kWav), want, what);
        Pcm wide = pcm;
        wide.tag = 0xfffe;
        ExpectSamples(Decode(reckless::test::WavBytes(wide, true), AudioFileFormat::kWav), want, "extensible " + what);
    }
}

TEST(AudioFiles, AiffIsReadEitherWayRoundAtItsOwnRate)
{
    for (const int bits : {8, 16, 24, 32})
    {
        Pcm pcm;
        pcm.width    = bits / 8;
        pcm.channels = 2;
        Channels           want(2);
        const std::int64_t top = (std::int64_t{1} << (bits - 1)) - 1;
        for (const std::int64_t sample : {-top - 1, -top, std::int64_t{-1}, std::int64_t{0}, std::int64_t{1}, top})
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                pcm.codes.push_back(static_cast<std::uint64_t>(sample) & ((std::uint64_t{1} << bits) - 1));
                want[static_cast<std::size_t>(ch)].push_back(Scaled(sample, bits)); // AIFF's 8-bit sample is signed
            }
        }
        ExpectSamples(Decode(reckless::test::AiffBytes(pcm), AudioFileFormat::kAiff), want, "AIFF");
        ExpectSamples(Decode(reckless::test::AiffBytes(pcm, "NONE"), AudioFileFormat::kAiff), want, "AIFF-C NONE");
        ExpectSamples(Decode(reckless::test::AiffBytes(pcm, "twos"), AudioFileFormat::kAiff), want, "AIFF-C twos");
        ExpectSamples(Decode(reckless::test::AiffBytes(pcm, "sowt"), AudioFileFormat::kAiff), want, "AIFF-C sowt");
    }

    for (const int width : {4, 8})
    {
        Pcm floats;
        floats.width = width;
        Channels want(1);
        for (const double value : {0.5, -2.0, 0.0, 0.1})
        {
            floats.codes.push_back(width == 4 ? std::bit_cast<std::uint32_t>(static_cast<float>(value))
                                              : std::bit_cast<std::uint64_t>(value));
            want[0].push_back(width == 4 ? static_cast<double>(static_cast<float>(value)) : value);
        }
        for (const char *coding :
             width == 4 ? std::vector<const char *>{"fl32", "FL32"} : std::vector<const char *>{"fl64", "FL64"})
        {
            ExpectSamples(Decode(reckless::test::AiffBytes(floats, coding), AudioFileFormat::kAiff), want,
                          std::string("AIFF-C ") + coding);
        }
    }

    // The rate, an 80-bit extended float, read whole: a fraction kept, anything under 1 Hz refused.
    Pcm one;
    one.codes = {0};
    for (const double rate : {1.0, 8000.0, 11025.0, 44100.0, 44100.75, 96000.0, 192000.0, 384000.0, 2822400.0})
    {
        EXPECT_EQ(Decode(reckless::test::AiffBytes(one, nullptr, rate), AudioFileFormat::kAiff).sample_rate, rate);
    }
    EXPECT_FALSE(Decode(reckless::test::AiffBytes(one, nullptr, 0.5), AudioFileFormat::kAiff).why.empty())
        << "a rate under 1 Hz";
    EXPECT_EQ(Decode(reckless::test::AiffBytes(one, "ima4"), AudioFileFormat::kAiff).why,
              "it is AIFF-C compressed as 'ima4', which the reader does not read");
}

TEST(AudioFiles, AWirTakesItsWidthFromTheBlockAlignment)
{
    // The format's float files say 23 in the bits field; the width is the block alignment's, as for every WAV.
    struct Case
    {
        int tag;
        int width;
    };

    for (const Case c : {Case{3, 4}, Case{3, 8}, Case{1, 2}, Case{1, 3}, Case{1, 4}})
    {
        Pcm pcm;
        pcm.tag      = c.tag;
        pcm.width    = c.width;
        pcm.channels = 2;
        Channels want(2);
        for (int i = 0; i < 6; ++i)
        {
            for (int ch = 0; ch < 2; ++ch)
            {
                const double value = (ch == 0 ? 0.37 : -0.81) * (i - 2) / 3.0;
                if (c.tag == 3 && c.width == 4)
                {
                    pcm.codes.push_back(std::bit_cast<std::uint32_t>(static_cast<float>(value)));
                    want[static_cast<std::size_t>(ch)].push_back(static_cast<float>(value));
                }
                else if (c.tag == 3)
                {
                    pcm.codes.push_back(std::bit_cast<std::uint64_t>(value));
                    want[static_cast<std::size_t>(ch)].push_back(value);
                }
                else
                {
                    const int  bits   = c.width * 8;
                    const auto sample = static_cast<std::int64_t>(std::lround(value * std::ldexp(1.0, bits - 1)));
                    pcm.codes.push_back(static_cast<std::uint64_t>(sample) & ((std::uint64_t{1} << bits) - 1));
                    want[static_cast<std::size_t>(ch)].push_back(Scaled(sample, bits));
                }
            }
        }
        for (const int bits_field : {23, c.width * 8})
        {
            ExpectSamples(Decode(reckless::test::WirBytes(pcm, bits_field), AudioFileFormat::kWir), want,
                          "wir tag " + std::to_string(c.tag) + " width " + std::to_string(c.width));
        }
    }
}

TEST(AudioFiles, AWirWithAnOrdinaryRiffWaveHeadIsReadTooAndAnyOtherHeadOrTagIsRefused)
{
    // The same chunk walk serves a `RIFF <size> WAVE` head, which some of the format's files carry: the samples are
    // the same as under `wvIR <size> ver1`. A head that is neither, and a format tag that is neither PCM nor float, are
    // refused rather than guessed at - a wrong guess is noise at full scale into a convolver.
    Pcm pcm;
    pcm.tag   = 3;
    pcm.width = 4;
    Channels want(1);
    for (int i = 0; i < 128; ++i)
    {
        const float value = static_cast<float>(i - 64) / 64.0f;
        pcm.codes.push_back(std::bit_cast<std::uint32_t>(value));
        want[0].push_back(value);
    }
    ExpectSamples(Decode(reckless::test::WirBytes(pcm, 23, "RIFF", "WAVE"), AudioFileFormat::kWir), want,
                  "a RIFF/WAVE head");
    ExpectSamples(Decode(reckless::test::WirBytes(pcm, 23), AudioFileFormat::kWir), want, "the format's own head");
    EXPECT_EQ(Decode(reckless::test::WirBytes(pcm, 23, "JUNK", "ver1"), AudioFileFormat::kWir).why,
              "it is not a .wir file");
    EXPECT_EQ(Decode(reckless::test::WirBytes(pcm, 23, "RIFX", "WAVE"), AudioFileFormat::kWir).why,
              "it is not a .wir file")
        << "a big-endian RIFF is no head the reader takes";
    Pcm other = pcm;
    other.tag = 7; // neither PCM nor float
    EXPECT_EQ(Decode(reckless::test::WirBytes(other, 32), AudioFileFormat::kWir).why,
              "its samples are format 7 at 4 bytes, which the reader does not read");
}

TEST(AudioFiles, AWidthNotWholeBytesIsReadAsItsContainerExactly)
{
    // 20-bit samples, and 12 and 18: each at the top of the smallest container of whole bytes, its low bits zero, as
    // WAVE and AIFF store them - read at the container's width, so each is its own value over 2^(bits - 1) exactly.
    const std::vector<std::int64_t> values = {0, 1, -1, 0x7ffff, -0x80000, 0x12345, -0x54321, 0x40000};
    for (const int bits : {20, 12, 18})
    {
        const int container = (bits + 7) / 8;
        Pcm       pcm;
        pcm.width    = container;
        pcm.channels = 2;
        Channels want(2);
        for (std::size_t i = 0; i < values.size(); ++i)
        {
            const std::int64_t fitted = values[i] % (std::int64_t{1} << (bits - 1)); // within the width
            pcm.codes.push_back(static_cast<std::uint64_t>(fitted << (container * 8 - bits)) &
                                ((std::uint64_t{1} << (container * 8)) - 1));
            want[i % 2].push_back(Scaled(fitted, bits));
        }
        Bytes wav = reckless::test::WavBytes(pcm);
        wav[34]   = static_cast<std::uint8_t>(bits); // the bits field; the block alignment counts the containers
        ExpectSamples(Decode(wav, AudioFileFormat::kWav), want, std::to_string(bits) + "-bit WAV");

        // AIFF, big-endian: its sample size field the width, the container its own rule.
        Bytes aiff = reckless::test::AiffBytes(pcm);
        aiff[26]   = 0;
        aiff[27]   = static_cast<std::uint8_t>(bits);
        ExpectSamples(Decode(aiff, AudioFileFormat::kAiff), want, std::to_string(bits) + "-bit AIFF");

        // A WAV whose block alignment is no whole number of samples a frame is refused.
        Bytes misaligned = wav;
        misaligned[32]   = static_cast<std::uint8_t>(2 * container + 1);
        EXPECT_FALSE(Decode(misaligned, AudioFileFormat::kWav).why.empty()) << bits << "-bit, block alignment off";
    }
}

TEST(AudioFiles, AWavsChunksAreWalkedToItsDataWhateverTheyAreAndTheRiffSizeSays)
{
    // Any chunk before the data is stepped over - an empty one of an unknown id, an odd-length one and its pad byte -
    // and the RIFF size is not read: a streaming writer leaves it zero.
    Writer w;
    w.Raw("RIFF").Le(0, 4).Raw("WAVE").Raw("junk").Le(0, 4).Raw("fmt ").Le(16, 4).Le(1, 2).Le(1, 2).Le(48000, 4);
    w.Le(96000, 4).Le(2, 2).Le(16, 2).Raw("odd ").Le(3, 4).Raw("abc").Byte(0).Raw("data").Le(4, 4);
    w.Le(0x1234, 2).Le(0xfedc, 2);
    ExpectSamples(Decode(w.Take(), AudioFileFormat::kWav), {{Scaled(0x1234, 16), Scaled(-0x124, 16)}},
                  "a WAV with an empty chunk, an odd one and no RIFF size");

    // The data must come after the format that describes it.
    Writer early;
    early.Raw("RIFF").Le(0, 4).Raw("WAVE").Raw("data").Le(2, 4).Le(0x1234, 2).Raw("fmt ").Le(16, 4).Le(1, 2).Le(1, 2);
    early.Le(48000, 4).Le(96000, 4).Le(2, 2).Le(16, 2);
    EXPECT_EQ(Decode(early.Take(), AudioFileFormat::kWav).why, "its data comes before its format");
}

TEST(AudioFiles, AFileThatCannotBeDecodedWholeIsRefused)
{
    Pcm pcm;
    pcm.codes = {1, 2, 3, 4, 5, 6, 7, 8};

    // Encodings the reader does not read: 16-bit floats, a format tag of its own, an extensible sub-format that is
    // neither PCM nor float, a compressed AIFF-C (above).
    Pcm sixteen_float = pcm;
    sixteen_float.tag = 3;
    EXPECT_EQ(Decode(reckless::test::WavBytes(sixteen_float), AudioFileFormat::kWav).why,
              "its samples are format 3 at 2 bytes, which the reader does not read");
    Pcm adpcm = pcm;
    adpcm.tag = 2;
    EXPECT_FALSE(Decode(reckless::test::WavBytes(adpcm), AudioFileFormat::kWav).why.empty()) << "ADPCM";
    Pcm wide   = pcm;
    wide.tag   = 0xfffe;
    Bytes guid = reckless::test::WavBytes(wide);
    guid[44]   = 0x55; // the sub-format's first byte
    EXPECT_FALSE(Decode(guid, AudioFileFormat::kWav).why.empty()) << "an extensible sub-format of another kind";

    // Cut short anywhere, from the header to the last sample.
    for (const auto &[name, format, whole] :
         {std::tuple{"WAV", AudioFileFormat::kWav, reckless::test::WavBytes(pcm)},
          std::tuple{"AIFF", AudioFileFormat::kAiff, reckless::test::AiffBytes(pcm)},
          std::tuple{".wir", AudioFileFormat::kWir, reckless::test::WirBytes(pcm, 16)}})
    {
        ASSERT_TRUE(Decode(whole, format).why.empty()) << name;
        for (std::size_t size = 0; size < whole.size(); ++size)
        {
            const Bytes cut(whole.begin(), whole.begin() + static_cast<std::ptrdiff_t>(size));
            EXPECT_FALSE(Decode(cut, format).why.empty()) << name << " cut to " << size << " bytes";
        }
    }

    // An AIFF whose SSND chunk holds fewer samples than its COMM chunk promises, though the file goes on past it.
    Bytes short_sound = reckless::test::AiffBytes(pcm);
    short_sound.insert(short_sound.end(), {'J', 'U', 'N', 'K', 0, 0, 0, 4, 0, 0, 0, 0});
    short_sound[45] = static_cast<std::uint8_t>(short_sound[45] - 2); // the SSND length, one sample short
    EXPECT_EQ(Decode(short_sound, AudioFileFormat::kAiff).why, "the file ends before the samples its header promises");

    // An RF64 file's sizes are in its ds64 chunk, which comes first: without it the file has no size, and with a data
    // size past any file it has none either.
    Bytes no_ds64 = reckless::test::Rf64Bytes(pcm);
    no_ds64[12]   = 'J';
    EXPECT_FALSE(Decode(no_ds64, AudioFileFormat::kWav).why.empty()) << "an RF64 file without its ds64 chunk";
    Bytes endless = reckless::test::Rf64Bytes(pcm);
    for (std::size_t i = 28; i < 36; ++i)
    {
        endless[i] = 0xff;
    }
    EXPECT_EQ(Decode(endless, AudioFileFormat::kWav).why, "the file ends before the samples its header promises");

    // The wrong format for the extension: no sniffing.
    EXPECT_FALSE(Decode(reckless::test::WavBytes(pcm), AudioFileFormat::kAiff).why.empty());
    EXPECT_FALSE(Decode(reckless::test::AiffBytes(pcm), AudioFileFormat::kWav).why.empty());

    // Empty and nonsense, in every format.
    for (const AudioFileFormat format : {AudioFileFormat::kWav, AudioFileFormat::kAiff, AudioFileFormat::kFlac,
                                         AudioFileFormat::kOggVorbis, AudioFileFormat::kMp3, AudioFileFormat::kWir})
    {
        EXPECT_FALSE(Decode({}, format).why.empty());
        Bytes        nonsense(4096);
        std::mt19937 random(7);
        for (auto &byte : nonsense)
        {
            byte = static_cast<std::uint8_t>(random() & 0xff);
        }
        EXPECT_FALSE(Decode(nonsense, format).why.empty()) << static_cast<int>(format);
    }
}

TEST(AudioFiles, TheMp3sAreTrimmedToTheSamplesThatWereEncoded)
{
    // The carried files (tests/audio, made by make_mp3_fixtures.py from its signals): where the file carries the
    // encoder's Info or Xing frame with its LAME tag, the decoder's delay and the encoder's padding are trimmed and the
    // file gives back as many samples as were encoded - the signal's seconds at its rate - with its onset, a response's
    // loudest instant, at the start; the file with no tag gives every frame it holds, 1152 samples each, the delay
    // before the onset included. Each read whole, its peak near the signal's 0.9 or 0.95 - lower where the codec has
    // taken the top off the noise.
    struct Case
    {
        const char  *name;
        double       rate;
        int          channels;
        std::int64_t length;
        bool         trimmed;
    };

    const Case cases[] = {
        {"cabinet_mono_44k1_cbr128.mp3", 44100.0, 1, 11025, true}, // 0.25 s
        {"room_stereo_48k_vbr.mp3", 48000.0, 2, 36000, true},      // 0.75 s
        {"room_stereo_44k1_cbr128_tags.mp3", 44100.0, 2, 21 * 1152, false},
        {"cabinet_mono_22k05_lsf.mp3", 22050.0, 1, 11025, true}, // 0.5 s, MPEG-2's 576-sample frames
        {"chords_mono_32k_v5.mp3", 32000.0, 1, 83200, true},     // 2.6 s
    };
    for (const Case &c : cases)
    {
        const auto decoded = Decode(ReadFixture(c.name), AudioFileFormat::kMp3);
        ASSERT_EQ(decoded.why, "") << c.name;
        EXPECT_EQ(decoded.sample_rate, c.rate) << c.name;
        EXPECT_EQ(decoded.channels, c.channels) << c.name;
        EXPECT_EQ(decoded.length, c.length) << c.name;
        double peak = 0.0;
        for (const auto &channel : decoded.samples)
        {
            for (const double s : channel)
            {
                peak = std::max(peak, std::fabs(s));
            }
        }
        std::int64_t onset = decoded.length;
        for (const auto &channel : decoded.samples)
        {
            const auto loud =
                std::find_if(channel.begin(), channel.end(), [peak](double s) { return std::fabs(s) > 0.1 * peak; });
            onset = std::min<std::int64_t>(onset, loud - channel.begin());
        }
        std::printf("  %-34s %6.0f Hz, %d ch, %6lld samples, onset at %4lld, peak %.3f\n", c.name, c.rate, c.channels,
                    static_cast<long long>(decoded.length), static_cast<long long>(onset), peak);
        EXPECT_GT(peak, 0.7) << c.name;
        EXPECT_LT(peak, 1.05) << c.name;
        if (c.trimmed)
        {
            EXPECT_LT(onset, 128) << c.name << ": the delay trimmed";
        }
        else
        {
            EXPECT_GT(onset, 512) << c.name << ": the encoder's and decoder's delay of some 1100 samples kept";
        }
    }
}

namespace
{
/** The bytes of the MPEG-1 layer III frame at `pos`: 144 x its bitrate over its rate, and its padding byte. */
std::size_t Mpeg1Layer3FrameBytes(const Bytes &mp3, std::size_t pos)
{
    static constexpr int kKbps[16] = {0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0};
    static constexpr int kRates[4] = {44100, 48000, 32000, 0};
    return static_cast<std::size_t>(144000 * kKbps[mp3[pos + 2] >> 4] / kRates[(mp3[pos + 2] >> 2) & 3] +
                                    ((mp3[pos + 2] >> 1) & 1));
}

/** Where the run of MPEG-1 layer III frames back to back from `from` ends, and how many there are. */
std::size_t EndOfMpeg1Layer3Frames(const Bytes &mp3, std::size_t from, int &frames)
{
    std::size_t pos = from;
    for (frames = 0; pos + 4 <= mp3.size() && mp3[pos] == 0xff && (mp3[pos + 1] & 0xfe) == 0xfa; ++frames)
    {
        pos += Mpeg1Layer3FrameBytes(mp3, pos);
    }
    return pos;
}
} // namespace

TEST(AudioFiles, AnMp3WhoseFramesChangeTheirChannelsPartWayIsRefused)
{
    // The carried stereo file with no tag, the carried mono file's audio frames - both 128 kbps at 44.1 kHz - spliced
    // in after its own and before its ID3v1 tag: one stream to the decoder, whose frames go from two channels to one.
    // The reader refuses the file there, where the decoder would lay one channel's samples out as two.
    const Bytes stereo = ReadFixture("room_stereo_44k1_cbr128_tags.mp3");
    const Bytes mono   = ReadFixture("cabinet_mono_44k1_cbr128.mp3");
    ASSERT_GT(stereo.size(), 10u);
    ASSERT_EQ(std::string(stereo.begin(), stereo.begin() + 3), "ID3");
    const std::size_t tag =
        10 + (static_cast<std::size_t>(stereo[6]) << 21 | static_cast<std::size_t>(stereo[7]) << 14 |
              static_cast<std::size_t>(stereo[8]) << 7 | static_cast<std::size_t>(stereo[9]));
    int               stereo_frames = 0, mono_frames = 0;
    const std::size_t stereo_end = EndOfMpeg1Layer3Frames(stereo, tag, stereo_frames);
    const std::size_t mono_start = Mpeg1Layer3FrameBytes(mono, 0); // past its Info frame
    const std::size_t mono_end   = EndOfMpeg1Layer3Frames(mono, mono_start, mono_frames);
    ASSERT_EQ(stereo_frames, 21) << "the stereo file's frames, back to back after its ID3v2 tag";
    ASSERT_EQ(mono_frames, 11) << "the mono file's audio frames";
    ASSERT_EQ(mono_end, mono.size());
    ASSERT_EQ(std::string(stereo.begin() + static_cast<std::ptrdiff_t>(stereo_end),
                          stereo.begin() + static_cast<std::ptrdiff_t>(stereo_end) + 3),
              "TAG");

    Bytes spliced(stereo.begin(), stereo.begin() + static_cast<std::ptrdiff_t>(stereo_end));
    spliced.insert(spliced.end(), mono.begin() + static_cast<std::ptrdiff_t>(mono_start), mono.end());
    spliced.insert(spliced.end(), stereo.begin() + static_cast<std::ptrdiff_t>(stereo_end), stereo.end());
    std::string why;
    const auto  reader = AudioFileReader::Open(spliced, AudioFileFormat::kMp3, why);
    ASSERT_NE(reader, nullptr) << why;
    EXPECT_EQ(reader->Info().channels, 2);
    EXPECT_EQ(reader->Info().length, (21 + 11) * 1152) << "every frame counted";
    Channels samples;
    EXPECT_EQ(reader->Read(21 * 1152, 2, samples), "") << "the stereo frames alone";
    EXPECT_EQ(reader->Read(21 * 1152 + 1, 2, samples), "its MPEG frames change rate or channels part-way");
}

TEST(AudioFiles, TheCarriedFlacAndOggReadToTheReferenceDecodersSamples)
{
    // The FLAC and Ogg Vorbis files tests/audio carries, beside the samples the reference decoders gave for them
    // (reckless::test::kCarriedFiles): each opened with its rate, channels and length and read whole - FLAC, which is
    // lossless, to the bit, through every frame of the stream and their numbering, Ogg Vorbis within
    // kCarriedVorbisTolerance; a read of the first channel alone, stopping part way into a frame, the start of the
    // same samples; and a FLAC file whose header gives no count, its samples counted by decoding the stream, to the
    // same length and the same samples.
    for (const auto &carried : reckless::test::kCarriedFiles)
    {
        const auto format = carried.vorbis ? AudioFileFormat::kOggVorbis : AudioFileFormat::kFlac;
        const auto expected =
            reckless::test::FloatsOf(ReadFixture(std::string(carried.name) + reckless::test::kDecodedSuffix));
        ASSERT_EQ(expected.size(), static_cast<std::size_t>(carried.frames * carried.channels)) << carried.name;
        std::string why;
        auto        reader = AudioFileReader::Open(ReadFixture(carried.name), format, why);
        ASSERT_NE(reader, nullptr) << carried.name << ": " << why;
        EXPECT_EQ(reader->Info().sample_rate, carried.rate) << carried.name;
        EXPECT_EQ(reader->Info().channels, carried.channels) << carried.name;
        EXPECT_EQ(reader->Info().length, carried.frames) << carried.name;
        Channels out;
        ASSERT_EQ(reader->Read(carried.frames, carried.channels, out), "") << carried.name;
        ASSERT_EQ(out.size(), static_cast<std::size_t>(carried.channels));
        double worst = 0.0;
        for (int c = 0; c < carried.channels; ++c)
        {
            ASSERT_EQ(out[static_cast<std::size_t>(c)].size(), static_cast<std::size_t>(carried.frames));
            for (int i = 0; i < carried.frames; ++i)
            {
                const double read      = out[static_cast<std::size_t>(c)][static_cast<std::size_t>(i)];
                const double reference = expected[static_cast<std::size_t>(i * carried.channels + c)];
                if (!carried.vorbis)
                {
                    ASSERT_EQ(read, reference) << carried.name << ", channel " << c << ", sample " << i;
                }
                worst = std::max(worst, std::fabs(read - reference));
            }
        }
        EXPECT_LE(worst, reckless::test::kCarriedVorbisTolerance) << carried.name;
        std::printf("  %-40s %d x %d at %.0f Hz: worst %.3g of the reference decoder's samples\n", carried.name,
                    carried.channels, carried.frames, carried.rate, worst);

        // Part way into a frame: past the first of a level-0 file's frames of 1152, inside a level-8 file's first.
        Channels  part;
        const int some = 1234;
        ASSERT_EQ(reader->Read(some, 1, part), "") << carried.name;
        ASSERT_EQ(part.size(), 1u);
        ASSERT_EQ(part[0].size(), static_cast<std::size_t>(some));
        for (int i = 0; i < some; ++i)
        {
            ASSERT_EQ(part[0][static_cast<std::size_t>(i)], out[0][static_cast<std::size_t>(i)])
                << carried.name << ": the first channel's start, sample " << i;
        }

        if (!carried.vorbis)
        {
            const auto uncounted = Decode(reckless::test::WithoutFlacCount(ReadFixture(carried.name)), format);
            EXPECT_EQ(uncounted.length, carried.frames) << carried.name << " with no count";
            ExpectSamples(uncounted, out, std::string(carried.name) + " with no count");
        }
    }
}

TEST(AudioFiles, EveryFileCutShortOrDamagedIsReadWithoutAFault)
{
    // Every prefix of every file, and a thousand damaged copies of each - one to four bytes overwritten - opened and,
    // where they open, read whole: none may fault, read out of bounds or hang. A cut through the samples of an
    // uncompressed file is refused; a cut MP3 or damaged bytes may decode or not, as their structure allows. The
    // address sanitizer, where the suite is built with it, holds the bounds; here it is the absence of a crash.
    // The carried FLAC and Ogg Vorbis files are among them; RECKLESS_AUDIO_FUZZ names a directory whose files join the
    // list by their extensions - more FLAC and Ogg files, which this suite has no encoder for.
    Pcm stereo;
    stereo.channels = 2;
    stereo.width    = 3;
    for (int i = 0; i < 64; ++i)
    {
        stereo.codes.push_back(static_cast<std::uint64_t>(i * 104729) & 0xffffff);
    }

    struct File
    {
        std::string     name;
        AudioFileFormat format;
        Bytes           bytes;
    };

    std::vector<File> files = {
        {"WAV", AudioFileFormat::kWav, reckless::test::WavBytes(stereo)},
        {"AIFF", AudioFileFormat::kAiff, reckless::test::AiffBytes(stereo)},
        {"AIFF-C", AudioFileFormat::kAiff, reckless::test::AiffBytes(stereo, "sowt")},
        {".wir", AudioFileFormat::kWir, reckless::test::WirBytes(stereo, 23)},
    };
    for (const char *name : {"cabinet_mono_44k1_cbr128.mp3", "room_stereo_44k1_cbr128_tags.mp3"})
    {
        files.push_back({name, AudioFileFormat::kMp3, ReadFixture(name)});
    }
    for (const auto &carried : reckless::test::kCarriedFiles)
    {
        files.push_back({carried.name, carried.vorbis ? AudioFileFormat::kOggVorbis : AudioFileFormat::kFlac,
                         ReadFixture(carried.name)});
    }
    // MSVC deprecates getenv for _dupenv_s, whose copy a test has no use for.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    if (const char *directory = std::getenv("RECKLESS_AUDIO_FUZZ"); directory != nullptr)
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    {
        for (const auto &entry : std::filesystem::directory_iterator(directory))
        {
            const std::string name   = entry.path().filename().string();
            const auto        format = AudioFileFormatFor(name);
            if (!format.has_value())
            {
                continue;
            }
            std::ifstream in(entry.path(), std::ios::binary);
            files.push_back({name, *format, Bytes(std::istreambuf_iterator<char>(in), {})});
        }
    }
    std::mt19937 random(2026);
    int          opened = 0, tried = 0;
    for (const File &file : files)
    {
        ASSERT_FALSE(file.bytes.empty()) << file.name;
        const std::size_t step = std::max<std::size_t>(1, file.bytes.size() / 2048);
        for (std::size_t size = 0; size <= file.bytes.size(); size += step)
        {
            const Bytes cut(file.bytes.begin(), file.bytes.begin() + static_cast<std::ptrdiff_t>(size));
            const auto  decoded = Decode(cut, file.format);
            opened += decoded.why.empty() ? 1 : 0;
            ++tried;
            if (file.format != AudioFileFormat::kMp3 && size < file.bytes.size())
            {
                EXPECT_FALSE(decoded.why.empty()) << file.name << " cut to " << size << " bytes";
            }
        }
        for (int copy = 0; copy < 1000; ++copy)
        {
            Bytes      damaged = file.bytes;
            const auto hits    = 1 + random() % 4;
            for (unsigned h = 0; h < hits; ++h)
            {
                damaged[random() % damaged.size()] = static_cast<std::uint8_t>(random() & 0xff);
            }
            opened += Decode(damaged, file.format).why.empty() ? 1 : 0;
            ++tried;
        }
    }
    std::printf("  %d cut or damaged files decoded without a fault, %d of them whole\n", tried, opened);
}

// ---- The WAV writer -------------------------------------------------------------------------------------------

namespace
{
using reckless::core::EncodeWav;
using reckless::core::WavSampleFormat;

struct WavLayout
{
    WavSampleFormat format;
    const char     *name;
    int             bits;      ///< valid bits a sample
    std::uint32_t   container; ///< bits a sample takes in the file
    std::uint16_t   tag;       ///< the `fmt ` chunk's format tag
};

constexpr WavLayout kWavLayouts[] = {
    {WavSampleFormat::kPcm16, "16-bit", 16, 16, 1},  {WavSampleFormat::kPcm20, "20-bit", 20, 24, 0xfffe},
    {WavSampleFormat::kPcm24, "24-bit", 24, 24, 1},  {WavSampleFormat::kPcm32, "32-bit", 32, 32, 1},
    {WavSampleFormat::kFloat32, "float", 32, 32, 3},
};

std::uint16_t At16(const Bytes &b, std::size_t at)
{
    return static_cast<std::uint16_t>(b[at] | b[at + 1] << 8);
}

std::uint32_t At32(const Bytes &b, std::size_t at)
{
    return static_cast<std::uint32_t>(At16(b, at)) | static_cast<std::uint32_t>(At16(b, at + 2)) << 16;
}

std::string At4(const Bytes &b, std::size_t at)
{
    return std::string(reinterpret_cast<const char *>(b.data() + at), 4);
}

/** What the reader should hand back for a sample written in `layout`: the sample scaled by 2^(bits - 1), rounded to
    the nearest code and held to the format's range, over 2^(bits - 1) again; a float the sample rounded to float. */
double WrittenAndRead(double sample, const WavLayout &layout)
{
    if (layout.tag == 3)
    {
        return static_cast<double>(static_cast<float>(sample));
    }
    const double full = std::ldexp(1.0, layout.bits - 1);
    return std::clamp(std::round(sample * full), -full, full - 1.0) / full;
}
} // namespace

TEST(AudioFiles, TheWavWriterLaysOutEveryFormatFieldByField)
{
    // RIFF, a `fmt ` chunk of 16 bytes - 40, extensible, for the 20-bit file, whose valid bits are fewer than its
    // container's - and the data chunk counting the samples alone, the RIFF size the pad byte after an odd count too.
    for (const WavLayout &layout : kWavLayouts)
    {
        for (const std::size_t channels : {std::size_t{1}, std::size_t{2}})
        {
            for (const std::size_t frames : {std::size_t{5}, std::size_t{6}})
            {
                const Channels    in(channels, std::vector<double>(frames, 0.25));
                const Bytes       b     = EncodeWav(in, 44100.0, layout.format);
                const auto        block = static_cast<std::uint32_t>(channels) * layout.container / 8;
                const auto        data  = block * frames;
                const auto        pad   = data & 1;
                const bool        wide  = layout.tag == 0xfffe;
                const auto        what  = std::string(layout.name) + ", " + std::to_string(channels) + " channel(s), " +
                                          std::to_string(frames) + " frames";
                const std::size_t header = wide ? 68 : 44;
                ASSERT_EQ(b.size(), header + data + pad) << what;
                EXPECT_EQ(At4(b, 0), "RIFF") << what;
                EXPECT_EQ(At32(b, 4), b.size() - 8) << what;
                EXPECT_EQ(At4(b, 8), "WAVE") << what;
                EXPECT_EQ(At4(b, 12), "fmt ") << what;
                EXPECT_EQ(At32(b, 16), wide ? 40u : 16u) << what;
                EXPECT_EQ(At16(b, 20), layout.tag) << what;
                EXPECT_EQ(At16(b, 22), channels) << what;
                EXPECT_EQ(At32(b, 24), 44100u) << what;
                EXPECT_EQ(At32(b, 28), 44100u * block) << what;
                EXPECT_EQ(At16(b, 32), block) << what;
                EXPECT_EQ(At16(b, 34), layout.container) << what;
                if (wide)
                {
                    EXPECT_EQ(At16(b, 36), 22u) << what;
                    EXPECT_EQ(At16(b, 38), 20u) << what << ": the valid bits";
                    EXPECT_EQ(At32(b, 40), channels == 1 ? 0x4u : 0x3u) << what << ": the speaker mask";
                    EXPECT_EQ(At32(b, 44), 1u) << what << ": the PCM subformat";
                }
                EXPECT_EQ(At4(b, header - 8), "data") << what;
                EXPECT_EQ(At32(b, header - 4), data) << what;
                if (pad != 0)
                {
                    EXPECT_EQ(b.back(), 0) << what;
                }
            }
        }
    }
    // A rate that is no whole number goes in rounded, the byte rate from it.
    const Bytes b = EncodeWav({{0.0, 0.0}}, 44100.6, WavSampleFormat::kPcm24);
    EXPECT_EQ(At32(b, 24), 44101u);
    EXPECT_EQ(At32(b, 28), 3u * 44101u);
}

TEST(AudioFiles, TheWavWriterRoundsEverySampleToItsFormatAndTheReaderReadsItBack)
{
    // Full scale and past it, the half codes rounding meets, the smallest values, and noise past full scale: each
    // sample written in every format and read back to its rounding; and what is read back, written again, comes back
    // the same.
    std::vector<double> samples = {0.0,       1.0,           -1.0,           1.5,           -1.5,         0.5,
                                   -0.5,      1.0e-9,        -1.0e-9,        0x1p-31,       -0x1p-31,     0.999999,
                                   -0.999999, 1.0 / 32768.0, -1.0 / 32768.0, 0.5 / 32768.0, 1.5 / 32768.0};
    std::mt19937_64     random(20261003);
    std::uniform_real_distribution<double> noise(-1.2, 1.2);
    for (int i = 0; i < 4000; ++i)
    {
        samples.push_back(noise(random));
    }
    std::vector<double> other(samples.rbegin(), samples.rend());
    for (const WavLayout &layout : kWavLayouts)
    {
        for (const std::size_t channels : {std::size_t{1}, std::size_t{2}})
        {
            Channels in = {samples};
            if (channels == 2)
            {
                in.push_back(other);
            }
            Channels want(channels);
            for (std::size_t c = 0; c < channels; ++c)
            {
                for (double s : in[c])
                {
                    want[c].push_back(WrittenAndRead(s, layout));
                }
            }
            const Decoded got = Decode(EncodeWav(in, 48000.0, layout.format), AudioFileFormat::kWav);
            EXPECT_EQ(got.sample_rate, 48000.0) << layout.name;
            EXPECT_EQ(got.channels, static_cast<int>(channels)) << layout.name;
            EXPECT_EQ(got.length, static_cast<std::int64_t>(samples.size())) << layout.name;
            ExpectSamples(got, want, std::string(layout.name) + " written and read back");
            ExpectSamples(Decode(EncodeWav(got.samples, 48000.0, layout.format), AudioFileFormat::kWav), want,
                          std::string(layout.name) + " read back, written again and read back");
        }
    }
    EXPECT_EQ(WrittenAndRead(1.0, kWavLayouts[0]), 32767.0 / 32768.0) << "full scale clips to the largest code";
    EXPECT_EQ(WrittenAndRead(-1.0, kWavLayouts[0]), -1.0);
    std::printf(
        "  %zu samples in each of the five formats, mono and stereo: rounded to the format, read back exactly\n",
        samples.size());
}

TEST(AudioFiles, TheWavWriterRefusesWhatItCannotWrite)
{
    const std::vector<double> one = {0.1, 0.2};
    EXPECT_TRUE(EncodeWav({}, 48000.0, WavSampleFormat::kPcm24).empty()) << "no channels";
    EXPECT_TRUE(EncodeWav({one, one, one}, 48000.0, WavSampleFormat::kPcm24).empty()) << "three channels";
    EXPECT_TRUE(EncodeWav({one, {0.1}}, 48000.0, WavSampleFormat::kPcm24).empty()) << "unequal lengths";
    EXPECT_TRUE(EncodeWav({{0.1, std::nan("")}}, 48000.0, WavSampleFormat::kFloat32).empty()) << "a NaN";
    EXPECT_TRUE(EncodeWav({{std::numeric_limits<double>::infinity()}}, 48000.0, WavSampleFormat::kPcm16).empty())
        << "an infinity";
    for (const double rate : {0.0, 0.5, -48000.0, std::nan(""), 1.0e12})
    {
        EXPECT_TRUE(EncodeWav({one}, rate, WavSampleFormat::kPcm24).empty()) << "a rate of " << rate;
    }
    EXPECT_EQ(EncodeWav({{}}, 48000.0, WavSampleFormat::kPcm24).size(), 44u) << "no frames: the header alone";
}
