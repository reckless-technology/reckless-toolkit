#pragma once

#include "DeviceBackends.h"

namespace reckless::audio
{
/**
    MIDI input on CoreMIDI, directly: the machine's MIDI sources as the suite's own MidiSystem.

    The inputs are CoreMIDI's sources, each under the name RtMidi gave it - the names of the external devices it is
    connected to, else its own endpoint's name composed with its entity's and device's (CoreMidiFormats.h) - so the
    settings written while RtMidi served MIDI still find theirs. A name is found exactly, else as the first source whose
    name holds it, as RtMidi found a shorter name.

    The system is one CoreMIDI client named after the Standalone, with an input port of its own for each source opened
    ("<name> MIDI in", RtMidi's), on MIDI 1.0's protocol: CoreMIDI hands each port's events over as Universal MIDI
    Packets, and each MIDI 1.0 message among them goes to the sink as its bytes, stamped with NowNanoseconds as it
    arrives. System exclusive, timing clock, MIDI time code and active sensing are left out, as RtMidi was told to leave
    them. Message thread throughout but for CoreMIDI's receive blocks, which closing a port waits for.
*/

/** This back end as DeviceBackends.h's MIDI back end on macOS.
    @return  the descriptor, one object for the program's life */
const MidiBackend &CoreMidiBackend();
} // namespace reckless::audio
