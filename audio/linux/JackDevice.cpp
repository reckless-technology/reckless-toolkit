// JACK through libjack loaded at run time (JackDevice.h says what it does and why): the one translation unit of the
// device layer that meets JACK, its calls renamed to the loader's (JackNames.h, before jack/jack.h).
#include "JackNames.h"

#include "JackDevice.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <jack/jack.h>

namespace reckless::audio
{
namespace
{
/** The most frames a process cycle may carry and still reach the callback: the channels are laid end to end in storage
    sized once, at the open, for this many - a server's largest block, which a person may change while a stream runs.
    A cycle past it plays silence and is told as an xrun. */
constexpr int kMaxFrames = 8192;

/** The client name a stream takes where the request gives none, and the one the probe lists devices through. */
constexpr const char *kDefaultClient = "Reckless";
constexpr const char *kProbeClient   = "Reckless probe"; ///< the client Devices() lists through with no stream open

/** libjack's error messages left unsaid: a machine with no server running would otherwise print libjack's complaint
    each time the devices are listed. What fails is said through LastError instead. */
void Quiet(const char *)
{
}

/** libjack told once, before the first client, to say nothing (Quiet). */
void SilenceLibjack()
{
    static std::once_flag once;
    std::call_once(once, [] { jack_set_error_function(&Quiet); });
}

/** A port list libjack handed out, given back to it as the object goes. */
class PortList
{
  public:
    /** The audio ports of `client`'s server whose flags carry `flags` (0 for every port), in the server's order.
        @param client  a client of the server
        @param flags   JackPortIsInput, JackPortIsOutput, or 0 */
    PortList(jack_client_t *client, unsigned long flags)
        : ports_(jack_get_ports(client, nullptr, JACK_DEFAULT_AUDIO_TYPE, flags))
    {
    }

    /** The list given back to libjack. */
    ~PortList()
    {
        jack_free(static_cast<void *>(ports_));
    }

    PortList(const PortList &)            = delete;
    PortList &operator=(const PortList &) = delete;

    /** The ports' full names, "client:port".
        @return  the names, in the server's order */
    std::vector<std::string> Names() const
    {
        std::vector<std::string> names;
        for (std::size_t i = 0; ports_ != nullptr && ports_[i] != nullptr; ++i)
        {
            names.emplace_back(ports_[i]);
        }
        return names;
    }

  private:
    const char **ports_; ///< the null-terminated list; null for none
};

/** The client a port's full name carries, before its first colon.
    @param port  the port's full name, "client:port"
    @return      the client's name; the whole name where it has no colon */
std::string ClientOf(const std::string &port)
{
    return port.substr(0, port.find(':'));
}

/** The ports among `ports` that belong to client `device`, in the server's order.
    @param ports   full port names
    @param device  the client's name
    @return        its ports' full names */
std::vector<std::string> PortsOf(const std::vector<std::string> &ports, const std::string &device)
{
    std::vector<std::string> mine;
    for (const std::string &port : ports)
    {
        if (ClientOf(port) == device)
        {
            mine.push_back(port);
        }
    }
    return mine;
}

/** The server's clients that have audio ports, as devices, in the order their first ports come: a client's input
    ports are the device's outputs, its output ports the device's inputs, its rate the server's; the first with
    outputs is the default output and the first with inputs the default input, as RtAudio chose them.
    @param client  a client of the server
    @return        the devices */
std::vector<DeviceInfo> List(jack_client_t *client)
{
    const std::vector<std::string> every   = PortList(client, 0).Names();
    const std::vector<std::string> inputs  = PortList(client, JackPortIsInput).Names();
    const std::vector<std::string> outputs = PortList(client, JackPortIsOutput).Names();
    const int                      rate    = static_cast<int>(jack_get_sample_rate(client));
    std::vector<DeviceInfo>        devices;
    for (const std::string &port : every)
    {
        const std::string name = ClientOf(port);
        if (std::any_of(devices.begin(), devices.end(), [&name](const DeviceInfo &d) { return d.name == name; }))
        {
            continue;
        }
        DeviceInfo device;
        device.name                  = name;
        device.output_channels       = static_cast<int>(PortsOf(inputs, name).size());
        device.input_channels        = static_cast<int>(PortsOf(outputs, name).size());
        device.duplex_channels       = std::min(device.input_channels, device.output_channels);
        device.sample_rates          = {rate};
        device.current_sample_rate   = rate;
        device.preferred_sample_rate = rate;
        if (device.input_channels + device.output_channels > 0)
        {
            devices.push_back(std::move(device));
        }
    }
    const auto first_output =
        std::find_if(devices.begin(), devices.end(), [](const DeviceInfo &d) { return d.output_channels > 0; });
    if (first_output != devices.end())
    {
        first_output->is_default_output = true;
    }
    const auto first_input =
        std::find_if(devices.begin(), devices.end(), [](const DeviceInfo &d) { return d.input_channels > 0; });
    if (first_input != devices.end())
    {
        first_input->is_default_input = true;
    }
    return devices;
}

/** A client of the machine's JACK server, never one started for it - on PipeWire's JACK asking the graph for `block`
    frames a cycle. PipeWire runs its graph at the smallest latency any running node asks for (its node.latency, within
    the graph's own bounds), and its libjack takes a client's from PIPEWIRE_LATENCY as the client connects: so the
    variable is set for the open alone, and put back as it was after. Nothing is forced on the graph - it goes back to
    its own quantum once the stream's client is gone - and a JACK server's libjack never reads the variable.
    @param name   the client's name
    @param block  the frames a cycle to ask PipeWire's graph for; 0 to ask nothing
    @param rate   the rate the stream asks for, in hertz, which the latency is counted at
    @return       the client, or null where no server answers */
jack_client_t *OpenClient(const std::string &name, int block = 0, int rate = 0)
{
    SilenceLibjack();
    const bool        asks = block > 0 && reckless_jack_is_pipewire() != 0;
    const char       *had  = asks ? std::getenv("PIPEWIRE_LATENCY") : nullptr;
    const std::string was  = had != nullptr ? had : "";
    if (asks)
    {
        const std::string latency = std::to_string(block) + "/" + std::to_string(rate > 0 ? rate : 48000);
        setenv("PIPEWIRE_LATENCY", latency.c_str(), 1);
    }
    jack_status_t  status = JackFailure;
    jack_client_t *client = jack_client_open(name.c_str(), JackNoStartServer, &status);
    if (asks)
    {
        if (had != nullptr)
        {
            setenv("PIPEWIRE_LATENCY", was.c_str(), 1);
        }
        else
        {
            unsetenv("PIPEWIRE_LATENCY");
        }
    }
    return client;
}

/** The latency a device port reports the way a stream uses it, the least of its range.
    @param client  the stream's client
    @param port    the device port's full name
    @param mode    JackPlaybackLatency for an output, JackCaptureLatency for an input
    @return        the latency, in frames; 0 where the port reports none */
int LatencyOf(jack_client_t *client, const std::string &port, jack_latency_callback_mode_t mode)
{
    jack_port_t *handle = jack_port_by_name(client, port.c_str());
    if (handle == nullptr)
    {
        return 0;
    }
    jack_latency_range_t range{0, 0};
    jack_port_get_latency_range(handle, mode, &range);
    return static_cast<int>(range.min);
}

/** JACK's AudioSystem: its devices, and its one stream as a client of the server. Message thread throughout but for
    Process, Xrun and Shutdown, which run on the server's threads. */
class JackSystem final : public AudioSystem
{
  public:
    JackSystem() = default;

    /** The stream closed (Close). */
    ~JackSystem() override
    {
        Close();
    }

    JackSystem(const JackSystem &)            = delete;
    JackSystem &operator=(const JackSystem &) = delete;

    std::string Name() const override
    {
        return "JACK";
    }

    std::vector<DeviceInfo> Devices() override
    {
        if (client_ != nullptr && !server_gone_.load(std::memory_order_acquire))
        {
            return List(client_); // the stream's own client lists them while it is open
        }
        jack_client_t *probe = OpenClient(kProbeClient);
        if (probe == nullptr)
        {
            SetError("no JACK server is running");
            return {};
        }
        std::vector<DeviceInfo> devices = List(probe);
        jack_client_close(probe);
        return devices;
    }

    bool Open(const StreamRequest &request, AudioCallback &callback) override
    {
        Close();
        SetError({});
        const std::string name = request.client_name.empty() ? std::string(kDefaultClient) : request.client_name;
        asked_block_           = request.block_size > 0 ? std::min(request.block_size, kMaxFrames) : 0;
        client_                = OpenClient(name, asked_block_, request.sample_rate);
        if (client_ == nullptr)
        {
            return Refuse("no JACK server is running, so nothing named '" + request.output_device +
                          "' can be opened on JACK");
        }
        const std::vector<DeviceInfo> devices = List(client_);
        const int                     output  = FindDevice(devices, request.output_device, Direction::kOutput);
        if (output < 0 || request.output_count <= 0)
        {
            return Refuse("no output device named '" + request.output_device + "' on JACK");
        }
        const DeviceInfo &out = devices[static_cast<std::size_t>(output)];
        if (request.first_output < 0 || request.first_output + request.output_count > out.output_channels)
        {
            return Refuse("'" + out.name + "' has " + std::to_string(out.output_channels) +
                          " output channels, and the stream asks for channels " +
                          std::to_string(request.first_output + 1) + " to " +
                          std::to_string(request.first_output + request.output_count));
        }
        const bool with_input = !request.input_device.empty() && request.input_count > 0;
        if (with_input)
        {
            const int input = FindDevice(devices, request.input_device, Direction::kInput);
            if (input < 0)
            {
                return Refuse("no input device named '" + request.input_device + "' on JACK");
            }
            const DeviceInfo &in = devices[static_cast<std::size_t>(input)];
            if (request.first_input < 0 || request.first_input + request.input_count > in.input_channels)
            {
                return Refuse("'" + in.name + "' has " + std::to_string(in.input_channels) +
                              " input channels, and the stream asks for channels " +
                              std::to_string(request.first_input + 1) + " to " +
                              std::to_string(request.first_input + request.input_count));
            }
        }
        const int rate = static_cast<int>(jack_get_sample_rate(client_));
        if (request.sample_rate > 0 && request.sample_rate != rate)
        {
            return Refuse("the JACK server runs at " + std::to_string(rate) + " Hz, and the stream asks for " +
                          std::to_string(request.sample_rate) + " Hz: JACK's rate is the server's to set");
        }

        // The device ports each of the stream's channels connects to as it starts: playback ports - JACK's inputs -
        // for its outputs, capture ports for its inputs, from the first channel asked.
        const std::vector<std::string> targets = PortsOf(PortList(client_, JackPortIsInput).Names(), out.name);
        targets_.assign(targets.begin() + request.first_output,
                        targets.begin() + request.first_output + request.output_count);
        sources_.clear();
        if (with_input)
        {
            const std::vector<std::string> sources =
                PortsOf(PortList(client_, JackPortIsOutput).Names(), request.input_device);
            sources_.assign(sources.begin() + request.first_input,
                            sources.begin() + request.first_input + request.input_count);
        }
        for (std::size_t c = 0; c < targets_.size(); ++c)
        {
            const std::string port = "outport " + std::to_string(c);
            out_ports_.push_back(
                jack_port_register(client_, port.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0));
        }
        for (std::size_t c = 0; c < sources_.size(); ++c)
        {
            const std::string port = "inport " + std::to_string(c);
            in_ports_.push_back(jack_port_register(client_, port.c_str(), JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0));
        }
        if (std::find(out_ports_.begin(), out_ports_.end(), nullptr) != out_ports_.end() ||
            std::find(in_ports_.begin(), in_ports_.end(), nullptr) != in_ports_.end())
        {
            return Refuse("the JACK server would not give the stream its ports");
        }
        if (jack_set_process_callback(client_, &JackSystem::Process, this) != 0 ||
            jack_set_xrun_callback(client_, &JackSystem::Xrun, this) != 0)
        {
            return Refuse("the JACK server would not take the stream's callbacks");
        }
        jack_on_shutdown(client_, &JackSystem::Shutdown, this);
        jack_set_buffer_size_callback(client_, &JackSystem::BufferSize, this);

        // A JACK server's block is a server's setting any client may change, and the one a person tunes a live rig by:
        // the block asked is asked of it. Never of PipeWire's, whose jack_set_buffer_size forces a quantum on the
        // whole graph that outlives the stream; there the client asked for its latency as it connected (OpenClient).
        if (asked_block_ > 0 && reckless_jack_is_pipewire() == 0 &&
            jack_get_buffer_size(client_) != static_cast<jack_nframes_t>(asked_block_))
        {
            jack_set_buffer_size(client_, static_cast<jack_nframes_t>(asked_block_));
        }
        block_.store(static_cast<int>(jack_get_buffer_size(client_)), std::memory_order_release);

        const auto most = static_cast<std::size_t>(kMaxFrames);
        input_storage_.assign(std::max<std::size_t>(in_ports_.size(), 1) * most, 0.0f);
        output_storage_.assign(out_ports_.size() * most, 0.0f);
        inputs_.assign(std::max<std::size_t>(in_ports_.size(), 1), nullptr);
        outputs_.assign(out_ports_.size(), nullptr);

        format_.sample_rate      = rate;
        format_.block_size       = block_.load(std::memory_order_acquire);
        format_.input_channels   = static_cast<int>(in_ports_.size());
        format_.output_channels  = static_cast<int>(out_ports_.size());
        format_.reported_latency = DeviceLatency();
        callback_.store(&callback, std::memory_order_release);
        failed_.store(false, std::memory_order_relaxed);
        server_gone_.store(false, std::memory_order_relaxed);
        pending_xrun_.store(false, std::memory_order_relaxed);
        open_ = true;
        return true;
    }

    bool Start() override
    {
        if (!open_ || server_gone_.load(std::memory_order_acquire))
        {
            return false;
        }
        if (active_)
        {
            return true;
        }
        running_.store(true, std::memory_order_release);
        if (jack_activate(client_) != 0)
        {
            running_.store(false, std::memory_order_release);
            SetError("the stream would not start on JACK: the server would not activate its client");
            return false;
        }
        active_ = true;
        AwaitBlock();
        format_.reported_latency = DeviceLatency(); // a device port's latency follows the block, settled now
        // Connected once active, as JACK requires; deactivating disconnects them, so each Start connects afresh.
        for (std::size_t c = 0; c < out_ports_.size(); ++c)
        {
            if (jack_connect(client_, jack_port_name(out_ports_[c]), targets_[c].c_str()) != 0)
            {
                Stop();
                SetError("the stream would not start on JACK: '" + std::string(jack_port_name(out_ports_[c])) +
                         "' would not connect to '" + targets_[c] + "'");
                return false;
            }
        }
        for (std::size_t c = 0; c < in_ports_.size(); ++c)
        {
            if (jack_connect(client_, sources_[c].c_str(), jack_port_name(in_ports_[c])) != 0)
            {
                Stop();
                SetError("the stream would not start on JACK: '" + sources_[c] + "' would not connect to '" +
                         std::string(jack_port_name(in_ports_[c])) + "'");
                return false;
            }
        }
        return true;
    }

    void Stop() override
    {
        running_.store(false, std::memory_order_release);
        if (active_)
        {
            // A server that has shut the client down runs none of its callbacks, and is not asked to deactivate it.
            if (!server_gone_.load(std::memory_order_acquire))
            {
                jack_deactivate(client_);
            }
            active_ = false;
        }
        WaitForCallbacks();
    }

    void Close() override
    {
        Stop();
        if (client_ != nullptr)
        {
            jack_client_close(client_); // its ports with it
            client_ = nullptr;
        }
        out_ports_.clear();
        in_ports_.clear();
        targets_.clear();
        sources_.clear();
        open_   = false;
        format_ = {};
        callback_.store(nullptr, std::memory_order_release);
    }

    bool IsOpen() const override
    {
        return open_;
    }

    StreamFormat Format() const override
    {
        StreamFormat format = format_;
        if (open_)
        {
            format.block_size = block_.load(std::memory_order_acquire); // the server's block, which it may change
        }
        return format;
    }

    std::string LastError() const override
    {
        const std::lock_guard lock(error_mutex_);
        return last_error_;
    }

  private:
    /** JACK's process callback, on the server's real-time thread: each input port's buffer copied into the channels
        laid end to end, the callback run, each output channel copied to its port - silence while the stream is
        stopped, or for a cycle past kMaxFrames. Counted in and out, so Stop can wait for none to be under way. Never
        allocates or locks.
        @param frames  the cycle's frames
        @param self    the JackSystem
        @return        0, to stay in the graph */
    static int Process(jack_nframes_t frames, void *self) noexcept
    {
        auto &system = *static_cast<JackSystem *>(self);
        system.in_flight_.fetch_add(1); // before running_ is read: Stop clears running_, then reads the count
        const std::int64_t now  = NowNanoseconds();
        AudioCallback     *run  = system.running_.load(std::memory_order_acquire)
                                      ? system.callback_.load(std::memory_order_acquire)
                                      : nullptr;
        const auto         n    = static_cast<std::size_t>(frames);
        const bool         fits = frames <= static_cast<jack_nframes_t>(kMaxFrames);
        if (run != nullptr && fits)
        {
            for (std::size_t c = 0; c < system.in_ports_.size(); ++c)
            {
                float *channel     = system.input_storage_.data() + c * n;
                system.inputs_[c]  = channel;
                const auto *buffer = static_cast<const float *>(jack_port_get_buffer(system.in_ports_[c], frames));
                std::memcpy(channel, buffer, n * sizeof(float));
            }
            for (std::size_t c = 0; c < system.out_ports_.size(); ++c)
            {
                system.outputs_[c] = system.output_storage_.data() + c * n;
            }
            const bool xrun = system.pending_xrun_.exchange(false, std::memory_order_acq_rel);
            run->Process(system.in_ports_.empty() ? nullptr : system.inputs_.data(),
                         static_cast<int>(system.in_ports_.size()), system.outputs_.data(),
                         static_cast<int>(system.out_ports_.size()), static_cast<int>(frames), now, xrun);
            for (std::size_t c = 0; c < system.out_ports_.size(); ++c)
            {
                std::memcpy(jack_port_get_buffer(system.out_ports_[c], frames), system.outputs_[c], n * sizeof(float));
            }
        }
        else
        {
            for (jack_port_t *port : system.out_ports_)
            {
                std::memset(jack_port_get_buffer(port, frames), 0, n * sizeof(float));
            }
            if (run != nullptr)
            {
                system.pending_xrun_.store(true, std::memory_order_release); // a cycle the callback never saw
            }
        }
        system.in_flight_.fetch_sub(1);
        return 0;
    }

    /** JACK's buffer-size callback, on a thread of the server's: the block it runs at now, for Format and AwaitBlock.
        @param frames  the new block, in frames
        @param self    the JackSystem
        @return        0 */
    static int BufferSize(jack_nframes_t frames, void *self) noexcept
    {
        static_cast<JackSystem *>(self)->block_.store(static_cast<int>(frames), std::memory_order_release);
        return 0;
    }

    /** PipeWire's graph moves to the latency the client asked for (OpenClient) only once the client is active, a cycle
        or two on: Start waits for it - up to half a second, since another node may hold the graph at a larger block,
        or the graph's bounds forbid the one asked - so that what the stream's format says once Start returns is the
        block it plays at. Nothing to wait for on a JACK server, whose block was set at the open. */
    void AwaitBlock() const
    {
        if (asked_block_ <= 0 || reckless_jack_is_pipewire() == 0)
        {
            return;
        }
        const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
        while (block_.load(std::memory_order_acquire) > asked_block_ && std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    /** The latency the device ports report beyond the block: the first output's playback latency and the first
        input's capture latency, summed.
        @return  the latency, in frames */
    int DeviceLatency() const
    {
        return LatencyOf(client_, targets_.front(), JackPlaybackLatency) +
               (sources_.empty() ? 0 : LatencyOf(client_, sources_.front(), JackCaptureLatency));
    }

    /** JACK's xrun callback, on a thread of the server's: the next process callback is told.
        @param self  the JackSystem
        @return      0 */
    static int Xrun(void *self) noexcept
    {
        static_cast<JackSystem *>(self)->pending_xrun_.store(true, std::memory_order_release);
        return 0;
    }

    /** JACK's shutdown callback, on a thread of the server's: the server stopped, or let the client go. The stream
        failing, told once; the client is closed from the message thread (Close), never from here, where libjack
        forbids it.
        @param self  the JackSystem */
    static void Shutdown(void *self) noexcept
    {
        auto &system = *static_cast<JackSystem *>(self);
        system.server_gone_.store(true, std::memory_order_release);
        {
            const std::unique_lock lock(system.error_mutex_, std::try_to_lock);
            if (lock.owns_lock())
            {
                try
                {
                    system.last_error_ = "the JACK server shut the stream's client down";
                }
                catch (const std::exception &)
                {
                }
            }
        }
        if (!system.failed_.exchange(true, std::memory_order_acq_rel) &&
            system.running_.load(std::memory_order_acquire))
        {
            if (AudioCallback *run = system.callback_.load(std::memory_order_acquire))
            {
                run->StreamFailed();
            }
        }
    }

    /** Until no process callback is under way: Stop's promise that the callback is not running once it returns. */
    void WaitForCallbacks() const noexcept
    {
        while (in_flight_.load() != 0)
        {
            std::this_thread::yield();
        }
    }

    /** Why an open failed, kept for LastError, and the client let go.
        @param text  the reason
        @return      false, the open's answer */
    bool Refuse(const std::string &text)
    {
        Close();
        SetError(text);
        return false;
    }

    /** The last error replaced.
        @param text  the new text; empty to clear it */
    void SetError(const std::string &text)
    {
        const std::lock_guard lock(error_mutex_);
        last_error_ = text;
    }

    jack_client_t               *client_{nullptr};     ///< the stream's client; null while closed
    std::vector<jack_port_t *>   out_ports_;           ///< the stream's output ports, "outport N"
    std::vector<jack_port_t *>   in_ports_;            ///< the stream's input ports, "inport N"
    std::vector<std::string>     targets_;             ///< the device ports the outputs connect to, by full name
    std::vector<std::string>     sources_;             ///< the device ports the inputs connect from, by full name
    std::vector<float>           input_storage_;       ///< the callback's input channels, kMaxFrames each
    std::vector<float>           output_storage_;      ///< the callback's output channels, kMaxFrames each
    std::vector<float *>         inputs_;              ///< the input channel pointers the callback is handed
    std::vector<float *>         outputs_;             ///< the output channel pointers the callback is handed
    StreamFormat                 format_;              ///< what the stream opened as
    bool                         open_{false};         ///< true from a successful Open until Close
    bool                         active_{false};       ///< true from Start until Stop: the client active
    std::atomic<AudioCallback *> callback_{nullptr};   ///< what runs the stream; null while closed
    std::atomic<bool>            running_{false};      ///< true from Start until Stop; false plays silence
    std::atomic<int>             in_flight_{0};        ///< process callbacks under way
    std::atomic<bool>            pending_xrun_{false}; ///< an xrun to tell the next callback of
    std::atomic<bool>            failed_{false};       ///< true once the stream's failure has been told
    std::atomic<bool>            server_gone_{false};  ///< true once the server has shut the client down
    std::atomic<int>             block_{0};            ///< the server's block now, from its buffer-size callback
    int                          asked_block_{0};      ///< the block the stream asked for; 0 for the server's
    mutable std::mutex           error_mutex_;         ///< guards `last_error_`
    std::string                  last_error_;          ///< the last error, for a person to read
};

/** The one name this back end serves.
    @return  "JACK" */
std::vector<std::string> ApiNames()
{
    return {"JACK"};
}

/** A system for JACK.
    @param name  the API's name
    @return      the system, owned by the caller; nullptr for any name but "JACK" */
std::unique_ptr<AudioSystem> MakeSystem(const std::string &name)
{
    if (name != "JACK")
    {
        return nullptr;
    }
    return std::make_unique<JackSystem>();
}

/** What this back end is: JACK, whichever server's libjack the machine has, loaded when first asked.
    @return  the text */
std::string Library()
{
    return "JACK";
}
} // namespace

const AudioBackend &JackBackend()
{
    static const AudioBackend backend{&Library, &ApiNames, &MakeSystem};
    return backend;
}
} // namespace reckless::audio
