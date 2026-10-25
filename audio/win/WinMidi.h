#pragma once

#include "DeviceBackends.h"

namespace reckless::audio
{
/**
    MIDI input on Windows' own MIDI API (WinMM), directly: the machine's MIDI inputs as the suite's own MidiSystem.

    The inputs are WinMM's, under the names RtMidi gave them - the device's own name with its index after it, "UMC1820
    MIDI In 0" (WinMidiFormats.h) - so the settings written while RtMidi served MIDI still find theirs; the names are
    read in UTF-16 and kept in UTF-8, where RtMidi read them in the system's code page. A name is found exactly, else
    with its index set aside - another device coming or going moves it - else as the first input whose name holds it.

    Each input opened is a WinMM input of its own, its messages handed by WinMM's callback to the sink as their bytes,
    stamped with NowNanoseconds as they arrive. System exclusive, timing clock, MIDI time code and active sensing are
    left out, as RtMidi was told to leave them. Message thread throughout but for WinMM's callback, which closing an
    input waits for.
*/

/** This back end as DeviceBackends.h's MIDI back end on Windows.
    @return  the descriptor, one object for the program's life */
const MidiBackend &WinMidiBackend();
} // namespace reckless::audio
