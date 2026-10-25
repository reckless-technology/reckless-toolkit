#pragma once

#include <memory>

#include "AudioDevice.h"
#include "DeviceBackends.h"
#include "WasapiFormats.h"

namespace reckless::audio
{
/**
    Windows Audio - WASAPI - as the suite's own AudioSystem, in the two modes a person chooses between as two APIs
    (wasapi::Mode, named as WasapiFormats.h names them), each a stream whose callback is the render endpoint's own
    period, with no ring, resampler or second thread between the device and the engine:

    - Windows Audio: shared, through the audio engine's mixer, event-driven. At the endpoint's own rate, through
      IAudioClient3 at the smallest period the driver offers that holds the block asked for (2.67 ms at 48 kHz where the
      driver offers small periods, as Microsoft's inbox HD Audio driver does; the engine's period where it offers only
      that, as many vendors' drivers do); at any other rate, or on a Windows or a driver that refuses the small period,
      at the engine's period (10 ms on most machines), the engine converting the rate (AUTOCONVERTPCM). Until
      2026-10-25 the small periods were a mode of their own, "Windows Audio (Low Latency Mode)": a name that now means
      this one (wasapi::ModeNamed).
    - Windows Audio (Exclusive Mode): the endpoint to the stream alone, nothing between it and the driver, event-driven
      at the block asked for, held to the device's minimum period and its alignment, at any rate and in any encoding
      the device takes (wasapi::kEncodings, the best first), refused with words where another program holds it or its
      Properties do not allow it.

    The devices are the active endpoints by their friendly names, each unique among the endpoints the same way - a
    later one of an earlier one's name carrying its ordinal, as RtAudio named them, so a settings file written then
    still finds them - an endpoint's channels and rate its mix format's, the exclusive mode's rates those the device
    says it takes (asked once an endpoint and kept, since a Standalone's settings list the devices every two seconds).
    In the shared mode an input named after a render endpoint, where no capture endpoint has that name, is that
    endpoint's loopback - the mix it plays, captured.

    The stream's thread, under the multimedia scheduler's Pro Audio class, waits on the render endpoint's event and on
    the capture endpoint's: each capture packet is written to a ring (wasapi::CaptureRing), and each render period the
    callback reads its input from the ring and writes its output into the endpoint's buffer, converted to the device's
    encoding. Stop wakes the thread and joins it, so no callback runs once it returns; a device that goes away, or a
    call that fails while the stream runs, is the stream failing, told once (AudioCallback::StreamFailed).

    Message thread throughout but for the stream's thread; COM is started on each thread that needs it, in whatever
    apartment the thread has.
*/

/** The WASAPI system in `mode`.
    @param mode  shared, low latency or exclusive
    @return      the system, owned by the caller */
std::unique_ptr<AudioSystem> MakeWasapiSystem(wasapi::Mode mode);

/** This back end as DeviceBackends.h chooses between them: the two modes' names, in the order a Standalone lists
    them, and a system for one of them. Listed ahead of every other Windows back end, since Windows Audio is what a
    machine with no ASIO driver has.
    @return  the descriptor, one object for the program's life */
const AudioBackend &WasapiBackend();
} // namespace reckless::audio
