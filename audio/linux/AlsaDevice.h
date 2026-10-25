#pragma once

#include "DeviceBackends.h"

namespace reckless::audio
{
/**
    ALSA as the suite's own AudioSystem, on alsa-lib directly: the machine's PCMs, and one stream whose callback is a
    period of the output device's own, with no ring or second thread between the device and the engine.

    The devices are the ones RtAudio listed, under the names it gave them, so the settings written while RtAudio served
    ALSA still find theirs: "Default ALSA Device" (the `default` PCM - PipeWire's, on a desktop that runs it),
    "PulseAudio Sound Server" (the `pulse` PCM, where alsa-lib defines one), then every card's every PCM device as
    "<card name> (<PCM id>)" - "HDA Intel PCH (ALC3266 Analog)", "Loopback (Loopback PCM)" - named apart as every back
    end names its devices (AudioDevice.h's NameApart). A device's channels and rates are probed by opening it each way,
    once in the program's life: a device the program itself has open, or a sound server holds, cannot be probed again,
    and is listed as it was. Where RtAudio passed over a device whose playback another program held, its capture -
    which may well be free - is listed here.

    A stream opens each device in the hardware's own interleaved layout - memory-mapped where the PCM allows it, so a
    period is converted straight into or out of the DMA buffer - at the rate asked, with no resampling on a hardware
    device, in the best of 32-bit float, 32-bit, 24-bit and 16-bit integer the device takes (SampleEncodings.h). The
    period is the block asked for, or the nearest the device takes, which the stream's format then says; an output's
    buffer is the periods the request asks for (StreamRequest::periods, two at the fewest), or three where it leaves
    them to the back end - its latency - and an input's at least 4096 frames, room that costs no latency. The output is
    primed with a buffer of silence as it starts, the input and output are linked where ALSA can link them (one card:
    started and stopped as one), and each pass of the stream's thread waits - in poll, bounded, never inside ALSA -
    until the input has a period and the output room for one, reads it, runs the callback and writes it.

    An xrun - the output run dry, the input overfilled, a suspend - stops the device, as ALSA stops it once its buffer
    is past (the stop threshold is the buffer's size): the stream is prepared, primed and started again, and the next
    callback told - but not before the first, since a device that runs dry before anything has played is the stream
    starting, as when PipeWire wakes a suspended input. A device that goes away, or stops answering for two seconds, is
    the stream failing, told once (AudioCallback::StreamFailed). Stop wakes the thread and joins it, so no callback runs
    once it returns, and drops whatever the device still holds.

    Message thread throughout but for the stream's thread, which asks for round-robin real-time priority 70 where the
    system allows it and runs at the normal priority where it does not.
*/

/** This back end as DeviceBackends.h chooses between them: "ALSA", and a system for it.
    @return  the descriptor, one object for the program's life */
const AudioBackend &AlsaBackend();
} // namespace reckless::audio
