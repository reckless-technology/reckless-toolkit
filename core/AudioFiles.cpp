#include "AudioFiles.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstring>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

// The vendored decoders' interfaces. Their implementations compile in AudioDecoders.c, under the configuration both
// files take from the reckless_audio_decoders target (no stdio, no Ogg FLAC, MP3 decoded to float, Vorbis pulled
// from memory).
#include "dr_flac.h"
#include "dr_mp3.h"
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace reckless::core
{
namespace
{
using Byte = std::uint8_t; ///< one byte of a file

// ---------------------------------------------------------------------------------------------------------------
// Bytes
// ---------------------------------------------------------------------------------------------------------------

/** A chunk's four-character id as one number: the bytes in file order, little-endian.
    @param name  the four characters, as a string literal
    @return      the id, the first character in the low byte */
constexpr std::uint32_t Id(const char (&name)[5]) noexcept
{
    return static_cast<std::uint32_t>(static_cast<Byte>(name[0])) |
           static_cast<std::uint32_t>(static_cast<Byte>(name[1])) << 8 |
           static_cast<std::uint32_t>(static_cast<Byte>(name[2])) << 16 |
           static_cast<std::uint32_t>(static_cast<Byte>(name[3])) << 24;
}

/** Two bytes as a little-endian number.
    @param p  the first byte, at least two readable
    @return   the number */
std::uint16_t Le16(const Byte *p) noexcept
{
    return static_cast<std::uint16_t>(p[0] | p[1] << 8);
}

/** Four bytes as a little-endian number.
    @param p  the first byte, at least four readable
    @return   the number */
std::uint32_t Le32(const Byte *p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) | static_cast<std::uint32_t>(p[1]) << 8 |
           static_cast<std::uint32_t>(p[2]) << 16 | static_cast<std::uint32_t>(p[3]) << 24;
}

/** Eight bytes as a little-endian number.
    @param p  the first byte, at least eight readable
    @return   the number */
std::uint64_t Le64(const Byte *p) noexcept
{
    return static_cast<std::uint64_t>(Le32(p)) | static_cast<std::uint64_t>(Le32(p + 4)) << 32;
}

/** Two bytes as a big-endian number.
    @param p  the first byte, at least two readable
    @return   the number */
std::uint16_t Be16(const Byte *p) noexcept
{
    return static_cast<std::uint16_t>(p[0] << 8 | p[1]);
}

/** Four bytes as a big-endian number.
    @param p  the first byte, at least four readable
    @return   the number */
std::uint32_t Be32(const Byte *p) noexcept
{
    return static_cast<std::uint32_t>(p[0]) << 24 | static_cast<std::uint32_t>(p[1]) << 16 |
           static_cast<std::uint32_t>(p[2]) << 8 | static_cast<std::uint32_t>(p[3]);
}

/** Whether `count` bytes from `offset` lie inside `size`, without overflow for any of the three.
    @param size    the length of the data, in bytes
    @param offset  where the run starts, in bytes
    @param count   the length of the run, in bytes
    @return        true when the run lies wholly inside the data */
bool Holds(std::uint64_t size, std::uint64_t offset, std::uint64_t count) noexcept
{
    return offset <= size && count <= size - offset;
}

// ---------------------------------------------------------------------------------------------------------------
// Uncompressed samples: WAV, AIFF and .wir
// ---------------------------------------------------------------------------------------------------------------

/** How a sample is stored. */
enum class Encoding
{
    kUnsigned8, ///< offset binary about 128: the 8-bit WAV sample
    kSigned,    ///< a two's-complement integer of the sample's width
    kFloat,     ///< an IEEE float of 4 or 8 bytes
};

/** How the samples of an uncompressed file are laid out. */
struct PcmLayout
{
    Encoding encoding{Encoding::kSigned}; ///< how a sample is stored
    bool     big_endian{false};           ///< true when a sample's most significant byte comes first
    int      bytes_per_sample{2};         ///< 1 to 4 for an integer, 4 or 8 for a float
    int      bytes_per_frame{2};          ///< the bytes a frame takes: one sample a channel
    double   scale{0x1p-15};              ///< an integer's: 2^(1 - bits) for the container's bits

    /** The layout of `width`-byte samples, `channels` to a frame. An integer is scaled by its container's width, so
        a sample narrower than its container, at the container's top with its low bits zero, reads its own value.
        @param encoding   how a sample is stored
        @param big_endian true when a sample's most significant byte comes first
        @param width      the bytes a sample takes
        @param channels   the samples in a frame
        @return           the layout */
    static PcmLayout Of(Encoding encoding, bool big_endian, int width, int channels) noexcept
    {
        return {encoding, big_endian, width, width * channels, std::ldexp(1.0, 1 - 8 * width)};
    }
};

/** One sample's value.
    @param layout  how the samples are laid out
    @param p       the sample's first byte, `layout.bytes_per_sample` readable
    @return        the value, full scale at 1 for an integer and as stored for a float */
double SampleAt(const PcmLayout &layout, const Byte *p) noexcept
{
    const int     width = layout.bytes_per_sample;
    std::uint64_t raw   = 0;
    for (int i = 0; i < width; ++i)
    {
        raw = raw << 8 | p[layout.big_endian ? i : width - 1 - i]; // the most significant byte first
    }
    switch (layout.encoding)
    {
    case Encoding::kUnsigned8:
        return (static_cast<double>(raw) - 128.0) * layout.scale;
    case Encoding::kSigned: {
        const int  shift = 64 - 8 * width;
        const auto value = static_cast<std::int64_t>(raw << shift) >> shift; // sign-extended
        return static_cast<double>(value) * layout.scale;
    }
    case Encoding::kFloat:
        return width == 4 ? static_cast<double>(std::bit_cast<float>(static_cast<std::uint32_t>(raw)))
                          : std::bit_cast<double>(raw);
    }
    return 0.0;
}

/** The uncompressed formats' reader: whole frames at a known place in the bytes, every one of them in the file. */
class PcmReader final : public AudioFileReader
{
  public:
    /** A reader over uncompressed samples.
        @param bytes       the file's whole contents, taken and owned
        @param info        what the header says of the samples
        @param layout      how the samples are laid out
        @param data_start  the offset of the first frame in `bytes` */
    PcmReader(std::vector<Byte> bytes, AudioFileInfo info, PcmLayout layout, std::uint64_t data_start) noexcept
        : AudioFileReader(std::move(bytes), info), layout_(layout), data_start_(data_start)
    {
    }

  private:
    std::string Decode(std::int64_t frames, int channels, std::vector<std::vector<double>> &out) override
    {
        const auto count  = static_cast<std::size_t>(frames);
        const auto stride = static_cast<std::size_t>(layout_.bytes_per_frame);
        for (int c = 0; c < channels; ++c)
        {
            auto &channel = out[static_cast<std::size_t>(c)];
            channel.resize(count);
            const Byte *p = bytes_.data() + data_start_ +
                            static_cast<std::size_t>(c) * static_cast<std::size_t>(layout_.bytes_per_sample);
            for (std::size_t i = 0; i < count; ++i, p += stride)
            {
                channel[i] = SampleAt(layout_, p);
            }
        }
        return {};
    }

    PcmLayout     layout_;     ///< how the samples are laid out
    std::uint64_t data_start_; ///< the offset of the first frame in the file's bytes
};

/** The reader of `frames` whole frames from `data_start`, or null with `why` when the file ends before them.
    @param bytes       the file's whole contents, taken and owned
    @param rate        the sample rate, in hertz
    @param channels    the channels in a frame
    @param layout      how the samples are laid out
    @param data_start  the offset of the first frame in `bytes`
    @param frames      the frames the header promises
    @param data_end    the offset past the last byte the data may use
    @param why         receives the reason when the result is null
    @return            the reader, or null */
std::unique_ptr<AudioFileReader> PcmFrames(std::vector<Byte> bytes, double rate, int channels, PcmLayout layout,
                                           std::uint64_t data_start, std::uint64_t frames, std::uint64_t data_end,
                                           std::string &why)
{
    const std::uint64_t available = std::min<std::uint64_t>(bytes.size(), data_end);
    if (frames > available / static_cast<std::uint64_t>(layout.bytes_per_frame) ||
        !Holds(available, data_start, frames * static_cast<std::uint64_t>(layout.bytes_per_frame)))
    {
        why = "the file ends before the samples its header promises";
        return nullptr;
    }
    const AudioFileInfo info{rate, channels, static_cast<std::int64_t>(frames)};
    return std::make_unique<PcmReader>(std::move(bytes), info, layout, data_start);
}

// ---------------------------------------------------------------------------------------------------------------
// WAV, BWF, RF64 and .wir
// ---------------------------------------------------------------------------------------------------------------

// WAVE_FORMAT_EXTENSIBLE's sub-formats, as a file stores them: the GUID's first three fields little-endian.

/** The sub-format of integer samples (format tag 1). */
constexpr std::array<Byte, 16> kPcmSubFormat = {0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};
/** The sub-format of float samples (format tag 3). */
constexpr std::array<Byte, 16> kFloatSubFormat = {0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00,
                                                  0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71};

/** What a `fmt ` chunk says of the samples. */
struct WaveFormat
{
    PcmLayout layout;      ///< how the samples are laid out
    double    rate{0.0};   ///< the sample rate, in hertz
    int       channels{0}; ///< the channels in a frame
};

/**
    A `fmt ` chunk read: the encoding from its tag - or, extensible, its sub-format - and the sample's width from the
    block alignment, the bytes a frame takes, never from the bits field. A sample narrower than its container is at its
    top, so the container's width reads it exactly, and the block alignment is what the frames are laid out by; the
    `.wir` format's float files say 23 bits there.
    @param b       the file's bytes
    @param size    the file's length, in bytes
    @param body    the offset of the chunk's body
    @param length  the chunk's length, in bytes, as its header says
    @param format  receives the layout, rate and channels when the result is true
    @param why     receives the reason when the result is false
    @return        true when the chunk describes samples the reader reads
*/
bool ReadWaveFormat(const Byte *b, std::uint64_t size, std::uint64_t body, std::uint32_t length, WaveFormat &format,
                    std::string &why)
{
    if (length < 16 || !Holds(size, body, 16))
    {
        why = "its fmt chunk is too short to describe the samples";
        return false;
    }
    std::uint32_t       tag         = Le16(b + body);
    const std::uint32_t channels    = Le16(b + body + 2);
    const std::uint32_t rate        = Le32(b + body + 4);
    const std::uint32_t block_align = Le16(b + body + 12);
    if (tag == 0xfffe)
    {
        if (length < 40 || !Holds(size, body, 40))
        {
            why = "its fmt chunk is too short to describe the samples";
            return false;
        }
        const Byte *guid = b + body + 24;
        if (std::memcmp(guid, kPcmSubFormat.data(), kPcmSubFormat.size()) == 0)
        {
            tag = 1;
        }
        else if (std::memcmp(guid, kFloatSubFormat.data(), kFloatSubFormat.size()) == 0)
        {
            tag = 3;
        }
    }
    if (channels == 0 || rate == 0 || block_align == 0 || block_align % channels != 0)
    {
        why = "its fmt chunk does not describe whole frames";
        return false;
    }
    const int width = static_cast<int>(block_align / channels);
    Encoding  encoding;
    if (tag == 1 && width <= 4)
    {
        encoding = width == 1 ? Encoding::kUnsigned8 : Encoding::kSigned;
    }
    else if (tag == 3 && (width == 4 || width == 8))
    {
        encoding = Encoding::kFloat;
    }
    else
    {
        why = "its samples are format " + std::to_string(tag) + " at " + std::to_string(width) +
              " bytes, which the reader does not read";
        return false;
    }
    format.layout   = PcmLayout::Of(encoding, false, width, static_cast<int>(channels));
    format.rate     = static_cast<double>(rate);
    format.channels = static_cast<int>(channels);
    return true;
}

/**
    A RIFF WAVE file: its chunks walked to the first `data` chunk, which a `fmt ` chunk must come before. An RF64 file
    takes the data size its `ds64` chunk gives, which must come first. A `.wir` is the same file under its own magic -
    `wvIR <size> ver1` - or an ordinary RIFF head, neither the size nor the form after the magic read.
    @param bytes  the file's whole contents, taken and owned
    @param wir    true to read the file as a `.wir`
    @param why    receives the reason when the result is null
    @return       the reader, or null
*/
std::unique_ptr<AudioFileReader> OpenRiffWave(std::vector<Byte> bytes, bool wir, std::string &why)
{
    const std::uint64_t size = bytes.size();
    const Byte         *b    = bytes.data();
    bool                rf64 = false;
    if (wir)
    {
        if (size < 12 || (Le32(b) != Id("wvIR") && Le32(b) != Id("RIFF")))
        {
            why = "it is not a .wir file";
            return nullptr;
        }
    }
    else
    {
        if (size < 12 || (Le32(b) != Id("RIFF") && Le32(b) != Id("RF64")) || Le32(b + 8) != Id("WAVE"))
        {
            why = "it is not a RIFF WAVE file";
            return nullptr;
        }
        rf64 = Le32(b) == Id("RF64");
    }

    std::uint64_t pos       = 12;
    std::uint64_t rf64_data = 0;
    if (rf64)
    {
        // ds64: the RIFF size, then the data size, as 64-bit numbers.
        if (!Holds(size, 12, 8 + 16) || Le32(b + 12) != Id("ds64") || Le32(b + 16) < 16)
        {
            why = "it is an RF64 file without the ds64 chunk that gives its sizes";
            return nullptr;
        }
        const std::uint32_t length = Le32(b + 16);
        rf64_data                  = Le64(b + 28);
        pos                        = 20 + static_cast<std::uint64_t>(length) + (length & 1);
    }

    WaveFormat format;
    bool       has_format = false;
    while (Holds(size, pos, 8))
    {
        const std::uint32_t id     = Le32(b + pos);
        const std::uint32_t length = Le32(b + pos + 4);
        const std::uint64_t body   = pos + 8;
        if (id == Id("fmt "))
        {
            if (!ReadWaveFormat(b, size, body, length, format, why))
            {
                return nullptr;
            }
            has_format = true;
        }
        else if (id == Id("data"))
        {
            if (!has_format)
            {
                why = "its data comes before its format";
                return nullptr;
            }
            const std::uint64_t data   = rf64 && length == 0xffffffffu ? rf64_data : length;
            const std::uint64_t frames = data / static_cast<std::uint64_t>(format.layout.bytes_per_frame);
            return PcmFrames(std::move(bytes), format.rate, format.channels, format.layout, body, frames,
                             std::numeric_limits<std::uint64_t>::max(), why);
        }
        pos = body + length + (length & 1);
    }
    why = has_format ? "it has no data chunk" : "it has no fmt chunk";
    return nullptr;
}

// ---------------------------------------------------------------------------------------------------------------
// AIFF and AIFF-C
// ---------------------------------------------------------------------------------------------------------------

/** An 80-bit extended float, AIFF's rate: a sign, a 15-bit exponent biased by 16383 and a 64-bit mantissa with its
    leading one. NaN for an infinity or a NaN.
    @param p  the ten bytes, big-endian
    @return   the value */
double ExtendedFloat(const Byte *p) noexcept
{
    const int           exponent = Be16(p) & 0x7fff;
    const std::uint64_t mantissa = static_cast<std::uint64_t>(Be32(p + 2)) << 32 | Be32(p + 6);
    if (exponent == 0x7fff)
    {
        return std::numeric_limits<double>::quiet_NaN();
    }
    const double magnitude = std::ldexp(static_cast<double>(mantissa), exponent - 16383 - 63);
    return (p[0] & 0x80) != 0 ? -magnitude : magnitude;
}

/** A chunk id or coding as text for a message.
    @param id  the four characters, as Id() makes them
    @return    the four characters, each non-printing one as '?' */
std::string FourCharacters(std::uint32_t id)
{
    std::string name(4, ' ');
    for (int i = 0; i < 4; ++i)
    {
        const auto c                      = static_cast<unsigned char>((id >> (8 * i)) & 0xff);
        name[static_cast<std::size_t>(i)] = std::isprint(c) != 0 ? static_cast<char>(c) : '?';
    }
    return name;
}

/**
    An AIFF or AIFF-C file: every chunk walked for the `COMM` chunk, which gives the samples, and the `SSND` chunk,
    which holds them, in either order. An integer sample of b bits takes the whole bytes b needs, big-endian - or
    little-endian, AIFF-C's `sowt` - left-justified; AIFF-C's `fl32` and `fl64` are big-endian floats.
    @param bytes  the file's whole contents, taken and owned
    @param why    receives the reason when the result is null
    @return       the reader, or null
*/
std::unique_ptr<AudioFileReader> OpenAiff(std::vector<Byte> bytes, std::string &why)
{
    const std::uint64_t size = bytes.size();
    const Byte         *b    = bytes.data();
    if (size < 12 || Le32(b) != Id("FORM") || (Le32(b + 8) != Id("AIFF") && Le32(b + 8) != Id("AIFC")))
    {
        why = "it is not an AIFF file";
        return nullptr;
    }
    const bool aifc = Le32(b + 8) == Id("AIFC");

    PcmLayout     layout;
    int           channels   = 0;
    std::uint64_t frames     = 0;
    double        rate       = 0.0;
    bool          has_format = false;
    bool          has_data   = false;
    std::uint64_t data_start = 0;
    std::uint64_t data_end   = 0;
    for (std::uint64_t pos = 12; Holds(size, pos, 8);)
    {
        const std::uint32_t id     = Le32(b + pos);
        const std::uint32_t length = Be32(b + pos + 4);
        const std::uint64_t body   = pos + 8;
        if (id == Id("COMM"))
        {
            if (length < (aifc ? 22u : 18u) || !Holds(size, body, aifc ? 22 : 18))
            {
                why = "its COMM chunk is too short to describe the samples";
                return nullptr;
            }
            channels                   = static_cast<std::int16_t>(Be16(b + body));
            frames                     = Be32(b + body + 2);
            const int           bits   = static_cast<std::int16_t>(Be16(b + body + 6));
            const std::uint32_t coding = aifc ? Le32(b + body + 18) : Id("NONE");
            rate                       = ExtendedFloat(b + body + 8);
            const bool integer         = coding == Id("NONE") || coding == Id("twos") || coding == Id("sowt");
            const int  float_size      = coding == Id("fl32") || coding == Id("FL32")   ? 4
                                         : coding == Id("fl64") || coding == Id("FL64") ? 8
                                                                                        : 0;
            if (!integer && float_size == 0)
            {
                why = "it is AIFF-C compressed as '" + FourCharacters(coding) + "', which the reader does not read";
                return nullptr;
            }
            if (channels < 1 || (integer && (bits < 1 || bits > 32)))
            {
                why = "its COMM chunk does not describe whole frames";
                return nullptr;
            }
            layout     = integer ? PcmLayout::Of(Encoding::kSigned, coding != Id("sowt"), (bits + 7) / 8, channels)
                                 : PcmLayout::Of(Encoding::kFloat, true, float_size, channels);
            has_format = true;
        }
        else if (id == Id("SSND"))
        {
            if (!Holds(size, body, 8))
            {
                why = "the file ends inside its SSND chunk";
                return nullptr;
            }
            data_start = body + 8 + Be32(b + body); // past the offset and block size fields, and the offset
            data_end   = body + length;
            has_data   = true;
        }
        pos = body + length + (length & 1);
    }

    if (!has_format)
    {
        why = "it has no COMM chunk";
        return nullptr;
    }
    if (!(rate >= 1.0) || !std::isfinite(rate))
    {
        why = "its sample rate is not one";
        return nullptr;
    }
    if (frames > 0 && !has_data)
    {
        why = "it has no SSND chunk for the samples its header promises";
        return nullptr;
    }
    return PcmFrames(std::move(bytes), rate, channels, layout, data_start, frames, data_end, why);
}

// ---------------------------------------------------------------------------------------------------------------
// FLAC: dr_flac's integers
// ---------------------------------------------------------------------------------------------------------------

/** One FLAC stream opened in the reader's bytes, closed with it. */
class OpenFlac
{
  public:
    /** Open the stream in `bytes`.
        @param bytes  the file's contents, which must outlive this object */
    explicit OpenFlac(const std::vector<Byte> &bytes) noexcept
        : flac_(drflac_open_memory(bytes.data(), bytes.size(), nullptr))
    {
    }

    /** Close the stream. */
    ~OpenFlac()
    {
        drflac_close(flac_);
    }

    OpenFlac(const OpenFlac &)            = delete; ///< Not copyable: it closes the stream.
    OpenFlac &operator=(const OpenFlac &) = delete; ///< Not assignable.

    /** The decoder.
        @return  the open stream, or null when the bytes are not a FLAC stream */
    drflac *get() const noexcept
    {
        return flac_;
    }

  private:
    drflac *flac_; ///< the open stream, or null
};

/**
    A FLAC stream's frames, decoded in order and held to their own numbering.

    dr_flac skips a frame whose checksum fails and goes on with the next, so a damaged frame would leave a hole in the
    samples that nothing after it shows. So every frame's number is checked against the frames before it - a
    fixed-blocksize stream numbers its frames, a variable one numbers its first sample - and a gap refuses the stream.
*/
class FlacFrames
{
  public:
    /** Decode frames of an open stream.
        @param flac  the open stream, which must outlive this object */
    explicit FlacFrames(drflac *flac) noexcept : flac_(flac)
    {
    }

    /** Up to `wanted` frames of interleaved samples, left-justified to 32 bits, appended to `out`; how many came, or
        -1 when a frame of the stream is missing. Fewer than wanted at the stream's end.
        @param wanted  the sample frames to read
        @param out     the interleaved samples are appended here
        @return        the sample frames appended, or -1 when a frame of the stream is missing */
    std::int64_t Read(std::int64_t wanted, std::vector<drflac_int32> &out)
    {
        const auto   channels = static_cast<std::size_t>(flac_->channels);
        std::int64_t got      = 0;
        while (got < wanted)
        {
            const bool starts_frame = flac_->currentFLACFrame.pcmFramesRemaining == 0;
            // One sample frame when a FLAC frame starts, which decodes it, so its header can be checked before
            // anything past it is read; then the rest of that frame, or what is wanted of it.
            const std::uint64_t ask = starts_frame ? 1
                                                   : std::min<std::uint64_t>(flac_->currentFLACFrame.pcmFramesRemaining,
                                                                             static_cast<std::uint64_t>(wanted - got));
            const std::size_t   at  = out.size();
            out.resize(at + static_cast<std::size_t>(ask) * channels);
            const drflac_uint64 read = drflac_read_pcm_frames_s32(flac_, ask, out.data() + at);
            out.resize(at + static_cast<std::size_t>(read) * channels);
            if (read == 0)
            {
                break; // the end of the stream
            }
            if (starts_frame && !FollowsTheLast())
            {
                return -1;
            }
            got += static_cast<std::int64_t>(read);
            produced_ += static_cast<std::int64_t>(read);
        }
        return got;
    }

  private:
    /** Whether the FLAC frame just started is the one after the last, by its own numbering; counts it.
        @return  true when no frame is missing before it */
    bool FollowsTheLast() noexcept
    {
        const drflac_frame_header &header = flac_->currentFLACFrame.header;
        if (frames_ == 0)
        {
            first_number_ = header.flacFrameNumber;
            first_sample_ = header.pcmFrameNumber;
        }
        const bool fixed    = header.pcmFrameNumber == 0 && header.flacFrameNumber == first_number_ + frames_;
        const bool variable = header.flacFrameNumber == 0 &&
                              header.pcmFrameNumber == first_sample_ + static_cast<std::uint64_t>(produced_);
        ++frames_;
        return fixed || variable;
    }

    drflac       *flac_;            ///< the open stream
    std::int64_t  produced_{0};     ///< the sample frames read so far
    std::uint64_t frames_{0};       ///< the FLAC frames started so far
    std::uint64_t first_number_{0}; ///< the first frame's number, in a fixed-blocksize stream
    std::uint64_t first_sample_{0}; ///< the first frame's first sample number, in a variable-blocksize stream
};

/** The FLAC reader: the stream decoded afresh on every Read. */
class FlacReader final : public AudioFileReader
{
  public:
    /** A reader over a FLAC stream.
        @param bytes  the file's whole contents, taken and owned
        @param info   what the stream's header says of the samples */
    FlacReader(std::vector<Byte> bytes, AudioFileInfo info) noexcept : AudioFileReader(std::move(bytes), info)
    {
    }

  private:
    std::string Decode(std::int64_t frames, int channels, std::vector<std::vector<double>> &out) override
    {
        const OpenFlac flac(bytes_);
        if (flac.get() == nullptr)
        {
            return "the FLAC stream could not be opened";
        }
        FlacFrames                stream(flac.get());
        std::vector<drflac_int32> block;
        const auto                stride       = static_cast<std::size_t>(flac.get()->channels);
        constexpr std::int64_t    kBlockFrames = 1 << 14;
        std::int64_t              produced     = 0;
        while (produced < frames)
        {
            block.clear();
            const std::int64_t got = stream.Read(std::min(kBlockFrames, frames - produced), block);
            if (got < 0)
            {
                return "a frame of the FLAC stream is damaged or missing";
            }
            if (got == 0)
            {
                return "the FLAC stream ends before the length its header gives";
            }
            for (int c = 0; c < channels; ++c)
            {
                auto &channel = out[static_cast<std::size_t>(c)];
                for (std::int64_t i = 0; i < got; ++i)
                {
                    // Left-justified to 32 bits, so 2^-31 scales every width by its own.
                    channel.push_back(
                        static_cast<double>(block[static_cast<std::size_t>(i) * stride + static_cast<std::size_t>(c)]) *
                        0x1p-31);
                }
            }
            produced += got;
        }
        return {};
    }
};

/** A FLAC file opened, its length counted by decoding where its header gives none.
    @param bytes  the file's whole contents, taken and owned
    @param why    receives the reason when the result is null
    @return       the reader, or null */
std::unique_ptr<AudioFileReader> OpenFlacFile(std::vector<Byte> bytes, std::string &why)
{
    AudioFileInfo info;
    {
        const OpenFlac flac(bytes);
        if (flac.get() == nullptr)
        {
            why = "it is not a FLAC stream";
            return nullptr;
        }
        info.sample_rate = static_cast<double>(flac.get()->sampleRate);
        info.channels    = flac.get()->channels;
        info.length      = static_cast<std::int64_t>(flac.get()->totalPCMFrameCount); // 36 bits at most
        if (info.length == 0)
        {
            // A stream whose header gives no count, as an encoder that cannot seek back to write it leaves it: its
            // samples counted by decoding it all.
            FlacFrames                stream(flac.get());
            std::vector<drflac_int32> block;
            for (;;)
            {
                block.clear();
                const std::int64_t got = stream.Read(1 << 14, block);
                if (got < 0)
                {
                    why = "a frame of the FLAC stream is damaged or missing";
                    return nullptr;
                }
                if (got == 0)
                {
                    break;
                }
                info.length += got;
            }
        }
    }
    if (!(info.sample_rate > 0.0) || info.channels <= 0)
    {
        why = "it does not say what its samples are";
        return nullptr;
    }
    return std::make_unique<FlacReader>(std::move(bytes), info);
}

// ---------------------------------------------------------------------------------------------------------------
// Ogg Vorbis: stb_vorbis, on a stream whose pages are checked first
// ---------------------------------------------------------------------------------------------------------------

/** The Ogg page checksum: CRC-32 on the polynomial 0x04c11db7, unreflected, from zero. */
constexpr std::array<std::uint32_t, 256> kOggCrcTable = [] {
    std::array<std::uint32_t, 256> table{};
    for (std::uint32_t i = 0; i < 256; ++i)
    {
        std::uint32_t r = i << 24;
        for (int k = 0; k < 8; ++k)
        {
            r = (r & 0x80000000u) != 0 ? (r << 1) ^ 0x04c11db7u : r << 1;
        }
        table[i] = r;
    }
    return table;
}();

/** What the walk of a file's Ogg pages finds. */
struct OggStream
{
    std::int64_t  last_granule{0};         ///< the last page's granule position: the stream's length in samples
    std::uint64_t first_audio_page_end{0}; ///< where the first page to give a granule position after the headers ends
};

/**
    The Ogg pages of a file, walked before anything is decoded: one logical stream from its first page to its last,
    every page whole and its checksum good, the sequence unbroken, the last page the stream's end, and no other stream
    after it. stb_vorbis reads one stream and plays on past a damaged or missing page without a word, so each of those
    is refused here, before it decodes.
    @param bytes   the file's whole contents
    @param stream  receives the last granule position and where the first audio page ends
    @param why     receives the reason when the result is false
    @return        true when the file is one whole Ogg stream
*/
bool WalkOggPages(const std::vector<Byte> &bytes, OggStream &stream, std::string &why)
{
    const std::uint64_t size     = bytes.size();
    const Byte         *b        = bytes.data();
    std::uint64_t       pos      = 0;
    std::uint32_t       serial   = 0;
    std::uint32_t       sequence = 0;
    std::uint64_t       packets  = 0; // packets completed so far: the first three are the Vorbis headers
    bool                ended    = false;
    stream                       = {};
    for (std::uint64_t page = 0; !ended; ++page)
    {
        if (!Holds(size, pos, 27) || Le32(b + pos) != Id("OggS") || b[pos + 4] != 0)
        {
            why = page == 0 ? "it does not start with an Ogg page" : "the file ends before its Ogg stream does";
            return false;
        }
        const Byte          flags    = b[pos + 5];
        const std::uint32_t segments = b[pos + 26];
        if (!Holds(size, pos + 27, segments))
        {
            why = "the file ends inside an Ogg page";
            return false;
        }
        std::uint64_t body = 0;
        for (std::uint32_t s = 0; s < segments; ++s)
        {
            const Byte lacing = b[pos + 27 + s];
            body += lacing;
            packets += lacing < 255 ? 1 : 0; // a lacing value under 255 ends a packet
        }
        const std::uint64_t header = 27 + segments;
        if (!Holds(size, pos, header + body))
        {
            why = "the file ends inside an Ogg page";
            return false;
        }
        std::uint32_t crc = 0;
        for (std::uint64_t i = 0; i < header + body; ++i)
        {
            const Byte byte = (i >= 22 && i < 26) ? 0 : b[pos + i]; // the checksum field counts as zeros
            crc             = (crc << 8) ^ kOggCrcTable[((crc >> 24) ^ byte) & 0xff];
        }
        if (crc != Le32(b + pos + 22))
        {
            why = "an Ogg page is damaged";
            return false;
        }
        const bool begins = (flags & 0x02) != 0;
        if (page == 0)
        {
            if (!begins)
            {
                why = "its first Ogg page does not begin a stream";
                return false;
            }
            serial   = Le32(b + pos + 14);
            sequence = Le32(b + pos + 18);
        }
        else if (begins || Le32(b + pos + 14) != serial)
        {
            why = "it holds more than one Ogg stream";
            return false;
        }
        else if (Le32(b + pos + 18) != ++sequence)
        {
            why = "an Ogg page is missing";
            return false;
        }
        const auto granule = static_cast<std::int64_t>(Le64(b + pos + 6));
        pos += header + body;
        ended               = (flags & 0x04) != 0;
        stream.last_granule = granule;
        if (stream.first_audio_page_end == 0 && packets > 3 && granule != -1)
        {
            stream.first_audio_page_end = pos;
        }
    }
    // Bytes past the stream's last page are no stream's - unless another stream starts there, a chained stream that
    // stb_vorbis would not play.
    for (std::uint64_t i = pos; i + 4 <= size; ++i)
    {
        if (Le32(b + i) == Id("OggS"))
        {
            why = "it holds more than one Ogg stream";
            return false;
        }
    }
    if (stream.last_granule < 0)
    {
        why = "its Ogg stream does not say how long it is";
        return false;
    }
    return true;
}

/** One Vorbis stream opened in the reader's bytes, closed with it. */
class OpenVorbis
{
  public:
    /** Open the stream in `bytes`.
        @param bytes  the file's contents, which must outlive this object */
    explicit OpenVorbis(const std::vector<Byte> &bytes) noexcept
    {
        if (bytes.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()))
        {
            int error = 0;
            vorbis_   = stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()), &error, nullptr);
        }
    }

    /** Close the stream. */
    ~OpenVorbis()
    {
        if (vorbis_ != nullptr)
        {
            stb_vorbis_close(vorbis_);
        }
    }

    OpenVorbis(const OpenVorbis &)            = delete; ///< Not copyable: it closes the stream.
    OpenVorbis &operator=(const OpenVorbis &) = delete; ///< Not assignable.

    /** The decoder.
        @return  the open stream, or null when the bytes are not a Vorbis stream */
    stb_vorbis *get() const noexcept
    {
        return vorbis_;
    }

  private:
    stb_vorbis *vorbis_{nullptr}; ///< the open stream, or null
};

/**
    A Vorbis stream's frames in order, each held to the stream's granule positions.

    A page's granule position counts the samples through its last packet. stb_vorbis numbers its first sample zero and
    counts on, and at the end of every page that gives a position takes its count from the page. A stream whose first
    page puts its start later than its packets do - one cut from a longer stream, whose samples before the start a
    decoder should drop - it does not trim, so its samples would run ahead of the positions. So after every frame its
    count must still be the samples it has handed out, and a stream where it is not is refused. The one exception is
    the stream's last frame, which stb_vorbis trims to the last page's position itself and counts past.
*/
class VorbisFrames
{
  public:
    /** Decode frames of an open stream.
        @param vorbis  the open stream, which must outlive this object */
    explicit VorbisFrames(stb_vorbis *vorbis) noexcept : vorbis_(vorbis)
    {
    }

    /** The next frame: its sample count, 0 at the stream's end, -1 when the frame before it - which this one shows
        was not the last - broke the count.
        @param samples  receives the frame's channels, one pointer a channel, valid until the next call
        @return         the sample count, 0 at the stream's end, or -1 when the count was broken */
    int Next(float **&samples) noexcept
    {
        int       channels = 0;
        const int count    = stb_vorbis_get_frame_float(vorbis_, &channels, &samples);
        if (count <= 0)
        {
            return 0;
        }
        if (offset_ != produced_)
        {
            return -1;
        }
        produced_ += count;
        offset_ = stb_vorbis_get_sample_offset(vorbis_);
        return count;
    }

  private:
    stb_vorbis  *vorbis_;      ///< the open stream
    std::int64_t produced_{0}; ///< the samples handed out so far
    std::int64_t offset_{0};   ///< the stream's own count of samples after the last frame
};

/** The Vorbis reader: the stream decoded afresh on every Read. */
class VorbisReader final : public AudioFileReader
{
  public:
    /** A reader over a Vorbis stream.
        @param bytes  the file's whole contents, taken and owned
        @param info   what the stream's header and last page say of the samples */
    VorbisReader(std::vector<Byte> bytes, AudioFileInfo info) noexcept : AudioFileReader(std::move(bytes), info)
    {
    }

  private:
    std::string Decode(std::int64_t frames, int channels, std::vector<std::vector<double>> &out) override
    {
        const OpenVorbis vorbis(bytes_);
        if (vorbis.get() == nullptr)
        {
            return "the Vorbis stream could not be opened";
        }
        VorbisFrames stream(vorbis.get());
        std::int64_t produced = 0;
        while (produced < frames)
        {
            float   **samples = nullptr;
            const int count   = stream.Next(samples);
            if (count < 0)
            {
                return "its Vorbis stream's samples do not run with its granule positions";
            }
            if (count == 0)
            {
                return "the Vorbis stream ends before the length its last page gives";
            }
            const auto use = static_cast<std::size_t>(std::min<std::int64_t>(count, frames - produced));
            for (int c = 0; c < channels; ++c)
            {
                auto &channel = out[static_cast<std::size_t>(c)];
                channel.insert(channel.end(), samples[c], samples[c] + use);
            }
            produced += count;
        }
        return {};
    }
};

/** An Ogg Vorbis file opened, its pages checked first.
    @param bytes  the file's whole contents, taken and owned
    @param why    receives the reason when the result is null
    @return       the reader, or null */
std::unique_ptr<AudioFileReader> OpenOggVorbis(std::vector<Byte> bytes, std::string &why)
{
    OggStream pages;
    if (!WalkOggPages(bytes, pages, why))
    {
        return nullptr;
    }
    const OpenVorbis vorbis(bytes);
    if (vorbis.get() == nullptr)
    {
        why = "its Ogg stream is not a Vorbis stream that can be decoded";
        return nullptr;
    }
    const stb_vorbis_info stream = stb_vorbis_get_info(vorbis.get());
    AudioFileInfo         info;
    info.sample_rate = static_cast<double>(stream.sample_rate);
    info.channels    = stream.channels;
    info.length      = pages.last_granule; // the stream starts at zero, by the check below
    if (!(info.sample_rate > 0.0) || info.channels <= 0)
    {
        why = "it does not say what its samples are";
        return nullptr;
    }
    // Where the stream starts: the frames through the end of the first page that gives a granule position, which
    // settles the start for every read. Cheap, a page's worth.
    VorbisFrames frames(vorbis.get());
    while (stb_vorbis_get_file_offset(vorbis.get()) <= pages.first_audio_page_end)
    {
        float   **samples = nullptr;
        const int count   = frames.Next(samples);
        if (count < 0)
        {
            why = "its Vorbis stream does not start where its first page says";
            return nullptr;
        }
        if (count == 0)
        {
            break; // a stream on one page, which stb_vorbis trims at its end
        }
    }
    return std::make_unique<VorbisReader>(std::move(bytes), info);
}

// ---------------------------------------------------------------------------------------------------------------
// MP3: dr_mp3
// ---------------------------------------------------------------------------------------------------------------

/**
    One MP3 stream opened in the reader's bytes, closed with it. dr_mp3 skips the tags - ID3v2 before the audio, ID3v1
    and APE after it - and a Xing or Info frame, whose frame count it takes for the stream's, with a LAME tag's encoder
    delay and padding, which it trims; a stream with no count it counts by walking its frames.
*/
class OpenMp3
{
  public:
    /** Open the stream in `bytes`.
        @param bytes  the file's contents, which must outlive this object */
    explicit OpenMp3(const std::vector<Byte> &bytes) : mp3_(std::make_unique<drmp3>())
    {
        open_ = drmp3_init_memory(mp3_.get(), bytes.data(), bytes.size(), nullptr) != DRMP3_FALSE;
    }

    /** Close the stream if it opened. */
    ~OpenMp3()
    {
        if (open_)
        {
            drmp3_uninit(mp3_.get());
        }
    }

    OpenMp3(const OpenMp3 &)            = delete; ///< Not copyable: it closes the stream.
    OpenMp3 &operator=(const OpenMp3 &) = delete; ///< Not assignable.

    /** The decoder.
        @return  the open stream, or null when the bytes hold no MPEG audio */
    drmp3 *get() const noexcept
    {
        return open_ ? mp3_.get() : nullptr;
    }

  private:
    std::unique_ptr<drmp3> mp3_;         ///< the decoder's state
    bool                   open_{false}; ///< whether the stream opened
};

/** The MP3 reader: the stream decoded afresh on every Read. */
class Mp3Reader final : public AudioFileReader
{
  public:
    /** A reader over an MP3 stream.
        @param bytes  the file's whole contents, taken and owned
        @param info   what the stream's header says of the samples */
    Mp3Reader(std::vector<Byte> bytes, AudioFileInfo info) noexcept : AudioFileReader(std::move(bytes), info)
    {
    }

  private:
    std::string Decode(std::int64_t frames, int channels, std::vector<std::vector<double>> &out) override
    {
        const OpenMp3 mp3(bytes_);
        drmp3        *stream = mp3.get();
        if (stream == nullptr)
        {
            return "the MP3 stream could not be opened";
        }
        // No more than the shortest MPEG frame, layer I's 384 samples, a read: so a read decodes one new frame at
        // most, and that frame is checked before the next. dr_mp3 lays a frame of other channels out at the stream's
        // count, and plays one of another rate at the stream's rate.
        constexpr std::int64_t kBlock = 384;
        const auto             stride = static_cast<std::size_t>(stream->channels);
        std::vector<float>     pcm(static_cast<std::size_t>(kBlock) * stride);
        std::int64_t           produced = 0;
        while (produced < frames)
        {
            const auto got = static_cast<std::int64_t>(drmp3_read_pcm_frames_f32(
                stream, static_cast<drmp3_uint64>(std::min(kBlock, frames - produced)), pcm.data()));
            if (stream->mp3FrameChannels != stream->channels || stream->mp3FrameSampleRate != stream->sampleRate)
            {
                return "its MPEG frames change rate or channels part-way";
            }
            if (got == 0)
            {
                return "the MP3 stream ends before the length it gives";
            }
            for (int c = 0; c < channels; ++c)
            {
                auto &channel = out[static_cast<std::size_t>(c)];
                for (std::int64_t i = 0; i < got; ++i)
                {
                    channel.push_back(pcm[static_cast<std::size_t>(i) * stride + static_cast<std::size_t>(c)]);
                }
            }
            produced += got;
        }
        return {};
    }
};

/** An MP3 file opened.
    @param bytes  the file's whole contents, taken and owned
    @param why    receives the reason when the result is null
    @return       the reader, or null */
std::unique_ptr<AudioFileReader> OpenMp3File(std::vector<Byte> bytes, std::string &why)
{
    AudioFileInfo info;
    {
        const OpenMp3 mp3(bytes);
        drmp3        *stream = mp3.get();
        if (stream == nullptr)
        {
            why = "it has no MPEG audio stream in it";
            return nullptr;
        }
        info.sample_rate          = static_cast<double>(stream->sampleRate);
        info.channels             = static_cast<int>(stream->channels);
        const drmp3_uint64 length = drmp3_get_pcm_frame_count(stream);
        if (length > static_cast<drmp3_uint64>(std::numeric_limits<std::int64_t>::max()))
        {
            why = "its frame count is not a count";
            return nullptr;
        }
        info.length = static_cast<std::int64_t>(length);
    }
    if (!(info.sample_rate > 0.0) || info.channels <= 0)
    {
        why = "it does not say what its samples are";
        return nullptr;
    }
    return std::make_unique<Mp3Reader>(std::move(bytes), info);
}

// ---------------------------------------------------------------------------------------------------------------
// Writing WAV
// ---------------------------------------------------------------------------------------------------------------

/** Append a number as little-endian bytes.
    @param out    the bytes to append to
    @param value  the number; only its low `bytes` bytes are written
    @param bytes  how many bytes to write, 1 to 4 */
void PutLe(std::vector<Byte> &out, std::uint32_t value, int bytes)
{
    for (int i = 0; i < bytes; ++i)
    {
        out.push_back(static_cast<Byte>(value >> (8 * i)));
    }
}
} // namespace

std::optional<AudioFileFormat> AudioFileFormatFor(std::string_view file_name)
{
    const auto separator = file_name.find_last_of("/\\");
    const auto dot       = file_name.find_last_of('.');
    if (dot == std::string_view::npos || (separator != std::string_view::npos && dot < separator))
    {
        return std::nullopt;
    }
    std::string extension(file_name.substr(dot));
    for (char &c : extension)
    {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    for (const auto &entry : kAudioFileExtensions)
    {
        if (entry.first == extension)
        {
            return entry.second;
        }
    }
    return std::nullopt;
}

std::unique_ptr<AudioFileReader> AudioFileReader::Open(std::vector<std::uint8_t> bytes, AudioFileFormat format,
                                                       std::string &why)
{
    why.clear();
    if (bytes.empty())
    {
        why = "the file is empty";
        return nullptr;
    }
    try
    {
        switch (format)
        {
        case AudioFileFormat::kWav:
            return OpenRiffWave(std::move(bytes), false, why);
        case AudioFileFormat::kAiff:
            return OpenAiff(std::move(bytes), why);
        case AudioFileFormat::kFlac:
            return OpenFlacFile(std::move(bytes), why);
        case AudioFileFormat::kOggVorbis:
            return OpenOggVorbis(std::move(bytes), why);
        case AudioFileFormat::kMp3:
            return OpenMp3File(std::move(bytes), why);
        case AudioFileFormat::kWir:
            return OpenRiffWave(std::move(bytes), true, why);
        }
    }
    catch (const std::bad_alloc &)
    {
        why = "there is not the memory to read it";
        return nullptr;
    }
    catch (const std::length_error &)
    {
        why = "there is not the memory to read it";
        return nullptr;
    }
    why = "its format is not one of the six";
    return nullptr;
}

std::string AudioFileReader::Read(std::int64_t frames, int channels, std::vector<std::vector<double>> &out)
{
    if (frames < 1 || frames > info_.length)
    {
        return "the file holds " + std::to_string(info_.length) + " frames, and " + std::to_string(frames) +
               " were asked for";
    }
    if (channels < 1 || channels > info_.channels)
    {
        return "the file has " + std::to_string(info_.channels) + " channels, and " + std::to_string(channels) +
               " were asked for";
    }
    try
    {
        std::vector<std::vector<double>> decoded(static_cast<std::size_t>(channels));
        if (std::string why = Decode(frames, channels, decoded); !why.empty())
        {
            return why;
        }
        for (auto &channel : decoded)
        {
            if (channel.size() < static_cast<std::size_t>(frames))
            {
                return "the file ends before the samples its header promises";
            }
            channel.resize(static_cast<std::size_t>(frames)); // a frame decoded past what was asked for, dropped
        }
        out = std::move(decoded);
        return {};
    }
    catch (const std::bad_alloc &)
    {
        return "there is not the memory to decode it";
    }
    catch (const std::length_error &)
    {
        return "there is not the memory to decode it";
    }
}

std::vector<std::uint8_t> EncodeWav(const std::vector<std::vector<double>> &channels, double sample_rate,
                                    WavSampleFormat format)
{
    const std::size_t count = channels.size();
    if (count == 0 || count > 2)
    {
        return {};
    }
    const std::size_t frames = channels[0].size();
    for (const auto &channel : channels)
    {
        if (channel.size() != frames ||
            !std::all_of(channel.begin(), channel.end(), [](double s) { return std::isfinite(s); }))
        {
            return {};
        }
    }

    const bool          floats     = format == WavSampleFormat::kFloat32;
    const bool          extensible = format == WavSampleFormat::kPcm20;
    const int           bits       = format == WavSampleFormat::kPcm16   ? 16
                                     : format == WavSampleFormat::kPcm20 ? 20
                                     : format == WavSampleFormat::kPcm24 ? 24
                                                                         : 32;
    const int           container  = extensible ? 24 : bits;
    const std::uint32_t block      = static_cast<std::uint32_t>(count) * static_cast<std::uint32_t>(container) / 8;
    const std::uint32_t fmt_size   = extensible ? 40 : 16;
    const std::uint64_t data       = static_cast<std::uint64_t>(block) * frames;
    const std::uint64_t pad        = data & 1;
    const std::uint64_t riff       = 4 + (8 + fmt_size) + (8 + data + pad);
    const double        rate       = std::round(sample_rate);
    if (!(sample_rate >= 1.0) || !(rate * block <= std::numeric_limits<std::uint32_t>::max()) ||
        riff > std::numeric_limits<std::uint32_t>::max())
    {
        return {};
    }

    try
    {
        std::vector<Byte> out;
        out.reserve(static_cast<std::size_t>(8 + riff));
        PutLe(out, Id("RIFF"), 4);
        PutLe(out, static_cast<std::uint32_t>(riff), 4);
        PutLe(out, Id("WAVE"), 4);
        PutLe(out, Id("fmt "), 4);
        PutLe(out, fmt_size, 4);
        PutLe(out, extensible ? 0xfffe : floats ? 3 : 1, 2);
        PutLe(out, static_cast<std::uint32_t>(count), 2);
        PutLe(out, static_cast<std::uint32_t>(rate), 4);
        PutLe(out, static_cast<std::uint32_t>(rate) * block, 4);
        PutLe(out, block, 2);
        PutLe(out, static_cast<std::uint32_t>(container), 2);
        if (extensible)
        {
            PutLe(out, 22, 2);                               // the extension's size
            PutLe(out, static_cast<std::uint32_t>(bits), 2); // the valid bits in each container
            PutLe(out, count == 1 ? 0x4u : 0x3u, 4); // the front centre for mono, the front left and right for stereo
            out.insert(out.end(), kPcmSubFormat.begin(), kPcmSubFormat.end());
        }
        PutLe(out, Id("data"), 4);
        PutLe(out, static_cast<std::uint32_t>(data), 4);

        const double full = std::ldexp(1.0, bits - 1);
        for (std::size_t i = 0; i < frames; ++i)
        {
            for (const auto &channel : channels)
            {
                if (floats)
                {
                    PutLe(out, std::bit_cast<std::uint32_t>(static_cast<float>(channel[i])), 4);
                    continue;
                }
                const auto code =
                    static_cast<std::int64_t>(std::clamp(std::round(channel[i] * full), -full, full - 1.0));
                PutLe(out, static_cast<std::uint32_t>(code << (container - bits)), container / 8);
            }
        }
        if (pad != 0)
        {
            out.push_back(0);
        }
        return out;
    }
    catch (const std::bad_alloc &)
    {
        return {};
    }
    catch (const std::length_error &)
    {
        return {};
    }
}
} // namespace reckless::core
