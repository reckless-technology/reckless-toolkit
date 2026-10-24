#pragma once

#include <memory>
#include <string>
#include <vector>

#include "AudioDevice.h"

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

/** The audio APIs this build carries, by the series' names, in the order a Standalone prefers them: CoreAudio; on
    Windows the suite's own WASAPI in its three modes (win/WasapiDevice.h: Windows Audio, its Low Latency Mode and its
    Exclusive Mode) before ASIO, since only a person with an ASIO driver wants it; JACK before ALSA, since on a PipeWire
    desktop JACK is how a program joins the graph without fighting it for the card. Every other is RtAudio's.
    @return  the names of the compiled APIs, most preferred first; empty when the build carries none of them */
std::vector<std::string> AudioApis();

/** The first of AudioApis() with a device that has an output, else the first of them, else none.
    @return  an API's name, or an empty string when the build carries none */
std::string DefaultAudioApi();

/** API `name` (one of AudioApis()): a WASAPI mode on the suite's own back end, any other on RtAudio; null for a name
    this build does not carry.
    @param name  the API's name, as AudioApis() lists it
    @return      the system, owned by the caller; nullptr for an unknown or uncompiled name */
std::unique_ptr<AudioSystem> MakeAudioSystem(const std::string &name);

/** The machine's MIDI inputs on RtMidi, each port it opens named after `client_name` (a Standalone's plugin name),
    each message stamped with NowNanoseconds as RtMidi hands it over.
    @param client_name  the name its ports carry, a Standalone's plugin name
    @return             the system, owned by the caller */
std::unique_ptr<MidiSystem> MakeMidiSystem(const std::string &client_name);

/** The versions of the two libraries, for a player to print.
    @return  the text "RtAudio <version>, RtMidi <version>", with the versions the libraries report */
std::string AudioLibraryVersions();
} // namespace reckless::audio
