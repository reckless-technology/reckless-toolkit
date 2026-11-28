#pragma once

#include "DeviceBackends.h"

namespace reckless::audio
{
/**
    MIDI input and output on ALSA's sequencer, directly: the machine's MIDI ports as the suite's own MidiSystem.

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

    The outputs are the ports another client lets be written to and subscribed to, listed and found the same way. Each
    output opened is a port of ours on a third client ("<name> out"), subscribed to by the output ("<name> MIDI out"),
    with a sequencer queue of its own. The system's one sender (MidiOutputSender.h) hands each message to the sequencer
    20 milliseconds ahead of its time, stamped with that time on the queue's real-time clock, and the kernel's timer
    sends it then: the queue's zero is measured against NowNanoseconds' clock once, when the output opens (QueueZero).
    A time already past is stamped as it is, and the kernel sends it at once. Closing an output frees its queue, and
    every event still on it with it, so nothing is sent after CloseAll returns.

    The owner's thread throughout but for the input thread and the sender, which are stopped while ports are opened and
    closed, and Send, which is any thread's.
*/

/** This back end as DeviceBackends.h's MIDI back end on Linux.
    @return  the descriptor, one object for the program's life */
const MidiBackend &AlsaMidiBackend();
} // namespace reckless::audio
