#pragma once

#include "DeviceBackends.h"

namespace reckless::audio
{
/**
    ASIO as the suite's own AudioSystem, on Steinberg's SDK directly (its host files, asio.cpp, asiodrivers.cpp and
    asiolist.cpp, compiled into the device library): the machine's ASIO drivers, and one stream on one of them whose
    callback is the driver's own buffer switch, with nothing between the driver's buffers and the engine but the
    conversion of each channel's samples.

    The drivers are the ones the registry lists as the program starts - the SDK's driver list is taken then, during the
    program's static initialisation, as RtAudio's was, so a driver installed while it runs appears at its next start -
    each under the name RtAudio listed it by, its driver name, named apart as every back end names its devices. A
    driver's channels and rates are probed by loading it, once in the program's life: only one ASIO driver can be loaded
    at a time, so while a stream is open the drivers are listed as they were probed.

    A stream opens one driver both ways - ASIO has no other kind - at the rate asked, at the block asked held to what
    the driver takes (RtAudio's rule: its preferred size where none is asked, within its smallest and largest, a power
    of two where it says so, else a whole number of its granularity), its buffers made for the channels asked alone.
    Each buffer switch reads the inputs from the half the driver has just filled, runs the callback and writes the
    outputs into the half the driver plays next, each channel in its own sample type: 16, 24 and 32-bit integer, 32-bit
    integer aligned low at 16, 18, 20 or 24 bits, and 32 or 64-bit float, little-endian. A driver that asks for a
    resync, or says it is overloaded, is an xrun told to the next callback; one that asks to be reset, or whose rate
    changes under the stream, is the stream failing, told once (AudioCallback::StreamFailed), and closed from the
    message thread.

    The SDK keeps one driver for the process, so one ASIO stream can be open at a time across every system: another
    system's open is refused while one is. Message thread throughout but for the driver's callbacks.
*/

/** This back end as DeviceBackends.h chooses between them: "ASIO", and a system for it. Listed after Windows Audio,
    since only a person with an ASIO driver wants it.
    @return  the descriptor, one object for the program's life */
const AudioBackend &AsioBackend();
} // namespace reckless::audio
