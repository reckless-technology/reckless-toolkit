#pragma once

#include "DeviceBackends.h"

namespace reckless::audio
{
/**
    CoreAudio as the suite's own AudioSystem, on the HAL directly: the machine's audio devices, and one stream whose
    callback is the device's own IOProc, with nothing between the device's buffers and the engine but the copy of each
    channel out of or into its stream's interleaved frames.

    The devices are the HAL's, each under the name RtAudio listed it by - its maker's name, a colon and its own, "Apple
    Inc.: MacBook Pro Speakers", read in UTF-8 - named apart as every back end names its devices, so the settings
    written while RtAudio served CoreAudio still find theirs. A device's channels are the sum of its streams' each way,
    its rates those it lists (a listed range as the series' rates within it), its current rate read afresh at every
    listing. The aggregates a stream of this back end makes are left out.

    A stream opens at the rate asked, the device's nominal rate set where it differs and waited for; at the block asked
    held to the device's buffer range; with the channels asked mapped across the device's streams. An input on the
    output's own device comes in the same IOProc. An input on another device - a Mac's built-in microphone and speakers
    are two - is joined to the output in a private aggregate device for the stream's life, the output's clock the
    aggregate's and the input drift-compensated against it, so one IOProc carries both and CoreAudio keeps their clocks
    together. A processor overload the HAL reports is an xrun told to the next callback - but not one before the first,
    which is the HAL's I/O thread starting at a tiny block, as ALSA's back end leaves one unreported; a device that dies
    under the stream is the stream failing, told once (AudioCallback::StreamFailed). Stop stops the IOProc, which the
    HAL returns from only once it no longer runs, and waits for any callback still counted in.

    Message thread throughout but for the IOProc and the property listeners, which run on the HAL's threads.
*/

/** This back end as DeviceBackends.h chooses between them: "CoreAudio", and a system for it.
    @return  the descriptor, one object for the program's life */
const AudioBackend &CoreAudioBackend();
} // namespace reckless::audio
