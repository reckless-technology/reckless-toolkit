// Windows Audio on WASAPI in its two modes (WasapiDevice.h says what each does and why): the one translation unit
// that meets the Core Audio interfaces.
#include "WasapiDevice.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <future>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// The Core Audio interfaces, the scheduler's and COM's, after windows.h, which they need.
#include <audioclient.h>
#include <avrt.h>
#include <mmdeviceapi.h>
#include <mmreg.h>
#include <propidl.h>
#include <wrl/client.h>

namespace reckless::audio
{
namespace
{
using Microsoft::WRL::ComPtr;
using wasapi::Encoding;
using wasapi::Mode;

// The PCM and IEEE float sub-formats of WAVEFORMATEXTENSIBLE (ksmedia.h's KSDATAFORMAT_SUBTYPE_PCM and _IEEE_FLOAT),
// written out so nothing links ksuser for two GUIDs.
constexpr GUID kSubtypePcm   = {0x00000001, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}}; ///< PCM
constexpr GUID kSubtypeFloat = {
    0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}}; ///< float

/** An endpoint's friendly name (functiondiscoverykeys_devpkey.h's PKEY_Device_FriendlyName), written out likewise. */
constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
                                       14};

/** How long the stream's thread waits for an endpoint's event before it takes the device to have stopped. */
constexpr DWORD kEventTimeoutMs = 2000;

/** COM on the calling thread for the object's life, in the apartment the thread has or a single-threaded one where it
    has none: a thread whose apartment is already chosen (RPC_E_CHANGED_MODE) uses it, and only a start that took is
    undone. */
class ComThread
{
  public:
    /** COM started on this thread, in `apartment` where the thread has none yet.
        @param apartment  COINIT_APARTMENTTHREADED or COINIT_MULTITHREADED */
    explicit ComThread(DWORD apartment) noexcept : started_(SUCCEEDED(CoInitializeEx(nullptr, apartment)))
    {
    }

    /** The start undone, where this object made one. */
    ~ComThread()
    {
        if (started_)
        {
            CoUninitialize();
        }
    }

    ComThread(const ComThread &)            = delete;
    ComThread &operator=(const ComThread &) = delete;

  private:
    bool started_; ///< true when this object's CoInitializeEx took and owes a CoUninitialize
};

/** UTF-16 as UTF-8.
    @param wide  the text, null-terminated; null for none
    @return      the UTF-8 text */
std::string Utf8(const wchar_t *wide)
{
    if (wide == nullptr || *wide == L'\0')
    {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
    {
        return {};
    }
    std::string text(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), bytes, nullptr, nullptr);
    return text;
}

/** What an HRESULT a WASAPI call returned means, in words a person can act on, with its code.
    @param hr  the result
    @return    the words and the code */
std::string Describe(HRESULT hr)
{
    char code[16];
    std::snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(hr));
    std::string words;
    switch (hr)
    {
    case AUDCLNT_E_DEVICE_INVALIDATED:
        words = "the device was unplugged or disabled";
        break;
    case AUDCLNT_E_DEVICE_IN_USE:
        words = "another program has the device in exclusive mode";
        break;
    case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
        words = "the device does not allow exclusive mode (Sound settings, the device's Properties, Advanced: allow "
                "applications to take exclusive control)";
        break;
    case AUDCLNT_E_UNSUPPORTED_FORMAT:
        words = "the device does not take that format";
        break;
    case AUDCLNT_E_SERVICE_NOT_RUNNING:
        words = "the Windows Audio service is not running";
        break;
    case AUDCLNT_E_CPUUSAGE_EXCEEDED:
        words = "the audio engine is overloaded";
        break;
    case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
        words = "Windows could not open the endpoint";
        break;
    case AUDCLNT_E_INVALID_DEVICE_PERIOD:
        words = "the device does not take that period";
        break;
    case AUDCLNT_E_BUFFER_SIZE_ERROR:
        words = "the device does not take that buffer size";
        break;
    case E_ACCESSDENIED:
        words = "Windows refused access (Privacy settings may deny this app the microphone)";
        break;
    case E_NOINTERFACE:
        words = "this Windows or this driver does not offer it";
        break;
    default:
        words = "WASAPI failed";
        break;
    }
    return words + " (" + code + ")";
}

/** A format's channel mask for `channels` where the mix format gives none: mono front centre, stereo front left and
    right, and none (the driver's own order) for more.
    @param channels  the channel count
    @return          the mask */
DWORD DefaultMask(int channels) noexcept
{
    return channels == 1 ? SPEAKER_FRONT_CENTER : (channels == 2 ? (SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT) : 0);
}

/** A device format: WAVEFORMATEXTENSIBLE at a rate, a channel count and mask, in an encoding.
    @param rate      the rate, in hertz
    @param channels  the channels
    @param mask      the channel mask
    @param encoding  the samples' encoding
    @return          the format */
WAVEFORMATEXTENSIBLE MakeFormat(int rate, int channels, DWORD mask, Encoding encoding) noexcept
{
    WAVEFORMATEXTENSIBLE format{};
    const int            bytes         = wasapi::BytesOf(encoding);
    format.Format.wFormatTag           = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels            = static_cast<WORD>(channels);
    format.Format.nSamplesPerSec       = static_cast<DWORD>(rate);
    format.Format.wBitsPerSample       = static_cast<WORD>(bytes * 8);
    format.Format.nBlockAlign          = static_cast<WORD>(channels * bytes);
    format.Format.nAvgBytesPerSec      = static_cast<DWORD>(rate) * format.Format.nBlockAlign;
    format.Format.cbSize               = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = static_cast<WORD>(wasapi::ValidBitsOf(encoding));
    format.dwChannelMask               = mask;
    format.SubFormat                   = encoding == Encoding::kFloat32 ? kSubtypeFloat : kSubtypePcm;
    return format;
}

/** The encoding of a format WASAPI handed over (a mix format), or none for one the back end does not convert.
    @param format  the format
    @return        its encoding, or none */
std::optional<Encoding> EncodingOf(const WAVEFORMATEX &format) noexcept
{
    bool is_float = format.wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
    bool is_pcm   = format.wFormatTag == WAVE_FORMAT_PCM;
    int  valid    = format.wBitsPerSample;
    if (format.wFormatTag == WAVE_FORMAT_EXTENSIBLE && format.cbSize >= 22)
    {
        const auto &ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(format);
        is_float        = IsEqualGUID(ext.SubFormat, kSubtypeFloat) != 0;
        is_pcm          = IsEqualGUID(ext.SubFormat, kSubtypePcm) != 0;
        valid           = ext.Samples.wValidBitsPerSample != 0 ? ext.Samples.wValidBitsPerSample : valid;
    }
    if (is_float && format.wBitsPerSample == 32)
    {
        return Encoding::kFloat32;
    }
    if (is_pcm)
    {
        switch (format.wBitsPerSample)
        {
        case 32:
            return valid == 24 ? Encoding::kInt24In32 : Encoding::kInt32;
        case 24:
            return Encoding::kInt24;
        case 16:
            return Encoding::kInt16;
        default:
            break;
        }
    }
    return std::nullopt;
}

/** One active endpoint as the system lists it. */
struct Endpoint
{
    std::wstring              id;                ///< the endpoint's id, which MMDevice opens it by
    std::string               name;              ///< its friendly name, unique among the endpoints the same way
    bool                      render{true};      ///< true for an output (render) endpoint, false for an input (capture)
    int                       channels{0};       ///< its mix format's channels
    int                       mix_rate{0};       ///< its mix format's rate, in hertz
    DWORD                     mask{0};           ///< its mix format's channel mask, or the default for its channels
    bool                      is_default{false}; ///< true for the system's default endpoint its way
    std::vector<std::uint8_t> mix;               ///< its mix format, whole (a WAVEFORMATEX and its extension)

    /** The mix format.
        @return  the format, valid while the endpoint lives */
    const WAVEFORMATEX *MixFormat() const noexcept
    {
        return mix.empty() ? nullptr : reinterpret_cast<const WAVEFORMATEX *>(mix.data());
    }
};

/** One side of a stream: an endpoint's client, opened. */
struct Side
{
    ComPtr<IAudioClient>        client;         ///< the endpoint's audio client
    ComPtr<IAudioRenderClient>  render;         ///< the render service, on the output side
    ComPtr<IAudioCaptureClient> capture;        ///< the capture service, on the input side
    HANDLE                      event{nullptr}; ///< the endpoint's period event; null where none is used (loopback)
    Encoding                    encoding{Encoding::kFloat32}; ///< the samples' encoding in the endpoint's buffer
    int                         channels{0};                  ///< the device frame's channels
    int                         buffer_frames{0};             ///< the endpoint buffer's frames (GetBufferSize)
    int                         period_frames{0};             ///< the frames a callback carries on this side
    int                         latency_frames{0};            ///< the stream latency WASAPI reports, in frames

    /** The event closed and the client let go. */
    void Reset() noexcept
    {
        render.Reset();
        capture.Reset();
        client.Reset();
        if (event != nullptr)
        {
            CloseHandle(event);
            event = nullptr;
        }
    }
};

/** Windows Audio in one mode: its endpoints and its one stream (WasapiDevice.h). */
class WasapiSystem final : public AudioSystem
{
  public:
    /** The system in `mode`, COM started on the constructing thread - the message thread - for its life.
        @param mode  the mode */
    explicit WasapiSystem(Mode mode) : mode_(mode), com_(COINIT_APARTMENTTHREADED)
    {
    }

    /** The stream closed (Close). */
    ~WasapiSystem() override
    {
        Close();
    }

    std::string Name() const override
    {
        return std::string(wasapi::NameOf(mode_));
    }

    std::vector<DeviceInfo> Devices() override
    {
        std::vector<DeviceInfo> devices;
        for (const Endpoint &endpoint : Enumerate())
        {
            DeviceInfo device;
            device.name                  = endpoint.name;
            device.input_channels        = endpoint.render ? 0 : endpoint.channels;
            device.output_channels       = endpoint.render ? endpoint.channels : 0;
            device.is_default_input      = !endpoint.render && endpoint.is_default;
            device.is_default_output     = endpoint.render && endpoint.is_default;
            device.current_sample_rate   = endpoint.mix_rate;
            device.preferred_sample_rate = endpoint.mix_rate;
            device.sample_rates          = RatesOf(endpoint);
            devices.push_back(std::move(device));
        }
        return devices;
    }

    bool Open(const StreamRequest &request, AudioCallback &callback) override
    {
        Close();
        SetError({});
        const std::vector<Endpoint> endpoints = Enumerate();
        const Endpoint             *output    = Find(endpoints, request.output_device, true);
        if (output == nullptr || request.output_count <= 0)
        {
            return Refuse("no output device named '" + request.output_device + "' on " + Name());
        }
        if (request.first_output < 0 || request.first_output + request.output_count > output->channels)
        {
            return Refuse("'" + output->name + "' has " + std::to_string(output->channels) +
                          " output channels, and the stream asks for channels " +
                          std::to_string(request.first_output + 1) + " to " +
                          std::to_string(request.first_output + request.output_count));
        }
        const bool      with_input = !request.input_device.empty() && request.input_count > 0;
        const Endpoint *input      = nullptr;
        bool            loopback   = false;
        if (with_input)
        {
            input = Find(endpoints, request.input_device, false);
            if (input == nullptr && mode_ != Mode::kExclusive)
            {
                input    = Find(endpoints, request.input_device, true); // a render endpoint's loopback
                loopback = input != nullptr;
            }
            if (input == nullptr)
            {
                return Refuse("no input device named '" + request.input_device + "' on " + Name());
            }
            if (request.first_input < 0 || request.first_input + request.input_count > input->channels)
            {
                return Refuse("'" + input->name + "' has " + std::to_string(input->channels) +
                              " input channels, and the stream asks for channels " +
                              std::to_string(request.first_input + 1) + " to " +
                              std::to_string(request.first_input + request.input_count));
            }
        }

        // The rate: the one asked for, the mix rate where none is. At the endpoint's own rate a shared stream opens at
        // the driver's small periods, and at another at the engine's, converting (wasapi::SharedRouteOf).
        rate_ = request.sample_rate > 0 ? request.sample_rate : output->mix_rate;

        std::string why;
        if (!OpenRender(*output, request.block_size, why))
        {
            Close();
            return Refuse("'" + output->name + "' would not open on " + Name() + ": " + why);
        }
        if (input != nullptr && !OpenCapture(*input, loopback, why))
        {
            Close();
            return Refuse("'" + input->name + "' would not open on " + Name() + ": " + why);
        }

        first_input_   = request.first_input;
        input_count_   = with_input ? request.input_count : 0;
        first_output_  = request.first_output;
        output_count_  = request.output_count;
        const int most = std::max(out_.buffer_frames, out_.period_frames);
        input_storage_.assign(static_cast<std::size_t>(std::max(input_count_, 1)) * static_cast<std::size_t>(most),
                              0.0f);
        output_storage_.assign(static_cast<std::size_t>(output_count_) * static_cast<std::size_t>(most), 0.0f);
        inputs_.assign(static_cast<std::size_t>(std::max(input_count_, 1)), nullptr);
        outputs_.assign(static_cast<std::size_t>(output_count_), nullptr); // pointed into the storage each callback
        if (in_.client != nullptr)
        {
            // A render callback's frames and a capture period's gathered before the callback reads them, so the two
            // endpoints' periods may fall anywhere against each other.
            const int frames = 4 * std::max({in_.buffer_frames, out_.buffer_frames, out_.period_frames});
            ring_.Prepare(in_.channels, frames, out_.period_frames + in_.period_frames);
            packet_.assign(static_cast<std::size_t>(std::max(in_.buffer_frames, 1)) * in_.channels * 2, 0.0f);
        }

        late_after_             = static_cast<std::int64_t>(out_.period_frames) * 2'500'000'000LL / std::max(rate_, 1);
        format_.sample_rate     = rate_;
        format_.block_size      = out_.period_frames;
        format_.input_channels  = input_count_;
        format_.output_channels = output_count_;
        // The latency beyond the block: each endpoint's, and on the way in the margin the ring keeps, a capture period.
        format_.reported_latency =
            out_.latency_frames + (in_.client != nullptr ? in_.latency_frames + in_.period_frames : 0);
        callback_.store(&callback, std::memory_order_release);
        failed_.store(false, std::memory_order_relaxed);
        open_ = true;
        return true;
    }

    bool Start() override
    {
        if (!open_)
        {
            return false;
        }
        if (thread_.joinable())
        {
            return true;
        }
        stop_.store(false, std::memory_order_relaxed);
        running_.store(true, std::memory_order_release);
        std::promise<std::string> started;
        std::future<std::string>  answer = started.get_future();
        thread_                          = std::thread([this, &started] { Run(started); });
        const std::string why            = answer.get();
        if (!why.empty())
        {
            thread_.join();
            running_.store(false, std::memory_order_release);
            SetError("the stream would not start on " + Name() + ": " + why);
            return false;
        }
        return true;
    }

    void Stop() override
    {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable())
        {
            stop_.store(true, std::memory_order_release);
            if (out_.event != nullptr)
            {
                SetEvent(out_.event);
            }
            thread_.join();
        }
        for (Side *side : {&in_, &out_})
        {
            if (side->client != nullptr)
            {
                side->client->Stop();
                side->client->Reset();
            }
        }
    }

    void Close() override
    {
        Stop();
        in_.Reset();
        out_.Reset();
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
    // ---- The endpoints
    // -----------------------------------------------------------------------------------------------

    /** The active endpoints, render and capture, each under a name no other endpoint the same way has: its friendly
        name, and a later one of an earlier one's name that name with its ordinal after it.
        @return  the endpoints; empty where MMDevice answers nothing */
    std::vector<Endpoint> Enumerate()
    {
        std::vector<Endpoint>       endpoints;
        ComPtr<IMMDeviceEnumerator> enumerator;
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                    IID_PPV_ARGS(enumerator.GetAddressOf()))))
        {
            return endpoints;
        }
        const std::wstring default_render  = DefaultId(*enumerator.Get(), eRender);
        const std::wstring default_capture = DefaultId(*enumerator.Get(), eCapture);
        for (const EDataFlow flow : {eRender, eCapture})
        {
            ComPtr<IMMDeviceCollection> collection;
            UINT                        count = 0;
            if (FAILED(enumerator->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, collection.GetAddressOf())) ||
                FAILED(collection->GetCount(&count)))
            {
                continue;
            }
            for (UINT i = 0; i < count; ++i)
            {
                ComPtr<IMMDevice> device;
                if (FAILED(collection->Item(i, device.GetAddressOf())))
                {
                    continue;
                }
                std::optional<Endpoint> endpoint = EndpointOf(*device.Get(), flow == eRender);
                if (!endpoint)
                {
                    continue;
                }
                endpoint->is_default   = endpoint->id == (flow == eRender ? default_render : default_capture);
                const std::string base = endpoint->name;
                for (int ordinal = 2; Taken(endpoints, endpoint->name, endpoint->render); ++ordinal)
                {
                    endpoint->name = base + " (" + std::to_string(ordinal) + ")";
                }
                endpoints.push_back(std::move(*endpoint));
            }
        }
        return endpoints;
    }

    /** The system's default endpoint's id one way, or nothing.
        @param enumerator  MMDevice's enumerator
        @param flow        eRender or eCapture
        @return            the id, empty where there is no default */
    static std::wstring DefaultId(IMMDeviceEnumerator &enumerator, EDataFlow flow)
    {
        ComPtr<IMMDevice> device;
        LPWSTR            id = nullptr;
        std::wstring      text;
        if (SUCCEEDED(enumerator.GetDefaultAudioEndpoint(flow, eConsole, device.GetAddressOf())) &&
            SUCCEEDED(device->GetId(&id)))
        {
            text = id;
            CoTaskMemFree(id);
        }
        return text;
    }

    /** An endpoint as the system lists it: its id, friendly name and mix format; nothing for one that cannot be asked.
        @param device  the endpoint
        @param render  true for a render endpoint
        @return        the endpoint, or nothing */
    static std::optional<Endpoint> EndpointOf(IMMDevice &device, bool render)
    {
        Endpoint endpoint;
        endpoint.render = render;
        LPWSTR id       = nullptr;
        if (FAILED(device.GetId(&id)))
        {
            return std::nullopt;
        }
        endpoint.id = id;
        CoTaskMemFree(id);

        ComPtr<IPropertyStore> properties;
        if (SUCCEEDED(device.OpenPropertyStore(STGM_READ, properties.GetAddressOf())))
        {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(properties->GetValue(kFriendlyName, &name)) && name.vt == VT_LPWSTR)
            {
                endpoint.name = Utf8(name.pwszVal);
            }
            PropVariantClear(&name);
        }
        if (endpoint.name.empty())
        {
            return std::nullopt;
        }

        ComPtr<IAudioClient> client;
        WAVEFORMATEX        *mix = nullptr;
        if (FAILED(device.Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                                   reinterpret_cast<void **>(client.GetAddressOf()))) ||
            FAILED(client->GetMixFormat(&mix)) || mix == nullptr)
        {
            return std::nullopt;
        }
        const auto *bytes = reinterpret_cast<const std::uint8_t *>(mix);
        endpoint.mix.assign(bytes, bytes + sizeof(WAVEFORMATEX) + mix->cbSize);
        endpoint.channels = mix->nChannels;
        endpoint.mix_rate = static_cast<int>(mix->nSamplesPerSec);
        endpoint.mask     = DefaultMask(endpoint.channels);
        if (mix->wFormatTag == WAVE_FORMAT_EXTENSIBLE && mix->cbSize >= 22)
        {
            endpoint.mask = reinterpret_cast<const WAVEFORMATEXTENSIBLE *>(mix)->dwChannelMask;
        }
        CoTaskMemFree(mix);
        return endpoint;
    }

    /** Whether an endpoint the way `render` says already carries `name`.
        @param endpoints  the endpoints listed so far
        @param name       the name
        @param render     the way
        @return           true when one does */
    static bool Taken(const std::vector<Endpoint> &endpoints, const std::string &name, bool render)
    {
        return std::any_of(endpoints.begin(), endpoints.end(),
                           [&](const Endpoint &e) { return e.name == name && e.render == render; });
    }

    /** The endpoint named `name` the way `render` says.
        @param endpoints  the endpoints
        @param name       the name, as Devices listed it
        @param render     the way
        @return           the endpoint, or null */
    static const Endpoint *Find(const std::vector<Endpoint> &endpoints, const std::string &name, bool render)
    {
        for (const Endpoint &endpoint : endpoints)
        {
            if (endpoint.name == name && endpoint.render == render)
            {
                return &endpoint;
            }
        }
        return nullptr;
    }

    /** The rates an endpoint opens at in this mode: the mix rate and the rates interfaces run at in shared mode, which
        the engine converts (at the mix rate the stream opens at the driver's small periods); and in exclusive mode the
        rates the device says it takes in any encoding, asked once an endpoint and kept - none kept where it answered
        none, so it is asked again, and a device that lists none takes any rate it is asked for and says why not at the
        open.
        @param endpoint  the endpoint
        @return          the rates, in hertz, ascending */
    std::vector<int> RatesOf(const Endpoint &endpoint)
    {
        std::vector<int> rates;
        if (mode_ == Mode::kShared)
        {
            rates.assign(std::begin(wasapi::kRates), std::end(wasapi::kRates));
            if (std::find(rates.begin(), rates.end(), endpoint.mix_rate) == rates.end() && endpoint.mix_rate > 0)
            {
                rates.insert(std::upper_bound(rates.begin(), rates.end(), endpoint.mix_rate), endpoint.mix_rate);
            }
            return rates;
        }
        if (const auto kept = exclusive_rates_.find(endpoint.id); kept != exclusive_rates_.end())
        {
            return kept->second;
        }
        ComPtr<IAudioClient> client = Activate(endpoint);
        if (client != nullptr)
        {
            for (const int rate : wasapi::kRates)
            {
                for (const Encoding encoding : wasapi::kEncodings)
                {
                    WAVEFORMATEXTENSIBLE format = MakeFormat(rate, endpoint.channels, endpoint.mask, encoding);
                    if (client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr) == S_OK)
                    {
                        rates.push_back(rate);
                        break;
                    }
                }
            }
        }
        if (!rates.empty())
        {
            exclusive_rates_[endpoint.id] = rates;
        }
        return rates;
    }

    /** An endpoint's audio client, activated afresh; null where it cannot be.
        @param endpoint  the endpoint
        @return          the client, or null */
    static ComPtr<IAudioClient> Activate(const Endpoint &endpoint)
    {
        ComPtr<IMMDeviceEnumerator> enumerator;
        ComPtr<IMMDevice>           device;
        ComPtr<IAudioClient>        client;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL,
                                       IID_PPV_ARGS(enumerator.GetAddressOf()))) &&
            SUCCEEDED(enumerator->GetDevice(endpoint.id.c_str(), device.GetAddressOf())))
        {
            device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr,
                             reinterpret_cast<void **>(client.GetAddressOf()));
        }
        return client;
    }

    // ---- Opening
    // -----------------------------------------------------------------------------------------------------

    /** The output side opened in the mode, at rate_, its period set from the block asked for.
        @param endpoint  the render endpoint
        @param block     the block asked for, in frames; 0 for the mode's default
        @param why       set to what went wrong
        @return          true when it opened */
    bool OpenRender(const Endpoint &endpoint, int block, std::string &why)
    {
        Side &side  = out_;
        side.client = Activate(endpoint);
        if (side.client == nullptr)
        {
            why = "it could not be activated";
            return false;
        }
        side.channels = endpoint.channels;
        HRESULT hr    = S_OK;
        if (mode_ == Mode::kExclusive)
        {
            if (!OpenExclusive(endpoint, block, side, why))
            {
                return false;
            }
        }
        else if (!OpenSharedAtTheBlock(endpoint, block, side, why))
        {
            return false;
        }
        hr = side.client->GetService(IID_PPV_ARGS(side.render.GetAddressOf()));
        if (SUCCEEDED(hr))
        {
            hr = Events(side);
        }
        if (FAILED(hr))
        {
            why = Describe(hr);
            return false;
        }
        Measure(side);
        side.period_frames = std::min(side.period_frames, side.buffer_frames);
        return true;
    }

    /** The input side opened in the mode at rate_: a capture endpoint, or a render endpoint's loopback.
        @param endpoint  the endpoint
        @param loopback  true to capture what a render endpoint plays
        @param why       set to what went wrong
        @return          true when it opened */
    bool OpenCapture(const Endpoint &endpoint, bool loopback, std::string &why)
    {
        Side &side  = in_;
        side.client = Activate(endpoint);
        if (side.client == nullptr)
        {
            why = "it could not be activated";
            return false;
        }
        side.channels = endpoint.channels;
        HRESULT hr    = S_OK;
        if (loopback)
        {
            hr = OpenShared(endpoint, side, AUDCLNT_STREAMFLAGS_LOOPBACK); // polled: a loopback raises no event
        }
        else if (mode_ == Mode::kExclusive)
        {
            if (!OpenExclusive(endpoint, out_.period_frames, side, why))
            {
                return false;
            }
        }
        else if (!OpenSharedAtTheBlock(endpoint, out_.period_frames, side, why))
        {
            return false;
        }
        if (SUCCEEDED(hr))
        {
            hr = side.client->GetService(IID_PPV_ARGS(side.capture.GetAddressOf()));
        }
        if (SUCCEEDED(hr) && !loopback)
        {
            hr = Events(side);
        }
        if (FAILED(hr))
        {
            why = Describe(hr);
            return false;
        }
        Measure(side);
        return true;
    }

    /** A side opened in shared mode, its period set: at the driver's smallest period that holds `block` where the
        stream runs at the endpoint's own rate and this Windows and this driver offer it (OpenLowLatency) - a few
        milliseconds where the driver offers small periods, as Microsoft's inbox HD Audio driver does - and else at the
        engine's period, the engine converting the rate (OpenShared): at another rate, on a Windows before 10, or where
        the driver refuses the small period, on a fresh client, since a refused initialise may leave one unusable.
        @param endpoint  the endpoint
        @param block     the block asked for, in frames; 0 for the driver's smallest
        @param side      the side, its client activated
        @param why       set to what went wrong
        @return          true when it opened */
    bool OpenSharedAtTheBlock(const Endpoint &endpoint, int block, Side &side, std::string &why)
    {
        if (wasapi::SharedRouteOf(rate_, endpoint.mix_rate) == wasapi::SharedRoute::kDriversPeriod)
        {
            if (OpenLowLatency(endpoint, block, side, why))
            {
                return true;
            }
            side.client = Activate(endpoint);
            if (side.client == nullptr)
            {
                return false; // `why` says why the driver's period was refused; the engine's cannot be tried
            }
            why.clear();
        }
        const HRESULT hr = OpenShared(endpoint, side, AUDCLNT_STREAMFLAGS_EVENTCALLBACK);
        if (FAILED(hr))
        {
            why = Describe(hr);
            return false;
        }
        if (&side == &out_)
        {
            // The render side's period is the engine's; a capture side's is left to Measure, its buffer, as it was.
            REFERENCE_TIME standard = 0;
            REFERENCE_TIME smallest = 0;
            side.client->GetDevicePeriod(&standard, &smallest);
            side.period_frames = std::max(wasapi::FramesOf(standard, rate_), 1);
        }
        return true;
    }

    /** A side initialised in shared mode at rate_ in float, the engine converting where the endpoint runs at another
        rate.
        @param endpoint  the endpoint
        @param side      the side, its client activated
        @param flags     the stream flags: the event, or the loopback
        @return          the result */
    HRESULT OpenShared(const Endpoint &endpoint, Side &side, DWORD flags)
    {
        if (rate_ != endpoint.mix_rate)
        {
            flags |= AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        }
        WAVEFORMATEXTENSIBLE format = MakeFormat(rate_, endpoint.channels, endpoint.mask, Encoding::kFloat32);
        side.encoding               = Encoding::kFloat32;
        // A loopback, polled, keeps a buffer of its own a period's worth longer than the engine's.
        const REFERENCE_TIME buffer = (flags & AUDCLNT_STREAMFLAGS_LOOPBACK) != 0 ? 400000 : 0;
        return side.client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, buffer, 0, &format.Format, nullptr);
    }

    /** Raw mode asked of `client`, where this Windows and this driver offer it: the stream then runs with none of
        the engine's own effects in its path. Worth asking for at the driver's small periods, since the effects chain
       both adds latency of its own and holds the engine's period up, and nothing in a guitar amplifier wants Windows to
        process its sound. It must be asked before the stream is initialised, which is the only time the property may
        be set, and a refusal is no reason not to open: the stream simply runs with the effects, as it did before.
        @param client  the client, not yet initialised */
    static void AskForRawMode(ComPtr<IAudioClient> &client) noexcept
    {
        ComPtr<IAudioClient2> client2;
        if (FAILED(client.As(&client2)))
        {
            return; // before Windows 8.1: there is no raw mode to ask for
        }
        AudioClientProperties properties{};
        properties.cbSize     = sizeof(properties);
        properties.bIsOffload = FALSE;
        properties.eCategory  = AudioCategory_Other; // as it is without this call, so only the option below changes
        properties.Options    = AUDCLNT_STREAMOPTIONS_RAW;
        client2->SetClientProperties(&properties);
    }

    /** `side` opened at whatever period and format the engine is already running, on a client of its own. Where
        another program holds the engine, a stream that asks for a period or a format of its own is refused outright
        rather than mixed in, and this is the way to open anyway: a block the person did not choose is better than no
        sound, and Format() reports what they were given rather than what they asked for. A failed initialise may
        leave a client unusable, so this activates a fresh one rather than asking the refused one again.
        @param endpoint  the render endpoint
        @param side      the side, its client replaced
        @param why       set to what went wrong
        @return          true when it opened */
    static bool OpenAtTheEnginesPeriod(const Endpoint &endpoint, Side &side, std::string &why)
    {
        side.client = Activate(endpoint);
        ComPtr<IAudioClient3> client3;
        if (side.client == nullptr || FAILED(side.client.As(&client3)))
        {
            why = "the engine is held by another program and this stream could not join it";
            return false;
        }
        WAVEFORMATEX *engine_format = nullptr;
        UINT32        engine_period = 0;
        HRESULT       hr            = client3->GetCurrentSharedModeEnginePeriod(&engine_format, &engine_period);
        if (FAILED(hr) || engine_format == nullptr || engine_period == 0)
        {
            why = Describe(FAILED(hr) ? hr : E_FAIL);
            if (engine_format != nullptr)
            {
                CoTaskMemFree(engine_format);
            }
            return false;
        }
        // The engine's own format, not the endpoint's mix: where it is the format that is held, they differ, and what
        // this stream writes has to be what the engine is running.
        const std::optional<Encoding> encoding = EncodingOf(*engine_format);
        if (encoding)
        {
            side.encoding = *encoding;
            hr = client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, engine_period, engine_format,
                                                      nullptr);
        }
        else
        {
            why = "the engine runs in a format this back end does not convert";
            hr  = E_FAIL;
        }
        CoTaskMemFree(engine_format);
        if (FAILED(hr))
        {
            if (why.empty())
            {
                why = Describe(hr);
            }
            return false;
        }
        side.period_frames = static_cast<int>(engine_period);
        return true;
    }

    /** A side initialised through IAudioClient3 at the driver's period that holds `block`, in the endpoint's mix
        format.
        @param endpoint  the endpoint
        @param block     the block asked for, in frames
        @param side      the side, its client activated
        @param why       set to what went wrong
        @return          true when it initialised */
    static bool OpenLowLatency(const Endpoint &endpoint, int block, Side &side, std::string &why)
    {
        ComPtr<IAudioClient3> client3;
        if (FAILED(side.client.As(&client3)))
        {
            why = "this Windows offers no small shared periods (IAudioClient3, Windows 10 and later)";
            return false;
        }
        const WAVEFORMATEX           *mix      = endpoint.MixFormat();
        const std::optional<Encoding> encoding = mix != nullptr ? EncodingOf(*mix) : std::nullopt;
        if (!encoding)
        {
            why = "its mix format is one this back end does not convert";
            return false;
        }
        side.encoding    = *encoding;
        UINT32  standard = 0;
        UINT32  step     = 0;
        UINT32  smallest = 0;
        UINT32  largest  = 0;
        HRESULT hr       = client3->GetSharedModeEnginePeriod(mix, &standard, &step, &smallest, &largest);
        if (FAILED(hr))
        {
            why = Describe(hr);
            return false;
        }
        AskForRawMode(side.client);
        const int period = wasapi::LowLatencyPeriod(block, static_cast<int>(step), static_cast<int>(smallest),
                                                    static_cast<int>(largest));
        hr = client3->InitializeSharedAudioStream(AUDCLNT_STREAMFLAGS_EVENTCALLBACK, static_cast<UINT32>(period), mix,
                                                  nullptr);
        if (hr == AUDCLNT_E_ENGINE_PERIODICITY_LOCKED || hr == AUDCLNT_E_ENGINE_FORMAT_LOCKED)
        {
            // Another program holds the engine at a period, or a format, of its own. Open at the engine's instead of
            // failing: the person gets sound at a block they did not choose, and Format() tells them which.
            return OpenAtTheEnginesPeriod(endpoint, side, why);
        }
        if (FAILED(hr))
        {
            why = Describe(hr);
            return false;
        }
        // Another program may hold the engine at a period of its own, which every stream on it then runs at.
        WAVEFORMATEX *current_format = nullptr;
        UINT32        current        = 0;
        side.period_frames           = period;
        if (SUCCEEDED(client3->GetCurrentSharedModeEnginePeriod(&current_format, &current)) && current > 0)
        {
            side.period_frames = static_cast<int>(current);
        }
        if (current_format != nullptr)
        {
            CoTaskMemFree(current_format);
        }
        return true;
    }

    /** A side initialised in exclusive mode at rate_, in the best encoding the device takes, at the period that holds
        `block` - the device's minimum or more - aligned as the device asks.
        @param endpoint  the endpoint
        @param block     the block asked for, in frames; 0 for the device's default period
        @param side      the side, its client activated
        @param why       set to what went wrong
        @return          true when it initialised */
    bool OpenExclusive(const Endpoint &endpoint, int block, Side &side, std::string &why)
    {
        std::optional<WAVEFORMATEXTENSIBLE> chosen;
        HRESULT                             refusal = AUDCLNT_E_UNSUPPORTED_FORMAT;
        for (const Encoding encoding : wasapi::kEncodings)
        {
            WAVEFORMATEXTENSIBLE format = MakeFormat(rate_, endpoint.channels, endpoint.mask, encoding);
            const HRESULT hr = side.client->IsFormatSupported(AUDCLNT_SHAREMODE_EXCLUSIVE, &format.Format, nullptr);
            if (hr == S_OK)
            {
                chosen        = format;
                side.encoding = encoding;
                break;
            }
            refusal = hr == S_FALSE ? AUDCLNT_E_UNSUPPORTED_FORMAT : hr;
        }
        if (!chosen)
        {
            why = Describe(refusal) + ", at " + std::to_string(rate_) + " Hz and " + std::to_string(endpoint.channels) +
                  " channels in any encoding";
            return false;
        }
        REFERENCE_TIME standard = 0;
        REFERENCE_TIME smallest = 0;
        side.client->GetDevicePeriod(&standard, &smallest);
        const int      smallest_frames = (static_cast<int>((smallest * rate_ + 9'999'999) / 10'000'000));
        const int      period   = wasapi::ExclusivePeriod(block, smallest_frames, wasapi::FramesOf(standard, rate_));
        REFERENCE_TIME duration = std::max<REFERENCE_TIME>(wasapi::HundredsOfNanoseconds(period, rate_), smallest);
        HRESULT hr = side.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration,
                                             duration, &chosen->Format, nullptr);
        if (hr == AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED)
        {
            // The device rounds the period to an alignment of its own: the buffer it would give says the period it
            // takes, and a fresh client is initialised at that.
            UINT32 aligned = 0;
            side.client->GetBufferSize(&aligned);
            duration    = wasapi::HundredsOfNanoseconds(static_cast<int>(aligned), rate_);
            side.client = Activate(endpoint);
            hr = side.client != nullptr
                     ? side.client->Initialize(AUDCLNT_SHAREMODE_EXCLUSIVE, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, duration,
                                               duration, &chosen->Format, nullptr)
                     : AUDCLNT_E_ENDPOINT_CREATE_FAILED;
        }
        if (FAILED(hr))
        {
            why = Describe(hr);
            return false;
        }
        UINT32 frames = 0;
        side.client->GetBufferSize(&frames);
        side.period_frames = static_cast<int>(frames); // an exclusive event-driven buffer is one period
        return true;
    }

    /** A side's period event made and handed to its client.
        @param side  the side, initialised event-driven
        @return      the result */
    static HRESULT Events(Side &side)
    {
        side.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (side.event == nullptr)
        {
            return HRESULT_FROM_WIN32(GetLastError());
        }
        return side.client->SetEventHandle(side.event);
    }

    /** A side's buffer and latency read once it is initialised.
        @param side  the side */
    void Measure(Side &side) const
    {
        UINT32 frames = 0;
        side.client->GetBufferSize(&frames);
        side.buffer_frames     = static_cast<int>(frames);
        REFERENCE_TIME latency = 0;
        side.latency_frames = SUCCEEDED(side.client->GetStreamLatency(&latency)) ? wasapi::FramesOf(latency, rate_) : 0;
        if (side.period_frames <= 0)
        {
            side.period_frames = side.buffer_frames;
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

    // ---- The stream's thread
    // -----------------------------------------------------------------------------------------

    /** The stream's thread: COM and the scheduler's Pro Audio class, the render buffer primed with silence, both sides
        started - the answer handed back to Start - then the wait on the endpoints' events until Stop or a failure.
        @param started  where Start waits for the start's answer: empty, or why the stream did not start */
    void Run(std::promise<std::string> &started)
    {
        const ComThread com(COINIT_MULTITHREADED);
        DWORD           task      = 0;
        HANDLE          scheduler = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
        if (scheduler == nullptr)
        {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        }
        ring_.Clear();
        last_play_    = 0;
        pending_xrun_ = false;

        HRESULT hr = Prime();
        if (SUCCEEDED(hr) && in_.client != nullptr)
        {
            hr = in_.client->Start();
        }
        if (SUCCEEDED(hr))
        {
            hr = out_.client->Start();
        }
        started.set_value(FAILED(hr) ? Describe(hr) : std::string());
        if (SUCCEEDED(hr))
        {
            Loop();
        }
        if (scheduler != nullptr)
        {
            AvRevertMmThreadCharacteristics(scheduler);
        }
    }

    /** The render buffer's first period written silent before the start, so the device starts on samples rather than
        on an empty buffer.
        @return  the result */
    HRESULT Prime()
    {
        const int frames = mode_ == Mode::kExclusive ? out_.buffer_frames : out_.period_frames;
        BYTE     *data   = nullptr;
        HRESULT   hr     = out_.render->GetBuffer(static_cast<UINT32>(frames), &data);
        if (SUCCEEDED(hr))
        {
            hr = out_.render->ReleaseBuffer(static_cast<UINT32>(frames), AUDCLNT_BUFFERFLAGS_SILENT);
        }
        return hr;
    }

    /** The wait: on each capture event the packets taken into the ring, on each render event a callback's worth
        played, until Stop wakes it or an endpoint stops calling or a call fails, which is the stream failing. */
    void Loop()
    {
        HANDLE      events[2] = {out_.event, in_.event};
        const DWORD count     = in_.event != nullptr ? 2 : 1;
        while (!stop_.load(std::memory_order_acquire))
        {
            const DWORD woke = WaitForMultipleObjects(count, events, FALSE, kEventTimeoutMs);
            if (stop_.load(std::memory_order_acquire))
            {
                break;
            }
            if (woke == WAIT_TIMEOUT || woke == WAIT_FAILED)
            {
                Fail();
                break;
            }
            if (in_.client != nullptr && FAILED(TakeCapture()))
            {
                Fail();
                break;
            }
            if (woke == WAIT_OBJECT_0 && FAILED(Play()))
            {
                Fail();
                break;
            }
        }
    }

    /** Every capture packet waiting taken into the ring, in float.
        @return  the result */
    HRESULT TakeCapture()
    {
        UINT32  packet = 0;
        HRESULT hr     = in_.capture->GetNextPacketSize(&packet);
        while (SUCCEEDED(hr) && packet > 0)
        {
            BYTE  *data   = nullptr;
            UINT32 frames = 0;
            DWORD  flags  = 0;
            hr            = in_.capture->GetBuffer(&data, &frames, &flags, nullptr, nullptr);
            if (FAILED(hr))
            {
                break;
            }
            if ((flags & AUDCLNT_BUFFERFLAGS_DATA_DISCONTINUITY) != 0 && ring_.Count() > 0)
            {
                pending_xrun_ = true; // a glitch in the capture, once the stream is under way
            }
            const int n =
                std::min(static_cast<int>(frames), static_cast<int>(packet_.size()) / std::max(in_.channels, 1));
            bool kept = true;
            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0 || data == nullptr)
            {
                kept = ring_.Write(nullptr, n);
            }
            else
            {
                const int bytes = wasapi::BytesOf(in_.encoding);
                for (int f = 0; f < n * in_.channels; ++f)
                {
                    packet_[static_cast<std::size_t>(f)] =
                        wasapi::DecodeOne(data + static_cast<std::size_t>(f) * bytes, in_.encoding);
                }
                kept = ring_.Write(packet_.data(), n);
            }
            pending_xrun_ = pending_xrun_ || !kept;
            hr            = in_.capture->ReleaseBuffer(frames);
            if (SUCCEEDED(hr))
            {
                hr = in_.capture->GetNextPacketSize(&packet);
            }
        }
        return hr;
    }

    /** One callback's worth played: in exclusive mode the whole buffer, a period; in the shared modes what the buffer
        has room for, up to a period. The input read from the ring, the callback run - or silence while the stream is
        stopped - and its output written in the device's encoding.
        @return  the result */
    HRESULT Play()
    {
        int  frames = out_.period_frames;
        bool xrun   = pending_xrun_;
        if (mode_ == Mode::kExclusive)
        {
            frames = out_.buffer_frames;
        }
        else
        {
            UINT32        padding = 0;
            const HRESULT hr      = out_.client->GetCurrentPadding(&padding);
            if (FAILED(hr))
            {
                return hr;
            }
            // What fits, up to a period: a buffer only a little longer than its period has room for less than one
            // at some events, and waiting for a whole one would let it run dry.
            const int room = out_.buffer_frames - static_cast<int>(padding);
            frames         = std::min(frames, room);
            if (frames <= 0)
            {
                return S_OK; // full: the next event
            }
        }
        // WASAPI tells a render stream of no underrun; what one is - a callback that overran, a thread held off - is a
        // period event come late, past two and a half periods after the last, which loses the device a period or more.
        const std::int64_t now = NowNanoseconds();
        xrun                   = xrun || (last_play_ != 0 && now - last_play_ > late_after_);
        last_play_             = now;
        pending_xrun_          = false;
        // Each channel's frames where the channel before's end, the layout every API's callback hands the engine.
        for (int c = 0; c < input_count_; ++c)
        {
            inputs_[static_cast<std::size_t>(c)] = input_storage_.data() + static_cast<std::size_t>(c) * frames;
        }
        for (int c = 0; c < output_count_; ++c)
        {
            outputs_[static_cast<std::size_t>(c)] = output_storage_.data() + static_cast<std::size_t>(c) * frames;
        }
        if (in_.client != nullptr && !ring_.Read(inputs_.data(), input_count_, first_input_, frames))
        {
            xrun = true;
        }
        BYTE   *data = nullptr;
        HRESULT hr   = out_.render->GetBuffer(static_cast<UINT32>(frames), &data);
        if (FAILED(hr))
        {
            return hr;
        }
        AudioCallback *run =
            running_.load(std::memory_order_acquire) ? callback_.load(std::memory_order_acquire) : nullptr;
        if (run != nullptr)
        {
            run->Process(input_count_ > 0 ? inputs_.data() : nullptr, input_count_, outputs_.data(), output_count_,
                         frames, NowNanoseconds(), xrun);
            wasapi::Encode(outputs_.data(), output_count_, first_output_, out_.channels, frames, out_.encoding, data);
            hr = out_.render->ReleaseBuffer(static_cast<UINT32>(frames), 0);
        }
        else
        {
            hr = out_.render->ReleaseBuffer(static_cast<UINT32>(frames), AUDCLNT_BUFFERFLAGS_SILENT);
        }
        return hr;
    }

    /** The stream failing, told once. */
    void Fail() noexcept
    {
        if (!failed_.exchange(true, std::memory_order_acq_rel) && running_.load(std::memory_order_acquire))
        {
            if (AudioCallback *run = callback_.load(std::memory_order_acquire))
            {
                run->StreamFailed();
            }
        }
    }

    Mode                                     mode_;            ///< the mode
    ComThread                                com_;             ///< COM on the message thread for the system's life
    std::map<std::wstring, std::vector<int>> exclusive_rates_; ///< the rates each endpoint took in exclusive mode
    int                                      rate_{0};         ///< the stream's rate, in hertz
    Side                                     out_;             ///< the output side
    Side                                     in_;              ///< the input side; its client null with no input
    int                          first_input_{0};  ///< the device input channel the callback's channel 0 reads
    int                          input_count_{0};  ///< the callback's input channels, 0 with no input
    int                          first_output_{0}; ///< the device output channel the callback's channel 0 plays
    int                          output_count_{0}; ///< the callback's output channels
    std::vector<float>           input_storage_;   ///< the callback's input channels' samples, sized at the open
    std::vector<float>           output_storage_;  ///< the callback's output channels' samples, sized at the open
    std::vector<float *>         inputs_;          ///< the input channel pointers the ring fills and the callback reads
    std::vector<float *>         outputs_;         ///< the output channel pointers the callback is handed
    std::vector<float>           packet_;          ///< one capture packet in float, interleaved, sized at the open
    wasapi::CaptureRing          ring_;            ///< the capture side's frames waiting for the render side
    bool                         pending_xrun_{false}; ///< a glitch to report with the next callback
    std::int64_t                 last_play_{0};        ///< the clock at the last period played, 0 before the first
    std::int64_t                 late_after_{0}; ///< two and a half periods, in nanoseconds: a later event is an xrun
    StreamFormat                 format_;        ///< what the stream opened as
    bool                         open_{false};   ///< true from a successful Open until Close
    std::atomic<AudioCallback *> callback_{nullptr}; ///< what runs the stream; null while closed
    std::atomic<bool>            running_{false};    ///< true from Start until Stop; false plays silence
    std::atomic<bool>            stop_{false};       ///< true tells the stream's thread to end
    std::atomic<bool>            failed_{false};     ///< true once the stream's failure has been told
    mutable std::mutex           error_mutex_;       ///< guards `last_error_`
    std::string                  last_error_;        ///< the last error, for a person to read
    std::thread                  thread_;            ///< the stream's thread, from Start until Stop
};
} // namespace

std::unique_ptr<AudioSystem> MakeWasapiSystem(wasapi::Mode mode)
{
    return std::make_unique<WasapiSystem>(mode);
}

namespace
{
/** Windows Audio's two modes, in the order a Standalone lists them. */
constexpr wasapi::Mode kModes[] = {wasapi::Mode::kShared, wasapi::Mode::kExclusive};

/** The two modes' names.
    @return  the names, in the order a Standalone lists them */
std::vector<std::string> ApiNames()
{
    std::vector<std::string> names;
    for (const wasapi::Mode mode : kModes)
    {
        names.emplace_back(wasapi::NameOf(mode));
    }
    return names;
}

/** A system for one of ApiNames().
    @param name  the API's name
    @return      the system, owned by the caller; nullptr for a name that is no WASAPI mode's */
std::unique_ptr<AudioSystem> MakeSystem(const std::string &name)
{
    if (const std::optional<wasapi::Mode> mode = wasapi::ModeNamed(name))
    {
        return MakeWasapiSystem(*mode);
    }
    return nullptr;
}

/** What this back end is. There is no library and so no version: it is Windows' own interface, called directly.
    @return  the text */
std::string Library()
{
    return "Windows Audio (WASAPI)";
}
} // namespace

const AudioBackend &WasapiBackend()
{
    static const AudioBackend backend{&Library, &ApiNames, &MakeSystem};
    return backend;
}
} // namespace reckless::audio
