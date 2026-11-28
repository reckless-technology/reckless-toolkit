#pragma once

#include "DeviceBackends.h"

namespace reckless::audio
{
/**
    MIDI input and output on CoreMIDI, directly: the machine's MIDI sources and destinations as the suite's own
    MidiSystem.

    The inputs are CoreMIDI's sources, each under the name RtMidi gave it - the names of the external devices it is
    connected to, else its own endpoint's name composed with its entity's and device's (CoreMidiFormats.h) - so the
    settings written while RtMidi served MIDI still find theirs. A name is found exactly, else as the first source whose
    name holds it, as RtMidi found a shorter name.

    The system is one CoreMIDI client named after the Standalone, with an input port of its own for each source opened
    ("<name> MIDI in", RtMidi's), on MIDI 1.0's protocol: CoreMIDI hands each port's events over as Universal MIDI
    Packets, and each MIDI 1.0 message among them goes to the sink as its bytes, stamped with NowNanoseconds as it
    arrives. System exclusive, timing clock, MIDI time code and active sensing are left out, as RtMidi was told to leave
    them.

    The outputs are CoreMIDI's destinations, listed and found the same way. Each output opened is an output port of
    ours ("<name> MIDI out"); the system's one sender (MidiOutputSender.h) hands each message to CoreMIDI 20
    milliseconds ahead of its time, as one Universal MIDI Packet on MIDI 1.0's protocol (MIDISendEventList) stamped
    with that time in host ticks - NowNanoseconds' clock and mach_absolute_time read together at each message, and only
    the distance between them converted (coremidi::HostTimeAt) - and CoreMIDI's scheduler sends it then. A time already
    past is stamped now. Closing an output unschedules what it had handed CoreMIDI (MIDIFlushOutput) before its port is
    disposed of, so nothing is sent after CloseAll returns.

    Message thread throughout but for CoreMIDI's receive blocks, which closing a port waits for, the sender, which is
    stopped while outputs are opened and closed, and Send, which is any thread's.
*/

/** This back end as DeviceBackends.h's MIDI back end on macOS.
    @return  the descriptor, one object for the program's life */
const MidiBackend &CoreMidiBackend();
} // namespace reckless::audio
