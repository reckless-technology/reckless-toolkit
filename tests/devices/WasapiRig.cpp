// Windows Audio's test device, in each of its two modes (audio/win/WasapiDevice.h): the machine's default render
// endpoint. In the shared mode it is played through and heard back by WASAPI's loopback capture - the back end opens an
// input named after a render endpoint as the capture of the mix that endpoint plays - so the samples come back through
// the audio engine's mixer, correlated rather than exact; at the endpoint's mix rate the render client is
// IAudioClient3's, opened by InitializeSharedAudioStream at the driver's period that holds the block, a few
// milliseconds where a WaveRT driver offers small periods, against the engine's ten. The rig asks the endpoint the same
// question and prints its periods. In the exclusive mode the stream has the endpoint to itself and there is no mixer to
// hear it by: the device is output only. A machine with no active render endpoint - CI's Windows runners, whose only
// endpoints are microphones - has no test device, and the tests skip with that reason.
#include "DeviceRig.h"

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <objbase.h>
#include <propidl.h>

namespace reckless::audio::test
{
namespace
{
/** An endpoint's friendly name (functiondiscoverykeys_devpkey.h's PKEY_Device_FriendlyName), written out as the back
    end writes it - the header needs DEFINE_PROPERTYKEY, which this translation unit has no other use for. */
constexpr PROPERTYKEY kFriendlyName = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}},
                                       14};

/** Release a COM interface and clear the pointer.
    @tparam T  a COM interface type with `Release()`
    @param p   the interface pointer, nullptr or one owned here; nullptr on return */
template <typename T> void Release(T *&p)
{
    if (p != nullptr)
    {
        p->Release();
        p = nullptr;
    }
}

/** An HRESULT as text.
    @param hr  the result code
    @return    the code as `0x` and eight hexadecimal digits */
std::string Hex(HRESULT hr)
{
    char text[16];
    std::snprintf(text, sizeof text, "0x%08lX", static_cast<unsigned long>(hr));
    return text;
}

/** The render endpoint named `wanted` among the enumerator's active ones, by the friendly name the back end lists it
    under; the default endpoint where `wanted` is empty or no endpoint carries it.
    @param enumerator  the device enumerator
    @param wanted      the endpoint's name, or empty for the default
    @param device      set to the endpoint on success; untouched otherwise
    @return            the call's status */
HRESULT FindRenderEndpoint(IMMDeviceEnumerator *enumerator, const std::string &wanted, IMMDevice **device)
{
    if (wanted.empty())
    {
        return enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device);
    }
    IMMDeviceCollection *collection = nullptr;
    HRESULT              hr         = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &collection);
    if (FAILED(hr))
    {
        return hr;
    }
    UINT32 count = 0;
    collection->GetCount(&count);
    for (UINT32 i = 0; i < count; ++i)
    {
        IMMDevice      *each  = nullptr;
        IPropertyStore *store = nullptr;
        if (FAILED(collection->Item(i, &each)))
        {
            continue;
        }
        if (SUCCEEDED(each->OpenPropertyStore(STGM_READ, &store)))
        {
            PROPVARIANT name;
            PropVariantInit(&name);
            if (SUCCEEDED(store->GetValue(kFriendlyName, &name)) && name.vt == VT_LPWSTR)
            {
                const int   size = WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, nullptr, 0, nullptr, nullptr);
                std::string text(size > 0 ? static_cast<std::size_t>(size - 1) : 0, '\0');
                if (size > 0)
                {
                    WideCharToMultiByte(CP_UTF8, 0, name.pwszVal, -1, text.data(), size, nullptr, nullptr);
                }
                if (text == wanted)
                {
                    PropVariantClear(&name);
                    Release(store);
                    Release(collection);
                    *device = each;
                    return S_OK;
                }
            }
            PropVariantClear(&name);
            Release(store);
        }
        Release(each);
    }
    Release(collection);
    return enumerator->GetDefaultAudioEndpoint(eRender, eConsole, device);
}

/** A render endpoint's mix format and the shared-mode path a stream on it takes, for the tables.
    @param wanted  the endpoint's name, or empty for the default
    @return        the report, or why the endpoint could not be asked */
std::string EndpointReport(const std::string &wanted)
{
    const HRESULT        started    = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator *enumerator = nullptr;
    IMMDevice           *device     = nullptr;
    IAudioClient        *client     = nullptr;
    IAudioClient3       *client3    = nullptr;
    WAVEFORMATEX        *format     = nullptr;
    std::string          report;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator),
                                  reinterpret_cast<void **>(&enumerator));
    if (SUCCEEDED(hr))
    {
        hr = FindRenderEndpoint(enumerator, wanted, &device);
    }
    if (SUCCEEDED(hr))
    {
        hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&client));
    }
    if (SUCCEEDED(hr))
    {
        hr = client->GetMixFormat(&format);
    }
    if (FAILED(hr))
    {
        report = "the render endpoint could not be asked its mix format (" + Hex(hr) + ")";
    }
    else
    {
        report = "mix format " + std::to_string(format->nSamplesPerSec) + " Hz, " + std::to_string(format->nChannels) +
                 " channels, " + std::to_string(format->wBitsPerSample) + "-bit; ";
        if (SUCCEEDED(client->QueryInterface(__uuidof(IAudioClient3), reinterpret_cast<void **>(&client3))))
        {
            UINT32 standard = 0, fundamental = 0, smallest = 0, largest = 0;
            if (SUCCEEDED(client3->GetSharedModeEnginePeriod(format, &standard, &fundamental, &smallest, &largest)))
            {
                const double ms = 1000.0 * smallest / format->nSamplesPerSec;
                char         text[64];
                std::snprintf(text, sizeof text, "%.2f ms", ms);
                report += "shared-mode periods (IAudioClient3): the smallest " + std::to_string(smallest) +
                          " frames (" + text + "; default " + std::to_string(standard) + ", step " +
                          std::to_string(fundamental) + ", largest " + std::to_string(largest) + ") - " +
                          (smallest < standard ? "small periods, which a stream at the mix rate opens at"
                                               : "none under the default: the driver offers no small periods, and a "
                                                 "stream at the mix rate runs at the default");
            }
            else
            {
                report += "IAudioClient3, but GetSharedModeEnginePeriod failed";
            }
        }
        else
        {
            report += "no IAudioClient3: no small periods on this Windows, the engine's period always";
        }
    }
    if (format != nullptr)
    {
        CoTaskMemFree(format);
    }
    Release(client3);
    Release(client);
    Release(device);
    Release(enumerator);
    if (SUCCEEDED(started))
    {
        CoUninitialize();
    }
    return report;
}

/** Windows Audio's rig: the default render endpoint and its loopback capture. */
class WasapiRig final : public DeviceRig
{
  public:
    /** The rig of one mode.
        @param exclusive  true for Windows Audio (Exclusive Mode), whose render bypasses the mixer a loopback hears */
    explicit WasapiRig(bool exclusive) : exclusive_(exclusive)
    {
    }

    /** The API's name, as DeviceBackends lists this mode.
        @return  the name */
    std::string ModeName() const
    {
        return exclusive_ ? "Windows Audio (Exclusive Mode)" : "Windows Audio";
    }

    TestDevice Find(const std::vector<DeviceInfo> &devices) override
    {
        TestDevice        device;
        const std::string named       = Environment("RECKLESS_WASAPI_DEVICE");
        const std::string input_named = Environment("RECKLESS_WASAPI_INPUT");
        const std::string cable       = Environment("RECKLESS_WASAPI_LOOP");
        const DeviceInfo *output      = nullptr;
        const DeviceInfo *input       = nullptr;
        for (const DeviceInfo &info : devices)
        {
            const bool wanted = named.empty() ? info.is_default_output : info.name == named;
            if (output == nullptr && wanted && info.output_channels > 0)
            {
                output = &info;
            }
            if (input == nullptr && !input_named.empty() && info.name == input_named && info.input_channels > 0)
            {
                input = &info;
            }
        }
        if (output == nullptr)
        {
            device.missing =
                named.empty()
                    ? std::string("no active render endpoint - speakers, headphones or a virtual cable - so no stream "
                                  "to open and no loopback to hear it by (CI's Windows runners have none: Windows "
                                  "Audio is tested on a machine with one)")
                    : "RECKLESS_WASAPI_DEVICE names '" + named + "', which " + ModeName() +
                          " lists no render endpoint by";
            return device;
        }
        if (!input_named.empty() && input == nullptr)
        {
            device.missing = "RECKLESS_WASAPI_INPUT names '" + input_named + "', which " + ModeName() +
                             " lists no capture endpoint by";
            return device;
        }
        device.found          = true;
        device.output         = output->name;
        device.engine_input   = {}; // the default capture endpoint would be a microphone
        device.channels       = std::min(output->output_channels, 2);
        device.sample_rate    = output->DefaultSampleRate(); // the mix rate: nothing resampled on the way
        device.block_size     = 256;
        device.rate_tolerance = 0.03;
        // A stalled callback brings the next period event late, which the back end reports as an xrun in every mode.
        device.xrun_by          = XrunBy::kStall;
        const std::string which = named.empty() ? std::string("the default render endpoint")
                                                : "the render endpoint RECKLESS_WASAPI_DEVICE "
                                                  "names";
        if (exclusive_)
        {
            // Exclusive: the endpoint the stream's alone, nothing between it and the driver - so no mixer for a
            // loopback to hear. A cable, though, comes back on a capture endpoint of its own.
            device.loop  = Loop::kNone;
            device.about = which + " in exclusive mode: " + EndpointReport(output->name);
        }
        else if (input == nullptr)
        {
            device.input = output->name; // the endpoint's loopback
            device.loop  = Loop::kCorrelated;
            device.about = which + ", heard back by WASAPI's loopback capture: " + EndpointReport(output->name);
        }
        else
        {
            device.about = which + ": " + EndpointReport(output->name);
        }
        if (!exclusive_)
        {
            // In the shared mode both sides run through the audio engine, and GetStreamLatency reports each client's
            // own path without the mixing between them: on a UMC1820 cabled output to input it reported 480 frames
            // beyond a 480-frame block where the cable measured 2579 - 1139 frames, 23.7 ms, unaccounted. The
            // exclusive mode, which has no mixer, reported the round trip to 35 frames on the same cable.
            device.does_report_whole_round_trip = false;
            device.partial_latency = "the audio engine's mixing path: the shared mode reports each client's own "
                                     "latency and nothing of the mixing between them";
        }
        // A capture endpoint of its own: the cable's input, in either mode. It is the engine's input too, so the
        // engine's test runs duplex through the same cable.
        if (input != nullptr)
        {
            device.input        = input->name;
            device.engine_input = input->name;
            device.engine_key   = false;
            device.channels     = std::min({output->output_channels, input->input_channels, 2});
            device.loop         = Loop::kCorrelated;
            device.about        = "duplex, " + device.about;
        }
        if (!cable.empty())
        {
            if (input == nullptr)
            {
                device.found   = false;
                device.missing = "RECKLESS_WASAPI_LOOP is '" + cable +
                                 "', but RECKLESS_WASAPI_INPUT names no capture endpoint for the cable to come back "
                                 "on - a render endpoint's loopback hears the mixer, not a cable";
                return device;
            }
            if (!ApplyCable(device, cable, output->output_channels, input->input_channels, "RECKLESS_WASAPI_LOOP"))
            {
                return device;
            }
        }
        return device;
    }

  private:
    bool exclusive_; ///< true for the exclusive mode's rig
};
} // namespace

std::unique_ptr<DeviceRig> MakeWasapiRig(bool exclusive)
{
    return std::make_unique<WasapiRig>(exclusive);
}
} // namespace reckless::audio::test
