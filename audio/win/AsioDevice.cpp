// ASIO on Steinberg's SDK (AsioDevice.h says what it does and why): the one translation unit that meets the SDK's host
// API.
#include "AsioDevice.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

// The SDK's host headers, after windows.h, which they need; asiosys.h first, in a block of its own, since asio.h reads
// the platform switches it sets (a double for ASIOSampleRate among them) and includes it not.
#include "asiosys.h"

#include "asio.h"
#include "asiodrivers.h"

#include "AsioFormats.h"
#include "SampleEncodings.h"

// The SDK's own driver list: the one its ASIOExit releases the current driver through (host/asiodrivers.cpp), which
// this back end points at the list it takes as the program starts.
extern AsioDrivers *asioDrivers;

namespace reckless::audio
{
namespace
{
/** The rates a driver is asked whether it takes, the ones RtAudio asked, so a driver lists the rates it listed. */
constexpr unsigned int kRates[] = {4000,  5512,  8000,  9600,  11025, 16000,  22050,
                                   32000, 44100, 48000, 88200, 96000, 176400, 192000};

/** The registry's ASIO drivers as the program starts, as RtAudio's static list was taken: the SDK's driver list, read
    during static initialisation, and the SDK's global pointed at it so that ASIOExit releases a driver through the list
    that loaded it. The device suite's test driver is registered in registry views that exist only until main begins,
    which is when this list must be read. */
struct DriverList
{
    /** The list read, and the SDK's global pointed at it where nothing else has set it. */
    DriverList()
    {
        if (asioDrivers == nullptr)
        {
            asioDrivers = &drivers;
        }
    }

    /** The SDK's global let go of before the list goes. */
    ~DriverList()
    {
        if (asioDrivers == &drivers)
        {
            asioDrivers = nullptr;
        }
    }

    DriverList(const DriverList &)            = delete;
    DriverList &operator=(const DriverList &) = delete;

    AsioDrivers drivers; ///< the SDK's list of the registry's drivers, which loads and releases them
};

/** The program's driver list, read during static initialisation (DriverList says why). */
DriverList g_driver_list;

/** What loading a driver once found: its channels and the rates it takes. */
struct Probed
{
    int              inputs{0};    ///< its input channels
    int              outputs{0};   ///< its output channels
    std::vector<int> rates;        ///< the rates it takes, of kRates
    int              preferred{0}; ///< the highest of them up to 48 kHz, else the lowest, as RtAudio preferred
};

/** Every probe that found a driver, by its name, for the program's life: a driver is probed once, since none can be
    loaded to be probed while a stream holds one.
    @return  the probes, one map for the program's life */
std::map<std::string, Probed> &Probes()
{
    static std::map<std::string, Probed> probes;
    return probes;
}

/** The drivers the registry listed as the program started, by their names.
    @return  the names, in the registry's order */
std::vector<std::string> DriverNames()
{
    std::vector<std::string> names;
    AsioDrivers             &list  = g_driver_list.drivers;
    const long               count = list.asioGetNumDev();
    for (long i = 0; i < count; ++i)
    {
        char name[64] = {};
        if (list.asioGetDriverName(static_cast<int>(i), name, 32) == 0)
        {
            names.emplace_back(name);
        }
    }
    return names;
}

/** Driver `name` loaded and initialised: the SDK's current driver until ASIOExit.
    @param name  the driver's name
    @param why   set to the reason when it will not load
    @return      true when it loaded */
bool Load(const std::string &name, std::string &why)
{
    std::vector<char> copy(name.begin(), name.end());
    copy.push_back('\0');
    if (!g_driver_list.drivers.loadDriver(copy.data()))
    {
        why = "the driver would not load";
        return false;
    }
    ASIODriverInfo info{};
    info.asioVersion = 2;
    info.sysRef      = GetForegroundWindow();
    if (ASIOInit(&info) != ASE_OK)
    {
        why = info.errorMessage[0] != '\0' ? std::string(info.errorMessage) : std::string("the driver would not start");
        ASIOExit();
        return false;
    }
    return true;
}

/** Driver `name` loaded for a moment: its channels and the rates it takes; nothing where it will not load.
    @param name  the driver's name
    @return      what the probe found */
std::optional<Probed> Probe(const std::string &name)
{
    std::string why;
    if (!Load(name, why))
    {
        return std::nullopt;
    }
    Probed probed;
    long   inputs = 0, outputs = 0;
    if (ASIOGetChannels(&inputs, &outputs) != ASE_OK || inputs + outputs <= 0)
    {
        ASIOExit();
        return std::nullopt;
    }
    probed.inputs  = static_cast<int>(inputs);
    probed.outputs = static_cast<int>(outputs);
    for (const unsigned int rate : kRates)
    {
        if (ASIOCanSampleRate(static_cast<ASIOSampleRate>(rate)) == ASE_OK)
        {
            probed.rates.push_back(static_cast<int>(rate));
            if (probed.preferred == 0 || (rate <= 48000 && static_cast<int>(rate) > probed.preferred))
            {
                probed.preferred = static_cast<int>(rate);
            }
        }
    }
    ASIOExit();
    return probed;
}

/** One driver as listed: its driver name, which loads it, and the device it is. */
struct Listed
{
    std::string driver; ///< the driver's name in the registry, which loads it
    DeviceInfo  info;   ///< the device as listed, under the name NameApart gave it
};

/** Whether a driver's sample type is one this back end converts: little-endian integers of 16, 24 and 32 bits, 32-bit
    integers aligned low at 16 to 24 bits, and 32 and 64-bit floats - every type a Windows driver offers.
    @param type  the channel's sample type
    @return      true when it converts it */
bool Converts(ASIOSampleType type) noexcept
{
    switch (type)
    {
    case ASIOSTInt16LSB:
    case ASIOSTInt24LSB:
    case ASIOSTInt32LSB:
    case ASIOSTFloat32LSB:
    case ASIOSTFloat64LSB:
    case ASIOSTInt32LSB16:
    case ASIOSTInt32LSB18:
    case ASIOSTInt32LSB20:
    case ASIOSTInt32LSB24:
        return true;
    default:
        return false;
    }
}

/** The bytes one sample of a converted type takes.
    @param type  the sample type, one Converts takes
    @return      2, 3, 4 or 8 */
int BytesOf(ASIOSampleType type) noexcept
{
    switch (type)
    {
    case ASIOSTInt16LSB:
        return 2;
    case ASIOSTInt24LSB:
        return 3;
    case ASIOSTFloat64LSB:
        return 8;
    default:
        return 4;
    }
}

/** The bits a low-aligned 32-bit type carries.
    @param type  ASIOSTInt32LSB16, 18, 20 or 24
    @return      16, 18, 20 or 24; 0 for any other type */
int LowAlignedBits(ASIOSampleType type) noexcept
{
    switch (type)
    {
    case ASIOSTInt32LSB16:
        return 16;
    case ASIOSTInt32LSB18:
        return 18;
    case ASIOSTInt32LSB20:
        return 20;
    case ASIOSTInt32LSB24:
        return 24;
    default:
        return 0;
    }
}

/** The callback's samples into one driver channel's buffer.
    @param in      the callback's channel, `frames` samples
    @param out     the driver's buffer for the channel
    @param frames  the frames
    @param type    the channel's sample type, one Converts takes */
void ToDriver(const float *in, void *out, int frames, ASIOSampleType type) noexcept
{
    auto              *bytes       = static_cast<std::uint8_t *>(out);
    const float *const channels[1] = {in};
    switch (type)
    {
    case ASIOSTInt16LSB:
        Encode(channels, 1, 0, 1, frames, Encoding::kInt16, bytes);
        break;
    case ASIOSTInt24LSB:
        Encode(channels, 1, 0, 1, frames, Encoding::kInt24, bytes);
        break;
    case ASIOSTInt32LSB:
        Encode(channels, 1, 0, 1, frames, Encoding::kInt32, bytes);
        break;
    case ASIOSTFloat32LSB:
        Encode(channels, 1, 0, 1, frames, Encoding::kFloat32, bytes);
        break;
    case ASIOSTFloat64LSB:
        asio::EncodeFloat64(in, bytes, frames);
        break;
    default:
        asio::EncodeLowAligned(in, bytes, frames, LowAlignedBits(type));
        break;
    }
}

/** One driver channel's buffer into the callback's samples.
    @param in      the driver's buffer for the channel
    @param out     the callback's channel, `frames` samples
    @param frames  the frames
    @param type    the channel's sample type, one Converts takes */
void FromDriver(const void *in, float *out, int frames, ASIOSampleType type) noexcept
{
    const auto  *bytes       = static_cast<const std::uint8_t *>(in);
    float *const channels[1] = {out};
    switch (type)
    {
    case ASIOSTInt16LSB:
        Decode(bytes, 1, frames, Encoding::kInt16, 0, 1, channels);
        break;
    case ASIOSTInt24LSB:
        Decode(bytes, 1, frames, Encoding::kInt24, 0, 1, channels);
        break;
    case ASIOSTInt32LSB:
        Decode(bytes, 1, frames, Encoding::kInt32, 0, 1, channels);
        break;
    case ASIOSTFloat32LSB:
        Decode(bytes, 1, frames, Encoding::kFloat32, 0, 1, channels);
        break;
    case ASIOSTFloat64LSB:
        asio::DecodeFloat64(bytes, out, frames);
        break;
    default:
        asio::DecodeLowAligned(bytes, out, frames, LowAlignedBits(type));
        break;
    }
}

class AsioSystem;

/** The system whose stream holds the SDK's one driver, which the driver's callbacks reach; null for none. */
std::atomic<AsioSystem *> g_owner{nullptr};

// The driver's callbacks, which carry no pointer of their own: each reaches the stream through g_owner.

/** The driver's buffer switch, on its thread: the owning stream's Switch.
    @param index   the half of each double buffer to read and write, 0 or 1
    @param direct  whether the driver calls from its own interrupt or thread directly; unread */
void BufferSwitch(long index, ASIOBool direct) noexcept;

/** The driver's rate changed: the owning stream's RateChanged.
    @param rate  the driver's new rate, in hertz */
void SampleRateChanged(ASIOSampleRate rate) noexcept;

/** The driver's questions and requests (the SDK's kAsio selectors): what the host supports, the engine's version, a
    reset, a resync, an overload.
    @param selector  what the driver asks
    @param value     the selector's argument: for kAsioSelectorSupported, the selector asked about
    @param message   the selector's data; unread
    @param opt       the selector's further data; unread
    @return          the answer: 1 for yes or done, 0 for no, 2 for the engine's version */
long AsioMessage(long selector, long value, void *message, double *opt) noexcept;

/** The driver's buffer switch with its time, which a driver may call whatever the host answered kAsioSupportsTimeInfo
    with: the same switch.
    @param params  the driver's time; unread
    @param index   the half of each double buffer to read and write, 0 or 1
    @param direct  whether the driver calls directly; unread
    @return        null: no time handed back */
ASIOTime *BufferSwitchTimeInfo(ASIOTime *params, long index, ASIOBool direct) noexcept;

/** The callbacks every stream's buffers are made with. */
ASIOCallbacks g_callbacks = {&BufferSwitch, &SampleRateChanged, &AsioMessage, &BufferSwitchTimeInfo};

/** ASIO's AudioSystem: the drivers, and one stream on one of them. Message thread throughout but for Switch, Xrun,
    RateChanged and Fail, which the driver's callbacks run. */
class AsioSystem final : public AudioSystem
{
  public:
    AsioSystem() = default;

    /** The stream closed (Close). */
    ~AsioSystem() override
    {
        Close();
    }

    AsioSystem(const AsioSystem &)            = delete;
    AsioSystem &operator=(const AsioSystem &) = delete;

    std::string Name() const override
    {
        return "ASIO";
    }

    std::vector<DeviceInfo> Devices() override
    {
        std::vector<DeviceInfo> devices;
        for (Listed &device : List())
        {
            devices.push_back(std::move(device.info));
        }
        return devices;
    }

    bool Open(const StreamRequest &request, AudioCallback &callback) override
    {
        Close();
        SetError({});
        const std::vector<Listed> listed = List();
        const Listed             *output = Find(listed, request.output_device, Direction::kOutput);
        if (output == nullptr || request.output_count <= 0)
        {
            return Refuse("no output device named '" + request.output_device + "' on ASIO");
        }
        if (request.first_output < 0 || request.first_output + request.output_count > output->info.output_channels)
        {
            return Refuse("'" + output->info.name + "' has " + std::to_string(output->info.output_channels) +
                          " output channels, and the stream asks for channels " +
                          std::to_string(request.first_output + 1) + " to " +
                          std::to_string(request.first_output + request.output_count));
        }
        const bool with_input = !request.input_device.empty() && request.input_count > 0;
        if (with_input)
        {
            const Listed *input = Find(listed, request.input_device, Direction::kInput);
            if (input == nullptr)
            {
                return Refuse("no input device named '" + request.input_device + "' on ASIO");
            }
            if (input->driver != output->driver)
            {
                return Refuse("ASIO opens one driver both ways, and the input '" + input->info.name +
                              "' is not the output '" + output->info.name + "': choose one driver for both");
            }
            if (request.first_input < 0 || request.first_input + request.input_count > input->info.input_channels)
            {
                return Refuse("'" + input->info.name + "' has " + std::to_string(input->info.input_channels) +
                              " input channels, and the stream asks for channels " +
                              std::to_string(request.first_input + 1) + " to " +
                              std::to_string(request.first_input + request.input_count));
            }
        }
        AsioSystem *none = nullptr;
        if (!g_owner.compare_exchange_strong(none, this, std::memory_order_acq_rel))
        {
            return Refuse("another stream holds an ASIO driver, and ASIO loads one at a time");
        }
        std::string why;
        if (!Load(output->driver, why))
        {
            g_owner.store(nullptr, std::memory_order_release);
            return Refuse("'" + output->info.name + "' would not open on ASIO: " + why);
        }
        loaded_ = true;
        if (!Prepare(request, with_input, why))
        {
            Unload();
            return Refuse("'" + output->info.name + "' would not open on ASIO: " + why);
        }
        callback_.store(&callback, std::memory_order_release);
        failed_.store(false, std::memory_order_relaxed);
        pending_xrun_.store(false, std::memory_order_relaxed);
        open_ = true;
        return true;
    }

    bool Start() override
    {
        if (!open_)
        {
            return false;
        }
        if (started_)
        {
            return true;
        }
        running_.store(true, std::memory_order_release);
        if (ASIOStart() != ASE_OK)
        {
            running_.store(false, std::memory_order_release);
            SetError("the stream would not start on ASIO: the driver would not start");
            return false;
        }
        started_ = true;
        return true;
    }

    void Stop() override
    {
        running_.store(false, std::memory_order_release);
        if (started_)
        {
            ASIOStop();
            started_ = false;
        }
        WaitForCallbacks();
    }

    void Close() override
    {
        Stop();
        Unload();
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
        return format_;
    }

    std::string LastError() const override
    {
        const std::lock_guard lock(error_mutex_);
        return last_error_;
    }

    /** The driver's buffer switch, on its thread: the inputs read from the half it has just filled, the callback run -
        or silence while the stream is stopped - and the outputs written into the half it plays next, the driver told
        they are ready where it asks to be. Counted in and out, so Stop can wait for none to be under way. Never
        allocates or locks.
        @param index  the half of each double buffer, 0 or 1 */
    void Switch(long index) noexcept
    {
        in_flight_.fetch_add(1); // before running_ is read: Stop clears running_, then reads the count
        const std::int64_t now  = NowNanoseconds();
        const auto         half = static_cast<std::size_t>(index & 1);
        AudioCallback     *run =
            running_.load(std::memory_order_acquire) ? callback_.load(std::memory_order_acquire) : nullptr;
        if (run != nullptr)
        {
            for (std::size_t c = 0; c < in_types_.size(); ++c)
            {
                FromDriver(buffers_[c].buffers[half], inputs_[c], block_, in_types_[c]);
            }
            run->Process(in_types_.empty() ? nullptr : inputs_.data(), static_cast<int>(in_types_.size()),
                         outputs_.data(), static_cast<int>(out_types_.size()), block_, now,
                         pending_xrun_.exchange(false, std::memory_order_acq_rel));
            for (std::size_t c = 0; c < out_types_.size(); ++c)
            {
                ToDriver(outputs_[c], buffers_[in_types_.size() + c].buffers[half], block_, out_types_[c]);
            }
        }
        else
        {
            for (std::size_t c = 0; c < out_types_.size(); ++c)
            {
                std::memset(buffers_[in_types_.size() + c].buffers[half], 0,
                            static_cast<std::size_t>(block_) * static_cast<std::size_t>(BytesOf(out_types_[c])));
            }
        }
        if (output_ready_)
        {
            ASIOOutputReady();
        }
        in_flight_.fetch_sub(1);
    }

    /** An xrun the driver reported - a resync it asked for, an overload it saw - told to the next callback. */
    void Xrun() noexcept
    {
        pending_xrun_.store(true, std::memory_order_release);
    }

    /** The driver's rate changed under the stream: the stream failing, where it is not the rate it opened at.
        @param rate  the driver's new rate, in hertz */
    void RateChanged(double rate) noexcept
    {
        if (open_ && static_cast<int>(rate + 0.5) != format_.sample_rate)
        {
            Fail("the driver's rate changed under the stream");
        }
    }

    /** The stream failing, its reason kept - without waiting, should the message thread be reading the last - and told
        once. The stream is closed from the message thread (Close), never from the driver's.
        @param why  the reason */
    void Fail(const char *why) noexcept
    {
        {
            const std::unique_lock lock(error_mutex_, std::try_to_lock);
            if (lock.owns_lock())
            {
                try
                {
                    last_error_ = std::string("the stream on ASIO failed: ") + why;
                }
                catch (const std::exception &)
                {
                }
            }
        }
        if (!failed_.exchange(true, std::memory_order_acq_rel) && running_.load(std::memory_order_acquire))
        {
            if (AudioCallback *run = callback_.load(std::memory_order_acquire))
            {
                run->StreamFailed();
            }
        }
    }

  private:
    /** The drivers as listed now: each probed once in the program's life, while no stream holds a driver, and named
        apart; the first with outputs the default output and the first with inputs the default input, as RtAudio chose
        them.
        @return  the drivers that probed, in the registry's order */
    static std::vector<Listed> List()
    {
        std::vector<Listed>            listed;
        std::map<std::string, Probed> &probes = Probes();
        for (const std::string &name : DriverNames())
        {
            auto known = probes.find(name);
            if (known == probes.end())
            {
                if (g_owner.load(std::memory_order_acquire) != nullptr)
                {
                    continue; // not probed yet, and no driver can be loaded while a stream holds one
                }
                const std::optional<Probed> found = Probe(name);
                if (!found)
                {
                    continue;
                }
                known = probes.emplace(name, *found).first;
            }
            const Probed &probed = known->second;
            Listed        device;
            device.driver                     = name;
            device.info.name                  = name;
            device.info.input_channels        = probed.inputs;
            device.info.output_channels       = probed.outputs;
            device.info.duplex_channels       = std::min(probed.inputs, probed.outputs);
            device.info.sample_rates          = probed.rates;
            device.info.preferred_sample_rate = probed.preferred;
            listed.push_back(std::move(device));
        }
        std::vector<DeviceInfo> infos;
        for (const Listed &device : listed)
        {
            infos.push_back(device.info);
        }
        NameApart(infos);
        bool output_named = false, input_named = false;
        for (std::size_t i = 0; i < listed.size(); ++i)
        {
            DeviceInfo &info       = listed[i].info;
            info.name              = std::move(infos[i].name);
            info.is_default_output = !output_named && info.output_channels > 0;
            info.is_default_input  = !input_named && info.input_channels > 0;
            output_named           = output_named || info.is_default_output;
            input_named            = input_named || info.is_default_input;
        }
        return listed;
    }

    /** The device named `name` with channels the way `direction` asks.
        @param listed     the drivers List() gave
        @param name       the device's name, as a settings file keeps it
        @param direction  the way the device is wanted
        @return           the device, or null for none */
    static const Listed *Find(const std::vector<Listed> &listed, const std::string &name, Direction direction)
    {
        for (const Listed &device : listed)
        {
            if (device.info.name == name && Carries(device.info, direction))
            {
                return &device;
            }
        }
        return nullptr;
    }

    /** The loaded driver set to the stream: its rate, its block, the sample type of every channel asked, and the
        buffers made for those channels alone.
        @param request     the stream asked for
        @param with_input  whether it has inputs
        @param why         set to the reason when it cannot be
        @return            true when the driver is ready to start */
    bool Prepare(const StreamRequest &request, bool with_input, std::string &why)
    {
        const auto rate = static_cast<ASIOSampleRate>(request.sample_rate > 0 ? request.sample_rate : 48000);
        if (ASIOCanSampleRate(rate) != ASE_OK)
        {
            why = "it does not take " + std::to_string(static_cast<int>(rate)) + " Hz";
            return false;
        }
        ASIOSampleRate current = 0.0;
        if ((ASIOGetSampleRate(&current) != ASE_OK || current != rate) && ASIOSetSampleRate(rate) != ASE_OK)
        {
            why = "it would not run at " + std::to_string(static_cast<int>(rate)) + " Hz";
            return false;
        }
        long smallest = 0, largest = 0, preferred = 0, granularity = 0;
        if (ASIOGetBufferSize(&smallest, &largest, &preferred, &granularity) != ASE_OK)
        {
            why = "it would not say what buffer sizes it takes";
            return false;
        }
        block_ = static_cast<int>(asio::ChooseBlock(request.block_size, smallest, largest, preferred, granularity));

        const int inputs  = with_input ? request.input_count : 0;
        const int outputs = request.output_count;
        buffers_.assign(static_cast<std::size_t>(inputs + outputs), ASIOBufferInfo{});
        in_types_.clear();
        out_types_.clear();
        for (int c = 0; c < inputs + outputs; ++c)
        {
            const bool      input   = c < inputs;
            ASIOBufferInfo &buffer  = buffers_[static_cast<std::size_t>(c)];
            buffer.isInput          = input ? ASIOTrue : ASIOFalse;
            buffer.channelNum       = input ? request.first_input + c : request.first_output + (c - inputs);
            ASIOChannelInfo channel = {};
            channel.channel         = buffer.channelNum;
            channel.isInput         = buffer.isInput;
            if (ASIOGetChannelInfo(&channel) != ASE_OK || !Converts(channel.type))
            {
                why = std::string(input ? "input" : "output") + " channel " + std::to_string(buffer.channelNum + 1) +
                      " is in a sample type this back end does not convert";
                return false;
            }
            (input ? in_types_ : out_types_).push_back(channel.type);
        }
        if (ASIOCreateBuffers(buffers_.data(), inputs + outputs, block_, &g_callbacks) != ASE_OK)
        {
            why = "it would not make buffers of " + std::to_string(block_) + " frames";
            return false;
        }
        buffers_made_ = true;
        output_ready_ = ASIOOutputReady() == ASE_OK;

        const auto block = static_cast<std::size_t>(block_);
        input_storage_.assign(static_cast<std::size_t>(std::max(inputs, 1)) * block, 0.0f);
        output_storage_.assign(static_cast<std::size_t>(outputs) * block, 0.0f);
        inputs_.assign(static_cast<std::size_t>(std::max(inputs, 1)), nullptr);
        outputs_.assign(static_cast<std::size_t>(outputs), nullptr);
        // Each channel's frames where the channel before's end, the layout every API's callback hands the engine.
        for (std::size_t c = 0; c < static_cast<std::size_t>(inputs); ++c)
        {
            inputs_[c] = input_storage_.data() + c * block;
        }
        for (std::size_t c = 0; c < static_cast<std::size_t>(outputs); ++c)
        {
            outputs_[c] = output_storage_.data() + c * block;
        }

        long input_latency = 0, output_latency = 0;
        if (ASIOGetLatencies(&input_latency, &output_latency) != ASE_OK)
        {
            input_latency = output_latency = 0;
        }
        format_.sample_rate      = static_cast<int>(rate);
        format_.block_size       = block_;
        format_.input_channels   = inputs;
        format_.output_channels  = outputs;
        format_.reported_latency = static_cast<int>((inputs > 0 ? input_latency : 0) + output_latency);
        return true;
    }

    /** The buffers disposed of and the driver released, and the SDK's one driver given up. */
    void Unload() noexcept
    {
        if (buffers_made_)
        {
            ASIODisposeBuffers();
            buffers_made_ = false;
        }
        if (loaded_)
        {
            ASIOExit(); // the driver released through the program's list (DriverList)
            loaded_ = false;
        }
        AsioSystem *self = this;
        g_owner.compare_exchange_strong(self, nullptr, std::memory_order_acq_rel);
        buffers_.clear();
        in_types_.clear();
        out_types_.clear();
    }

    /** Until no buffer switch is under way: Stop's promise that the callback is not running once it returns. */
    void WaitForCallbacks() const noexcept
    {
        while (in_flight_.load() != 0)
        {
            std::this_thread::yield();
        }
    }

    /** Why an open failed, kept for LastError.
        @param text  the reason
        @return      false, the open's answer */
    bool Refuse(const std::string &text)
    {
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

    std::vector<ASIOBufferInfo>  buffers_;             ///< the inputs' and then the outputs' double buffers
    std::vector<ASIOSampleType>  in_types_;            ///< each input channel's sample type
    std::vector<ASIOSampleType>  out_types_;           ///< each output channel's sample type
    std::vector<float>           input_storage_;       ///< the callback's input channels' samples, sized at the open
    std::vector<float>           output_storage_;      ///< the callback's output channels' samples, sized at the open
    std::vector<float *>         inputs_;              ///< the input channel pointers the callback is handed
    std::vector<float *>         outputs_;             ///< the output channel pointers the callback is handed
    int                          block_{0};            ///< the frames a buffer switch carries
    bool                         loaded_{false};       ///< true while this system's driver is the SDK's current one
    bool                         buffers_made_{false}; ///< true while the driver's buffers are made
    bool                         output_ready_{false}; ///< true when the driver asks to be told the outputs are ready
    bool                         started_{false};      ///< true from a successful Start until Stop
    StreamFormat                 format_;              ///< what the stream opened as
    bool                         open_{false};         ///< true from a successful Open until Close
    std::atomic<AudioCallback *> callback_{nullptr};   ///< what runs the stream; null while closed
    std::atomic<bool>            running_{false};      ///< true from Start until Stop; false plays silence
    std::atomic<int>             in_flight_{0};        ///< buffer switches under way
    std::atomic<bool>            pending_xrun_{false}; ///< an xrun to tell the next callback of
    std::atomic<bool>            failed_{false};       ///< true once the stream's failure has been told
    mutable std::mutex           error_mutex_;         ///< guards `last_error_`
    std::string                  last_error_;          ///< the last error, for a person to read
};

void BufferSwitch(long index, ASIOBool) noexcept
{
    if (AsioSystem *system = g_owner.load(std::memory_order_acquire))
    {
        system->Switch(index);
    }
}

ASIOTime *BufferSwitchTimeInfo(ASIOTime *, long index, ASIOBool direct) noexcept
{
    // A driver may call this one whatever the host answered kAsioSupportsTimeInfo with: the same switch.
    BufferSwitch(index, direct);
    return nullptr;
}

void SampleRateChanged(ASIOSampleRate rate) noexcept
{
    if (AsioSystem *system = g_owner.load(std::memory_order_acquire))
    {
        system->RateChanged(static_cast<double>(rate));
    }
}

long AsioMessage(long selector, long value, void *, double *) noexcept
{
    AsioSystem *system = g_owner.load(std::memory_order_acquire);
    switch (selector)
    {
    case kAsioSelectorSupported:
        return value == kAsioResetRequest || value == kAsioEngineVersion || value == kAsioResyncRequest ||
                       value == kAsioLatenciesChanged || value == kAsioSupportsTimeInfo || value == kAsioOverload
                   ? 1L
                   : 0L;
    case kAsioEngineVersion:
        return 2L;
    case kAsioResetRequest:
        // Unplugged, or its buffer size changed in its panel: the stream failing, closed from the message thread.
        if (system != nullptr)
        {
            system->Fail("the driver asked to be reset - unplugged, or its settings changed in its panel");
        }
        return 1L;
    case kAsioResyncRequest:
    case kAsioOverload:
        if (system != nullptr)
        {
            system->Xrun();
        }
        return 1L;
    case kAsioLatenciesChanged:
        return 1L;
    case kAsioSupportsTimeInfo:
        return 0L; // bufferSwitch, which carries what the stream reads; BufferSwitchTimeInfo answers either way
    default:
        return 0L;
    }
}

/** The one name this back end serves.
    @return  "ASIO" */
std::vector<std::string> ApiNames()
{
    return {"ASIO"};
}

/** A system for ASIO.
    @param name  the API's name
    @return      the system, owned by the caller; nullptr for any name but "ASIO" */
std::unique_ptr<AudioSystem> MakeSystem(const std::string &name)
{
    if (name != "ASIO")
    {
        return nullptr;
    }
    return std::make_unique<AsioSystem>();
}

/** What this back end is.
    @return  the text */
std::string Library()
{
    return "ASIO (Steinberg's SDK)";
}
} // namespace

const AudioBackend &AsioBackend()
{
    static const AudioBackend backend{&Library, &ApiNames, &MakeSystem};
    return backend;
}
} // namespace reckless::audio
