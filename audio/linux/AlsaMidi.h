#pragma once

#include "DeviceBackends.h"

namespace reckless::audio
{
/**
    MIDI input on ALSA's sequencer, directly: the machine's MIDI ports as the suite's own MidiSystem.

    The inputs are the sequencer ports RtMidi listed - every MIDI, synth or application port another client lets be
    read and subscribed to - under the names it gave them, "<client>:<port> <client number>:<port number>"
    ("UMC1820:UMC1820 MIDI 1 24:0"), so the settings written while RtMidi served MIDI still find theirs. A name is
    found exactly, else with its numbers set aside - a USB interface plugged in again comes back under new ones - else
    as the first port whose name holds it, as RtMidi found a shorter name.

    The system is one sequencer client named after the Standalone, with a port of its own for each input opened
    ("<name> MIDI in", RtMidi's), subscribed to that input; a thread of its own waits in poll for events and hands each
    one to the sink as MIDI bytes, stamped with NowNanoseconds as it arrives. System exclusive, timing clock and active
    sensing are left out, as RtMidi was told to leave them. Listing goes through a second client, so the message
    thread's queries never share a sequencer handle with the input thread's reads.

    Message thread throughout but for the input thread, which is stopped while ports are opened and closed.
*/

/** This back end as DeviceBackends.h's MIDI back end on Linux.
    @return  the descriptor, one object for the program's life */
const MidiBackend &AlsaMidiBackend();
} // namespace reckless::audio
