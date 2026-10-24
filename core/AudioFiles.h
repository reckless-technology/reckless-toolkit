#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reckless::core
{
/*
    Audio files decoded: the six formats the Cab's and the Space's loaders read - WAV (BWF and RF64 with it), AIFF (and
    AIFF-C), FLAC, Ogg Vorbis, MP3 and `.wir` - from a file's bytes to a rate and channels of samples in double. WAV,
    AIFF and `.wir` are read here; FLAC is decoded by dr_flac, Ogg Vorbis by stb_vorbis and MP3 by dr_mp3, single-file
    decoders compiled once in AudioDecoders.c. It writes one format too: WAV, the impulse-response exports' (EncodeWav,
    at the end).

    An integer sample is scaled to [-1, 1) by its width: a sample s of b bits reads s / 2^(b-1), the 8-bit WAV sample
    offset binary about 128. A sample narrower than its container - 20 bits in three bytes - sits at the container's
    top with its low bits zero, as WAVE and AIFF store it, so it is read at the container's width and lands on its own
    value exactly. A float sample, 32 or 64 bits, is its value. FLAC's integers are scaled the same way; Vorbis and MP3
    decode to floats, which are taken as they come. MP3 is trimmed of the encoder's delay and padding where the file
    says what they are (a LAME tag), so a tagged file gives back the samples that were encoded, no more.

    A file that cannot be decoded whole is refused, never padded or patched: a file cut short of the samples its header
    promises, a FLAC frame or an Ogg page that is damaged or missing, a second Ogg stream, a Vorbis stream whose samples
    do not run with its page positions, an MP3 whose frames change their rate or channels, a sample encoding the reader
    does not know. The core's suite holds it (tests/test_audio_files.cpp).
*/

/** The six formats the loaders read. */
enum class AudioFileFormat
{
    kWav,       ///< .wav and .bwf: RIFF WAVE and RF64, 8 to 32-bit PCM and 32 and 64-bit float
    kAiff,      ///< .aif and .aiff: AIFF and AIFF-C, 1 to 32-bit PCM either way round and 32 and 64-bit float
    kFlac,      ///< .flac: native FLAC
    kOggVorbis, ///< .ogg: one Vorbis stream
    kMp3,       ///< .mp3: MPEG audio, layers I to III
    kWir,       ///< .wir: RIFF WAVE under its own magic
};

/** The extensions the six formats claim, lower case with the dot: the chooser's patterns and the drop target's test
    (ResponseFiles.h), and the format a file is read as. */
inline constexpr std::array<std::pair<std::string_view, AudioFileFormat>, 8> kAudioFileExtensions{{
    {".wav", AudioFileFormat::kWav},
    {".bwf", AudioFileFormat::kWav},
    {".aif", AudioFileFormat::kAiff},
    {".aiff", AudioFileFormat::kAiff},
    {".flac", AudioFileFormat::kFlac},
    {".ogg", AudioFileFormat::kOggVorbis},
    {".mp3", AudioFileFormat::kMp3},
    {".wir", AudioFileFormat::kWir},
}};

/** The format a file is read as, by its name's extension and nothing else, case ignored: a file whose content is
    another format's is refused rather than sniffed. Empty when no format claims the extension.
    @param file_name  the file's name or path, UTF-8; only what follows its last dot is read
    @return           the format, or nothing when no format claims the extension */
std::optional<AudioFileFormat> AudioFileFormatFor(std::string_view file_name);

/** What a file's header says of its samples. */
struct AudioFileInfo
{
    double       sample_rate{0.0}; ///< the file's rate, in hertz
    int          channels{0};      ///< the channels the file holds
    std::int64_t length{0};        ///< frames; zero for a file with no samples in it
};

/**
    A file's bytes opened as one format: its header read, nothing decoded until Read. The reader owns the bytes.
*/
class AudioFileReader
{
  public:
    /** The bytes read as `format`: a reader, or null with `why` saying why not.
        @param bytes   the file's whole contents, which the reader takes and owns
        @param format  the format to read them as
        @param why     receives the reason when the result is null, in a sentence a window can show
        @return        the reader, or null when the header is not that format's or says something the reader cannot play
     */
    static std::unique_ptr<AudioFileReader> Open(std::vector<std::uint8_t> bytes, AudioFileFormat format,
                                                 std::string &why);

    /** Destroy through the base. */
    virtual ~AudioFileReader() = default;

    AudioFileReader(const AudioFileReader &)            = delete; ///< Not copyable: the reader owns the file's bytes.
    AudioFileReader &operator=(const AudioFileReader &) = delete; ///< Not assignable.

    /** What the header says of the samples.
        @return  the rate, channels and length, valid for the reader's life */
    const AudioFileInfo &Info() const noexcept
    {
        return info_;
    }

    /** The first `frames` frames of the first `channels` channels into `out`, one vector a channel. `frames` from 1 to
        Info().length and `channels` from 1 to Info().channels. Empty on success, otherwise why the samples cannot be
        read, with `out` left as it was.
        @param frames    the frames to read from the start, 1 to Info().length
        @param channels  the channels to read, the first ones, 1 to Info().channels
        @param out       receives one vector of `frames` samples a channel, scaled as the file header describes
                         (integers to [-1, 1), floats as they are)
        @return          an empty string on success, otherwise why the samples cannot be read */
    std::string Read(std::int64_t frames, int channels, std::vector<std::vector<double>> &out);

  protected:
    /** A reader over `bytes`, whose header says `info`.
        @param bytes  the file's whole contents, taken and owned
        @param info   what the header says of the samples */
    AudioFileReader(std::vector<std::uint8_t> bytes, AudioFileInfo info) noexcept
        : bytes_(std::move(bytes)), info_(info)
    {
    }

    /** Read's work, past its checks: `out` is `channels` empty vectors to fill with `frames` samples each, or more -
        Read drops what is past `frames`.
        @param frames    the frames to decode from the start
        @param channels  the channels to decode, the first ones
        @param out       `channels` empty vectors to fill with the samples
        @return          an empty string on success, otherwise why the samples cannot be decoded */
    virtual std::string Decode(std::int64_t frames, int channels, std::vector<std::vector<double>> &out) = 0;

    std::vector<std::uint8_t> bytes_; ///< the file's whole contents
    AudioFileInfo             info_;  ///< what the header says of the samples
};

/** The sample formats the WAV writer writes. */
enum class WavSampleFormat
{
    kPcm16,   ///< 16-bit integers
    kPcm20,   ///< 20-bit integers, each at the top of a three-byte container with its low four bits zero
    kPcm24,   ///< 24-bit integers
    kPcm32,   ///< 32-bit integers
    kFloat32, ///< 32-bit IEEE floats
};

/**
    A WAV file's bytes: `channels` (one or two, each as long as the first) at `sample_rate`, rounded to whole hertz, in
    `format`. The file is a RIFF header, a `fmt ` chunk, the interleaved samples in a `data` chunk and a zero byte after
    an odd number of them. The `fmt ` chunk is the plain one (WAVE_FORMAT_PCM, or WAVE_FORMAT_IEEE_FLOAT for floats) but
    for 20 bits, whose valid bits are fewer than its container's: that is WAVE_FORMAT_EXTENSIBLE, 24 bits a container
    and 20 of them valid, the speaker mask mono's or stereo's.

    An integer sample is the double scaled by 2^(b-1) for b bits, rounded to the nearest integer and held to the
    format's range, so full scale clips to the largest code and any sample the reader gives back is written back
    exactly; a 20-bit sample is put at the top of its container. A float sample is the double rounded to float.

    Empty when the channels cannot be written: none or more than two, unequal lengths, a sample that is not finite, a
    rate under 1 Hz or past what the header holds, more data than RIFF's sizes count, or no memory for it.
    @param channels     the samples, one vector a channel, full scale at 1
    @param sample_rate  the rate, in hertz, rounded to whole hertz
    @param format       the sample format to write
    @return             the file's bytes, or an empty vector when the channels cannot be written
*/
std::vector<std::uint8_t> EncodeWav(const std::vector<std::vector<double>> &channels, double sample_rate,
                                    WavSampleFormat format);
} // namespace reckless::core
