#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "RealtimeThread.h"

namespace reckless::audio
{
/**
    The machine's audio and MIDI as a Standalone sees them, written against no library: an audio API's devices and the
    one stream it opens on them (AudioSystem), the stream's callback (AudioCallback), and the MIDI ports (MidiSystem):
    inputs whose messages land in a sink (MidiSink), and outputs that send what any thread queues, at its time. The
   implementations are the toolkit's own back ends on each platform's API (DeviceBackends.h); a suite's is a fake that
   runs the callback itself. The engine between them and a plugin's core is StandaloneEngine.

    Threads: everything on AudioSystem and MidiSystem is the message thread's but MidiSystem::Send, which is any
    thread's; AudioCallback::Process runs on the API's audio thread and MidiSink::Arrived on a MIDI port's thread, and
    StreamFailed on whichever thread the API reports from.
*/

/** The clock every device and port stamps its events with, in nanoseconds: the steady clock, the one a MIDI message's
    arrival and an audio callback's start can be compared on whichever threads read it.
    @return  the steady clock's reading, in nanoseconds since its epoch */
inline std::int64_t NowNanoseconds() noexcept
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/** One device an API offers: its name - unique within the API among the devices with channels the same way, what a
    settings file keeps it by (an input and an output may share one: Direction) - its channels each way and together,
    whether it is the system's default each way, and the rates it lists (some APIs list one, the rate a server runs
    at). */
struct DeviceInfo
{
    std::string      name;                     ///< the device's name, unique among those with channels the same way
    int              input_channels{0};        ///< the input channels it offers, 0 for an output-only device
    int              output_channels{0};       ///< the output channels it offers, 0 for an input-only device
    int              duplex_channels{0};       ///< the channels it offers in and out at once, 0 where the API has none
    bool             is_default_input{false};  ///< true when it is the system's default input
    bool             is_default_output{false}; ///< true when it is the system's default output
    std::vector<int> sample_rates;             ///< the rates it lists, in hertz; empty where the API lists none
    int              current_sample_rate{0};   ///< the rate it runs at now, 0 where the API does not say
    int              preferred_sample_rate{0}; ///< the rate the API would open it at, 0 where it does not say

    /** The names of its input channels, as the API or the driver gives them, in UTF-8, one per channel in the device's
        order: either empty - the back end cannot tell - or exactly `input_channels` long, a channel it has no name for
        left as "". Never made up: a product shows "Input 3" itself for a channel with no name. Read from what the API
        lists, never by opening a stream; ChannelNames.h says what each back end reads and what it cannot. Where a name
        is a speaker position ("FL"; ALSA's channel map) it says what the driver declares the channel carries, which on
        an interface that is not a speaker layout is its default and no label. Appended after the older fields so a
        positional initialiser of those still means what it did. */
    std::vector<std::string> input_channel_names;
    std::vector<std::string> output_channel_names; ///< the same for the output channels (`input_channel_names`)

    /** The name of one input channel.
        @param channel  the channel, counted from 0
        @return         its name, empty where the back end has none or the channel is not there; a view into this */
    std::string_view InputChannelName(int channel) const noexcept
    {
        return NameAt(input_channel_names, channel);
    }

    /** The name of one output channel.
        @param channel  the channel, counted from 0
        @return         its name, empty where the back end has none or the channel is not there; a view into this */
    std::string_view OutputChannelName(int channel) const noexcept
    {
        return NameAt(output_channel_names, channel);
    }

    /** What it can be opened as: "duplex" (in and out at once), "input", "output", or "none".
        @return  the kind's name, a string literal that outlives the device */
    std::string_view Kind() const noexcept
    {
        if (duplex_channels > 0 || (input_channels > 0 && output_channels > 0))
        {
            return "duplex";
        }
        return input_channels > 0 ? "input" : (output_channels > 0 ? "output" : "none");
    }

    /** The rate to open it at when none is asked for: the one it runs at, else the API's preference, else the
        highest it lists under 96 kHz, else 48 kHz.
        @return  the rate, in hertz */
    int DefaultSampleRate() const noexcept
    {
        if (current_sample_rate > 0)
        {
            return current_sample_rate;
        }
        if (preferred_sample_rate > 0)
        {
            return preferred_sample_rate;
        }
        int best = 0;
        for (const int rate : sample_rates)
        {
            best = rate <= 96000 && rate > best ? rate : best;
        }
        return best > 0 ? best : 48000;
    }

    /** Whether the device lists `rate`; a device that lists none takes any.
        @param rate  the sample rate, in hertz
        @return      true when the device lists it or lists no rates */
    bool Lists(int rate) const noexcept
    {
        if (sample_rates.empty())
        {
            return true;
        }
        for (const int listed : sample_rates)
        {
            if (listed == rate)
            {
                return true;
            }
        }
        return false;
    }

  private:
    /** One name of a list.
        @param names    the list, empty or one name a channel
        @param channel  the channel, counted from 0
        @return         its name; empty where the list has none or the channel is not there */
    static std::string_view NameAt(const std::vector<std::string> &names, int channel) noexcept
    {
        return channel >= 0 && static_cast<std::size_t>(channel) < names.size()
                   ? std::string_view(names[static_cast<std::size_t>(channel)])
                   : std::string_view();
    }
};

/** The stream to open: the input device (empty for none) and the output device, the run of each one's channels to
    open - from `first_*`, `*_count` of them, a callback seeing them as its channels 0 on - the rate, the block (the
    frames a callback carries; an API may give another, which the opened format says), the name a server lists the
    client under (JACK's) and the periods the output buffers.

    The periods are the product's to choose, where the API lets a stream choose them: the blocks the output device
    holds ahead of the one being written, each a block's latency and a block's margin against a late callback. ALSA
    takes them (at least two: one playing while the next is written); JACK's are its server's, and WASAPI, CoreAudio and
    ASIO buffer as they do. 0 leaves them to the back end. */
struct StreamRequest
{
    std::string input_device;       ///< the input device's name, empty for none
    std::string output_device;      ///< the output device's name
    int         first_input{0};     ///< the first device input channel to open, counted from 0
    int         input_count{0};     ///< how many input channels to open, from `first_input`
    int         first_output{0};    ///< the first device output channel to open, counted from 0
    int         output_count{0};    ///< how many output channels to open, from `first_output`
    int         sample_rate{48000}; ///< the rate to open at, in hertz
    int         block_size{512};    ///< the frames a callback carries; the API may give another
    std::string client_name;        ///< the name a server lists the client under, JACK's
    int         periods{0};         ///< the blocks the output buffers, where the API lets a stream choose; 0: its own
};

/** What a stream opened as: its rate and block, the channels the callback sees each way, the latency the API reports
    beyond the block itself - input and output summed, in samples, 0 where it reports none - and the periods its output
    buffers where the API says. The two sides of that sum are told apart as well (`input_latency_frames`,
    `output_latency_frames`), for a program that places a take by the input's alone or shows a round trip from both. */
struct StreamFormat
{
    int sample_rate{0};      ///< the rate the stream runs at, in hertz
    int block_size{0};       ///< the frames a callback carries
    int input_channels{0};   ///< the input channels the callback sees, 0 with no input
    int output_channels{0};  ///< the output channels the callback sees
    int reported_latency{0}; ///< the latency the API reports beyond the block, in samples, input and output summed
    int periods{0};          ///< the blocks the output buffers, where the API says; 0 where it does not
    /** The input side's share of `reported_latency`, in samples: what a recording is late by beyond the block the
        callback reads, which a program places a take by. 0 with no input, and 0 where the back end cannot tell the
        input's from the output's (ALSA: the figure it has is the playback buffer alone, whose capture side adds
        no latency of its own beyond the block, and a driver's own capture delay is not reported). */
    int input_latency_frames{0};
    /** The output side's share of `reported_latency`, in samples: what a sound written is late by beyond the block the
        callback writes. 0 where the back end cannot tell. With `input_latency_frames` it is `reported_latency`,
        exactly, on every back end - a back end that tells neither side leaves both 0 and the sum as it was. */
    int output_latency_frames{0};
};

/** What runs a stream. */
class AudioCallback
{
  public:
    virtual ~AudioCallback() = default;

    /** Audio thread: one block of `frames` samples - each input channel's samples at `inputs[c]` (null with none), each
        output channel's to be written at `outputs[c]`, every channel the stream opened - with the clock's reading as
        the API called (NowNanoseconds, or a fake's) and whether the API reported an overrun or underrun since the last
        block. Never allocates, locks or logs.
        @param inputs           one pointer a device input channel to `frames` samples; null with no input
        @param input_channels   the input channels, the stream's `StreamFormat::input_channels`
        @param outputs          one pointer a device output channel to `frames` samples to be written
        @param output_channels  the output channels, the stream's `StreamFormat::output_channels`
        @param frames           the samples a channel carries in this block
        @param now              the clock's reading as the API called, in nanoseconds
        @param xrun             true when the API reported an overrun or underrun since the last block */
    virtual void Process(const float *const *inputs, int input_channels, float *const *outputs, int output_channels,
                         int frames, std::int64_t now, bool xrun) noexcept = 0;

    /** Any thread: the stream stopped of its own accord - its device went away, or the API failed under it. The
        callback will not be called again; the stream is closed from the message thread. */
    virtual void StreamFailed() noexcept = 0;
};

/** One audio API - CoreAudio, Windows Audio (WASAPI), ASIO, ALSA, JACK - its devices and its one stream. Message
    thread throughout. */
class AudioSystem
{
  public:
    virtual ~AudioSystem() = default;

    /** The API's name as the series' settings keep it - JUCE's device type names, so a person's choice carries over:
        "CoreAudio", "Windows Audio", "ASIO", "ALSA", "JACK".
        @return  the name */
    virtual std::string Name() const = 0;

    /** The devices it offers now, probed afresh (one plugged in since appears).
        @return  the devices, in the API's order */
    virtual std::vector<DeviceInfo> Devices() = 0;

    /** The stream `request` asks for, opened on `callback` and not started; false, with LastError() saying why, when
        it cannot be. A stream already open is closed first.
        @param request   the devices, channels, rate and block to open
        @param callback  what runs the stream; it must outlive the stream, until Close()
        @return          true when the stream opened */
    virtual bool Open(const StreamRequest &request, AudioCallback &callback) = 0;

    /** The opened stream started: the callback runs from here. False, with LastError(), when it does not start.
        @return  true when the stream is running */
    virtual bool Start() = 0;

    /** The stream stopped: when this returns, the callback is not running and will not run until the next Start. */
    virtual void Stop() = 0;

    /** The stream stopped and closed; nothing to do with none open. */
    virtual void Close() = 0;

    /** Whether a stream is open.
        @return  true from a successful Open() until Close() */
    virtual bool IsOpen() const = 0;

    /** What the open stream opened as.
        @return  the stream's rate, block, channels and reported latency; zeros with none open */
    virtual StreamFormat Format() const = 0;

    /** The last error or warning the API reported, for a person to read.
        @return  the text, empty when there is none */
    virtual std::string LastError() const = 0;

    /** The calling thread - one of the consumer's own, a worker that renders beside the callback inside its deadline -
        given the standing the open stream's callback thread has: the scheduling policy and priority, the multimedia
        scheduler's class or the audio workgroup, whichever the platform and API give (docs/audio/native-backends.md,
        "Threads of a consumer's own"). Called by the thread itself, once at its start, and undone by `membership` at
        its end - RealtimeThreadScope does both. Any thread, but not while the stream is being opened or closed: the
        caller joins its workers after Open() and lets them leave before Close().

        Not realtime-safe, on purpose: it may allocate and ask the kernel, since a thread does it once, never in steady
        state. A refusal - Linux with no `rtprio` limit, no stream open - is a result with its reason, never an error
        or an abort; the thread then runs as it was. This default refuses with a stream not open or an API that gives
        a consumer's thread nothing; a back end that can overrides it.
        @param membership  receives what undoes the join, when one was made
        @return            what was achieved and what was refused, and why */
    virtual RealtimeThreadResult JoinAudioThread(RealtimeThreadMembership &membership) const
    {
        static_cast<void>(membership);
        RealtimeThreadResult result;
        result.refused = IsOpen() ? "this audio API gives a consumer's own threads no realtime standing"
                                  : "no stream is open, so there is no callback thread whose standing to give";
        return result;
    }
};

/** Where MIDI input lands. */
class MidiSink
{
  public:
    virtual ~MidiSink() = default;

    /** A port's thread: one message of `size` bytes from the input opened as `port`, arrived at `time` on the clock
        audio callbacks are stamped with. Never blocks.
        @param port   the index the input was opened under
        @param time   the arrival, in nanoseconds on the clock audio callbacks are stamped with
        @param bytes  the message's bytes, valid for the call only
        @param size   the message's length, in bytes */
    virtual void Arrived(int port, std::int64_t time, const std::uint8_t *bytes, int size) noexcept = 0;
};

/** How many MIDI outputs can be open at once, and so the port numbers MidiSystem::OpenOutput takes: 0 to 63. A table
    of fixed size is what lets Send find a port's queue without a lock or a search. */
inline constexpr int kMidiOutputPorts = 64;

/** The machine's MIDI inputs and outputs.

    Threads: Inputs, Open, Outputs, OpenOutput, CloseAll and LastError are the owner's - the message thread, or
    whichever one thread owns the system. Send is for any thread, an audio callback among them, and for several at
    once on one port: it is wait-free, takes no lock, allocates nothing and calls nothing of the system's.

    Output: each output opened has a queue of its own (kMidiOutputQueueCapacity messages) and is served by the back
    end's sender, an ordinary thread that is never a realtime one and that no Send waits for. A message is queued with
    the time it is to leave on NowNanoseconds' clock, and leaves then - or at once when that time has passed when the
    sender reaches it - in the order of the times; two of one time leave in the order one thread sent them. System
    exclusive is not sent, nor running status, nor the system common messages: Send takes one whole channel message or
    one system real-time message (IsSendableMidiMessage in MidiOutputQueue.h).

    Lifetime: a Send racing CloseAll, or racing an OpenOutput that reopens its port, either returns false or has its
    message dropped; it never touches an output that has been closed, and once CloseAll returns nothing is sent. What
    was queued and not yet sent when an output closes is dropped, including what a back end had handed its platform
    ahead of the time. A device that goes away while open is not reported - the interface has no notice of a port
    going for inputs either - and its messages go nowhere until it is opened again. */
class MidiSystem
{
  public:
    virtual ~MidiSystem() = default;

    /** The inputs it offers now, by name.
        @return  the input names, in the API's order */
    virtual std::vector<std::string> Inputs() = 0;

    /** Input `name` opened as port `port` (the index its messages carry to `sink`); false, with LastError(), when
        there is no such input or it will not open. A port already open under that index is closed first.
        @param name  the input's name, as Inputs() lists it
        @param port  the index its messages carry to `sink`
        @param sink  where the messages land; it must outlive the port, until CloseAll()
        @return      true when the port opened */
    virtual bool Open(const std::string &name, int port, MidiSink &sink) = 0;

    /** The outputs it offers now, by name. The owner's thread.
        @return  the output names, in the API's order */
    virtual std::vector<std::string> Outputs() = 0;

    /** Output `name` opened as port `port`, the number Send names it by; false, with LastError(), when there is no such
        output, `port` is outside 0 to kMidiOutputPorts - 1, or the output will not open. An output already open as
        `port` is closed first, what it had queued dropped. Several outputs may be open at once, each as its own port.
        The owner's thread.
        @param name  the output's name, as Outputs() lists it; found as Open finds an input's
        @param port  the number Send names it by, 0 to kMidiOutputPorts - 1
        @return      true when the output opened */
    virtual bool OpenOutput(const std::string &name, int port) = 0;

    /** Any thread, wait-free: a message queued to the output open as `port`, to leave at `time_nanoseconds` on
        NowNanoseconds' clock, or at once if that time has passed (the class says how the messages are ordered). False,
        with nothing queued, when the port is not open, its queue is full, or the message is not one whole channel
        message or system real-time message of the length its status asks (IsSendableMidiMessage). Sets no LastError,
        which is the owner's.
        @param port              the port the output was opened as
        @param time_nanoseconds  when the message is to leave, in nanoseconds on NowNanoseconds' clock
        @param bytes             the message's bytes, the status first; read during the call only
        @param size              its length, 1 to 3
        @return                  true when it was queued */
    virtual bool Send(int port, std::int64_t time_nanoseconds, const std::uint8_t *bytes, int size) noexcept = 0;

    /** Every open input and output closed: when this returns, no sink is called again and nothing more is sent - what
        an output had queued and not yet sent is dropped, and its sender has stopped. The owner's thread. */
    virtual void CloseAll() = 0;

    /** The last error the system reported, for a person to read.
        @return  the text, empty when there is none */
    virtual std::string LastError() const = 0;
};

/** Which way a device is wanted: a name may stand for two devices, an input and an output (a USB microphone with a
    headphone socket lists as both under one name on CoreAudio), so a device is looked up by its name and its way. */
enum class Direction
{
    kInput,
    kOutput
};

/** Whether `device` has channels the way `direction` asks.
    @param device     the device
    @param direction  the way a device is wanted
    @return           true when it has input channels for kInput, output channels for kOutput */
inline bool Carries(const DeviceInfo &device, Direction direction) noexcept
{
    return direction == Direction::kInput ? device.input_channels > 0 : device.output_channels > 0;
}

/** The device in `devices` a settings file's `name` means: the device of that name among those with channels the way
    `direction` asks, as an index, or -1 for none.
    @param devices    the devices an API offers
    @param name       the device's name
    @param direction  the way the device is wanted
    @return           the index in `devices`, or -1 for none */
inline int FindDevice(const std::vector<DeviceInfo> &devices, std::string_view name, Direction direction) noexcept
{
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        if (devices[i].name == name && Carries(devices[i], direction))
        {
            return static_cast<int>(i);
        }
    }
    return -1;
}

/** `devices` named apart, as FindDevice needs them: a device whose name an earlier one with channels the same way
    already carries takes that name with its ordinal after it - "Loopback (Loopback PCM) (2)", ALSA's loopback card's
    second device - the lowest from 2 that is free; an input and an output of one name keep it. This is how RtAudio's
    devices were named apart when the settings in the field were written, so every back end names its devices so.
    @param devices  the devices, in the API's order, each under the name the API gives it; renamed in place */
inline void NameApart(std::vector<DeviceInfo> &devices)
{
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        const auto taken = [&devices, i](const std::string &name) {
            for (std::size_t j = 0; j < i; ++j)
            {
                const bool same_way = (devices[j].input_channels > 0 && devices[i].input_channels > 0) ||
                                      (devices[j].output_channels > 0 && devices[i].output_channels > 0);
                if (same_way && devices[j].name == name)
                {
                    return true;
                }
            }
            return false;
        };
        const std::string given = devices[i].name;
        for (int ordinal = 2; taken(devices[i].name); ++ordinal)
        {
            devices[i].name = given + " (" + std::to_string(ordinal) + ")";
        }
    }
}
} // namespace reckless::audio
