#pragma once

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "AudioDevice.h"

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
        format_.input_channels   = request.input_count;
        format_.output_channels  = request.output_count;
        format_.reported_latency = 0;
        open_                    = true;
        failed_                  = false;
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

/** MIDI inputs with nothing behind them: the names it was made with, each opened onto the sink the engine gives, and
    a message delivered when the owner sends one - on the owner's thread, at the owner's time. */
class OfflineMidiSystem final : public MidiSystem
{
  public:
    /** Inputs made to order.
        @param inputs  the names Inputs() offers */
    explicit OfflineMidiSystem(std::vector<std::string> inputs) : inputs_(std::move(inputs))
    {
    }

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

    void CloseAll() override
    {
        open_.clear();
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

    /** How many inputs are open.
        @return  the open inputs */
    int open_count() const noexcept
    {
        return static_cast<int>(open_.size());
    }

  private:
    /** An input that is open: its name, the index it was opened under and where its messages go. */
    struct OpenInput
    {
        std::string name;          ///< the input's name
        int         port{0};       ///< the index its messages carry to the sink
        MidiSink   *sink{nullptr}; ///< where they land; not owned
    };

    std::vector<std::string> inputs_; ///< the names offered
    std::vector<OpenInput>   open_;   ///< the inputs open
    std::string              error_;  ///< the last error's text
};
} // namespace reckless::audio
