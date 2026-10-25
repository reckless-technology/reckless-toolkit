#pragma once

#include "DeviceBackends.h"

namespace reckless::audio
{
/**
    JACK as the suite's own AudioSystem: one client of whatever JACK server the machine runs - JACK's own jackd, or
    PipeWire's, whose libjack is the graph - reached through libjack loaded by name at run time (JackLoader.cpp), so a
    machine without it starts as any other and lists no JACK device.

    The devices are the server's clients that have audio ports, by name - "system", or PipeWire's nodes - as RtAudio
    listed them, so the settings written while RtAudio served JACK still find theirs: a client's input ports are the
    device's output channels and its output ports its input channels, and its one rate is the server's. A port belongs
    to the client its name carries before the colon, exactly - not to every client whose name merely holds the
    device's, as RtAudio's pattern counted it.

    A stream is a client of the name the request gives (JACK's list shows it), with one port a channel - "outport 0",
    "inport 0", RtAudio's names - connected on Start to the device's ports from the first channel asked, at the server's
    rate, which the request must ask for. The block asked is asked of the server: of PipeWire's graph as the client's
    latency (PIPEWIRE_LATENCY as it connects), which the graph runs at while no other node asks for less and leaves once
    the client goes, nothing forced on it; of a JACK server as its buffer size (jack_set_buffer_size), a server's
    setting any client may change. The server may give another, or change it while the stream runs, and the stream's
    format says the block it runs at - after Start, once PipeWire's graph has moved to it. The callback is JACK's
    process callback, on the server's real-time thread, each port's buffer copied in and out of channels laid end to
    end. An xrun the server reports is told to the next callback; the server shutting the client down - stopped, or gone
    - is the stream failing, told once (AudioCallback::StreamFailed). Stop deactivates the client, which JACK returns
    from only once no process callback runs, and waits for any still counted in.

    Message thread throughout but for the process, xrun and shutdown callbacks, which run on the server's threads.
*/

/** This back end as DeviceBackends.h chooses between them: "JACK", and a system for it.
    @return  the descriptor, one object for the program's life */
const AudioBackend &JackBackend();
} // namespace reckless::audio
