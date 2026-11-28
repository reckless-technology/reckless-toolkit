#pragma once

#include "DeviceBackends.h"

namespace reckless::audio
{
/**
    MIDI input and output on Windows' own MIDI API (WinMM), directly: the machine's MIDI inputs and outputs as the
    suite's own MidiSystem.

    The inputs are WinMM's, under the names RtMidi gave them - the device's own name with its index after it, "UMC1820
    MIDI In 0" (WinMidiFormats.h) - so the settings written while RtMidi served MIDI still find theirs; the names are
    read in UTF-16 and kept in UTF-8, where RtMidi read them in the system's code page. A name is found exactly, else
    with its index set aside - another device coming or going moves it - else as the first input whose name holds it.

    Each input opened is a WinMM input of its own, its messages handed by WinMM's callback to the sink as their bytes,
    stamped with NowNanoseconds as they arrive. System exclusive, timing clock, MIDI time code and active sensing are
    left out, as RtMidi was told to leave them.

    The outputs are WinMM's, listed and found the same way. WinMM's short messages carry no time, so the system's one
    sender (MidiOutputSender.h) sleeps until each message is due - on a high-resolution waitable timer
    (CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, Windows 10 1803 and later), or on an ordinary one with the multimedia
    timer's period held at a millisecond while an output is open - and sends it then (midiOutShortMsg). Closing an
    output closes WinMM's handle and sends nothing more, no note off among it.

    Message thread throughout but for WinMM's callback, which closing an input waits for, the sender, which is stopped
    while outputs are opened and closed, and Send, which is any thread's.
*/

/** This back end as DeviceBackends.h's MIDI back end on Windows.
    @return  the descriptor, one object for the program's life */
const MidiBackend &WinMidiBackend();
} // namespace reckless::audio
