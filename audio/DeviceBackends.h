#pragma once

#include <memory>
#include <string>
#include <vector>

#include "AudioDevice.h"

namespace reckless::audio
{
/**
    Which back ends this build carries, and which one serves an API the caller names. AudioDevice.h is the interface
    every back end implements; this is the only place that knows which implementations exist, so nothing else - no
    product, no suite - names one. The four functions below are what a Standalone calls, and they keep the names and
    signatures they had when RtAudio's translation unit owned them, so moving them here changes nothing above this
    seam.

    A back end offers a set of API names (the series' names, as AudioSystem::Name() returns them) and makes a system
    for one of them. It says which names it can serve *on this machine*, not which it was compiled with: JACK is only
    there when libjack loads, ASIO only when a driver is installed, so the answer is a call rather than a constant.

    The list is assembled by hand in DeviceBackends.cpp, under the same `#if`s that compile each back end, and
    deliberately not by self-registration. A registrar in a translation unit nothing else references is stripped by
    the linker when the toolkit is built as a static library, which is how every product adds it - the back end would
    simply vanish, and only on an optimised build. Explicit assembly also fixes the order, which decides which back
    end wins a name two of them offer, and puts the whole set in one readable place.

    The list's order decides which back end serves a name two of them offer: the first. The order a Standalone offers
    the names in - and DefaultAudioApi() tries them in - is the series' own, apart from it (AudioApis()), so a back end
    listed ahead of another takes its name without moving it ahead of the other's names. Both orders live in
    DeviceBackends.cpp.
*/
struct AudioBackend
{
    /** What it is, for AudioLibraryVersions(): "ALSA", "CoreAudio", or a name and version where there is a library to
        report one. A function, not a literal, because a version is only known at run time.
        @return  the text */
    std::string (*Library)();

    /// The API names this back end can serve on this machine, most preferred first; empty when it can serve none.
    std::vector<std::string> (*Apis)();

    /** A system for one of this back end's API names.
        @param name  the API's name, one of Apis()
        @return      the system, owned by the caller; nullptr when this back end does not serve that name */
    std::unique_ptr<AudioSystem> (*Make)(const std::string &name);
};

/**
    A MIDI back end: the one the platform has, so unlike audio there is no choice to offer a person and no name to
    select on. It is a descriptor all the same, so that MakeMidiSystem() lives here beside MakeAudioSystem() and the
    platform `#if`s stay in one file.
*/
struct MidiBackend
{
    /** What it is, for AudioLibraryVersions(): "ALSA sequencer", "CoreMIDI", "Windows MIDI", or a name and version
        where there is a library to report one.
        @return  the text */
    std::string (*Library)();

    /** The machine's MIDI inputs on the platform's API.
        @param client_name  the name its ports carry, a Standalone's plugin name
        @return             the system, owned by the caller */
    std::unique_ptr<MidiSystem> (*Make)(const std::string &client_name);
};

/** The audio APIs this build carries, by the series' names, in the order a Standalone prefers them: CoreAudio; on
    Windows the suite's own WASAPI in its two modes (win/WasapiDevice.h: Windows Audio and its Exclusive Mode) before
    ASIO, since only a person with an ASIO driver wants it; JACK before ALSA, since on a
    PipeWire desktop JACK is how a program joins the graph without fighting it for the card.
    @return  the names of the APIs this machine can open, most preferred first; empty when it can open none */
std::vector<std::string> AudioApis();

/** The first of AudioApis() with a device that has an output, else the first of them, else none.
    @return  an API's name, or an empty string when the build carries none */
std::string DefaultAudioApi();

/** API `name` (one of AudioApis()) on whichever back end offers it, the first in the list where two do.
    @param name  the API's name, as AudioApis() lists it
    @return      the system, owned by the caller; nullptr for a name this machine cannot open */
std::unique_ptr<AudioSystem> MakeAudioSystem(const std::string &name);

/** The machine's MIDI inputs on the platform's MIDI API, each port it opens named after `client_name` (a Standalone's
    plugin name), each message stamped on the clock audio callbacks are stamped with.
    @param client_name  the name its ports carry, a Standalone's plugin name
    @return             the system, owned by the caller */
std::unique_ptr<MidiSystem> MakeMidiSystem(const std::string &client_name);

/** What this build opens devices with, for a player to print: every audio back end it carries and its MIDI one, in
    the order AudioApis() prefers them, with a version where the library reports one.
    @return  the text, as "ALSA, JACK, ALSA sequencer" */
std::string AudioLibraryVersions();
} // namespace reckless::audio
