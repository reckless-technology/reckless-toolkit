#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <vector>

#include "SampleEncodings.h"

namespace reckless::audio::wasapi
{
/**
    The arithmetic of the suite's WASAPI back end (win/WasapiDevice.cpp) that needs no Windows to hold: the two modes
    and their names, the encodings an exclusive stream asks a device for (the encodings themselves, and their
    conversion to and from the callback's float channels, are SampleEncodings.h's, which ALSA's back end shares), the
    period each mode opens at, and the ring the capture side's samples wait in until the render side's
    callback takes them. Header only and platform-neutral, so the audio suite holds it on every platform
    (tests/test_wasapi_formats.cpp); the COM is the .cpp's alone.
*/

/** How a stream opens the endpoints: through the audio engine's mixer (kShared), at the driver's smallest period that
    holds the block where it can (SharedRouteOf), or with the device to itself and nothing between (kExclusive). */
enum class Mode
{
    kShared,   ///< shared, through the engine's mixer, at the driver's period that holds the block or the engine's
    kExclusive ///< the device to the stream alone, nothing between it and the driver
};

// The two modes' names as the series lists them - an AudioSystem's Name(), a settings file's "api" - JUCE's device
// type names, so a person's choice carries over.
inline constexpr std::string_view kSharedName    = "Windows Audio";                  ///< Mode::kShared's name
inline constexpr std::string_view kExclusiveName = "Windows Audio (Exclusive Mode)"; ///< Mode::kExclusive's name

/** The name a third mode was listed under until 2026-10-25, when it became what Windows Audio does wherever it can:
    a settings file that kept it means Windows Audio (ModeNamed, and AudioSettings.h's ReadSettings). */
inline constexpr std::string_view kFormerLowLatencyName = "Windows Audio (Low Latency Mode)";

/** A mode's name.
    @param mode  the mode
    @return      its name, one of the two above */
constexpr std::string_view NameOf(Mode mode) noexcept
{
    return mode == Mode::kShared ? kSharedName : kExclusiveName;
}

/** The mode a name stands for - the former low-latency mode's name standing for the shared mode, which does what it
    did wherever it can.
    @param name  an API's name
    @return      the mode, or none for a name that is no WASAPI mode's */
constexpr std::optional<Mode> ModeNamed(std::string_view name) noexcept
{
    if (name == kSharedName || name == kFormerLowLatencyName)
    {
        return Mode::kShared;
    }
    if (name == kExclusiveName)
    {
        return Mode::kExclusive;
    }
    return std::nullopt;
}

/** Which way a shared stream's side opens: at the driver's period that holds the block (IAudioClient3's
    InitializeSharedAudioStream, LowLatencyPeriod) or at the audio engine's period (IAudioClient's Initialize). */
enum class SharedRoute
{
    kDriversPeriod, ///< the driver's smallest period that holds the block, in the endpoint's mix format
    kEnginesPeriod  ///< the engine's period, the engine converting the stream's rate to the endpoint's
};

/** The way a shared side opens at `rate`: at the driver's period where it runs at the endpoint's own rate - nothing to
    convert, which a small period cannot do - and else at the engine's, converting. The driver's period is the one
    tried first; a Windows or a driver that refuses it opens at the engine's all the same.
    @param rate      the stream's rate, in hertz
    @param mix_rate  the endpoint's mix rate, in hertz
    @return          the route */
constexpr SharedRoute SharedRouteOf(int rate, int mix_rate) noexcept
{
    return rate > 0 && rate == mix_rate ? SharedRoute::kDriversPeriod : SharedRoute::kEnginesPeriod;
}

/** The rates an exclusive stream is tried at, each asked of the device (IsFormatSupported): the ones interfaces run at,
    in order. */
inline constexpr int kRates[] = {44100, 48000, 88200, 96000, 176400, 192000};

// The encodings and their conversion, SampleEncodings.h's, under the names this back end has always called them by.
using audio::BytesOf;
using audio::DecodeOne;
using audio::Encode;
using audio::Encoding;
using audio::NameOf;
using audio::ToInteger;
using audio::ValidBitsOf;

/** The encodings an exclusive stream asks the device for, the best first: float, which the callback is in, then the
    widest integers. */
inline constexpr Encoding kEncodings[] = {Encoding::kFloat32, Encoding::kInt32, Encoding::kInt24In32, Encoding::kInt24,
                                          Encoding::kInt16};

/** The period, in frames, a low-latency stream opens at (IAudioClient3::InitializeSharedAudioStream): the smallest
    the driver offers that holds the block asked for - a whole number of its fundamental periods over its smallest,
    no more than its largest - or the smallest for a block of 0.
    @param requested    the block asked for, in frames; 0 or less for the smallest
    @param fundamental  the driver's step between periods, in frames
    @param smallest     its smallest period, in frames
    @param largest      its largest period, in frames
    @return             the period, in frames */
constexpr int LowLatencyPeriod(int requested, int fundamental, int smallest, int largest) noexcept
{
    if (fundamental <= 0 || smallest <= 0 || largest < smallest)
    {
        return smallest > 0 ? smallest : requested;
    }
    if (requested <= smallest)
    {
        return smallest;
    }
    const int steps  = (requested - smallest + fundamental - 1) / fundamental;
    const int period = smallest + steps * fundamental;
    return std::min(period, largest - (largest - smallest) % fundamental);
}

/** The period, in frames, an exclusive stream asks for: the block asked for, no shorter than the device's minimum
    period, and the device's default period for a block of 0 - the device may still round it to its own alignment.
    @param requested      the block asked for, in frames; 0 or less for the default
    @param minimum        the device's minimum period, in frames
    @param default_frames the device's default period, in frames
    @return               the period, in frames */
constexpr int ExclusivePeriod(int requested, int minimum, int default_frames) noexcept
{
    if (requested <= 0)
    {
        return std::max(default_frames, minimum);
    }
    return std::max(requested, minimum);
}

/** A duration in frames as WASAPI's REFERENCE_TIME, hundreds of nanoseconds, rounded to the nearest.
    @param frames  the frames
    @param rate    the rate, in hertz
    @return        the duration, in 100 ns units */
constexpr std::int64_t HundredsOfNanoseconds(int frames, int rate) noexcept
{
    return rate > 0 ? (static_cast<std::int64_t>(frames) * 10'000'000 + rate / 2) / rate : 0;
}

/** A REFERENCE_TIME as frames at a rate, rounded to the nearest.
    @param duration  the duration, in 100 ns units
    @param rate      the rate, in hertz
    @return          the frames */
constexpr int FramesOf(std::int64_t duration, int rate) noexcept
{
    return static_cast<int>((duration * rate + 5'000'000) / 10'000'000);
}

/**
    The capture side's samples on their way to the render side's callback: a ring of interleaved float frames, written
    as the capture endpoint hands its packets over and read a callback's frames at a time into the callback's
    channels. One thread - the stream's - both writes and reads it, so it holds no atomic. Its capacity is fixed when
    the stream opens and nothing is allocated after.

    The two endpoints' periods do not fall together, so the ring is read only once it has gathered a margin - a
    callback's frames and a capture period's, the prime - and until then reads silence and says nothing of it: the
    stream starting. The two endpoints may also run on two clocks: a primed ring that runs dry is read as silence and
    said once (Read's false) and gathers its margin again, and a full one drops its oldest frames as it is written
    (Write's false), so a drift costs a click and never a delay past the ring's capacity.
*/
class CaptureRing
{
  public:
    /** A ring of `channels` channels holding `capacity` frames, read once it holds `prime` of them.
        @param channels  the frame's channels
        @param capacity  the frames it holds
        @param prime     the frames it gathers before it is read; at least one */
    void Prepare(int channels, int capacity, int prime = 1)
    {
        channels_ = std::max(channels, 1);
        capacity_ = std::max(capacity, 1);
        prime_    = std::clamp(prime, 1, capacity_);
        data_.assign(static_cast<std::size_t>(channels_) * static_cast<std::size_t>(capacity_), 0.0f);
        Clear();
    }

    /** Empty and unprimed, as the stream starts. */
    void Clear() noexcept
    {
        head_   = 0;
        count_  = 0;
        primed_ = false;
    }

    /** The frames waiting.
        @return  the count */
    int Count() const noexcept
    {
        return count_;
    }

    /** The most frames it holds.
        @return  the capacity */
    int Capacity() const noexcept
    {
        return capacity_;
    }

    /** `frames` interleaved frames written - silence for a null `samples`, a packet the endpoint marked silent - the
        oldest dropped where they would overfill it.
        @param samples  `frames` frames of the ring's channels, interleaved; null for silence
        @param frames   the frames
        @return         false when frames were dropped to make room */
    bool Write(const float *samples, int frames) noexcept
    {
        bool kept = true;
        for (int f = 0; f < frames; ++f)
        {
            if (count_ == capacity_)
            {
                head_ = (head_ + 1) % capacity_;
                --count_;
                kept = false;
            }
            const int at = (head_ + count_) % capacity_;
            float    *to = &data_[static_cast<std::size_t>(at) * static_cast<std::size_t>(channels_)];
            for (int c = 0; c < channels_; ++c)
            {
                to[c] = samples != nullptr ? samples[static_cast<std::size_t>(f) * channels_ + c] : 0.0f;
            }
            ++count_;
        }
        return kept;
    }

    /** `frames` frames read into callback channels - ring channel `first + c` into channel c - silence for what the
        ring lacks, and silence alone until it has gathered its prime.
        @param channels  the callback's channels, `count` pointers to `frames` samples
        @param count     the callback's channels
        @param first     the ring channel channel 0 reads
        @param frames    the frames
        @return          false when a primed ring ran dry, which unprimes it - before its prime, silence is the stream
                         starting, not a dropout */
    bool Read(float *const *channels, int count, int first, int frames) noexcept
    {
        if (!primed_ && count_ >= prime_)
        {
            primed_ = true;
        }
        const int have = primed_ ? std::min(frames, count_) : 0;
        for (int c = 0; c < count; ++c)
        {
            const int r = first + c;
            for (int f = 0; f < frames; ++f)
            {
                float x = 0.0f;
                if (f < have && r >= 0 && r < channels_)
                {
                    x = data_[static_cast<std::size_t>((head_ + f) % capacity_) * channels_ + r];
                }
                channels[c][f] = x;
            }
        }
        head_ = (head_ + have) % capacity_;
        count_ -= have;
        if (!primed_ || have == frames)
        {
            return true;
        }
        primed_ = false; // dry: the margin gathered again before the next read
        return false;
    }

  private:
    std::vector<float> data_;          ///< the frames, interleaved
    int                channels_{1};   ///< the frame's channels
    int                capacity_{1};   ///< the frames it holds
    int                prime_{1};      ///< the frames it gathers before it is read
    int                head_{0};       ///< the oldest frame's slot
    int                count_{0};      ///< the frames waiting
    bool               primed_{false}; ///< true once it has gathered its prime, until it runs dry
};
} // namespace reckless::audio::wasapi
