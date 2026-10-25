#pragma once

#include <memory>
#include <string>
#include <vector>

#include "AudioDevice.h"
#include "DeviceBackends.h"

namespace reckless::audio
{
/**
    The machine's audio and MIDI through RtAudio and RtMidi, for what is not yet the toolkit's own: CoreAudio and
    CoreMIDI, on macOS alone (the root CMakeLists.txt fetches and builds them there) - every other API is a back end of
    the toolkit's own (DeviceBackends.h). Only RtAudioDevice.cpp includes the two libraries' headers, so nothing else
    meets them.

    A stream is opened non-interleaved in 32-bit float, the format every API delivers or RtAudio converts to in buffers
    it allocates at the open, and asks the API for a real-time callback thread; its callback reads the clock as the API
    calls it and hands the engine the channels as pointers into RtAudio's buffers, set up again each call from arrays
    sized at the open. An error RtAudio reports from one of its own threads while a stream runs - a disconnection, or a
    system, driver or thread error - is the stream failing; the same from a call the message thread made into RtAudio (a
    probe of a device, say) is a message for the person and no more. Every device is listed under a name no other device
    with channels the same way has, a later one of an earlier one's name carrying its ordinal.
*/

/** RtAudio as one of the back ends DeviceBackends.h chooses between: the API names RtAudio is compiled with and can
    open on this machine (CoreAudio - Windows Audio, ASIO, ALSA and JACK are never RtAudio's), and a system for one of
    them. This descriptor goes when CoreAudio moves, the last name it serves.
    @return  the descriptor, one object for the program's life */
const AudioBackend &RtAudioBackend();

/** RtMidi as DeviceBackends.h's MIDI back end: the machine's MIDI inputs, each message stamped with NowNanoseconds as
    RtMidi hands it over.
    @return  the descriptor, one object for the program's life */
const MidiBackend &RtMidiBackend();
} // namespace reckless::audio
