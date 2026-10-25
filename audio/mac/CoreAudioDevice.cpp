// CoreAudio on the HAL directly (CoreAudioDevice.h says what it does and why): the one translation unit that meets the
// HAL. CoreAudio's headers bring the Carbon types into the global namespace, so nothing here names its own the same.
#include "CoreAudioDevice.h"

#include <algorithm>
#include <atomic>
#include <chrono>
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

#include <unistd.h>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

namespace reckless::audio
{
namespace
{
/** The rates a device that lists a range is offered at, the ones RtAudio offered, so it lists the rates it listed. */
constexpr unsigned int kRates[] = {4000,  5512,  8000,  9600,  11025, 16000,  22050,
                                   32000, 44100, 48000, 88200, 96000, 176400, 192000};

/** The UID every aggregate this back end makes begins with, so the listing leaves them out. */
constexpr const char *kAggregatePrefix = "com.recklesstechnology.audio.stream-aggregate.";

/** How long a nominal rate set is waited for before the open gives up on it, and how long its latencies are waited
    for to settle after it. */
constexpr auto kRateWait = std::chrono::milliseconds(2000);

/** How long what a device reports of its latency must hold still before it is taken as settled: the HAL moves it in
    steps up to some 75 ms apart after a rate changes (AwaitLatencySettled). */
constexpr auto kLatencySettle = std::chrono::milliseconds(150);

/** The address of a property on an object's main element.
    @param selector  the property
    @param scope     its scope: global, input or output
    @return          the address */
AudioObjectPropertyAddress Address(AudioObjectPropertySelector selector,
                                   AudioObjectPropertyScope    scope = kAudioObjectPropertyScopeGlobal) noexcept
{
    return AudioObjectPropertyAddress{selector, scope, kAudioObjectPropertyElementMain};
}

/** A CFString as UTF-8.
    @param text  the string; may be null
    @return      the UTF-8 text */
std::string Utf8(CFStringRef text)
{
    if (text == nullptr)
    {
        return {};
    }
    const CFIndex     length = CFStringGetLength(text);
    const CFIndex     most   = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::vector<char> bytes(static_cast<std::size_t>(std::max<CFIndex>(most, 1)), '\0');
    if (!CFStringGetCString(text, bytes.data(), most, kCFStringEncodingUTF8))
    {
        return {};
    }
    return std::string(bytes.data());
}

/** A string property of an object, as UTF-8: its name, its maker, its UID.
    @param object    the object
    @param selector  the property
    @return          the text; empty where the object has none */
std::string StringProperty(AudioObjectID object, AudioObjectPropertySelector selector)
{
    const AudioObjectPropertyAddress address = Address(selector);
    CFStringRef                      value   = nullptr;
    UInt32                           size    = sizeof(value);
    if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &value) != noErr || value == nullptr)
    {
        return {};
    }
    std::string text = Utf8(value);
    CFRelease(value);
    return text;
}

/** The channels of each of a device's streams one way.
    @param device  the device
    @param scope   kAudioObjectPropertyScopeInput or kAudioObjectPropertyScopeOutput
    @return        each stream's channels, in the device's order; empty where it has none, or would not say */
std::vector<int> StreamChannels(AudioObjectID device, AudioObjectPropertyScope scope)
{
    const AudioObjectPropertyAddress address = Address(kAudioDevicePropertyStreamConfiguration, scope);
    UInt32                           size    = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size < sizeof(UInt32))
    {
        return {};
    }
    std::vector<std::uint8_t> storage(size, 0);
    auto                     *list = reinterpret_cast<AudioBufferList *>(storage.data());
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, list) != noErr)
    {
        return {};
    }
    std::vector<int> channels;
    for (UInt32 i = 0; i < list->mNumberBuffers; ++i)
    {
        channels.push_back(static_cast<int>(list->mBuffers[i].mNumberChannels));
    }
    return channels;
}

/** A device's streams one way.
    @param device  the device
    @param scope   kAudioObjectPropertyScopeInput or kAudioObjectPropertyScopeOutput
    @return        the streams, in the device's order - the order of its IOProc's buffers; empty where it has none */
std::vector<AudioObjectID> Streams(AudioObjectID device, AudioObjectPropertyScope scope)
{
    const AudioObjectPropertyAddress address = Address(kAudioDevicePropertyStreams, scope);
    UInt32                           size    = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0)
    {
        return {};
    }
    std::vector<AudioObjectID> streams(size / sizeof(AudioObjectID));
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, streams.data()) != noErr)
    {
        return {};
    }
    return streams;
}

/** Whether a stream hands its IOProc buffers the back end reads: 32-bit float, interleaved, native-endian linear PCM -
    the HAL's own format for every mixable stream, which a stream another process has hogged for a format of its own
    (an encoded passthrough, an integer format) need not be.
    @param stream  the stream
    @return        true for 32-bit float */
bool IsFloatStream(AudioObjectID stream) noexcept
{
    const AudioObjectPropertyAddress address = Address(kAudioStreamPropertyVirtualFormat);
    AudioStreamBasicDescription      format{};
    UInt32                           size = sizeof(format);
    if (AudioObjectGetPropertyData(stream, &address, 0, nullptr, &size, &format) != noErr)
    {
        return false;
    }
    return format.mFormatID == kAudioFormatLinearPCM && (format.mFormatFlags & kAudioFormatFlagIsFloat) != 0 &&
           (format.mFormatFlags & kAudioFormatFlagIsNonInterleaved) == 0 &&
           (format.mFormatFlags & kAudioFormatFlagIsBigEndian) == 0 && format.mBitsPerChannel == 32;
}

/** The sum of a stream list's channels.
    @param streams  each stream's channels
    @return         their sum */
int Total(const std::vector<int> &streams) noexcept
{
    int total = 0;
    for (const int channels : streams)
    {
        total += channels;
    }
    return total;
}

/** A device's nominal rate now.
    @param device  the device
    @return        the rate, in hertz; 0 where it would not say */
double NominalRate(AudioObjectID device) noexcept
{
    const AudioObjectPropertyAddress address = Address(kAudioDevicePropertyNominalSampleRate);
    Float64                          rate    = 0.0;
    UInt32                           size    = sizeof(rate);
    return AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, &rate) == noErr ? rate : 0.0;
}

/** The rates a device lists one way, as RtAudio read them: a single rate as itself, and a range as the series' rates
    within the narrowest of the ranges listed; sorted, each once.
    @param device  the device
    @param scope   the way it carries channels: output, or input for a device that has no outputs
    @return        the rates, in hertz */
std::vector<int> Rates(AudioObjectID device, AudioObjectPropertyScope scope)
{
    const AudioObjectPropertyAddress address = Address(kAudioDevicePropertyAvailableNominalSampleRates, scope);
    UInt32                           size    = 0;
    if (AudioObjectGetPropertyDataSize(device, &address, 0, nullptr, &size) != noErr || size == 0)
    {
        return {};
    }
    std::vector<AudioValueRange> ranges(size / sizeof(AudioValueRange));
    if (AudioObjectGetPropertyData(device, &address, 0, nullptr, &size, ranges.data()) != noErr)
    {
        return {};
    }
    std::vector<int> rates;
    double           lowest = 1.0, highest = 1e10;
    bool             ranged = false;
    for (const AudioValueRange &range : ranges)
    {
        if (range.mMinimum == range.mMaximum)
        {
            rates.push_back(static_cast<int>(range.mMinimum));
        }
        else
        {
            ranged  = true;
            lowest  = std::max(lowest, range.mMinimum);
            highest = std::min(highest, range.mMaximum);
        }
    }
    if (ranged)
    {
        for (const unsigned int rate : kRates)
        {
            if (rate >= static_cast<unsigned int>(lowest) && rate <= static_cast<unsigned int>(highest))
            {
                rates.push_back(static_cast<int>(rate));
            }
        }
    }
    std::sort(rates.begin(), rates.end());
    rates.erase(std::unique(rates.begin(), rates.end()), rates.end());
    return rates;
}

/** What probing a device once found: its listed name, its channels each way and its rates. */
struct Probed
{
    std::string      name;         ///< its maker's name, ": " and its own, as RtAudio named it
    int              inputs{0};    ///< its input channels
    int              outputs{0};   ///< its output channels
    std::vector<int> rates;        ///< the rates it lists
    int              preferred{0}; ///< the highest of them up to 48 kHz, else the lowest, as RtAudio preferred
};

/** A device probed: nothing where it lists no rates, as RtAudio passed it over.
    @param device  the device
    @return        what the probe found */
std::optional<Probed> Probe(AudioObjectID device)
{
    Probed probed;
    probed.name    = StringProperty(device, kAudioObjectPropertyManufacturer) + ": " +
                     StringProperty(device, kAudioObjectPropertyName);
    probed.outputs = Total(StreamChannels(device, kAudioObjectPropertyScopeOutput));
    probed.inputs  = Total(StreamChannels(device, kAudioObjectPropertyScopeInput));
    probed.rates = Rates(device, probed.outputs > 0 ? kAudioObjectPropertyScopeOutput : kAudioObjectPropertyScopeInput);
    if (probed.rates.empty() || probed.inputs + probed.outputs <= 0)
    {
        return std::nullopt;
    }
    for (const int rate : probed.rates)
    {
        if (probed.preferred == 0 || (rate <= 48000 && rate > probed.preferred))
        {
            probed.preferred = rate;
        }
    }
    return probed;
}

/** Every probe that found a device, by its id, for the program's life, as RtAudio kept them.
    @return  the probes, one map for the program's life */
std::map<AudioObjectID, Probed> &Probes()
{
    static std::map<AudioObjectID, Probed> probes;
    return probes;
}

/** One device as listed: its id and the device it is. */
struct Listed
{
    AudioObjectID id{kAudioObjectUnknown}; ///< the HAL's id for it
    DeviceInfo    info;                    ///< the device as listed, under the name NameApart gave it
};

/** The system's default device one way.
    @param selector  kAudioHardwarePropertyDefaultOutputDevice or kAudioHardwarePropertyDefaultInputDevice
    @return          the device; kAudioObjectUnknown for none */
AudioObjectID DefaultDevice(AudioObjectPropertySelector selector) noexcept
{
    const AudioObjectPropertyAddress address = Address(selector);
    AudioObjectID                    device  = kAudioObjectUnknown;
    UInt32                           size    = sizeof(device);
    return AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &device) == noErr
               ? device
               : kAudioObjectUnknown;
}

/** The HAL's devices now - but this back end's own aggregates - each probed once in the program's life, its current
    rate read afresh, named apart, the system's defaults marked.
    @return  the devices, in the HAL's order */
std::vector<Listed> Enumerate()
{
    const AudioObjectPropertyAddress address = Address(kAudioHardwarePropertyDevices);
    UInt32                           size    = 0;
    std::vector<AudioObjectID>       ids;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &address, 0, nullptr, &size) == noErr && size > 0)
    {
        ids.resize(size / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, ids.data()) != noErr)
        {
            ids.clear();
        }
    }
    const AudioObjectID default_output = DefaultDevice(kAudioHardwarePropertyDefaultOutputDevice);
    const AudioObjectID default_input  = DefaultDevice(kAudioHardwarePropertyDefaultInputDevice);
    std::vector<Listed> listed;
    auto               &probes = Probes();
    for (const AudioObjectID id : ids)
    {
        if (StringProperty(id, kAudioDevicePropertyDeviceUID).rfind(kAggregatePrefix, 0) == 0)
        {
            continue; // a stream's own aggregate
        }
        auto known = probes.find(id);
        if (known == probes.end())
        {
            const std::optional<Probed> found = Probe(id);
            if (!found)
            {
                continue;
            }
            known = probes.emplace(id, *found).first;
        }
        const Probed &probed = known->second;
        Listed        device;
        device.id                         = id;
        device.info.name                  = probed.name;
        device.info.input_channels        = probed.inputs;
        device.info.output_channels       = probed.outputs;
        device.info.duplex_channels       = std::min(probed.inputs, probed.outputs);
        device.info.sample_rates          = probed.rates;
        device.info.preferred_sample_rate = probed.preferred;
        device.info.current_sample_rate   = static_cast<int>(NominalRate(id));
        device.info.is_default_output     = id == default_output && probed.outputs > 0;
        device.info.is_default_input      = id == default_input && probed.inputs > 0;
        listed.push_back(std::move(device));
    }
    std::vector<DeviceInfo> infos;
    for (const Listed &device : listed)
    {
        infos.push_back(device.info);
    }
    NameApart(infos);
    for (std::size_t i = 0; i < listed.size(); ++i)
    {
        listed[i].info.name = std::move(infos[i].name);
    }
    return listed;
}

/** Where a callback channel's samples are in a device's streams: the stream, the channel within it and the stream's
    channels, its frames' stride. */
struct Place
{
    UInt32 stream{0};  ///< the stream's index in the IOProc's buffer list
    UInt32 channel{0}; ///< the channel within the stream
    UInt32 stride{1};  ///< the stream's channels, the samples a frame takes
};

/** The places of `count` device channels from `first` across streams of `streams` channels each.
    @param streams  each stream's channels
    @param first    the first device channel, counted from 0 across the streams
    @param count    how many
    @return         each channel's place; empty where the streams do not hold them */
std::vector<Place> PlacesOf(const std::vector<int> &streams, int first, int count)
{
    std::vector<Place> places;
    for (int c = first; c < first + count; ++c)
    {
        int  before = 0;
        bool found  = false;
        for (std::size_t s = 0; s < streams.size() && !found; ++s)
        {
            if (c < before + streams[s])
            {
                places.push_back({static_cast<UInt32>(s), static_cast<UInt32>(c - before),
                                  static_cast<UInt32>(std::max(streams[s], 1))});
                found = true;
            }
            before += streams[s];
        }
        if (!found)
        {
            return {};
        }
    }
    return places;
}

/** CoreAudio's AudioSystem: the devices, and one stream on a device, or on a private aggregate of two. Message thread
    throughout but for Proc and Listener, which the HAL's threads run. */
class CoreAudioSystem final : public AudioSystem
{
  public:
    CoreAudioSystem() = default;

    /** The stream closed (Close). */
    ~CoreAudioSystem() override
    {
        Close();
    }

    CoreAudioSystem(const CoreAudioSystem &)            = delete;
    CoreAudioSystem &operator=(const CoreAudioSystem &) = delete;

    std::string Name() const override
    {
        return "CoreAudio";
    }

    std::vector<DeviceInfo> Devices() override
    {
        std::vector<DeviceInfo> devices;
        for (Listed &device : Enumerate())
        {
            devices.push_back(std::move(device.info));
        }
        return devices;
    }

    bool Open(const StreamRequest &request, AudioCallback &callback) override
    {
        Close();
        SetError({});
        const std::vector<Listed> listed = Enumerate();
        const Listed             *output = Find(listed, request.output_device, Direction::kOutput);
        if (output == nullptr || request.output_count <= 0)
        {
            return Refuse("no output device named '" + request.output_device + "' on CoreAudio");
        }
        if (request.first_output < 0 || request.first_output + request.output_count > output->info.output_channels)
        {
            return Refuse("'" + output->info.name + "' has " + std::to_string(output->info.output_channels) +
                          " output channels, and the stream asks for channels " +
                          std::to_string(request.first_output + 1) + " to " +
                          std::to_string(request.first_output + request.output_count));
        }
        const bool    with_input = !request.input_device.empty() && request.input_count > 0;
        const Listed *input      = nullptr;
        if (with_input)
        {
            input = Find(listed, request.input_device, Direction::kInput);
            if (input == nullptr)
            {
                return Refuse("no input device named '" + request.input_device + "' on CoreAudio");
            }
            if (request.first_input < 0 || request.first_input + request.input_count > input->info.input_channels)
            {
                return Refuse("'" + input->info.name + "' has " + std::to_string(input->info.input_channels) +
                              " input channels, and the stream asks for channels " +
                              std::to_string(request.first_input + 1) + " to " +
                              std::to_string(request.first_input + request.input_count));
            }
        }

        // The device the IOProc runs on: the output's, or an aggregate of the output and an input on another device -
        // the output's inputs first in the aggregate's input channels, then the input device's.
        device_         = output->id;
        int first_input = request.first_input;
        if (input != nullptr && input->id != output->id)
        {
            std::string why;
            if (!MakeAggregate(output->id, input->id, why))
            {
                return Refuse("'" + output->info.name + "' and '" + input->info.name +
                              "' could not be joined into one stream on CoreAudio: " + why);
            }
            device_ = aggregate_;
            first_input += output->info.input_channels;
        }
        watched_ = {output->id};
        if (input != nullptr && input->id != output->id)
        {
            watched_.push_back(input->id);
        }

        std::string why;
        const int   rate         = request.sample_rate > 0 ? request.sample_rate : output->info.DefaultSampleRate();
        bool        rate_changed = false;
        if (!SetRate(rate, rate_changed, why))
        {
            Close();
            return Refuse("'" + output->info.name + "' would not open on CoreAudio: " + why);
        }
        if (rate_changed || aggregate_ != kAudioObjectUnknown)
        {
            AwaitLatencySettled();
        }
        if (!SetBlock(request.block_size, why) ||
            !MapChannels(first_input, with_input ? request.input_count : 0, request.first_output, request.output_count,
                         why) ||
            !Attach(why))
        {
            Close();
            return Refuse("'" + output->info.name + "' would not open on CoreAudio: " + why);
        }

        format_.sample_rate      = rate;
        format_.block_size       = block_;
        format_.input_channels   = static_cast<int>(input_places_.size());
        format_.output_channels  = static_cast<int>(output_places_.size());
        format_.reported_latency = Latency(kAudioObjectPropertyScopeOutput) +
                                   (input_places_.empty() ? 0 : Latency(kAudioObjectPropertyScopeInput));
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
        has_called_.store(false, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        if (AudioDeviceStart(device_, proc_) != noErr)
        {
            running_.store(false, std::memory_order_release);
            SetError("the stream would not start on CoreAudio");
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
            AudioDeviceStop(device_, proc_);
            started_ = false;
        }
        WaitForCallbacks();
    }

    void Close() override
    {
        Stop();
        Detach();
        if (aggregate_ != kAudioObjectUnknown)
        {
            AudioHardwareDestroyAggregateDevice(aggregate_);
            aggregate_ = kAudioObjectUnknown;
        }
        device_ = kAudioObjectUnknown;
        watched_.clear();
        input_places_.clear();
        output_places_.clear();
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

  private:
    /** The device named `name` with channels the way `direction` asks.
        @param listed     the devices Enumerate() gave
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

    /** A private aggregate of `output` and `input` made: the output's clock the aggregate's, the input
       drift-compensated against it, seen by this process alone and gone with it.
        @param output  the output's device
        @param input   the input's device
        @param why     set to the reason when it cannot be made
        @return        true when it was made (aggregate_) */
    bool MakeAggregate(AudioObjectID output, AudioObjectID input, std::string &why)
    {
        const std::string output_uid = StringProperty(output, kAudioDevicePropertyDeviceUID);
        const std::string input_uid  = StringProperty(input, kAudioDevicePropertyDeviceUID);
        if (output_uid.empty() || input_uid.empty())
        {
            why = "a device has no UID";
            return false;
        }
        static std::atomic<int> made{0};
        const std::string uid = std::string(kAggregatePrefix) + std::to_string(getpid()) + "." + std::to_string(++made);
        const auto        cfstr = [](const std::string &text) {
            return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(text.data()),
                                           static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
        };
        CFStringRef            cf_output = cfstr(output_uid);
        CFStringRef            cf_input  = cfstr(input_uid);
        CFStringRef            cf_uid    = cfstr(uid);
        CFStringRef            cf_name   = cfstr("Reckless stream");
        const int              one       = 1;
        CFNumberRef            yes       = CFNumberCreate(nullptr, kCFNumberIntType, &one);
        CFMutableDictionaryRef sub_output =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(sub_output, CFSTR(kAudioSubDeviceUIDKey), cf_output);
        CFMutableDictionaryRef sub_input =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(sub_input, CFSTR(kAudioSubDeviceUIDKey), cf_input);
        CFDictionarySetValue(sub_input, CFSTR(kAudioSubDeviceDriftCompensationKey), yes);
        const void            *subs[]   = {sub_output, sub_input};
        CFArrayRef             sub_list = CFArrayCreate(nullptr, subs, 2, &kCFTypeArrayCallBacks);
        CFMutableDictionaryRef description =
            CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
        CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceNameKey), cf_name);
        CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceUIDKey), cf_uid);
        CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceIsPrivateKey), yes);
        CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceSubDeviceListKey), sub_list);
        CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceMainSubDeviceKey), cf_output);
        AudioObjectID  device = kAudioObjectUnknown;
        const OSStatus result = AudioHardwareCreateAggregateDevice(description, &device);
        CFRelease(description);
        CFRelease(sub_list);
        CFRelease(sub_input);
        CFRelease(sub_output);
        CFRelease(yes);
        CFRelease(cf_name);
        CFRelease(cf_uid);
        CFRelease(cf_input);
        CFRelease(cf_output);
        if (result != noErr || device == kAudioObjectUnknown)
        {
            why = "AudioHardwareCreateAggregateDevice: " + std::to_string(result);
            return false;
        }
        aggregate_ = device;
        return true;
    }

    /** The device's nominal rate set to `rate` where it runs at another, and waited for.
        @param rate     the rate, in hertz
        @param changed  set to true where the device ran at another and was set to `rate`
        @param why      set to the reason when it will not run at it
        @return         true when it runs at `rate` */
    bool SetRate(int rate, bool &changed, std::string &why)
    {
        changed = false;
        if (static_cast<int>(NominalRate(device_) + 0.5) == rate)
        {
            return true;
        }
        changed                                  = true;
        const AudioObjectPropertyAddress address = Address(kAudioDevicePropertyNominalSampleRate);
        const Float64                    wanted  = rate;
        if (AudioObjectSetPropertyData(device_, &address, 0, nullptr, sizeof(wanted), &wanted) != noErr)
        {
            why = "it would not run at " + std::to_string(rate) + " Hz";
            return false;
        }
        const auto until = std::chrono::steady_clock::now() + kRateWait;
        while (static_cast<int>(NominalRate(device_) + 0.5) != rate)
        {
            if (std::chrono::steady_clock::now() > until)
            {
                why = "it did not come to " + std::to_string(rate) + " Hz";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return true;
    }

    /** Everything the device reports of its latency: its own latency and safety offset each way, and each of its
        streams' latency.
        @return  the values, in a fixed order */
    std::vector<UInt32> LatencyReport() const
    {
        std::vector<UInt32> report;
        for (const AudioObjectPropertyScope scope : {kAudioObjectPropertyScopeInput, kAudioObjectPropertyScopeOutput})
        {
            for (const AudioObjectPropertySelector selector :
                 {kAudioDevicePropertyLatency, kAudioDevicePropertySafetyOffset})
            {
                const AudioObjectPropertyAddress address = Address(selector, scope);
                UInt32                           frames  = 0;
                UInt32                           size    = sizeof(frames);
                AudioObjectGetPropertyData(device_, &address, 0, nullptr, &size, &frames);
                report.push_back(frames);
            }
            for (const AudioObjectID stream : Streams(device_, scope))
            {
                const AudioObjectPropertyAddress address = Address(kAudioStreamPropertyLatency);
                UInt32                           frames  = 0;
                UInt32                           size    = sizeof(frames);
                AudioObjectGetPropertyData(stream, &address, 0, nullptr, &size, &frames);
                report.push_back(frames);
            }
        }
        return report;
    }

    /** Until what the device reports of its latency has held still for kLatencySettle, at most kRateWait: after its
        rate changes, or an aggregate is made, the HAL moves the nominal rate first and the latencies after it, in steps
        some 60 to 75 ms apart - an aggregate of a UMC1820 and a UMIK-2 taken from 96 to 44.1 kHz read 262, then 244,
        then 162 frames over 80 ms once its nominal rate had come - so latencies read when the rate comes are the old
        rate's, or a mix. */
    void AwaitLatencySettled() const
    {
        const auto          until  = std::chrono::steady_clock::now() + kRateWait;
        std::vector<UInt32> last   = LatencyReport();
        auto                steady = std::chrono::steady_clock::now();
        while (std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            std::vector<UInt32> report = LatencyReport();
            if (report != last)
            {
                last   = std::move(report);
                steady = std::chrono::steady_clock::now();
            }
            else if (std::chrono::steady_clock::now() - steady >= kLatencySettle)
            {
                return;
            }
        }
    }

    /** The device's buffer set to the block asked, held to its range, and read back.
        @param asked  the block asked for, in frames; 0 or less for the device's own
        @param why    set to the reason when it cannot be set
        @return       true when block_ holds the block the device runs at */
    bool SetBlock(int asked, std::string &why)
    {
        UInt32                           frames = 0;
        UInt32                           size   = sizeof(frames);
        const AudioObjectPropertyAddress buffer = Address(kAudioDevicePropertyBufferFrameSize);
        if (asked > 0)
        {
            const AudioObjectPropertyAddress range_address = Address(kAudioDevicePropertyBufferFrameSizeRange);
            AudioValueRange                  range{};
            UInt32                           range_size = sizeof(range);
            double                           wanted     = asked;
            if (AudioObjectGetPropertyData(device_, &range_address, 0, nullptr, &range_size, &range) == noErr)
            {
                wanted = std::clamp(wanted, range.mMinimum, range.mMaximum);
            }
            frames = static_cast<UInt32>(wanted);
            if (AudioObjectSetPropertyData(device_, &buffer, 0, nullptr, sizeof(frames), &frames) != noErr)
            {
                why = "it would not take a buffer of " + std::to_string(frames) + " frames";
                return false;
            }
        }
        size = sizeof(frames);
        if (AudioObjectGetPropertyData(device_, &buffer, 0, nullptr, &size, &frames) != noErr || frames == 0)
        {
            why = "it would not say its buffer's size";
            return false;
        }
        block_ = static_cast<int>(frames);
        return true;
    }

    /** The callback's channels placed in the device's streams, and their storage made.
        @param first_input   the first device input channel, counted across the device's input streams
        @param inputs        the input channels
        @param first_output  the first device output channel
        @param outputs       the output channels
        @param why           set to the reason when the streams do not hold them
        @return              true when every channel has its place */
    bool MapChannels(int first_input, int inputs, int first_output, int outputs, std::string &why)
    {
        input_places_  = PlacesOf(StreamChannels(device_, kAudioObjectPropertyScopeInput), first_input, inputs);
        output_places_ = PlacesOf(StreamChannels(device_, kAudioObjectPropertyScopeOutput), first_output, outputs);
        if (static_cast<int>(input_places_.size()) != inputs || static_cast<int>(output_places_.size()) != outputs)
        {
            why = "its streams do not hold the channels asked for";
            return false;
        }
        // The IOProc reads and writes 32-bit float: a stream in use that hands over anything else is refused, rather
        // than its samples misread.
        const std::vector<AudioObjectID> input_streams  = Streams(device_, kAudioObjectPropertyScopeInput);
        const std::vector<AudioObjectID> output_streams = Streams(device_, kAudioObjectPropertyScopeOutput);
        const auto all_float = [](const std::vector<Place> &places, const std::vector<AudioObjectID> &streams) {
            return std::all_of(places.begin(), places.end(), [&streams](const Place &place) {
                return place.stream < streams.size() && IsFloatStream(streams[place.stream]);
            });
        };
        if (!all_float(input_places_, input_streams) || !all_float(output_places_, output_streams))
        {
            why = "a stream it would play or record through is not 32-bit float - is another program holding it in a "
                  "format of its own?";
            return false;
        }
        // Room for twice the block, should the HAL hand over more than it was asked for once.
        capacity_       = 2 * block_;
        const auto most = static_cast<std::size_t>(capacity_);
        input_storage_.assign(static_cast<std::size_t>(std::max(inputs, 1)) * most, 0.0f);
        output_storage_.assign(static_cast<std::size_t>(outputs) * most, 0.0f);
        inputs_.assign(static_cast<std::size_t>(std::max(inputs, 1)), nullptr);
        outputs_.assign(static_cast<std::size_t>(outputs), nullptr);
        return true;
    }

    /** The IOProc made on the device and the listeners added: a processor overload on the device, and each device the
        stream plays on going away.
        @param why  set to the reason when the IOProc cannot be made
        @return     true when it was */
    bool Attach(std::string &why)
    {
        if (AudioDeviceCreateIOProcID(device_, &CoreAudioSystem::Proc, this, &proc_) != noErr || proc_ == nullptr)
        {
            proc_ = nullptr;
            why   = "it would not take an IOProc";
            return false;
        }
        const AudioObjectPropertyAddress overload = Address(kAudioDeviceProcessorOverload);
        AudioObjectAddPropertyListener(device_, &overload, &CoreAudioSystem::Listener, this);
        const AudioObjectPropertyAddress alive = Address(kAudioDevicePropertyDeviceIsAlive);
        for (const AudioObjectID device : watched_)
        {
            AudioObjectAddPropertyListener(device, &alive, &CoreAudioSystem::Listener, this);
        }
        listening_ = true;
        return true;
    }

    /** The listeners removed and the IOProc destroyed: once this returns, neither runs. */
    void Detach() noexcept
    {
        if (listening_)
        {
            const AudioObjectPropertyAddress overload = Address(kAudioDeviceProcessorOverload);
            AudioObjectRemovePropertyListener(device_, &overload, &CoreAudioSystem::Listener, this);
            const AudioObjectPropertyAddress alive = Address(kAudioDevicePropertyDeviceIsAlive);
            for (const AudioObjectID device : watched_)
            {
                AudioObjectRemovePropertyListener(device, &alive, &CoreAudioSystem::Listener, this);
            }
            listening_ = false;
        }
        if (proc_ != nullptr)
        {
            AudioDeviceDestroyIOProcID(device_, proc_);
            proc_ = nullptr;
        }
    }

    /** The latency the device reports one way, beyond the buffer: its own, its safety offset, and the latency of the
        stream the stream's first channel that way is in - a built-in device's processing is there (690 samples on a
        MacBook Pro's speakers, 1439 on its microphone, at 48 kHz), where a USB interface's streams report none.
        @param scope  input or output
        @return       the latency, in frames */
    int Latency(AudioObjectPropertyScope scope) const
    {
        int                       total  = 0;
        const std::vector<Place> &places = scope == kAudioObjectPropertyScopeInput ? input_places_ : output_places_;
        if (!places.empty())
        {
            const std::vector<AudioObjectID> streams = Streams(device_, scope);
            if (places.front().stream < streams.size())
            {
                const AudioObjectPropertyAddress address = Address(kAudioStreamPropertyLatency);
                UInt32                           frames  = 0;
                UInt32                           size    = sizeof(frames);
                if (AudioObjectGetPropertyData(streams[places.front().stream], &address, 0, nullptr, &size, &frames) ==
                    noErr)
                {
                    total += static_cast<int>(frames);
                }
            }
        }
        const AudioObjectPropertySelector selectors[] = {kAudioDevicePropertyLatency, kAudioDevicePropertySafetyOffset};
        for (const AudioObjectPropertySelector selector : selectors)
        {
            const AudioObjectPropertyAddress address = Address(selector, scope);
            UInt32                           frames  = 0;
            UInt32                           size    = sizeof(frames);
            if (AudioObjectGetPropertyData(device_, &address, 0, nullptr, &size, &frames) == noErr)
            {
                total += static_cast<int>(frames);
            }
        }
        return total;
    }

    /** The device's IOProc, on the HAL's real-time thread: each input channel copied out of its stream's interleaved
        frames into the channels laid end to end, the callback run, each output channel copied into its stream - every
        output stream cleared first, and left silent while the stream is stopped. Counted in and out, so Stop can wait
        for none to be under way. Never allocates or locks.
        @param in    the input streams' buffers
        @param out   the output streams' buffers
        @param self  the CoreAudioSystem
        @return      noErr */
    static OSStatus Proc(AudioObjectID, const AudioTimeStamp *, const AudioBufferList *in, const AudioTimeStamp *,
                         AudioBufferList *out, const AudioTimeStamp *, void *self) noexcept
    {
        auto &system = *static_cast<CoreAudioSystem *>(self);
        system.in_flight_.fetch_add(1); // before running_ is read: Stop clears running_, then reads the count
        const std::int64_t now = NowNanoseconds();
        for (UInt32 s = 0; out != nullptr && s < out->mNumberBuffers; ++s)
        {
            if (out->mBuffers[s].mData != nullptr)
            {
                std::memset(out->mBuffers[s].mData, 0, out->mBuffers[s].mDataByteSize);
            }
        }
        int frames = 0;
        if (out != nullptr && !system.output_places_.empty() &&
            system.output_places_.front().stream < out->mNumberBuffers)
        {
            const AudioBuffer &first = out->mBuffers[system.output_places_.front().stream];
            frames =
                static_cast<int>(first.mDataByteSize / (sizeof(float) * std::max<UInt32>(first.mNumberChannels, 1)));
        }
        AudioCallback *run = system.running_.load(std::memory_order_acquire)
                                 ? system.callback_.load(std::memory_order_acquire)
                                 : nullptr;
        if (run != nullptr && frames > 0 && frames <= system.capacity_)
        {
            const auto n = static_cast<std::size_t>(frames);
            for (std::size_t c = 0; c < system.input_places_.size(); ++c)
            {
                float *channel     = system.input_storage_.data() + c * n;
                system.inputs_[c]  = channel;
                const Place &place = system.input_places_[c];
                const auto  *data  = in != nullptr && place.stream < in->mNumberBuffers
                                         ? static_cast<const float *>(in->mBuffers[place.stream].mData)
                                         : nullptr;
                for (std::size_t f = 0; f < n; ++f)
                {
                    channel[f] = data != nullptr ? data[f * place.stride + place.channel] : 0.0f;
                }
            }
            for (std::size_t c = 0; c < system.output_places_.size(); ++c)
            {
                system.outputs_[c] = system.output_storage_.data() + c * n;
            }
            run->Process(system.input_places_.empty() ? nullptr : system.inputs_.data(),
                         static_cast<int>(system.input_places_.size()), system.outputs_.data(),
                         static_cast<int>(system.output_places_.size()), frames, now,
                         system.pending_xrun_.exchange(false, std::memory_order_acq_rel));
            for (std::size_t c = 0; c < system.output_places_.size(); ++c)
            {
                const Place &place = system.output_places_[c];
                auto        *data  = place.stream < out->mNumberBuffers
                                         ? static_cast<float *>(out->mBuffers[place.stream].mData)
                                         : nullptr;
                if (data == nullptr)
                {
                    continue;
                }
                for (std::size_t f = 0; f < n; ++f)
                {
                    data[f * place.stride + place.channel] = system.outputs_[c][f];
                }
            }
        }
        else if (run != nullptr && frames > system.capacity_)
        {
            system.pending_xrun_.store(true, std::memory_order_release); // a cycle the callback never saw
        }
        system.has_called_.store(true, std::memory_order_release);
        system.in_flight_.fetch_sub(1);
        return noErr;
    }

    /** The HAL's property listener, on a thread of its own: a processor overload is an xrun for the next callback -
        but not one before the first IOProc, which is the HAL's I/O thread starting: at a cycle of a third of a
        millisecond (32 frames at 88.2 or 96 kHz, 16 at 48 kHz) the HAL reports one overload at cycle 0 of nearly
        every stream, an IOProc that does nothing but clear its buffers included, and none after it; nothing the
        callback made was lost - and a device that is no longer alive is the stream failing.
        @param object     the device whose property changed
        @param count      the properties changed
        @param addresses  their addresses
        @param self       the CoreAudioSystem
        @return           noErr */
    static OSStatus Listener(AudioObjectID object, UInt32 count, const AudioObjectPropertyAddress *addresses,
                             void *self) noexcept
    {
        auto &system = *static_cast<CoreAudioSystem *>(self);
        for (UInt32 i = 0; i < count; ++i)
        {
            if (addresses[i].mSelector == kAudioDeviceProcessorOverload &&
                system.has_called_.load(std::memory_order_acquire))
            {
                system.pending_xrun_.store(true, std::memory_order_release);
            }
            else if (addresses[i].mSelector == kAudioDevicePropertyDeviceIsAlive)
            {
                const AudioObjectPropertyAddress address = Address(kAudioDevicePropertyDeviceIsAlive);
                UInt32                           alive   = 1;
                UInt32                           size    = sizeof(alive);
                if (AudioObjectGetPropertyData(object, &address, 0, nullptr, &size, &alive) != noErr || alive == 0)
                {
                    system.Fail("a device went away under the stream");
                }
            }
        }
        return noErr;
    }

    /** The stream failing, its reason kept - without waiting, should the message thread be reading the last - and told
        once. The stream is closed from the message thread (Close), never from the HAL's.
        @param why  the reason */
    void Fail(const char *why) noexcept
    {
        {
            const std::unique_lock lock(error_mutex_, std::try_to_lock);
            if (lock.owns_lock())
            {
                try
                {
                    last_error_ = std::string("the stream on CoreAudio failed: ") + why;
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

    /** Until no IOProc is under way: Stop's promise that the callback is not running once it returns. */
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

    AudioObjectID                device_{kAudioObjectUnknown};    ///< the device the IOProc runs on
    AudioObjectID                aggregate_{kAudioObjectUnknown}; ///< the stream's own aggregate; unknown for none
    std::vector<AudioObjectID>   watched_;                        ///< the devices whose going away fails the stream
    AudioDeviceIOProcID          proc_{nullptr};                  ///< the IOProc on device_; null for none
    bool                         listening_{false};               ///< true while the listeners are added
    std::vector<Place>           input_places_;        ///< each input channel's place in the device's input streams
    std::vector<Place>           output_places_;       ///< each output channel's place in its output streams
    std::vector<float>           input_storage_;       ///< the callback's input channels, capacity_ each
    std::vector<float>           output_storage_;      ///< the callback's output channels, capacity_ each
    std::vector<float *>         inputs_;              ///< the input channel pointers the callback is handed
    std::vector<float *>         outputs_;             ///< the output channel pointers the callback is handed
    int                          block_{0};            ///< the frames the device's buffer holds
    int                          capacity_{0};         ///< the most frames an IOProc may carry to the callback
    bool                         started_{false};      ///< true from a successful Start until Stop
    StreamFormat                 format_;              ///< what the stream opened as
    bool                         open_{false};         ///< true from a successful Open until Close
    std::atomic<AudioCallback *> callback_{nullptr};   ///< what runs the stream; null while closed
    std::atomic<bool>            running_{false};      ///< true from Start until Stop; false plays silence
    std::atomic<int>             in_flight_{0};        ///< IOProcs under way
    std::atomic<bool>            pending_xrun_{false}; ///< an xrun to tell the next callback of
    std::atomic<bool>            has_called_{false};   ///< true once an IOProc has run since the Start
    std::atomic<bool>            failed_{false};       ///< true once the stream's failure has been told
    mutable std::mutex           error_mutex_;         ///< guards `last_error_`
    std::string                  last_error_;          ///< the last error, for a person to read
};

/** The one name this back end serves.
    @return  "CoreAudio" */
std::vector<std::string> ApiNames()
{
    return {"CoreAudio"};
}

/** A system for CoreAudio.
    @param name  the API's name
    @return      the system, owned by the caller; nullptr for any name but "CoreAudio" */
std::unique_ptr<AudioSystem> MakeSystem(const std::string &name)
{
    if (name != "CoreAudio")
    {
        return nullptr;
    }
    return std::make_unique<CoreAudioSystem>();
}

/** What this back end is.
    @return  "CoreAudio" */
std::string Library()
{
    return "CoreAudio";
}
} // namespace

const AudioBackend &CoreAudioBackend()
{
    static const AudioBackend backend{&Library, &ApiNames, &MakeSystem};
    return backend;
}
} // namespace reckless::audio
