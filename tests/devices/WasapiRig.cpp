// Windows Audio's test device, in each of its three modes (audio/win/WasapiDevice.h): the machine's default
// render endpoint. In the shared and low-latency modes it is played through and heard back by WASAPI's loopback
// capture - the back end opens an input named after a render endpoint as the capture of the mix that endpoint plays -
// so the samples come back through the audio engine's mixer, correlated rather than exact; in the low-latency mode the
// render client is IAudioClient3's, opened by InitializeSharedAudioStream at the driver's period that holds the block,
// a few milliseconds where a WaveRT driver offers small periods, against the classic shared mode's ten. The rig asks
// the endpoint the same question and prints its periods. In the exclusive mode the stream has the endpoint to itself
// and there is no mixer to hear it by: the device is output only. A machine with no active render endpoint - CI's
// Windows runners, whose only endpoints are microphones - has no test device, and the tests skip with that reason.
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

namespace reckless::audio::test
{
namespace
{
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

/** The default render endpoint's mix format and the shared-mode path a stream on it takes, for the tables.
    @return  the report, or why the endpoint could not be asked */
std::string EndpointReport()
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
        hr = enumerator->GetDefaultAudioEndpoint(eRender, eConsole, &device);
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
        report = "the default render endpoint could not be asked its mix format (" + Hex(hr) + ")";
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
                          (smallest < standard ? "small periods, which the low-latency mode opens at"
                                               : "none under the default: the driver offers no small periods, and the "
                                                 "low-latency mode runs at the default");
            }
            else
            {
                report += "IAudioClient3, but GetSharedModeEnginePeriod failed";
            }
        }
        else
        {
            report += "no IAudioClient3: no low-latency mode on this Windows";
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

    TestDevice Find(const std::vector<DeviceInfo> &devices) override
    {
        TestDevice device;
        for (const DeviceInfo &info : devices)
        {
            if (!info.is_default_output || info.output_channels <= 0)
            {
                continue;
            }
            device.found          = true;
            device.output         = info.name;
            device.engine_input   = {}; // the default capture endpoint would be a microphone
            device.channels       = std::min(info.output_channels, 2);
            device.sample_rate    = info.DefaultSampleRate(); // the mix rate: nothing resampled on the way
            device.block_size     = 256;
            device.rate_tolerance = 0.03;
            // A stalled callback brings the next period event late, which the back end reports as an xrun in every
            // mode.
            device.xrun_by = XrunBy::kStall;
            if (exclusive_)
            {
                // Exclusive: the endpoint the stream's alone, nothing between it and the driver - so no mixer for a
                // loopback to hear.
                device.loop  = Loop::kNone;
                device.about = "the default render endpoint in exclusive mode: " + EndpointReport();
            }
            else
            {
                device.input = info.name; // the endpoint's loopback
                device.loop  = Loop::kCorrelated;
                device.about =
                    "the default render endpoint, heard back by WASAPI's loopback capture: " + EndpointReport();
            }
            return device;
        }
        device.missing = "no active render endpoint - speakers, headphones or a virtual cable - so no stream to open "
                         "and no loopback to hear it by (CI's Windows runners have none: Windows Audio is tested on a "
                         "machine with one)";
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
