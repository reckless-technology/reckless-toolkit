// The suite's AudioSystem and MidiSystem on RtAudio and RtMidi (RtAudioDevice.h says what each does and why). The
// one translation unit that includes the two libraries' headers.
#include "RtAudioDevice.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <RtAudio.h>
#include <RtMidi.h>

#if defined(_WIN32)
#include "win/WasapiDevice.h"
#endif

#if defined(RECKLESS_AUDIO_ASIO_HOST)
#include "asiodrivers.h" // the ASIO SDK's host code (RtAudio.h has brought in windows.h, with NOMINMAX)

// The SDK's own list of drivers (its host/asiodrivers.cpp, compiled into RtAudio's library): the one its ASIOExit
// releases the current driver through.
extern AsioDrivers *asioDrivers;
#endif

namespace reckless::audio
{
namespace
{
#if defined(RECKLESS_AUDIO_ASIO_HOST)
/** The SDK's ASIOExit, which RtAudio calls after every probe of a driver and as it closes a stream, releases the
    current driver through the SDK's global list - set only by the SDK's loadAsioDriver, which RtAudio never calls,
    keeping a list of its own - and dereferences it without a check: RtAudio's bundled copy of the host code checks it,
    but the build compiles the SDK's own (the root CMakeLists.txt says why), and with it the first probe of any driver
    read through a null pointer. So the global is given a list of its own, which loads no driver: ASIOExit asks it to
    release none, and the release is RtAudio's, through its own list, as with RtAudio's copy. Made after RtAudio's ASIO
    back end, so the list's own start of COM on this thread is a nested one. */
void GiveAsioExitAList()
{
    static AsioDrivers none;
    if (asioDrivers == nullptr)
    {
        asioDrivers = &none;
    }
}
#endif

/** The APIs the series opens, by its names, in the order a Standalone prefers them (AudioApis says why). */
struct ApiName
{
    RtAudio::Api api;  ///< RtAudio's identifier for the API
    const char  *name; ///< the series' name for it, a string literal
};

/** Every API the series opens on RtAudio, in its order of preference; AudioApis() keeps those RtAudio is compiled with.
    Windows Audio is the suite's own (win/WasapiDevice.h), never RtAudio's. */
constexpr ApiName kApis[] = {
    {RtAudio::MACOSX_CORE, "CoreAudio"},
    {RtAudio::WINDOWS_ASIO, "ASIO"},
    {RtAudio::UNIX_JACK, "JACK"},
    {RtAudio::LINUX_ALSA, "ALSA"},
};

#if defined(_WIN32)
/** Windows Audio's three modes, in the order a Standalone lists them, ahead of ASIO. */
constexpr wasapi::Mode kWasapiModes[] = {wasapi::Mode::kShared, wasapi::Mode::kLowLatency, wasapi::Mode::kExclusive};
#endif

/** Whether the message thread is inside one of its own calls into RtAudio: an error RtAudio reports from within one
    is that call's - a probe, an open - and not the running stream failing. */
thread_local bool t_in_call = false;

/** The message thread's mark that it is inside a call into RtAudio, for the scope of the object: t_in_call is true
    until it is destroyed and then what it was, so calls nest. */
class InCall
{
  public:
    /** Mark this thread as inside an RtAudio call, remembering the mark it found so a nested call puts it back. */
    InCall() noexcept : was_(t_in_call)
    {
        t_in_call = true;
    }

    /** Put the thread's mark back as it stood on entry. */
    ~InCall()
    {
        t_in_call = was_;
    }

    InCall(const InCall &)            = delete;
    InCall &operator=(const InCall &) = delete;

  private:
    bool was_; ///< t_in_call as it stood on entry, put back on exit
};

/** ALSA reports a device that went away mid-stream only as a warning, in the text of a failed read or write.
    @param text  the text of an RtAudio warning
    @return      true when the text says the device is disconnected or gone */
bool NamesDisconnection(const std::string &text)
{
    return text.find("DISCONNECTED") != std::string::npos || text.find("No such device") != std::string::npos;
}

/** One audio API on RtAudio: its devices, listed under unique names, and its one non-interleaved float stream, whose
    callback counts itself in and out so that Stop is a promise. Message thread throughout but for Callback and the
    error callback (Report), which run on RtAudio's threads. */
class RtAudioSystem final : public AudioSystem
{
  public:
    /** An RtAudio on `api`, its errors reported to this system.
        @param api   the RtAudio API to use
        @param name  the series' name for it, which Name() returns */
    RtAudioSystem(RtAudio::Api api, std::string name) : name_(std::move(name)), api_(api), audio_(MakeRtAudio())
    {
    }

    /** The stream closed (Close). */
    ~RtAudioSystem() override
    {
        Close();
    }

    std::string Name() const override
    {
        return name_;
    }

    std::vector<DeviceInfo> Devices() override
    {
        const InCall call;
        if (name_ == "ASIO" && audio_->isStreamOpen())
        {
            return devices_; // an ASIO driver cannot be probed while one is open: the list from before
        }
        std::vector<DeviceInfo> devices;
        for (const Listed &listed : List())
        {
            const RtAudio::DeviceInfo &info = listed.info;
            DeviceInfo                 device;
            device.name                  = listed.name;
            device.input_channels        = static_cast<int>(info.inputChannels);
            device.output_channels       = static_cast<int>(info.outputChannels);
            device.duplex_channels       = static_cast<int>(info.duplexChannels);
            device.is_default_input      = info.isDefaultInput;
            device.is_default_output     = info.isDefaultOutput;
            device.current_sample_rate   = static_cast<int>(info.currentSampleRate);
            device.preferred_sample_rate = static_cast<int>(info.preferredSampleRate);
            for (const unsigned int rate : info.sampleRates)
            {
                device.sample_rates.push_back(static_cast<int>(rate));
            }
            devices.push_back(std::move(device));
        }
        devices_ = devices;
        return devices;
    }

    bool Open(const StreamRequest &request, AudioCallback &callback) override
    {
        Close();
        SetError({}); // what an open that fails reports is its own reason, not an earlier warning
        const InCall call;
        // The devices listed afresh, once for both names, so a device gone since the settings listed it is caught here:
        // each listing asks the API for its devices again (every ALSA card's control opened, every ASIO driver named).
        const std::vector<Listed> listed    = List();
        const unsigned int        output_id = IdOf(listed, request.output_device, Direction::kOutput);
        if (output_id == 0 || request.output_count <= 0)
        {
            SetError("no output device named '" + request.output_device + "' on " + name_);
            return false;
        }
        RtAudio::StreamParameters output;
        output.deviceId     = output_id;
        output.nChannels    = static_cast<unsigned int>(request.output_count);
        output.firstChannel = static_cast<unsigned int>(request.first_output);

        const bool                with_input = !request.input_device.empty() && request.input_count > 0;
        RtAudio::StreamParameters input;
        if (with_input)
        {
            input.deviceId = IdOf(listed, request.input_device, Direction::kInput);
            if (input.deviceId == 0)
            {
                SetError("no input device named '" + request.input_device + "' on " + name_);
                return false;
            }
            input.nChannels    = static_cast<unsigned int>(request.input_count);
            input.firstChannel = static_cast<unsigned int>(request.first_input);
        }

        // Non-interleaved float, a real-time callback thread, and the client's name for a server that lists it. The
        // priority is the thread's round-robin priority where RtAudio makes the thread itself (ALSA's), held to the
        // range the system allows; a user the system lets raise none runs it at the normal priority.
        RtAudio::StreamOptions options;
        options.flags      = RTAUDIO_NONINTERLEAVED | RTAUDIO_SCHEDULE_REALTIME;
        options.streamName = request.client_name.empty() ? std::string("Reckless") : request.client_name;
        options.priority   = 70;

        input_count_  = with_input ? request.input_count : 0;
        output_count_ = request.output_count;
        inputs_.assign(static_cast<std::size_t>(std::max(input_count_, 1)), nullptr);
        outputs_.assign(static_cast<std::size_t>(output_count_), nullptr);
        callback_.store(&callback, std::memory_order_release);
        failed_.store(false, std::memory_order_relaxed);
        closed_by_api_.store(false, std::memory_order_relaxed);
        stopped_by_api_ = false;

        unsigned int           frames = request.block_size > 0 ? static_cast<unsigned int>(request.block_size) : 0;
        const RtAudioErrorType result = audio_->openStream(&output, with_input ? &input : nullptr, RTAUDIO_FLOAT32,
                                                           static_cast<unsigned int>(request.sample_rate), &frames,
                                                           &RtAudioSystem::Callback, this, &options);
        if (result != RTAUDIO_NO_ERROR || !audio_->isStreamOpen())
        {
            if (LastError().empty())
            {
                SetError("the stream would not open on " + name_);
            }
            callback_.store(nullptr, std::memory_order_release);
            return false;
        }
        format_.sample_rate      = static_cast<int>(audio_->getStreamSampleRate());
        format_.block_size       = static_cast<int>(frames);
        format_.input_channels   = input_count_;
        format_.output_channels  = output_count_;
        format_.reported_latency = static_cast<int>(audio_->getStreamLatency());
        return true;
    }

    bool Start() override
    {
        const InCall call;
        if (!audio_->isStreamOpen())
        {
            return false;
        }
        running_.store(true);
        if (audio_->startStream() != RTAUDIO_NO_ERROR)
        {
            running_.store(false);
            return false;
        }
        return true;
    }

    void Stop() override
    {
        const bool   was_running = running_.exchange(false);
        const InCall call;
        // abortStream, not stopStream: a stop drains the output first, waiting on callbacks a device that has gone
        // will never make; the abort stops the device's callbacks and returns.
        const bool stopped_here =
            audio_->isStreamOpen() && audio_->isStreamRunning() && audio_->abortStream() == RTAUDIO_NO_ERROR;
        if (was_running && !stopped_here && name_ == "ASIO")
        {
            stopped_by_api_ = true; // RtAudio's own close stopped it first, which the Close after it lets finish
        }
        WaitForCallbacks();
    }

    /** RtAudio closes a stream whose device went away - on ASIO, whose driver asked to be reset: unplugged, its buffer
        size changed in its panel - on a thread of its own, and that close is let finish, never raced or freed under
        it. It is known by the disconnection it reports (Report notes one from RtAudio's threads whatever the stream is
        doing), or on ASIO by a stream that was running and that neither Stop nor Close stopped, which no longer runs:
        the closing thread marks the stream stopped before the driver's stop, which takes a block or more, and closed
        before it reports. A close that does not finish within two seconds leaves its RtAudio to it (Park), where
        destroying it would free the object the thread is still in. */
    void Close() override
    {
        const bool   was_running = running_.exchange(false);
        const InCall call;
        const bool   asio           = name_ == "ASIO";
        const bool   closing_itself = closed_by_api_.load(std::memory_order_acquire) ||
                                      (asio && (was_running || stopped_by_api_) && !audio_->isStreamRunning());
        stopped_by_api_             = false;
        if (closing_itself)
        {
            for (int i = 0; i < 200 && audio_->isStreamOpen(); ++i)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
            if (audio_->isStreamOpen() || (asio && !WaitForAsioRelease()))
            {
                Park();
            }
        }
        else if (audio_->isStreamOpen())
        {
            if (audio_->isStreamRunning())
            {
                audio_->abortStream();
            }
            audio_->closeStream();
        }
        WaitForCallbacks();
        callback_.store(nullptr, std::memory_order_release);
    }

    bool IsOpen() const override
    {
        return audio_->isStreamOpen();
    }

    StreamFormat Format() const override
    {
        return format_;
    }

    std::string LastError() const override
    {
        const std::lock_guard lock(error_mutex_);
        return last_error_;
    }

  private:
    /** RtAudio's callback: the channels as pointers into its non-interleaved buffers - channel c's frames after c
        whole channels' - handed to the engine with the clock as the API called and whether it reported an xrun.
        Counted in and out (in_flight_), and once the stream is stopped silence alone: an API's stop need not wait for
        a callback already under way - ALSA's abort drops its devices and returns while its thread may be between its
        own check and the call - and Stop waits for the count to fall to none, so no callback runs once it returns.
        Audio thread. Never allocates, locks or logs.
        @param output  the output buffer, `frames` samples for each opened output channel in turn
        @param input   the input buffer laid out the same way; nullptr with no input
        @param frames  the samples a channel carries
        @param status  RtAudio's stream status, non-zero for an overrun or underrun
        @param user    the RtAudioSystem, as openStream was given it
        @return        0, to continue the stream */
    static int Callback(void *output, void *input, unsigned int frames, double, RtAudioStreamStatus status,
                        void *user) noexcept
    {
        auto &self = *static_cast<RtAudioSystem *>(user);
        self.in_flight_.fetch_add(1); // before running_ is read: Stop clears running_, then reads the count
        const auto     now = NowNanoseconds();
        AudioCallback *run = self.running_.load() ? self.callback_.load(std::memory_order_acquire) : nullptr;
        const auto     n   = static_cast<std::size_t>(frames);
        auto          *in  = static_cast<const float *>(input);
        auto          *out = static_cast<float *>(output);
        for (std::size_t c = 0; c < static_cast<std::size_t>(self.input_count_); ++c)
        {
            self.inputs_[c] = in != nullptr ? in + c * n : nullptr;
        }
        for (std::size_t c = 0; c < static_cast<std::size_t>(self.output_count_); ++c)
        {
            self.outputs_[c] = out + c * n;
        }
        if (run != nullptr)
        {
            run->Process(self.inputs_.data(), self.input_count_, self.outputs_.data(), self.output_count_,
                         static_cast<int>(frames), now, status != 0);
        }
        else if (out != nullptr)
        {
            std::fill(out, out + n * static_cast<std::size_t>(self.output_count_), 0.0f);
        }
        self.in_flight_.fetch_sub(1);
        return 0;
    }

    /** Until no callback is under way (Callback): the stop's promise that the callback is not running once it returns,
        on whatever thread the API runs it. */
    void WaitForCallbacks() const noexcept
    {
        while (in_flight_.load() != 0)
        {
            std::this_thread::yield();
        }
    }

    /** RtAudio's error callback, on whichever thread it reports from: the text kept for LastError - without waiting,
        should another thread be reading it - and, for a failure reported by one of RtAudio's own threads while the
        stream runs, the engine told the stream failed.
        @param type  what RtAudio reports the error as
        @param text  RtAudio's description of it */
    void Report(RtAudioErrorType type, const std::string &text)
    {
        {
            const std::unique_lock lock(error_mutex_, std::try_to_lock);
            if (lock.owns_lock())
            {
                try
                {
                    last_error_ = text;
                }
                catch (const std::exception &)
                {
                }
            }
        }
        if (!t_in_call && type == RTAUDIO_DEVICE_DISCONNECT)
        {
            // RtAudio's own close of the stream under way, on this thread: known to Close however the stream stood,
            // a Close that has just stopped it among the rest.
            closed_by_api_.store(true, std::memory_order_release);
        }
        if (t_in_call || !running_.load(std::memory_order_acquire))
        {
            return;
        }
        const bool failure = type == RTAUDIO_DEVICE_DISCONNECT || type == RTAUDIO_SYSTEM_ERROR ||
                             type == RTAUDIO_DRIVER_ERROR || type == RTAUDIO_THREAD_ERROR ||
                             type == RTAUDIO_MEMORY_ERROR || type == RTAUDIO_UNKNOWN_ERROR ||
                             (type == RTAUDIO_WARNING && NamesDisconnection(text));
        if (!failure)
        {
            return;
        }
        if (!failed_.exchange(true, std::memory_order_acq_rel))
        {
            if (AudioCallback *run = callback_.load(std::memory_order_acquire))
            {
                run->StreamFailed();
            }
        }
    }

    /** The last error replaced, waiting for a reader that holds the text.
        @param text  the new text; empty to clear it */
    void SetError(const std::string &text)
    {
        const std::lock_guard lock(error_mutex_);
        last_error_ = text;
    }

    /** ASIO, after a stream RtAudio closed itself: its closing thread marks the stream closed first and only then
        disposes of the buffers, exits the driver and removes it, so an open begun as soon as the stream reads closed
        would load a driver while the old one is being taken out. RtAudio refuses to probe its drivers - with a warning
        that a stream is open - until that thread is done, so the probe is asked until it answers, for up to two
        seconds. False when it never did.
        @return  true when the probe was answered, so the driver is released */
    bool WaitForAsioRelease()
    {
        for (int i = 0; i < 200; ++i)
        {
            SetError({});
            static_cast<void>(audio_->getDeviceCount());
            if (LastError().find("stream is open") == std::string::npos)
            {
                return true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    }

    /** A fresh RtAudio on the API, its errors reported to Report().
        @return  the RtAudio, owned by the caller */
    std::unique_ptr<RtAudio> MakeRtAudio()
    {
        return std::make_unique<RtAudio>(
            api_, [this](RtAudioErrorType type, const std::string &text) { Report(type, text); });
    }

    /** An RtAudio whose own close of a stream has not finished, left to finish on an object kept alive - never
        destroyed, since its thread is still inside it - its word no longer this system's, which may be gone by then,
        and a fresh one in its place for what comes next, its driver still the old close's until that is done. Said,
        as the last error. */
    void Park()
    {
        audio_->setErrorCallback(nullptr);
        static_cast<void>(audio_.release());
        audio_ = MakeRtAudio();
        closed_by_api_.store(false, std::memory_order_relaxed);
        SetError("RtAudio's own close of the stream on " + name_ +
                 " did not finish within two seconds: it is left to finish, its device not yet released");
    }

    /** One device as RtAudio lists it, under the name the series gives it. */
    struct Listed
    {
        unsigned int        id{0}; ///< RtAudio's id for the device
        std::string         name;  ///< the series' unique name for it
        RtAudio::DeviceInfo info;  ///< what RtAudio reports of it
    };

    /** The devices RtAudio lists now, probed afresh, each under a name no other device carrying channels the same way
        has: RtAudio's own, and for a later device that shares it with an earlier one the same way, that name with its
        ordinal after it - "Loopback (Loopback PCM)" and "Loopback (Loopback PCM) (2)" for the two devices of ALSA's
        loopback card, whose PCMs carry one name and both play and record. A device is found by its name and its way (a
        settings file, a request: FindDevice), so two of one name the same way would be one device to a person and the
        first of them to an open; an input and an output of one name - a USB microphone with a headphone socket, on
        CoreAudio - keep it, the settings naming each by its way. The ordinals follow RtAudio's order, which is
        the system's: on ALSA, card by card and device by device.
        @return  the devices, in RtAudio's order */
    std::vector<Listed> List()
    {
        std::vector<Listed> listed;
        for (const unsigned int id : audio_->getDeviceIds())
        {
            RtAudio::DeviceInfo info = audio_->getDeviceInfo(id);
            if (info.ID == 0)
            {
                continue; // gone between the list and the probe
            }
            const auto taken = [&listed, &info](const std::string &name) {
                return std::any_of(listed.begin(), listed.end(), [&name, &info](const Listed &l) {
                    return l.name == name && ((l.info.inputChannels > 0 && info.inputChannels > 0) ||
                                              (l.info.outputChannels > 0 && info.outputChannels > 0));
                });
            };
            std::string name = info.name;
            for (int ordinal = 2; taken(name); ++ordinal)
            {
                name = info.name + " (" + std::to_string(ordinal) + ")";
            }
            listed.push_back({id, std::move(name), std::move(info)});
        }
        return listed;
    }

    /** RtAudio's id for the device named `name` with channels the way `direction` asks among `listed` (List()) - one
        name may be an input device and an output device; 0, RtAudio's none, when there is no such device.
        @param listed     the devices List() gave
        @param name       the device's name, as a settings file keeps it
        @param direction  the way the device is wanted
        @return           RtAudio's id for the device, or 0 for none */
    static unsigned int IdOf(const std::vector<Listed> &listed, const std::string &name, Direction direction)
    {
        for (const Listed &device : listed)
        {
            const unsigned int channels =
                direction == Direction::kInput ? device.info.inputChannels : device.info.outputChannels;
            if (device.name == name && channels > 0)
            {
                return device.id;
            }
        }
        return 0;
    }

    std::string                name_;    ///< the series' name for the API
    RtAudio::Api               api_;     ///< RtAudio's identifier for it
    std::vector<DeviceInfo>    devices_; ///< the devices as last listed, which ASIO answers with while a stream is open
    StreamFormat               format_;  ///< what the stream opened as
    int                        input_count_{0};  ///< the input channels the stream opened, 0 with no input
    int                        output_count_{0}; ///< the output channels the stream opened
    std::vector<const float *> inputs_;          ///< the channel pointers Callback hands the engine, sized at the open
    std::vector<float *>       outputs_; ///< the output channel pointers Callback hands the engine, sized at the open
    std::atomic<AudioCallback *> callback_{nullptr}; ///< what runs the stream; null while closed
    std::atomic<bool>            running_{false}; ///< true from Start until Stop or Close; false makes Callback silent
    std::atomic<int>             in_flight_{0};   ///< callbacks under way (Callback)
    std::atomic<bool>            failed_{false};  ///< true once the stream's failure has been told to the engine
    std::atomic<bool>  closed_by_api_{false};     ///< true when RtAudio reported a disconnection and is closing itself
    bool               stopped_by_api_{false};    ///< ASIO: Stop found RtAudio's own close had stopped it
    mutable std::mutex error_mutex_;              ///< guards `last_error_`
    std::string        last_error_;               ///< the last error or warning RtAudio reported
    // Last, so it is destroyed first: its destructor closes the stream while everything its callbacks touch lives.
    std::unique_ptr<RtAudio> audio_; ///< the RtAudio; replaced by Park() when its own close is left to finish
};

/** The machine's MIDI inputs on RtMidi: a probe that lists the ports, and an RtMidi input per open port whose callback
    stamps each message and hands it to the engine's sink. Message thread, but for Message and Error, which run on
    RtMidi's threads. */
class RtMidiSystem final : public MidiSystem
{
  public:
    /** A system whose ports are named after `client_name`.
        @param client_name  the name its ports carry */
    explicit RtMidiSystem(std::string client_name) : client_name_(std::move(client_name))
    {
    }

    /** Every open MIDI input port closed (CloseAll). */
    ~RtMidiSystem() override
    {
        CloseAll();
    }

    std::vector<std::string> Inputs() override
    {
        std::vector<std::string> names;
        try
        {
            if (probe_ == nullptr)
            {
                probe_ = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, client_name_);
                probe_->setErrorCallback(&RtMidiSystem::Error, this);
            }
            const unsigned int count = probe_->getPortCount();
            for (unsigned int i = 0; i < count; ++i)
            {
                names.push_back(probe_->getPortName(i));
            }
        }
        catch (const RtMidiError &error)
        {
            Keep(error.getMessage());
        }
        return names;
    }

    bool Open(const std::string &name, int port, MidiSink &sink) override
    {
        Close(port);
        try
        {
            auto entry   = std::make_unique<Port>();
            entry->index = port;
            entry->sink  = &sink;
            entry->input = std::make_unique<RtMidiIn>(RtMidi::UNSPECIFIED, client_name_);
            entry->input->setErrorCallback(&RtMidiSystem::Error, this);
            const int number = Find(*entry->input, name);
            if (number < 0)
            {
                Keep("no MIDI input named '" + name + "'");
                return false;
            }
            // System exclusive, timing clock and active sensing left out, as RtMidi leaves them by default: the
            // cores read short messages, and the clock alone would fill a ring at a fast tempo.
            entry->input->ignoreTypes(true, true, true);
            entry->input->setCallback(&RtMidiSystem::Message, entry.get());
            entry->input->openPort(static_cast<unsigned int>(number), client_name_ + " MIDI in");
            if (!entry->input->isPortOpen())
            {
                if (LastError().empty())
                {
                    Keep("the MIDI input '" + name + "' would not open");
                }
                return false;
            }
            ports_.push_back(std::move(entry));
            return true;
        }
        catch (const RtMidiError &error)
        {
            Keep(error.getMessage());
            return false;
        }
    }

    void CloseAll() override
    {
        ports_.clear();
    }

    std::string LastError() const override
    {
        const std::lock_guard lock(error_mutex_);
        return error_;
    }

  private:
    /** One open input: the sink and the port index its messages carry, and RtMidi's input - last, so it is closed,
        and its callbacks stopped, before the rest goes. */
    struct Port
    {
        int                       index{0};      ///< the index its messages carry to the sink
        MidiSink                 *sink{nullptr}; ///< where they land; not owned
        std::unique_ptr<RtMidiIn> input;         ///< RtMidi's input
    };

    /** The port opened as `port` closed, if there is one.
        @param port  the port's index */
    void Close(int port)
    {
        ports_.erase(std::remove_if(ports_.begin(), ports_.end(),
                                    [port](const std::unique_ptr<Port> &p) { return p->index == port; }),
                     ports_.end());
    }

    /** The input named `name`: that name, else the first whose name holds it (an ALSA port's name carries its client's
        before it and its numbers after); -1 for none.
        @param input  an RtMidi input to read the port names from
        @param name   the port's name, from Inputs()
        @return       RtMidi's port number, or -1 for none */
    static int Find(RtMidiIn &input, const std::string &name)
    {
        const unsigned int count = input.getPortCount();
        for (unsigned int i = 0; i < count; ++i)
        {
            if (input.getPortName(i) == name)
            {
                return static_cast<int>(i);
            }
        }
        for (unsigned int i = 0; i < count; ++i)
        {
            if (!name.empty() && input.getPortName(i).find(name) != std::string::npos)
            {
                return static_cast<int>(i);
            }
        }
        return -1;
    }

    /** RtMidi's callback, on the port's thread: the message stamped with the clock as it arrives.
        @param message  the message's bytes, valid for the call only; null or empty ones are dropped
        @param user     the Port the callback was set on */
    static void Message(double, std::vector<unsigned char> *message, void *user)
    {
        const auto *port = static_cast<const Port *>(user);
        if (message != nullptr && !message->empty())
        {
            port->sink->Arrived(port->index, NowNanoseconds(), message->data(), static_cast<int>(message->size()));
        }
    }

    /** RtMidi's error callback - with one set, RtMidi reports through it rather than throwing - on the thread the
        error met: the message thread's open, or a port's own, which never waits for the text.
        @param text  RtMidi's description of the error
        @param user  the RtMidiSystem the callback was set on */
    static void Error(RtMidiError::Type, const std::string &text, void *user)
    {
        static_cast<RtMidiSystem *>(user)->Keep(text, false);
    }

    /** `text` kept for LastError; a caller that may not wait (`waits` false) keeps nothing while a reader holds it.
        @param text   the error's text
        @param waits  true to wait for a reader to finish, false to drop the text rather than wait */
    void Keep(const std::string &text, bool waits = true)
    {
        std::unique_lock lock(error_mutex_, std::defer_lock);
        if (waits)
        {
            lock.lock();
        }
        else if (!lock.try_lock())
        {
            return;
        }
        try
        {
            error_ = text;
        }
        catch (const std::exception &)
        {
        }
    }

    std::string                        client_name_; ///< the name its ports carry
    mutable std::mutex                 error_mutex_; ///< guards `error_`
    std::string                        error_;       ///< the last error RtMidi reported
    std::vector<std::unique_ptr<Port>> ports_;       ///< the open ports
    std::unique_ptr<RtMidiIn>          probe_;       ///< the input Inputs() lists ports through, made on first use
};
} // namespace

std::vector<std::string> AudioApis()
{
    std::vector<RtAudio::Api> compiled;
    RtAudio::getCompiledApi(compiled);
    std::vector<std::string> names;
#if defined(_WIN32)
    for (const wasapi::Mode mode : kWasapiModes)
    {
        names.emplace_back(wasapi::NameOf(mode));
    }
#endif
    for (const ApiName &api : kApis)
    {
        if (std::find(compiled.begin(), compiled.end(), api.api) != compiled.end())
        {
            names.emplace_back(api.name);
        }
    }
    return names;
}

std::string DefaultAudioApi()
{
    const std::vector<std::string> apis = AudioApis();
    for (const std::string &name : apis)
    {
        const std::unique_ptr<AudioSystem> system = MakeAudioSystem(name);
        for (const DeviceInfo &device : system->Devices())
        {
            if (device.output_channels > 0)
            {
                return name;
            }
        }
    }
    return apis.empty() ? std::string() : apis.front();
}

std::unique_ptr<AudioSystem> MakeAudioSystem(const std::string &name)
{
#if defined(_WIN32)
    if (const std::optional<wasapi::Mode> mode = wasapi::ModeNamed(name))
    {
        return MakeWasapiSystem(*mode);
    }
#endif
    std::vector<RtAudio::Api> compiled;
    RtAudio::getCompiledApi(compiled);
    for (const ApiName &api : kApis)
    {
        if (name == api.name && std::find(compiled.begin(), compiled.end(), api.api) != compiled.end())
        {
            auto system = std::make_unique<RtAudioSystem>(api.api, api.name);
#if defined(RECKLESS_AUDIO_ASIO_HOST)
            if (api.api == RtAudio::WINDOWS_ASIO)
            {
                GiveAsioExitAList();
            }
#endif
            return system;
        }
    }
    return nullptr;
}

std::unique_ptr<MidiSystem> MakeMidiSystem(const std::string &client_name)
{
    return std::make_unique<RtMidiSystem>(client_name);
}

std::string AudioLibraryVersions()
{
    return "RtAudio " + RtAudio::getVersion() + ", RtMidi " + RtMidi::getVersion();
}
} // namespace reckless::audio
