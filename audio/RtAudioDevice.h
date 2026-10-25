#pragma once

#include <memory>
#include <string>
#include <vector>

#include "AudioDevice.h"
#include "DeviceBackends.h"

namespace reckless::audio
{
/**
    The machine's audio and MIDI through RtAudio and RtMidi (the root CMakeLists.txt fetches and builds them on each
    platform's APIs) - and on Windows through the suite's own WASAPI back end (win/WasapiDevice.h), which this file's
    AudioApis and MakeAudioSystem list and make beside RtAudio's: AudioSystem and MidiSystem as the suite's Standalone
    opens them. Only RtAudioDevice.cpp includes
    the two libraries' headers - RtAudio's brings in windows.h on Windows - so nothing else meets them.

    A stream is opened non-interleaved in 32-bit float, the format every API delivers or RtAudio converts to in buffers
    it allocates at the open, and asks the API for a real-time callback thread; its callback reads the clock as the API
    calls it and hands the engine the channels as pointers into RtAudio's buffers, set up again each call from arrays
    sized at the open. An error RtAudio reports from one of its own threads while a stream runs - a disconnection, or a
    system, driver or thread error, or ALSA's warning that its device went away - is the stream failing; the same from
    a call the message thread made into RtAudio (a probe of a device, say) is a message for the person and no more.

    What the layer adds to RtAudio. No two devices that carry channels the same way share a name, so a name and a way
    (FindDevice) find one device: a later device RtAudio lists under the name of an earlier one the same way carries its
    ordinal after it ("Loopback (Loopback PCM) (2)", ALSA's loopback card's second device), while an input and an output
    of one name keep it. And on ASIO, the SDK's host code is given the driver list its ASIOExit
    releases a driver through, which RtAudio never sets, and after a stream RtAudio closed itself the next open waits
    until RtAudio has let go of the driver (RtAudioDevice.cpp says why each is needed).
*/

/** RtAudio as one of the back ends DeviceBackends.h chooses between: the API names RtAudio is compiled with and can
    open on this machine, in the series' order of preference (CoreAudio, ASIO, JACK - Windows Audio and ALSA are never
    RtAudio's), and a system for one of them. A native back end that offers the same name is listed ahead of this one
    and takes it; this descriptor goes when the last name it serves has moved.
    @return  the descriptor, one object for the program's life */
const AudioBackend &RtAudioBackend();

/** RtMidi as DeviceBackends.h's MIDI back end: the machine's MIDI inputs, each message stamped with NowNanoseconds as
    RtMidi hands it over.
    @return  the descriptor, one object for the program's life */
const MidiBackend &RtMidiBackend();
} // namespace reckless::audio
