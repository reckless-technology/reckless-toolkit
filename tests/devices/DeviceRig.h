#pragma once

#include <cstdint>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "AudioDevice.h"

namespace reckless::audio::test
{
/**
    The real audio APIs under test (test_audio_devices.cpp): for each API the build carries, a deterministic device a
    suite can open on any machine set up for it - CI's runners among them - and what that device can show of the
    samples a stream carried. A rig finds its device in the API's list and, where the device is the suite's own to
    make, makes it: JACK's is a dummy-driver server the rig starts and stops, ASIO's the SDK's sample driver the binary
    registers for itself (AsioRig.cpp); ALSA's is the kernel's loopback card a CI job loads, CoreAudio's and Windows
    Audio's the machine's default output.
*/

/** What a stream on the test device can show of the samples it carried. */
enum class Loop
{
    kNone,       ///< output only: nothing comes back (CoreAudio's default output - an input there would ask the
                 ///< person at the machine for the microphone)
    kExact,      ///< the output comes back on the input sample for sample, bit for bit: ALSA's loopback card, a JACK
                 ///< client's outputs wired to its own inputs
    kCorrelated, ///< the output comes back through a mixer - WASAPI's loopback capture of the endpoint's mix - the
                 ///< same signal at some gain and some delay
    kKnownInput, ///< the input carries what the device makes itself: the ASIO test driver's sine and sawtooth
    kCable       ///< the output comes back on the device's own inputs through a cable a person patched: through the
                 ///< converters, the same signal at some gain, as late as the latency the API reports and two blocks
};

/** How the test device is made to report an overrun or underrun to a running stream. */
enum class XrunBy
{
    kNothing, ///< it cannot be, for the reason TestDevice::no_xrun gives
    kStall,   ///< a callback that overruns its block by several runs the device's buffer dry under it
    kDevice   ///< the device reports one of its own accord when the rig asks (DeviceRig::Xrun)
};

/** The test device on one API, as its rig found it - or why it is not there. */
struct TestDevice
{
    bool        found{false};         ///< whether the device is there; when false `missing` says why not
    std::string missing;              ///< why not, and what would provide it
    std::string output;               ///< the output device's name in the API's list
    std::string input;                ///< the input device's name for the loop; empty for none
    std::string engine_input;         ///< the input the engine's test opens; empty for none
    bool        engine_key{true};     ///< whether the engine's key bus may take the inputs past the main pair
    int         channels{2};          ///< the channels each way a stream opens
    int         first_output{0};      ///< the output device's first channel a stream opens, counted from 0
    int         first_input{0};       ///< the input device's first channel a stream opens, counted from 0
    int         sample_rate{48000};   ///< the rate a stream opens at, in hertz
    int         block_size{256};      ///< the block a stream opens at, in samples
    Loop        loop{Loop::kNone};    ///< what a stream can show of the samples it carried
    double      rate_tolerance{0.03}; ///< how far the rate by the clock may stray, as a fraction
    /** The device's clock a thread of the machine's, timed without real-time priority (JACK's dummy driver: a runner
        gives jackd none): the machine's load delays its cycles, and a cycle that comes past its time is an xrun the
        driver starts its schedule again after, the time lost. Such xruns and the time they lose are the machine's, not
        the layer's, so a stream on it is allowed one in ten of its callbacks (LoadAllowance) beside what any device
        is allowed, and its rate_tolerance is wider. */
    bool        is_scheduler_timed{false}; ///< whether the device's clock is a thread of the machine's, as above
    XrunBy      xrun_by{XrunBy::kNothing}; ///< how the device is made to report an xrun
    std::string no_xrun;                   ///< for XrunBy::kNothing: why an xrun cannot reach the callback
    /** Whether what the API reports accounts for the whole path a cable measures. Where it does, a cable's round trip
        is held to two blocks and the reported latency within a 64th of a second. Where it does not - Windows Audio's
        shared mode, whose GetStreamLatency leaves out the audio engine's own mixing path - the round trip is held only
        to being no sooner than what is reported, and the shortfall is printed rather than judged. */
    bool        does_report_whole_round_trip{true};
    std::string partial_latency; ///< for the above being false: what the API leaves out of the figure it reports
    std::string about;           ///< what the device is, printed with the tables
};

/** The text with the spaces and tabs at either end taken off.
    @param text  the text
    @return      the text trimmed; empty where it is nothing but spaces */
inline std::string Trim(const std::string &text)
{
    const auto first = text.find_first_not_of(" \t");
    const auto last  = text.find_last_not_of(" \t");
    return first == std::string::npos ? std::string() : text.substr(first, last - first + 1);
}

/** An environment variable's value, trimmed; empty where it is not set.
    @param name  the variable's name
    @return      its value, or empty */
inline std::string Environment(const char *name)
{
    // MSVC deprecates getenv for _dupenv_s, whose copy a test has no use for.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char *text = std::getenv(name);
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return text != nullptr ? Trim(std::string(text)) : std::string();
}

/** A cable a person patched, as every rig's loop variable names it: "<output>:<input>", the first of two output
    channels and the first of two inputs, counted from 1 - "3:1" for outputs 3 and 4 into inputs 1 and 2. It makes the
    device's loop a cable's, so the stream, loop and silence tests run duplex through it.

    One parser for CoreAudio's, Windows Audio's and ASIO's rigs: the variable's name differs, nothing else does.

    @param device           the test device, whose channels, first channel each way, loop and description it sets
    @param cable            the variable's value
    @param output_channels  the output device's channels, which the pair must be within
    @param input_channels   the input device's channels, which the pair must be within
    @param variable         the variable's name, for the description and the reason it is refused
    @return                 true where the cable was applied; false with `device.missing` set where it names no such
                            pair, `device.found` cleared */
inline bool ApplyCable(TestDevice &device, const std::string &cable, int output_channels, int input_channels,
                       const char *variable)
{
    const auto colon        = cable.find(':');
    const int  first_output = colon == std::string::npos ? 0 : std::atoi(cable.substr(0, colon).c_str());
    const int  first_input  = colon == std::string::npos ? 0 : std::atoi(cable.substr(colon + 1).c_str());
    if (first_output < 1 || first_input < 1 || first_output + 1 > output_channels || first_input + 1 > input_channels)
    {
        device.found   = false;
        device.missing = std::string(variable) + " is '" + cable +
                         "': it names two outputs and two inputs the devices have as \"<output>:<input>\", counted "
                         "from 1 - \"3:1\" for outputs 3 and 4 into inputs 1 and 2";
        return false;
    }
    device.channels     = 2;
    device.first_output = first_output - 1;
    device.first_input  = first_input - 1;
    device.loop         = Loop::kCable;
    device.about        = "outputs " + std::to_string(first_output) + " and " + std::to_string(first_output + 1) +
                          " patched by cable to inputs " + std::to_string(first_input) + " and " +
                          std::to_string(first_input + 1) + " (" + variable + "), " + device.about;
    return true;
}

/** One API's test device: found, made, wired, and made to fail. Message thread throughout. */
class DeviceRig
{
  public:
    virtual ~DeviceRig() = default;

    /** The test device among `devices`, the API's list probed afresh.
        @param devices  the API's devices, as its system lists them
        @return         the test device, or one with `found` false saying why it is not there */
    virtual TestDevice Find(const std::vector<DeviceInfo> &devices) = 0;

    /** A stream whose client is named `client` has started on the device: JACK's rig wires its outputs to its inputs
        here.
        @param client  the name of the stream's client on the API
        @param why     set to the reason when the loop could not be made
        @return        false when the loop could not be made */
    virtual bool Started(const std::string &client, std::string &why)
    {
        (void)client;
        (void)why;
        return true;
    }

    /** The device or its server taken away from under a running stream.
        @param why  set to the reason where the rig cannot
        @return     false where the rig cannot take it away */
    virtual bool Vanish(std::string &why)
    {
        why = "this API's test device cannot be taken away by a test";
        return false;
    }

    /** The device brought back after Vanish, for a stream to open on again.
        @param why  set to the reason where the rig cannot
        @return     false where the rig cannot bring it back */
    virtual bool Return(std::string &why)
    {
        why = "this API's test device cannot be taken away by a test";
        return false;
    }

    /** For XrunBy::kDevice: one overrun or underrun reported to the running stream, as the device reports one; false,
        where the rig cannot make one.
        @param why  set to the reason where the rig cannot
        @return     false where the rig cannot make one */
    virtual bool Xrun(std::string &why)
    {
        why = "this API's test device cannot be made to report an xrun";
        return false;
    }
};

/** The rig of JACK, made in JackRig.cpp.
    @return  the rig */
std::unique_ptr<DeviceRig> MakeJackRig();

/** The rig of ASIO, made in AsioRig.cpp.
    @return  the rig */
std::unique_ptr<DeviceRig> MakeAsioRig();

/** The rig of one of Windows Audio's modes, made in WasapiRig.cpp.
    @param exclusive  true for the exclusive mode, output only; false for the shared mode
    @return           the rig */
std::unique_ptr<DeviceRig> MakeWasapiRig(bool exclusive);

/** Windows with the ASIO test driver: the registry views the binary gave the ASIO back end's driver list as it was
   built, put back; elsewhere nothing. Called first thing in main (AsioRig.cpp says why). */
void FinishAsioRegistration();

/** What the ASIO test driver's registration did, for the header the suite prints; empty where there is none.
    @return  the report, one or more lines of text */
std::string AsioRegistrationReport();

/** The ASIO test driver's two input waves, one block each - its sine on the first input and its sawtooth on the
    second, every block alike - as a stream reads them, int16 / 32768; empty where there is no driver.
    @return  one block of samples for each of the two inputs, in -1 to 1 */
std::vector<std::vector<float>> AsioTestDriverWaves();

#if defined(__APPLE__)
/** A CoreAudio device of the test's own (AggregateMac.cpp): an aggregate of the default output alone, private to this
    process - no other process lists it, and it goes with the process - under the name it is made with, for as long as
    the object lives. */
class PrivateAggregate
{
  public:
    explicit PrivateAggregate(const std::string &name);
    ~PrivateAggregate();

    PrivateAggregate(const PrivateAggregate &)            = delete;
    PrivateAggregate &operator=(const PrivateAggregate &) = delete;

    bool made() const noexcept
    {
        return device_ != 0;
    }

    /** Why none was made. */
    const std::string &why() const noexcept
    {
        return why_;
    }

  private:
    std::uint32_t device_{0}; ///< the aggregate's AudioObjectID; 0, kAudioObjectUnknown, for none
    std::string   why_;
};

/** A CoreMIDI source of the test's own (MidiSourceMac.cpp): a virtual source on MIDI 1.0's protocol, under the name it
    is made with, listed while the object lives, which sends Universal MIDI Packets to whoever is connected to it. */
class PrivateMidiSource
{
  public:
    explicit PrivateMidiSource(const std::string &name);
    ~PrivateMidiSource();

    PrivateMidiSource(const PrivateMidiSource &)            = delete;
    PrivateMidiSource &operator=(const PrivateMidiSource &) = delete;

    bool made() const noexcept
    {
        return source_ != 0;
    }

    /** Why none was made. */
    const std::string &why() const noexcept
    {
        return why_;
    }

    /** One Universal MIDI Packet of `count` words sent at once. */
    void Send(const std::uint32_t *words, int count);

  private:
    std::uint32_t client_{0}; ///< the MIDIClientRef the source is made on; 0 for none
    std::uint32_t source_{0}; ///< the MIDIEndpointRef; 0 for none
    std::string   why_;
};
#endif
} // namespace reckless::audio::test
