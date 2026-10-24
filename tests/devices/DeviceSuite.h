#pragma once
// The harness of the audio layer's device suites, the toolkit's (test_audio_devices.cpp, CTest `audio_devices`) and a
// consumer's that plays an engine through the same devices: what a run requires (RECKLESS_AUDIO_REQUIRE) and what the
// configure promised (RECKLESS_AUDIO_PROMISED_APIS), the printed tables, the loop's sample codes and their scan, the
// recording callback, a relay counting a callback's allocations, the rigs of the platform's test devices
// (DeviceRig.h), running and measuring a stream, and the fixture each API's cases run on. Included by exactly one
// translation unit of each suite: it defines the ASIO registration's stand-ins where no test driver is built, and the
// counting allocator (CountAllocations.h).
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "AudioSettings.h"
#include "CountAllocations.h"
#include "DeviceRig.h"
#include "RtAudioDevice.h"
#include "Text.h"

#include <gtest/gtest.h>

namespace reckless::audio::test::suite
{
using reckless::audio::AudioCallback;
using reckless::audio::AudioSettings;
using reckless::audio::AudioSystem;
using reckless::audio::DeviceInfo;
using reckless::audio::NowNanoseconds;
using reckless::audio::StreamFormat;
using reckless::audio::StreamRequest;
using reckless::audio::test::DeviceRig;
using reckless::audio::test::Loop;
using reckless::audio::test::TestDevice;

/** The client name every stream of the suite's own opens under (JACK lists it; the loop is wired by it). */
inline constexpr const char *kClient = "Reckless Device Test";

// ---- What the run requires -----------------------------------------------------------------------------------------

inline std::string Trim(const std::string &text)
{
    const auto first = text.find_first_not_of(" \t");
    const auto last  = text.find_last_not_of(" \t");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

inline std::vector<std::string> SplitList(const std::string &text)
{
    std::vector<std::string> items;
    std::stringstream        stream(text);
    std::string              item;
    while (std::getline(stream, item, ','))
    {
        if (!Trim(item).empty())
        {
            items.push_back(Trim(item));
        }
    }
    return items;
}

inline std::string JoinList(const std::vector<std::string> &items)
{
    std::string text;
    for (const std::string &item : items)
    {
        text += (text.empty() ? "" : ", ") + item;
    }
    return text.empty() ? std::string("none") : text;
}

/** The APIs RECKLESS_AUDIO_REQUIRE names: each must have its test device, or its tests fail. */
inline const std::vector<std::string> &RequiredApis()
{
    static const std::vector<std::string> required = [] {
    // MSVC deprecates getenv for _dupenv_s, whose copy a test has no use for.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
        const char *text = std::getenv("RECKLESS_AUDIO_REQUIRE");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
        return text != nullptr ? SplitList(text) : std::vector<std::string>{};
    }();
    return required;
}

inline bool Required(const std::string &api)
{
    const auto &required = RequiredApis();
    return std::find(required.begin(), required.end(), api) != required.end();
}

/** The APIs the configure promised this build carries (the toolkit's CMakeLists.txt, from the platform, the ASIO SDK
    and the JACK and ALSA headers it found). */
inline std::vector<std::string> PromisedApis()
{
    return SplitList(RECKLESS_AUDIO_PROMISED_APIS);
}

/** The APIs the parameterised tests run on: those the build carries, and any the run requires that it does not - whose
    tests then fail, saying so. */
inline std::vector<std::string> TestedApis()
{
    std::vector<std::string> apis = reckless::audio::AudioApis();
    for (const std::string &api : RequiredApis())
    {
        if (std::find(apis.begin(), apis.end(), api) == apis.end())
        {
            apis.push_back(api);
        }
    }
    return apis;
}

// ---- The tables ----------------------------------------------------------------------------------------------------

/** One table a test judges, printed as name and value rows under its title. */
class Table
{
  public:
    /** A table with no rows yet.
        @param title  what the table shows, printed over its rows */
    explicit Table(std::string title) : title_(std::move(title))
    {
    }

    /** One row more.
        @param name   the row's name, the left column
        @param value  its value, the right column
        @return       the table, for the next row */
    Table &Row(const std::string &name, const std::string &value)
    {
        rows_.emplace_back(name, value);
        return *this;
    }

    /** The title and the rows on the standard output, the names in a column as wide as the longest. */
    void Print() const
    {
        std::size_t width = 0;
        for (const auto &row : rows_)
        {
            width = std::max(width, row.first.size());
        }
        std::printf("  -- %s\n", title_.c_str());
        for (const auto &row : rows_)
        {
            std::printf("     %-*s  %s\n", static_cast<int>(width), row.first.c_str(), row.second.c_str());
        }
        std::fflush(stdout);
    }

  private:
    std::string                                      title_; ///< what the table shows
    std::vector<std::pair<std::string, std::string>> rows_;  ///< the rows, name and value, in order
};

inline std::string Fixed(double value, int decimals)
{
    char text[64];
    std::snprintf(text, sizeof text, "%.*f", decimals, value);
    return text;
}

inline std::string Str(std::int64_t value)
{
    return std::to_string(value);
}

inline std::string Rates(const std::vector<int> &rates)
{
    std::string text;
    for (const int rate : rates)
    {
        text += (text.empty() ? "" : " ") + std::to_string(rate);
    }
    return text.empty() ? std::string("any") : text;
}

// ---- The signals ---------------------------------------------------------------------------------------------------

/** A sample that names its own position: frame n of the stream's output as (n mod 2^20 + 1) / 2^21 - exact in a float,
    never zero, distinct across 21.8 s at 48 kHz - positive on channel 0 and negative on channel 1, so each sample a
    loop carries back says which frame and which channel it was, and any arithmetic on the way (a gain, a dither, a
    resampler) leaves a value that is no such sample. */
inline constexpr double kIndexScale = 1.0 / 2097152.0; // 2^-21
/** The frames IndexSample counts before it starts again: 2^20. */
inline constexpr int kIndexPeriod = 1 << 20;

inline float IndexSample(std::int64_t frame, int channel)
{
    const auto k = static_cast<double>(frame % kIndexPeriod + 1);
    return static_cast<float>((channel == 0 ? 1.0 : -1.0) * k * kIndexScale);
}

/** The frame (mod 2^20) that `sample` on `channel` names, or -1 for a value that names none. */
inline std::int64_t DecodeIndex(float sample, int channel)
{
    const double k = static_cast<double>(sample) / kIndexScale * (channel == 0 ? 1.0 : -1.0);
    if (!(k >= 1.0 && k <= static_cast<double>(kIndexPeriod)) || std::floor(k) != k)
    {
        return -1;
    }
    return static_cast<std::int64_t>(k) - 1;
}

/** What an exact loop brought back, each sample read as the frame and the channel it names (IndexSample), from the
    first frame the loop carries on both channels - a loop may close a channel at a time: JACK's rig wires the two
    with two connections, and a period can fall between them, the first channel carrying the loop while the second
    still hears the dummy's silence. */
struct LoopScan
{
    std::size_t  first{0};            ///< the first frame carried on both channels; the recording's length for none
    std::int64_t played_at_first{-1}; ///< the frame it names
    std::int64_t compared{0};         ///< frames that name a frame on both channels, read bit for bit
    std::int64_t corrupt{0};          ///< frames that name none, or the channels two
    std::size_t  first_corrupt{0};    ///< the first frame that names none; the recording's length for none
    std::int64_t jumps{0};            ///< frames that are not the frame after the one before
    std::int64_t gaps{0};             ///< silent frames after the first
    std::size_t  settled{0};          ///< from where the frames come back one after another to the end
};

inline LoopScan ScanLoop(const std::vector<float> &in0, const std::vector<float> &in1, std::size_t recorded)
{
    LoopScan scan;
    while (scan.first < recorded && (in0[scan.first] == 0.0f || in1[scan.first] == 0.0f))
    {
        ++scan.first;
    }
    scan.first_corrupt = recorded;
    scan.settled       = scan.first;
    if (scan.first < recorded)
    {
        scan.played_at_first = DecodeIndex(in0[scan.first], 0);
    }
    std::int64_t previous = -1;
    for (std::size_t i = scan.first; i < recorded; ++i)
    {
        if (in0[i] == 0.0f && in1[i] == 0.0f)
        {
            ++scan.gaps;
            previous     = -1;
            scan.settled = i + 1;
            continue;
        }
        const std::int64_t k0 = DecodeIndex(in0[i], 0);
        const std::int64_t k1 = DecodeIndex(in1[i], 1);
        if (k0 < 0 || k1 != k0)
        {
            scan.first_corrupt = std::min(scan.first_corrupt, i);
            ++scan.corrupt;
            previous     = -1;
            scan.settled = i + 1;
            continue;
        }
        if (previous >= 0 && k0 != (previous + 1) % kIndexPeriod)
        {
            ++scan.jumps;
            scan.settled = i;
        }
        previous = k0;
        ++scan.compared;
    }
    return scan;
}

/** White noise peaking 30 dB under full scale - it plays through a person's speakers, and the correlation that judges
    it is blind to level - a function of the frame and the channel alone (a hash, no state), so the analysis
    regenerates what was played. */
inline float NoiseSample(std::int64_t frame, int channel)
{
    std::uint64_t z =
        static_cast<std::uint64_t>(frame) * 2 + static_cast<std::uint64_t>(channel) + 0x9E3779B97F4A7C15ULL;
    z              = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
    z              = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
    z              = z ^ (z >> 31);
    const double u = static_cast<double>(z >> 40) / static_cast<double>(1ULL << 24); // [0, 1)
    return static_cast<float>(0.0316 * (2.0 * u - 1.0));
}

// ---- A stream's callback for the suite -----------------------------------------------------------------------------

/**
    What a stream test opens a stream on: each callback stamped and counted, its block and its channel pointers checked
    against the layout RtAudioDevice promises (every channel's frames after the channel before's), a signal played and
    the first two inputs recorded - all into storage sized when it is built, so the audio thread allocates nothing.
    Read only once the stream has stopped, but for the count.
*/
class Recorder final : public AudioCallback
{
  public:
    /** What the stream plays. */
    enum class Play
    {
        kSilence, ///< nothing
        kIndices, ///< each sample naming its frame and its channel (IndexSample), for an exact loop
        kNoise    ///< a deterministic noise (NoiseSample), for a loop that is not exact
    };

    static constexpr std::size_t kMaxCallbacks = 1 << 13; ///< the callbacks stamped
    static constexpr std::size_t kMaxFrames    = 1 << 19; ///< the frames recorded per channel: 10.9 s at 48 kHz

    /** A recorder for a stream of these counts, its storage made here.
        @param input_channels   the inputs the stream opens, which every callback must carry
        @param output_channels  the outputs it opens, likewise
        @param play             what it plays */
    Recorder(int input_channels, int output_channels, Play play)
        : expected_inputs_(input_channels), expected_outputs_(output_channels), play_(play),
          test_thread_(std::this_thread::get_id()), times_(kMaxCallbacks), frames_(kMaxCallbacks), xruns_(kMaxCallbacks)
    {
        for (auto &channel : recorded_)
        {
            channel.assign(kMaxFrames, 0.0f);
        }
    }

    void Process(const float *const *inputs, int input_channels, float *const *outputs, int output_channels, int frames,
                 std::int64_t now, bool xrun) noexcept override
    {
        const auto n = static_cast<std::size_t>(callbacks_.load(std::memory_order_relaxed));
        if (n < kMaxCallbacks)
        {
            times_[n]  = now;
            frames_[n] = frames;
            xruns_[n]  = xrun ? 1 : 0;
        }
        if (std::this_thread::get_id() == test_thread_)
        {
            ++on_test_thread_;
        }
        CheckLayout(inputs, input_channels, outputs, output_channels, frames);

        if (const int stall_ms = stall_ms_.exchange(0, std::memory_order_acq_rel); stall_ms > 0)
        {
            const std::int64_t until = now + static_cast<std::int64_t>(stall_ms) * 1'000'000;
            while (NowNanoseconds() < until)
            {
                // A callback that overruns its block by several: the device's buffer runs dry under it.
            }
        }

        for (int c = 0; c < output_channels; ++c)
        {
            if (outputs == nullptr || outputs[c] == nullptr)
            {
                continue;
            }
            for (int i = 0; i < frames; ++i)
            {
                const std::int64_t frame = played_ + i;
                outputs[c][i]            = play_ == Play::kIndices ? IndexSample(frame, c)
                                           : play_ == Play::kNoise ? NoiseSample(frame, c)
                                                                   : 0.0f;
            }
        }
        played_ += frames;

        const std::size_t room = kMaxFrames - std::min(kMaxFrames, recorded_frames_);
        const auto        take = std::min(room, static_cast<std::size_t>(frames));
        for (int c = 0; c < std::min(input_channels, 2); ++c)
        {
            if (inputs != nullptr && inputs[c] != nullptr)
            {
                std::copy(inputs[c], inputs[c] + take,
                          recorded_[static_cast<std::size_t>(c)].begin() +
                              static_cast<std::ptrdiff_t>(recorded_frames_));
            }
        }
        recorded_frames_ += take;
        callbacks_.store(static_cast<std::int64_t>(n) + 1, std::memory_order_release);
    }

    void StreamFailed() noexcept override
    {
        failed_.store(true, std::memory_order_release);
    }

    /** The next callback runs `ms` milliseconds, as a callback that misses its deadline does.
        @param ms  how long it runs */
    void StallOnce(int ms) noexcept
    {
        stall_ms_.store(ms, std::memory_order_release);
    }

    /** @return the callbacks so far, read on any thread */
    std::int64_t callbacks() const noexcept
    {
        return callbacks_.load(std::memory_order_acquire);
    }

    /** @return whether the API said the stream failed */
    bool failed() const noexcept
    {
        return failed_.load(std::memory_order_acquire);
    }

    /** @return the callbacks stamped: the first kMaxCallbacks */
    std::size_t stamped() const noexcept
    {
        return std::min(static_cast<std::size_t>(callbacks()), kMaxCallbacks);
    }

    /** @param i  a stamped callback's index
        @return   when it came, in the steady clock's nanoseconds */
    std::int64_t time_at(std::size_t i) const noexcept
    {
        return times_[i];
    }

    /** @param i  a stamped callback's index
        @return   the frames it carried */
    int frames_at(std::size_t i) const noexcept
    {
        return frames_[i];
    }

    /** @return the stamped callbacks told of an xrun */
    std::int64_t xruns() const noexcept
    {
        std::int64_t count = 0;
        for (std::size_t i = 0; i < stamped(); ++i)
        {
            count += xruns_[i];
        }
        return count;
    }

    /** @param i  a stamped callback's index
        @return   whether it was told of an xrun */
    bool xrun_at(std::size_t i) const noexcept
    {
        return xruns_[i] != 0;
    }

    /** @return the callbacks whose channels were not laid out as RtAudioDevice promises */
    std::int64_t layout_faults() const noexcept
    {
        return layout_faults_;
    }

    /** @return the first of them described, or empty */
    const char *first_layout_fault() const noexcept
    {
        return first_fault_;
    }

    /** @return the callbacks that ran on the thread that made the recorder, which none should */
    std::int64_t on_test_thread() const noexcept
    {
        return on_test_thread_;
    }

    /** @param channel  0 or 1
        @return         the input recorded on it, kMaxFrames long, the first recorded_frames() of it written */
    const std::vector<float> &recorded(int channel) const noexcept
    {
        return recorded_[static_cast<std::size_t>(channel)];
    }

    /** @return the frames recorded */
    std::size_t recorded_frames() const noexcept
    {
        return recorded_frames_;
    }

    /** @return the frames played */
    std::int64_t played() const noexcept
    {
        return played_;
    }

  private:
    /** The block's channels as RtAudioDevice lays them out: the counts the stream opened, every pointer there, each
        channel's frames after the channel before's in one buffer each way. The first fault is described (a fixed
        string, chosen not built).
        @param inputs           the block's input pointers
        @param input_channels   how many
        @param outputs          its output pointers
        @param output_channels  how many
        @param frames           its frames */
    void CheckLayout(const float *const *inputs, int input_channels, float *const *outputs, int output_channels,
                     int frames) noexcept
    {
        const char *fault = nullptr;
        if (input_channels != expected_inputs_ || output_channels != expected_outputs_)
        {
            fault = "a callback carried other channel counts than the stream opened";
        }
        else if (frames <= 0)
        {
            fault = "a callback carried no frames";
        }
        else if (outputs == nullptr)
        {
            fault = "a callback had no output pointers";
        }
        else if (expected_inputs_ > 0 && inputs == nullptr)
        {
            fault = "a callback of a stream with inputs had no input pointers";
        }
        else
        {
            for (int c = 0; c < output_channels && fault == nullptr; ++c)
            {
                if (outputs[c] == nullptr || outputs[c] != outputs[0] + static_cast<std::ptrdiff_t>(c) * frames)
                {
                    fault = "an output channel was not where the channel before it ends";
                }
            }
            for (int c = 0; c < input_channels && fault == nullptr; ++c)
            {
                if (inputs[c] == nullptr || inputs[c] != inputs[0] + static_cast<std::ptrdiff_t>(c) * frames)
                {
                    fault = "an input channel was not where the channel before it ends";
                }
            }
        }
        if (fault != nullptr)
        {
            if (layout_faults_ == 0)
            {
                first_fault_ = fault;
            }
            ++layout_faults_;
        }
    }

    const int             expected_inputs_;  ///< the inputs every callback must carry
    const int             expected_outputs_; ///< the outputs every callback must carry
    const Play            play_;             ///< what it plays
    const std::thread::id test_thread_;      ///< the thread that made it, which no callback should run on

    std::atomic<std::int64_t>         callbacks_{0};       ///< the callbacks so far
    std::atomic<bool>                 failed_{false};      ///< the API said the stream failed
    std::atomic<int>                  stall_ms_{0};        ///< the next callback's stall, then none
    std::vector<std::int64_t>         times_;              ///< each stamped callback's time
    std::vector<int>                  frames_;             ///< each stamped callback's frames
    std::vector<int>                  xruns_;              ///< each stamped callback's xrun, 1 or 0
    std::array<std::vector<float>, 2> recorded_;           ///< the first two inputs recorded
    std::size_t                       recorded_frames_{0}; ///< the frames recorded
    std::int64_t                      played_{0};          ///< the frames played
    std::int64_t                      on_test_thread_{0};  ///< callbacks run on the test's thread
    std::int64_t                      layout_faults_{0};   ///< callbacks laid out other than promised
    const char                       *first_fault_{""};    ///< the first of them described
};

// ---- The engine's callbacks, counted ------------------------------------------------------------------------------

/** The stream stopped and closed as the scope it is declared in ends, however it ends - a failed ASSERT returns from
    a test with the stream running - and before a Recorder declared ahead of it goes, so no callback writes into a
    Recorder that has gone. Stop and Close again after a test's own are nothing. */
class StreamCloser
{
  public:
    /** A closer for the system's stream.
        @param system  the system whose stream it closes, outliving it */
    explicit StreamCloser(AudioSystem &system) : system_(system)
    {
    }

    /** The stream stopped and closed. */
    ~StreamCloser()
    {
        system_.Stop();
        system_.Close();
    }

    StreamCloser(const StreamCloser &)            = delete;
    StreamCloser &operator=(const StreamCloser &) = delete;

  private:
    AudioSystem &system_; ///< the system whose stream it closes
};

/**
    An AudioSystem round the real one that runs the engine's callback between two counts of the allocations the audio
    thread makes (CountAllocations.h), so a total of zero is an engine and a core that allocated nothing on the API's
    own thread - RtAudio's work round the callback is not counted, the engine's and the core's is.
*/
class CountingAudioSystem final : public AudioSystem, private AudioCallback
{
  public:
    /** A relay round the real system.
        @param inner  the system the stream opens on */
    explicit CountingAudioSystem(std::unique_ptr<AudioSystem> inner) : inner_(std::move(inner))
    {
    }

    std::string Name() const override
    {
        return inner_->Name();
    }

    std::vector<DeviceInfo> Devices() override
    {
        return inner_->Devices();
    }

    bool Open(const StreamRequest &request, AudioCallback &callback) override
    {
        callback_ = &callback;
        return inner_->Open(request, *this);
    }

    bool Start() override
    {
        return inner_->Start();
    }

    void Stop() override
    {
        inner_->Stop();
    }

    void Close() override
    {
        inner_->Close();
    }

    bool IsOpen() const override
    {
        return inner_->IsOpen();
    }

    StreamFormat Format() const override
    {
        return inner_->Format();
    }

    std::string LastError() const override
    {
        return inner_->LastError();
    }

    /** @return the engine's callbacks so far */
    std::int64_t callbacks() const noexcept
    {
        return callbacks_.load(std::memory_order_acquire);
    }

    /** @return the allocations the engine's callbacks made, all of them */
    std::int64_t allocations() const noexcept
    {
        return allocations_.load(std::memory_order_acquire);
    }

  private:
    void Process(const float *const *inputs, int input_channels, float *const *outputs, int output_channels, int frames,
                 std::int64_t now, bool xrun) noexcept override
    {
        std::size_t made = 0;
        {
            const reckless::test::CountAllocations count;
            callback_->Process(inputs, input_channels, outputs, output_channels, frames, now, xrun);
            made = count.count();
        }
        allocations_.store(allocations_.load(std::memory_order_relaxed) + static_cast<std::int64_t>(made),
                           std::memory_order_release);
        callbacks_.store(callbacks_.load(std::memory_order_relaxed) + 1, std::memory_order_release);
    }

    void StreamFailed() noexcept override
    {
        callback_->StreamFailed();
    }

    std::unique_ptr<AudioSystem> inner_;             ///< the real system
    AudioCallback               *callback_{nullptr}; ///< the engine's callback, which the relay runs
    std::atomic<std::int64_t>    callbacks_{0};      ///< the engine's callbacks so far
    std::atomic<std::int64_t>    allocations_{0};    ///< the allocations they made
};

// ---- The rigs that need no platform code ---------------------------------------------------------------------------

/** CoreAudio's test device: the machine's default output, written with silence - the Mac runner's own speakers. An
    input is never opened: on macOS it would ask the person at the machine for the microphone. */
class CoreAudioRig final : public DeviceRig
{
  public:
    TestDevice Find(const std::vector<DeviceInfo> &devices) override
    {
        TestDevice device;
        for (const DeviceInfo &info : devices)
        {
            if (info.is_default_output && info.output_channels > 0)
            {
                device.found       = true;
                device.output      = info.name;
                device.channels    = std::min(info.output_channels, 2);
                device.sample_rate = info.DefaultSampleRate();
                device.block_size  = 256;
                device.loop        = Loop::kNone;
                device.xrun_by     = reckless::audio::test::XrunBy::kStall;
                device.about       = "the default output, written with silence (no input: one would ask the person at "
                                     "the machine for the microphone)";
                return device;
            }
        }
        device.missing = "CoreAudio lists no default output device";
        return device;
    }
};

/** ALSA's test device: the kernel's loopback card (snd-aloop), whose playback on device 0 comes back as capture on
    device 1, subdevice by subdevice, bit for bit - the CI job loads it and gives its user the card's nodes. Its two
    devices carry one name in RtAudio's list, the second with its ordinal (RtAudioDevice.h). */
class AlsaRig final : public DeviceRig
{
  public:
    static constexpr const char *kPlayback = "Loopback (Loopback PCM)";     ///< the card's device 0, played
    static constexpr const char *kCapture  = "Loopback (Loopback PCM) (2)"; ///< its device 1, the playback back

    TestDevice Find(const std::vector<DeviceInfo> &devices) override
    {
        TestDevice device;
        const int  out = reckless::audio::FindDevice(devices, kPlayback, reckless::audio::Direction::kOutput);
        const int  in  = reckless::audio::FindDevice(devices, kCapture, reckless::audio::Direction::kInput);
        if (out < 0 || in < 0)
        {
            device.missing = CardLoaded() ? "the loopback card is loaded but ALSA lists it to this user only in part "
                                            "or not at all: give the user read and write on its /dev/snd nodes "
                                            "(tools/audio-test-devices.sh does it with setfacl)"
                                          : "no ALSA loopback card: sudo modprobe snd-aloop, then give the user "
                                            "read and write on its /dev/snd nodes (tools/audio-test-devices.sh "
                                            "does both)";
            return device;
        }
        device.found          = true;
        device.output         = kPlayback;
        device.input          = kCapture;
        device.engine_input   = kCapture;
        device.engine_key     = false; // the capture side of a loopback pair opens as many channels as its playback
        device.channels       = 2;
        device.sample_rate    = 48000;
        device.block_size     = 256;
        device.loop           = Loop::kExact;
        device.rate_tolerance = 0.03;
        device.xrun_by        = reckless::audio::test::XrunBy::kStall; // RtAudio's ALSA back end, patched by the build
        device.about = "snd-aloop: playback on hw:Loopback,0 comes back as capture on hw:Loopback,1, bit for bit";
        return device;
    }

  private:
    /** @return whether the kernel has a loopback card, whoever may open it (/proc/asound/cards is everyone's to
                read) */
    static bool CardLoaded()
    {
        std::ifstream cards("/proc/asound/cards");
        std::string   line;
        while (std::getline(cards, line))
        {
            if (line.find("Loopback") != std::string::npos)
            {
                return true;
            }
        }
        return false;
    }
};

inline std::unique_ptr<DeviceRig> MakeRig(const std::string &api)
{
    if (api == "CoreAudio")
    {
        return std::make_unique<CoreAudioRig>();
    }
    if (api == "ALSA")
    {
        return std::make_unique<AlsaRig>();
    }
#if defined(__linux__) && defined(RECKLESS_AUDIO_TEST_JACK)
    if (api == "JACK")
    {
        return reckless::audio::test::MakeJackRig();
    }
#endif
#if defined(_WIN32)
    if (api == "Windows Audio" || api == "Windows Audio (Low Latency Mode)")
    {
        return reckless::audio::test::MakeWasapiRig(false);
    }
    if (api == "Windows Audio (Exclusive Mode)")
    {
        return reckless::audio::test::MakeWasapiRig(true);
    }
#if defined(RECKLESS_AUDIO_TEST_ASIO)
    if (api == "ASIO")
    {
        return reckless::audio::test::MakeAsioRig();
    }
#endif
#endif
    return nullptr;
}
} // namespace reckless::audio::test::suite

#if !(defined(_WIN32) && defined(RECKLESS_AUDIO_TEST_ASIO))
// No ASIO test driver in this build: nothing was registered, and nothing needs putting back.
void reckless::audio::test::FinishAsioRegistration()
{
}

std::string reckless::audio::test::AsioRegistrationReport()
{
    return {};
}

std::vector<std::vector<float>> reckless::audio::test::AsioTestDriverWaves()
{
    return {};
}
#endif

namespace reckless::audio::test::suite
{
// ---- Running and measuring a stream --------------------------------------------------------------------------------

/** The message thread's wait while a stream runs: `seconds` in 10 ms steps, `each` called at every step. */
template <typename Each> void RunFor(double seconds, Each each)
{
    const std::int64_t until = NowNanoseconds() + static_cast<std::int64_t>(seconds * 1e9);
    while (NowNanoseconds() < until)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        each();
    }
}

inline void RunFor(double seconds)
{
    RunFor(seconds, [] {});
}

inline StreamRequest RequestFor(const TestDevice &device, bool with_input)
{
    StreamRequest request;
    request.output_device = device.output;
    request.output_count  = device.channels;
    if (with_input && !device.input.empty())
    {
        request.input_device = device.input;
        request.input_count  = device.channels;
    }
    request.sample_rate = device.sample_rate;
    request.block_size  = device.block_size;
    request.client_name = kClient;
    return request;
}

/** What a run's callbacks did against what its rate and block predict. */
struct Timing
{
    std::int64_t callbacks{0};          ///< the callbacks counted
    int          fewest{0};             ///< the fewest frames a stamped callback carried
    int          most{0};               ///< the most
    double       to_first_ms{0.0};      ///< from the Start to the first callback
    double       predicted{0.0};        ///< the callbacks the rate and the block predict from the first to the Stop
    double       rate_by_clock{0.0};    ///< frames over the clock's time, past the first 200 ms
    double       clock_seconds{0.0};    ///< the time that rate was read over
    double       worst_gap_blocks{0.0}; ///< the longest wait between two callbacks, in blocks
};

inline Timing Measure(const Recorder &recorder, std::int64_t started, std::int64_t stopped, int rate, int block)
{
    Timing      timing;
    std::size_t n    = recorder.stamped();
    timing.callbacks = recorder.callbacks();
    if (n == 0 || rate <= 0 || block <= 0)
    {
        return timing;
    }
    timing.fewest = recorder.frames_at(0);
    timing.most   = recorder.frames_at(0);
    for (std::size_t i = 0; i < n; ++i)
    {
        timing.fewest = std::min(timing.fewest, recorder.frames_at(i));
        timing.most   = std::max(timing.most, recorder.frames_at(i));
    }
    const double block_ns = static_cast<double>(block) * 1e9 / rate;
    const auto   t0       = recorder.time_at(0);
    timing.to_first_ms    = static_cast<double>(t0 - started) * 1e-6;
    timing.predicted      = static_cast<double>(stopped - t0) / block_ns + 0.5;
    for (std::size_t i = 1; i < n; ++i)
    {
        timing.worst_gap_blocks = std::max(
            timing.worst_gap_blocks, static_cast<double>(recorder.time_at(i) - recorder.time_at(i - 1)) / block_ns);
    }
    // The rate by the clock past the start, where an API may call back in a burst to fill its buffers.
    std::size_t k = 0;
    while (k + 1 < n && recorder.time_at(k) < t0 + 200'000'000)
    {
        ++k;
    }
    std::int64_t frames = 0;
    for (std::size_t i = k; i + 1 < n; ++i)
    {
        frames += recorder.frames_at(i);
    }
    timing.clock_seconds = static_cast<double>(recorder.time_at(n - 1) - recorder.time_at(k)) * 1e-9;
    timing.rate_by_clock = timing.clock_seconds > 0.0 ? static_cast<double>(frames) / timing.clock_seconds : 0.0;
    return timing;
}

/** The xruns a run of `callbacks` may report and still pass: a slack for a runner's scheduler, printed beside the
    count. */
inline std::int64_t XrunAllowance(std::int64_t callbacks)
{
    return 2 + callbacks / 100;
}

/** The xruns a run of `callbacks` on `device` may report beside XrunAllowance and the ones a test makes: on a device
    whose clock is a thread the machine schedules (TestDevice::is_scheduler_timed), one in ten - its late cycles are
    the machine's load, and each loses its time, which the device's wider rate_tolerance allows for too - and on any
    other device none. */
inline std::int64_t LoadAllowance(const TestDevice &device, std::int64_t callbacks)
{
    return device.is_scheduler_timed ? callbacks / 10 : 0;
}

// ---- The fixture ---------------------------------------------------------------------------------------------------

/** One API of the build (or one the run requires that the build lacks): its rig - made first, as JACK's starts the
    server the API then finds - and its AudioSystem, destroyed in the other order. */
class AudioDevices : public ::testing::TestWithParam<std::string>
{
  protected:
    /** The rig and the system for the case's API, or the case failed where the build does not carry it. */
    void SetUp() override
    {
        api_            = GetParam();
        const auto apis = reckless::audio::AudioApis();
        if (std::find(apis.begin(), apis.end(), api_) == apis.end())
        {
            FAIL() << api_ << " is required (RECKLESS_AUDIO_REQUIRE) and this build does not carry it: the build "
                   << "carries " << JoinList(apis);
        }
        rig_ = MakeRig(api_);
        ASSERT_NE(rig_, nullptr) << "no test rig for " << api_;
        system_ = reckless::audio::MakeAudioSystem(api_);
        ASSERT_NE(system_, nullptr) << api_;
    }

    /** The system, then the rig. */
    void TearDown() override
    {
        system_.reset();
        rig_.reset();
    }

    /** @return the test device, in the API's list probed afresh */
    TestDevice Find()
    {
        return rig_->Find(system_->Devices());
    }

    /** The settings an engine opens the test device with: its devices, rate, block and every channel.
        @param device  the test device
        @return        the settings */
    AudioSettings EngineSettings(const TestDevice &device) const
    {
        AudioSettings settings;
        settings.api           = api_;
        settings.output_device = device.output;
        settings.input_device  = device.engine_input;
        settings.sample_rate   = device.sample_rate;
        settings.block_size    = device.block_size;
        std::vector<int> channels;
        for (int c = 0; c < device.channels; ++c)
        {
            channels.push_back(c);
        }
        settings.outputs = channels;
        if (!device.engine_input.empty())
        {
            settings.inputs = channels;
            if (!device.engine_key)
            {
                settings.key = std::vector<int>{};
            }
        }
        return settings;
    }

    std::string                  api_;    ///< the case's API
    std::unique_ptr<DeviceRig>   rig_;    ///< its test device's rig, made first
    std::unique_ptr<AudioSystem> system_; ///< its system
};

/** The test device, or the test skipped with the reason printed - or failed, naming what is missing, where the run
    requires the API. */
#define RECKLESS_NEED_TEST_DEVICE(device)                                                                       \
    if ((device).found)                                                                                         \
    {                                                                                                           \
    }                                                                                                           \
    else if (Required(api_))                                                                                    \
        FAIL() << api_                                                                                          \
               << " is required (RECKLESS_AUDIO_REQUIRE) and its test device is missing: " << (device).missing; \
    else                                                                                                        \
        GTEST_SKIP() << api_ << " has no test device here: " << (device).missing

inline std::string ApiTestName(const ::testing::TestParamInfo<std::string> &info)
{
    std::string name;
    for (const char c : info.param)
    {
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9'))
        {
            name += c;
        }
    }
    return name;
}
} // namespace reckless::audio::test::suite

/** The time a device has to start again after an xrun - the one a test makes, from the callback told of it, before the
    stream after it is read, and any the engine's run reports, each of which may cost it this time's callbacks - and how
    long the run goes on after the xrun a test makes. The reading holds the stream once it has started
    again as a stream from its start is held (RunsAStreamAtTheRateAndTheBlockItOpenedAt): its callbacks and its rate by
    the clock within the device's rate_tolerance, and no more xruns than a runner's scheduler is allowed. So the window
    is long: a loaded runner resuming a device is slow for a moment past any settling time - on a four-core runner the
    ALSA loopback's callbacks came 36 ms late in the first 0.6 s, 6 % of so short a window and under 2 % of the two
    seconds read here - while a stream that never resumes its rate, or stops, is as far off over two seconds as over
    any, and one that goes on running dry reports many times the scheduler's allowance. */
inline constexpr std::int64_t kStartAgainNs        = 300'000'000;
inline constexpr double       kRunAfterXrunSeconds = 2.4;

namespace reckless::audio::test::suite
{
/** A device suite's main: the ASIO test driver's registration put back first, the run's APIs printed, then the cases.
    @param argc  the command line's count
    @param argv  the command line
    @return      the cases' exit code */
inline int RunDeviceSuite(int argc, char **argv)
{
    // First, before anything else reads the registry: the views the ASIO test driver's registration gave RtAudio's
    // driver list as the binary loaded, put back (AsioRig.cpp).
    reckless::audio::test::FinishAsioRegistration();
    std::printf("Audio devices: %s; the build carries %s; the run requires %s\n",
                reckless::audio::AudioLibraryVersions().c_str(), JoinList(reckless::audio::AudioApis()).c_str(),
                JoinList(RequiredApis()).c_str());
    const std::string asio = reckless::audio::test::AsioRegistrationReport();
    if (!asio.empty())
    {
        std::printf("ASIO test driver: %s\n", asio.c_str());
    }
    std::fflush(stdout);
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}
} // namespace reckless::audio::test::suite
