#pragma once

#include <algorithm>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AudioDevice.h"
#include "MidiOutputPorts.h"

namespace reckless::audio
{
/**
    An audio API with no hardware behind it: devices made to order, a stream that opens on anything they list, and a
    callback that runs only when its owner calls Run - on the owner's thread, with the owner's samples and the owner's
    clock. What the engine's suite plays a core through, and the player's check of every core, since a CI runner has no
    audio interface. It keeps the last request it was asked to open and the formats it opened, can be told to fail an
    open or a start, and can fail a running stream as an unplugged device does (Fail: the engine is told, and Run does
    nothing after). One device by default, "Offline", duplex, of the channels it was made with, at the usual rates.
*/
class OfflineAudioSystem final : public AudioSystem
{
  public:
    /** One duplex device, "Offline", with the channels given.
        @param inputs   the device's input channels
        @param outputs  the device's output channels */
    OfflineAudioSystem(int inputs, int outputs)
    {
        DeviceInfo device;
        device.name                = "Offline";
        device.input_channels      = inputs;
        device.output_channels     = outputs;
        device.duplex_channels     = std::min(inputs, outputs);
        device.is_default_input    = inputs > 0;
        device.is_default_output   = outputs > 0;
        device.sample_rates        = {44100, 48000, 88200, 96000};
        device.current_sample_rate = 48000;
        devices_.push_back(device);
    }

    /** Devices made to order.
        @param devices  the devices Devices() offers */
    explicit OfflineAudioSystem(std::vector<DeviceInfo> devices) : devices_(std::move(devices))
    {
    }

    std::string Name() const override
    {
        return name_;
    }

    std::vector<DeviceInfo> Devices() override
    {
        return devices_;
    }

    /** Opens whatever its devices can carry, at the rate asked and the block asked - or, for a block of 0, 256, or
        whatever block SetBlockOverride names, as an API that picks its own does.
        @param request   the stream to open; its devices must be among those offered and carry the channels asked
        @param callback  what Run calls; it must outlive the stream
        @return          false, with LastError() saying why, when the open is refused or a device is missing */
    bool Open(const StreamRequest &request, AudioCallback &callback) override
    {
        Close();
        request_ = request;
        ++opens_;
        if (refuse_open_)
        {
            error_ = "the offline device refused the stream";
            return false;
        }
        const auto output = std::find_if(devices_.begin(), devices_.end(), [&request](const DeviceInfo &d) {
            return d.name == request.output_device && Carries(d, Direction::kOutput);
        });
        if (output == devices_.end() || request.first_output + request.output_count > output->output_channels)
        {
            error_ = "no such output";
            return false;
        }
        if (request.input_count > 0)
        {
            const auto input = std::find_if(devices_.begin(), devices_.end(), [&request](const DeviceInfo &d) {
                return d.name == request.input_device && Carries(d, Direction::kInput);
            });
            if (input == devices_.end() || request.first_input + request.input_count > input->input_channels)
            {
                error_ = "no such input";
                return false;
            }
        }
        callback_           = &callback;
        format_.sample_rate = request.sample_rate;
        format_.block_size =
            block_override_ > 0 ? block_override_ : (request.block_size > 0 ? request.block_size : 256);
        format_.input_channels        = request.input_count;
        format_.output_channels       = request.output_count;
        format_.reported_latency      = 0;
        format_.input_latency_frames  = 0; // no device, no latency, on either side
        format_.output_latency_frames = 0;
        open_                         = true;
        failed_                       = false;
        return true;
    }

    bool Start() override
    {
        if (!open_ || refuse_start_)
        {
            error_ = "the offline device would not start";
            return false;
        }
        running_ = true;
        return true;
    }

    void Stop() override
    {
        running_ = false;
    }

    void Close() override
    {
        running_  = false;
        open_     = false;
        callback_ = nullptr;
    }

    bool IsOpen() const override
    {
        return open_;
    }

    StreamFormat Format() const override
    {
        return format_;
    }

    std::string LastError() const override
    {
        return error_;
    }

    // ---- The owner's side ------------------------------------------------------------------------------------------

    /** One callback of `frames`: the stream's input channels from `inputs` (as many as it opened; null for silence),
        its output channels into `outputs`, the clock at `now`. Nothing while the stream is not running.
        @param frames   the samples a channel carries
        @param inputs   one pointer an opened input channel; null for silence
        @param outputs  one pointer an opened output channel, written by the callback
        @param now      the clock's reading, in nanoseconds
        @param xrun     true to report an overrun or underrun since the last block */
    void Run(int frames, const float *const *inputs, float *const *outputs, std::int64_t now, bool xrun = false)
    {
        if (running_ && callback_ != nullptr)
        {
            callback_->Process(inputs, format_.input_channels, outputs, format_.output_channels, frames, now, xrun);
        }
    }

    /** The running stream fails as an unplugged device's does: the engine is told from here, and no callback runs
        again until the next open and start.
        @param why  the text LastError() then gives */
    void Fail(std::string why = "the offline device went away")
    {
        error_   = std::move(why);
        running_ = false;
        failed_  = true;
        if (callback_ != nullptr)
        {
            callback_->StreamFailed();
        }
    }

    /** The name Name() gives.
        @param name  the API's name */
    void SetName(std::string name)
    {
        name_ = std::move(name);
    }

    /** The devices Devices() offers from now on.
        @param devices  the devices */
    void SetDevices(std::vector<DeviceInfo> devices)
    {
        devices_ = std::move(devices);
    }

    /** The block the next opens give, whatever is asked (0: the block asked).
        @param frames  the block, in frames */
    void SetBlockOverride(int frames)
    {
        block_override_ = frames;
    }

    /** Make every Open() fail, or not.
        @param refuse  true to refuse */
    void RefuseOpen(bool refuse)
    {
        refuse_open_ = refuse;
    }

    /** Make every Start() fail, or not.
        @param refuse  true to refuse */
    void RefuseStart(bool refuse)
    {
        refuse_start_ = refuse;
    }

    /** The last stream asked for.
        @return  the request of the latest Open(), refused or not */
    const StreamRequest &request() const noexcept
    {
        return request_;
    }

    /** How many opens have been asked.
        @return  the count of Open() calls, refused ones included */
    int opens() const noexcept
    {
        return opens_;
    }

    /** Whether Run() reaches the callback.
        @return  true from a successful Start() until Stop(), Close() or Fail() */
    bool running() const noexcept
    {
        return running_;
    }

  private:
    std::string             name_{"Offline"};     ///< the API's name
    std::vector<DeviceInfo> devices_;             ///< the devices offered
    StreamRequest           request_;             ///< the latest open's request
    StreamFormat            format_;              ///< what the stream opened as
    AudioCallback          *callback_{nullptr};   ///< what Run calls; not owned, null while closed
    std::string             error_;               ///< the last error's text
    int                     opens_{0};            ///< the opens asked
    int                     block_override_{0};   ///< the block every open gives; 0 for the block asked
    bool                    open_{false};         ///< true while a stream is open
    bool                    running_{false};      ///< true while the stream runs
    bool                    failed_{false};       ///< true after Fail() until the next open
    bool                    refuse_open_{false};  ///< whether Open() refuses
    bool                    refuse_start_{false}; ///< whether Start() refuses
};

/** One message an OfflineMidiSystem was sent: the port it was sent to, the time it was to leave and its bytes. */
struct SentMidiMessage
{
    int                       port{0}; ///< the port the output was opened as
    std::int64_t              time{0}; ///< when it was to leave, in nanoseconds on NowNanoseconds' clock
    std::vector<std::uint8_t> bytes;   ///< its bytes, the status first
};

/** MIDI with nothing behind it. Inputs: the names it was made with, each opened onto the sink the engine gives, and a
    message delivered when the owner sends one - on the owner's thread, at the owner's time. Outputs: the names it was
    made with, each opened as a port Send queues to exactly as a back end's does - from any thread, wait-free, through
    the same queue and port table (MidiOutputPorts.h) - with no sender and no clock behind it: what Send accepted is
    recorded, in the order of the times, and read back with Sent(), for a suite of a consumer that sends. */
class OfflineMidiSystem final : public MidiSystem
{
  public:
    /** Inputs and outputs made to order.
        @param inputs   the names Inputs() offers
        @param outputs  the names Outputs() offers */
    explicit OfflineMidiSystem(std::vector<std::string> inputs, std::vector<std::string> outputs = {})
        : inputs_(std::move(inputs)), outputs_(std::move(outputs))
    {
    }

    /** Every output closed, what it held recorded. */
    ~OfflineMidiSystem() override
    {
        CloseAll();
    }

    OfflineMidiSystem(const OfflineMidiSystem &)            = delete;
    OfflineMidiSystem &operator=(const OfflineMidiSystem &) = delete;

    std::vector<std::string> Inputs() override
    {
        return inputs_;
    }

    bool Open(const std::string &name, int port, MidiSink &sink) override
    {
        if (std::find(inputs_.begin(), inputs_.end(), name) == inputs_.end())
        {
            error_ = "no MIDI input named '" + name + "'";
            return false;
        }
        open_.erase(std::remove_if(open_.begin(), open_.end(), [port](const OpenInput &o) { return o.port == port; }),
                    open_.end());
        open_.push_back(OpenInput{name, port, &sink});
        return true;
    }

    std::vector<std::string> Outputs() override
    {
        return outputs_;
    }

    bool OpenOutput(const std::string &name, int port) override
    {
        if (std::find(outputs_.begin(), outputs_.end(), name) == outputs_.end())
        {
            error_ = "no MIDI output named '" + name + "'";
            return false;
        }
        if (!MidiOutputPorts::IsPortNumber(port))
        {
            error_ = "the MIDI output '" + name + "' cannot be port " + std::to_string(port) + ": ports are 0 to " +
                     std::to_string(kMidiOutputPorts - 1);
            return false;
        }
        CloseOutput(port);
        OpenOutputPort output;
        output.port  = port;
        output.queue = std::make_unique<MidiOutputQueue>();
        ports_.Publish(port, *output.queue);
        outputs_open_.push_back(std::move(output));
        return true;
    }

    bool Send(int port, std::int64_t time_nanoseconds, const std::uint8_t *bytes, int size) noexcept override
    {
        return ports_.Send(port, time_nanoseconds, bytes, size);
    }

    void CloseAll() override
    {
        open_.clear();
        while (!outputs_open_.empty())
        {
            CloseOutput(outputs_open_.back().port);
        }
    }

    std::string LastError() const override
    {
        return error_;
    }

    /** A message from input `name`, arrived at `time`; false when that input is not open.
        @param name   the input's name
        @param time   the arrival, in nanoseconds
        @param bytes  the message's bytes
        @return       true when the open input's sink was handed it */
    bool Send(const std::string &name, std::int64_t time, std::vector<std::uint8_t> bytes)
    {
        for (const OpenInput &o : open_)
        {
            if (o.name == name)
            {
                o.sink->Arrived(o.port, time, bytes.data(), static_cast<int>(bytes.size()));
                return true;
            }
        }
        return false;
    }

    /** The owner's thread: every message Send has accepted since the last ClearSent, on every output, whether or not
        it is still open, in the order of their times - two of one time in the order they were queued on one port, and
        port by port in the order the ports were opened. Not to be called while another thread may Send to an output
        that closes meanwhile: the owner's thread is the queues' one reader.
        @return  the messages */
    const std::vector<SentMidiMessage> &Sent()
    {
        for (OpenOutputPort &output : outputs_open_)
        {
            Record(output);
        }
        return sent_;
    }

    /** The owner's thread: the record emptied, what the queues held with it. */
    void ClearSent()
    {
        Sent();
        sent_.clear();
    }

    /** How many inputs are open.
        @return  the open inputs */
    int open_count() const noexcept
    {
        return static_cast<int>(open_.size());
    }

    /** How many outputs are open.
        @return  the open outputs */
    int open_output_count() const noexcept
    {
        return static_cast<int>(outputs_open_.size());
    }

  private:
    /** An input that is open: its name, the index it was opened under and where its messages go. */
    struct OpenInput
    {
        std::string name;          ///< the input's name
        int         port{0};       ///< the index its messages carry to the sink
        MidiSink   *sink{nullptr}; ///< where they land; not owned
    };

    /** An output that is open: its port and the queue Send fills. */
    struct OpenOutputPort
    {
        int                              port{0}; ///< the port it was opened as
        std::unique_ptr<MidiOutputQueue> queue;   ///< what Send fills; at one address while open
    };

    /** What `output`'s queue holds moved into the record, which stays in the order of the times.
        @param output  the output */
    void Record(OpenOutputPort &output)
    {
        const std::size_t before = sent_.size();
        MidiOutputMessage message;
        while (output.queue->TryPop(message))
        {
            sent_.push_back(SentMidiMessage{
                output.port, message.time,
                std::vector<std::uint8_t>(message.bytes.begin(), message.bytes.begin() + message.size)});
        }
        if (sent_.size() != before)
        {
            std::stable_sort(sent_.begin(), sent_.end(),
                             [](const SentMidiMessage &a, const SentMidiMessage &b) { return a.time < b.time; });
        }
    }

    /** The output open as `port` closed, if there is one: withdrawn from Send, and what it held recorded.
        @param port  the port */
    void CloseOutput(int port)
    {
        const auto at = std::find_if(outputs_open_.begin(), outputs_open_.end(),
                                     [port](const OpenOutputPort &output) { return output.port == port; });
        if (at == outputs_open_.end())
        {
            return;
        }
        ports_.Withdraw(port);
        Record(*at);
        outputs_open_.erase(at);
    }

    std::vector<std::string>     inputs_;       ///< the input names offered
    std::vector<std::string>     outputs_;      ///< the output names offered
    std::vector<OpenInput>       open_;         ///< the inputs open
    std::vector<OpenOutputPort>  outputs_open_; ///< the outputs open
    MidiOutputPorts              ports_;        ///< the open outputs' queues, as Send finds them
    std::vector<SentMidiMessage> sent_;         ///< what Send accepted, in the order of the times
    std::string                  error_;        ///< the last error's text
};
} // namespace reckless::audio
