# The device layer goes native

The decision, taken 2026-10-25: the audio and MIDI back ends become the platforms' own APIs, and both third-party
libraries go. This says what was decided, what it rests on, and what order the work takes, so that a session starting
cold - on another machine, or months later - does not have to re-argue it.

## What is already in

`audio/DeviceBackends.{h,cpp}` is the seam. It owns `AudioApis()`, `DefaultAudioApi()`, `MakeAudioSystem()`,
`MakeMidiSystem()` and `AudioLibraryVersions()`, and knows no back end's types: each back end hands it a descriptor -
what it is, which API names it can open on this machine, and how to make a system for one - and the list is assembled
by hand in `DeviceBackends.cpp`.

That is what makes this a migration rather than a rewrite. **A native back end is listed ahead of RtAudio's and takes
its name**, so one API moves at a time and the device suite can be run against either. The order a Standalone offers
the names in is the series' own (`kPreference` in `DeviceBackends.cpp`), apart from which back end serves each, so a
native back end listed first does not jump its name ahead of RtAudio's others. Once a name has moved, RtAudio stops
compiling that API's back end and its patches go - the last, `cmake/PatchRtAudio.cmake`, with RtAudio's ASIO. RtAudio's
descriptor goes when CoreAudio moves, the last name it serves.

## Where it stands

**Done, 2026-10-25: every API on every platform is the toolkit's own**, and RtAudio and RtMidi are compiled into no
binary, nor fetched: the products' notices name neither. macOS and Windows have both been run on real hardware, below.
Linux has been run on real hardware too, below, and with it nothing is left to run by hand that a CI runner lacks.

| API | back end | since |
|---|---|---|
| Windows Audio, its two modes | the toolkit's, `audio/win/WasapiDevice.cpp`; its low-latency mode folded into Windows Audio | before 2026-10-25; two modes 2026-10-25 |
| ALSA | the toolkit's, `audio/linux/AlsaDevice.cpp`; RtAudio no longer compiles its ALSA, and both ALSA patches are gone | 2026-10-25 |
| JACK | the toolkit's, `audio/linux/JackDevice.cpp`, libjack loaded at run time (`JackLoader.cpp`); RtAudio compiles no API on Linux and is not listed there | 2026-10-25 |
| ASIO | the toolkit's, `audio/win/AsioDevice.cpp`, on Steinberg's SDK; RtAudio compiles no API on Windows and is not listed there | 2026-10-25 |
| CoreAudio | the toolkit's, `audio/mac/CoreAudioDevice.cpp`, on the HAL; an input on another device joined in a private aggregate; RtAudio listed nowhere | 2026-10-25 |
| MIDI on Linux | the toolkit's, `audio/linux/AlsaMidi.cpp`, on ALSA's sequencer | 2026-10-25 |
| MIDI on Windows | the toolkit's, `audio/win/WinMidi.cpp`, on WinMM; neither library fetched on Windows | 2026-10-25 |
| MIDI on macOS | the toolkit's, `audio/mac/CoreMidi.cpp`, on CoreMIDI's MIDI 1.0 protocol; `RtAudioDevice.cpp` deleted, RtAudio and RtMidi built nowhere; `RtAudioDevice.h`, the forwarding header left for the products, deleted once none included it | 2026-10-25 |

ALSA, measured against RtAudio's on the same machine and the same loopback card
([device-tests.md](device-tests.md), "Why a case took a minute"): every device named as RtAudio named it, a stop in
under a millisecond where RtAudio's took seconds, the loop's latency 768 samples at 256 frames where RtAudio's was
1280, and an xrun recovered in one restart. Settled on the way: an output buffer of the periods the product asks for (`StreamRequest::periods`, two at the
fewest), three where it leaves them to the back end (RtAudio's was four) - measured on the loopback card at 256
frames, the loop's latency exactly two, three and four blocks -
an input buffer of at least 4096 frames since PipeWire's `default` hands its input over a 1024-frame quantum at a time,
and a device's free direction listed where another program holds the other - the UMC1820's inputs, which RtAudio
dropped while PipeWire played through it.

JACK, measured against RtAudio's on the suite's dummy-driver server: the same block, rate, loop (bit for bit, 1024
samples) and xrun recovery to the sample, and the server stopped under a running stream told to the callback within
5 ms, the stream closed in under 25 ms and opened again once the server was back. The block asked is asked of the server, where
RtAudio took the server's whatever was asked: of PipeWire's graph as the client's latency, which moved this machine's
graph from its 1024-frame quantum to 64 for the stream's life - the UMC1820 duplex through it at 64 frames, no xruns -
and of a JACK server as its buffer size. PipeWire's own `jack_set_buffer_size` is never called: it forces a quantum
on the whole graph that outlives the stream. A device's ports are its client's
by exact name, where RtAudio's pattern counted any client whose name held the device's.

ASIO, held by the Windows job's device suite on the SDK's sample driver: the driver list taken during static
initialisation as RtAudio's was (the suite registers its test driver in registry views that exist only until main),
the SDK's own `asioDrivers` global pointed at it, so the two workarounds `RtAudioDevice.cpp` carried for RtAudio are
gone, and the input read from the half of the buffer the driver has just filled - a block sooner than RtAudio, which
read the half before.

CoreAudio and CoreMIDI, measured on Apple silicon (an M1 Pro, macOS 27) against a BEHRINGER UMC1820 - class
compliant, on Apple's own USB audio driver - with its outputs 3 and 4 patched by cable to its inputs 1 and 2, and a
Nektar Impact LX88+ on USB ([device-tests.md](device-tests.md), "On a Mac with an interface"):

- **Every name as the libraries gave it.** RtAudio 6.0.1 and RtMidi 6.0.0, built as the toolkit built them on macOS
  (`__MACOSX_CORE__`, no `UNICODE`), listed side by side with the toolkit's back ends: every CoreAudio device the same
  name, channels each way, rates, defaults and preferred rate, every CoreMIDI input the same name ("UMC1820",
  "Impact LX88+ MIDI1", "Impact LX88+ MIDI2"). The one departure, kept on purpose, is a name past ASCII: read in UTF-8
  here, in Mac OS Roman by RtAudio.
- **Duplex on one device, through the cable**, at every rate the UMC1820 offers (44.1, 48, 88.2 and 96 kHz) and every
  block from 32 to 1024 frames: the rate and block asked, no xruns, each channel back as it was played (a 64-tap fit
  of 0.9996 or better), one stream playing bursts with silence between them bringing every burst back within a
  hundredth of a frame of the first.
- **The round trip, measured against what the HAL reports.** Two blocks and the latency reported, and then the
  driver's error: the UMC1820's driver reports its latency and safety offsets as a fixed time (about 1.5 ms each in,
  0.28 ms each out: 74 and 14 frames at 48 kHz, 146 and 26 at 96), where its converters' delay is fixed in samples -
  so the measured round trip is 21 to 39 frames more than reported at 44.1 and 48 kHz and 41 to 70 frames less at
  88.2 and 96, within 0.9 ms either way. RtAudio counted the device latency alone, without the safety offsets, which
  is about 110 frames short at every rate. Within a stream the round trip never moves. Between one stream and the
  next it moves by up to one USB high-speed microframe (125 µs: 6 frames at 48 kHz, 12 at 96) - how the driver lines
  its input and output packets up at a start - but for one case: the first stream at 48 kHz after the device has run
  at 96 comes back 24 to 30 frames sooner than any other (1197 frames at 512, against 1221 to 1228), every time, and
  the next one is ordinary again. A bare HAL stream, one click on an output and no toolkit code, does exactly the
  same, so it is the driver's; Reckless IR Capture, which trims each response at its own onset, does not see it.
- **Duplex across two devices**, the UMC1820's outputs and a miniDSP UMIK-2's inputs joined in the back end's private
  aggregate, at 44.1, 48 and 96 kHz: the rate and block asked, no xruns, the input the UMIK-2's (its -55 dBFS room,
  not the UMC1820's -100 dBFS idle inputs), and the aggregate gone with the stream.
- **Reckless IR Capture's own capture** through the same cable (`ir-capture-cli --api CoreAudio`, 512 frames):
  every noise period aligned with the next to the sample, and the sweep, deconvolved from the raw recording over the
  same band as the noise, at the noise's delay to a hundredth of a frame - 1227.07 frames, 25.56 ms - so the stream
  holds its timing through the capture's silent gap. (The tool's own "the sweep's response lagged the noise's" note
  compares the two after each is trimmed at its own onset, and is the onset detectors', not the stream's.)
- **A real keyboard**: notes held and let go, controllers and pitch bend (its full 14 bits) arriving whole and in
  order, a note off sent as a note on of velocity 0 arriving as sent, as RtMidi delivered it.

Three faults the hardware found, each fixed in the back end and held by the suite:

- **A stream's latency left out a stream's own.** What the HAL reports beyond the block is the device's latency, its
  safety offset and the stream's latency; the last is none on the UMC1820 and most of it on a Mac's own devices - 690
  frames on a MacBook Pro's speakers, 1439 on its microphone, at 48 kHz - which the back end first left out.
- **A latency read before it had settled.** When a stream's rate changes, or an aggregate is made, the HAL moves the
  nominal rate first and the latencies some 60 to 80 ms after, in steps: an aggregate taken from 96 to 44.1 kHz read
  262, 244, then 162 frames. The back end read them as the rate came, and reported 262; it now waits until they have
  held still for 150 ms.
- **An xrun at the start of every stream at a tiny block.** At a cycle of a third of a millisecond - 32 frames at
  88.2 or 96 kHz, 16 at 48 - the HAL reports one processor overload at cycle 0, before the first IOProc, of nearly
  every stream, a bare IOProc that only clears its buffers included, and none after it. It is the HAL's I/O thread
  starting, and the back end leaves it unreported, as ALSA's leaves an xrun before its first callback.

Windows Audio and ASIO, measured on the same BEHRINGER UMC1820 - its Thesycon driver and its own ASIO driver - with
outputs 3 and 4 patched by cable to inputs 1 and 2, on Windows 11 ([device-tests.md](device-tests.md), "On Windows with
an interface"):

- **ASIO opens every block the driver offers, at every rate.** Twenty-four runs, 44.1, 48, 88.2 and 96 kHz against 32,
  64, 128, 256, 512 and 1024 frames: the block opened exactly as asked in all twenty-four, the rate by the clock within
  0.06 %, no xruns, and each channel's 64-tap fit 0.9995 or better. Windows Audio's exclusive mode opens the block
  asked for too; **its shared mode cannot** - this driver reports `GetSharedModeEnginePeriod` smallest, default, step
  and largest all 480 frames, so a block of 256 opens at 480, as the suite's own note already said of it.
- **The round trip, against what each API reports.** ASIO and the exclusive mode report it honestly; the shared mode
  does not, and cannot.

  | API | block asked / opened | reports beyond the block | the cable measures | measured less reported |
  |---|---|---|---|---|
  | ASIO | 256 / 256 | 224 | 797 frames, 16.60 ms | +61 (1.27 ms) |
  | Windows Audio (Exclusive Mode) | 256 / 256 | 768 | 1309 frames, 27.27 ms | +29 (0.60 ms) |
  | Windows Audio | 256 / 480 | 480 | 2573 frames, 53.60 ms | **+1133 (23.60 ms)** |

  **The shared mode leaves the audio engine's mixing out of what it reports.** `IAudioClient::GetStreamLatency` gives
  each client its own path and nothing of the mixing between the render client and the capture one, so a cable measures
  23.6 ms more than the mode reports. There is nothing to add it to: no call discloses it. The suite therefore holds
  the shared mode only to a round trip no sooner than it reports
  (`TestDevice::does_report_whole_round_trip`), and prints the shortfall rather than judging it. The exclusive mode,
  which has no mixer, reported the same cable to 29 frames.
- **What ASIO leaves unaccounted shrinks as the rate rises**, and does not move with the block: 58 to 70 frames at
  44.1 kHz and 61 to 67 at 48 (about 1.5 and 1.4 ms), 17 to 28 at 88.2 and 5 to 6 at 96 (0.19 and 0.06 ms). Within one
  rate it is the same at 32 frames as at 1024, which is what says it is the converters' own delay and not anything the
  stream does.

One fault the hardware found, in the back end, and two in the suite that hid it:

- **ASIO reported the blocks twice.** `ASIOGetLatencies` is measured from the buffer switch, so its two figures already
  contain the block each way - the SDK's `asio.h` has the input's as "usually the size of one block in sample frames,
  plus device specific latencies" - where `StreamFormat::reported_latency` is the latency *beyond* the block. The back
  end assigned the sum straight across, so a stream's reported round trip ran two blocks long: 60 frames out at 64,
  445 at 256 and 1987 at 1024, the error growing with the block until the loop test failed on it. One block now comes
  off each direction that is open, and what is left over is the constant above.
- **The suite's ASIO rig hid every driver but its own.** It copies the machine's drivers into the registry view it
  registers its test driver through, and copied them with `RegCopyTreeW`, which an application hive refuses
  (`ERROR_ACCESS_DENIED`) - a result nothing checked. The keys were created, and counted, and arrived empty, so the
  SDK read no class from them and dropped the lot: the header said "3 of the machine's own drivers listed beside it"
  while the device list held the test driver alone. On no machine with a real ASIO driver had that driver ever listed.
- **Neither Windows rig could be pointed at an interface.** Only CoreAudio's took a device and a cable from the
  environment; Windows Audio's was the default render endpoint and ASIO's the sample driver, with no way to ask for
  anything else. Both now take the same shape of variables, and the cable parser is one function all three rigs call
  (`DeviceRig.h`'s `ApplyCable`) rather than a second and third copy of it.

**MIDI on Windows, from a real keyboard** (WinMM, `audio/win/WinMidi.cpp`), measured on a Nektar Impact LX88+ on USB:
its two ports listed as Windows names them ("Impact LX88+" and "MIDIIN2 (Impact LX88+)"), beside the UMC1820's own
"UMC1820 MIDI In"; the keyboard opened by the name a settings file keeps, and over 1.72 s of playing, **20 notes held
and let go - every one paired with its release** - and 24 controller messages, the mod wheel's CC 1 climbing one step
at a time about 3 ms apart. A note off sent as a note on of velocity 0 arrives as sent (`90 42 00`), as it does on
CoreMIDI. Every message whole, on the index the input was opened under, stamped in order, with system exclusive,
timing clock and active sensing left out.

**A real ASIO driver cannot be asked for an xrun**, so `CountsAnXrunAndPlaysOnAfterIt` skips where
`RECKLESS_ASIO_DEVICE` names one. On ASIO an xrun reaches the callback only as the driver's own resync request, and
nothing in the API obliges a driver to send one: the UMC1820 carried on without a word through a callback stalled well
past its block. The SDK's sample driver, which asks to order, is what holds that test.

**CoreAudio has no exclusive mode here, on purpose.** Its exclusive access is hog mode
(`kAudioDevicePropertyHogMode`), and it buys nothing a stream of this layer wants: the nominal rate is the hardware's
own and any client may set it, the IOProc's buffer is per process and may be any size in the device's range (15 to
4096 frames on the UMC1820), and no mixer stands between the IOProc and the driver's buffer, as Windows Audio's
shared mode puts one - which is why Windows Audio has an exclusive mode and CoreAudio does not need one. What hog mode
adds is silencing every other program and formats that cannot be mixed (integer, encoded passthrough), at the cost of
taking the device from everything else, and leaving it taken should the program die holding it. The back end refuses a
stream whose buffers are not 32-bit float - the format of every mixable stream - rather than misread one another
program has hogged.

**ALSA and the ALSA sequencer, on real hardware**, measured on a Dell Precision (Ubuntu 24.04, kernel 7.0) against the
same BEHRINGER UMC1820 - class compliant, on the kernel's `snd-usb-audio` - outputs 3 and 4 patched by cable to inputs 1
and 2, and the same Nektar Impact LX88+ on USB ([device-tests.md](device-tests.md), "On Linux with an interface"):

- **Duplex on one device, through the cable**, at 44.1, 48, 88.2 and 96 kHz against every block from 32 to 1024
  frames, with the stream thread at real-time priority: the rate and block asked, each channel back as it was played (a
  64-tap fit of 0.9996 or better at 44.1 and 48 kHz, 0.9998 at 88.2 and 96), every burst through silence at the first one's
  delay to a hundredth of a frame, and the stalled callback reported as one xrun and played on from - but for the fault
  at 96 kHz below.
- **The round trip, against what the back end reports**: 77 to 117 frames more than reported, at every rate and block
  - the converters' and the USB transfer's, which ALSA does not count - about 2.2 ms at 48 kHz and 1 ms at 96. It moves
  by a microframe or two between one stream and the next, as on a Mac, and does not move with the block.
- **A real keyboard on the sequencer**: notes held and let go (a note off sent as a note on of velocity 0 arriving as
  sent), the mod wheel's CC 1 a step about every 3 ms, and pitch bend to its full 14 bits (`E0 7F 7F`), every message
  whole, on the index it was opened under and in order.

One fault the hardware found in the back end:

- **ALSA reported the output's block twice.** The back end reported the output's buffer less one block, which is how
  late a block written plays - but `StreamFormat::reported_latency` is the latency beyond the block each way, and a
  round trip counts two blocks beside it. So every stream reported a block more than it had: through the cable the
  round trip came back a block sooner than reported at every rate and block, and at 1024 frames outside the suite's
  64th of a second. It reports the buffer less two blocks now - none at two periods - and the loopback card, whose loop
  is exactly the buffer, holds it: 512 frames back at two periods of 256 and nothing reported beyond them.

And two things the machine taught, not the back end:

- **Without real-time priority, small blocks at high rates xrun.** A user in no group the system grants `rtprio` -
  this machine's, until it was put in `audio` and the jackd package's `/etc/security/limits.d/audio.conf` enabled -
  runs the stream thread at the ordinary priority, since the back end's `SCHED_RR` request is refused without a word.
  The cable then took xruns at 88.2 kHz and 32 frames and at 96 kHz from 32 to 128, which at real-time priority it
  never did. A Linux product's user is told to be in `audio`, as JACK's users are.
- **At 96 kHz, a small buffer slips under `snd-usb-audio` with nothing reported.** At 96 kHz and 64 frames on three
  periods, about one stream in two gains 24 to 49 frames - two to four USB microframes - at a moment somewhere in its
  first few seconds, and runs that much later from then on; no xrun reaches ALSA, so none reaches the callback. A bare
  ALSA duplex stream, alsa-lib's `snd_pcm_readi` and `snd_pcm_writei` on `hw:UMC1820` with no toolkit code, does the
  same - 5 runs in 10 - so it is below the back end: the kernel's USB audio path, the controller, or the interface.
  The same bare stream never slipped at 96 kHz on eight periods of 64 or three of 256, nor at 48 kHz on three of 64 or
  of 32 (8 runs each). It is accepted, not worked round: the suite's loop test fails when it happens, as it should, and
  a person who wants 96 kHz at that block on this interface runs with more periods or at 48 kHz. Do not relax the test
  for it.

**Linux carries neither library now**: with the ALSA sequencer serving MIDI, RtAudio and RtMidi are not fetched or
compiled there. The sequencer lists every input under the name RtMidi gave it - checked side by side against RtMidi
on this machine, identical - and also finds a name whose client and port numbers have changed, as a USB interface's
do when it is plugged in again. A Linux build without ALSA's headers has no MIDI inputs at all, as it has no audio
devices.

Nothing above the seam has to change: `AudioDevice.h`'s `AudioSystem` (nine pure virtuals) and `MidiSystem` (four
then, seven since it sends as well: "MIDI output" below) were already written against no library, and no test names a
back end.

## The latency, each side apart

`StreamFormat::reported_latency` is one figure, the latency beyond the block input and output summed. Beside it,
`input_latency_frames` and `output_latency_frames` give the two sides, and they **add up to it exactly** on every back
end (the sum itself is as it always was, so no caller changed): a recording program places a take by the input's, and
shows a round trip from both. Each is from what the back end already knew:

| Back end | `input_latency_frames` | `output_latency_frames` |
|---|---|---|
| ALSA | **0: not told.** The back end's figure is the playback buffer less two blocks, which is the output's; a capture side's room costs no latency and its driver's own delay is not reported | all of `reported_latency` |
| JACK | the first input port's capture latency (`jack_port_get_latency_range`, its least) | the first output port's playback latency |
| CoreAudio | the input scope's device latency, safety offset and its stream's latency (`Latency`); 0 with no input | the output scope's the same |
| Windows Audio | the capture stream's latency (`GetStreamLatency`) plus the capture period the ring keeps; 0 with no input | the render stream's latency |
| ASIO | `ASIOGetLatencies`' input less a block; 0 with no input | its output less a block; 0 with no output |

A side that is not open is 0; so is a back end that tells neither (the offline device). **Unrun:** the ALSA split
is held by the device suite (`tests/test_audio_devices.cpp`) in CI on the loopback card, and was not run on the
machine that wrote it; the JACK split is the same arithmetic, run there by the same suite; CoreAudio's, Windows Audio's
and ASIO's are compiled by CI and have never run - the sums are the lines they replace, split.

## The channels' names

`DeviceInfo::input_channel_names` and `output_channel_names` (2026-11-19, for a recording program's "Input 3: Mic/Line 3")
carry what the API or the driver calls each channel, in UTF-8: **empty where the back end cannot tell, else exactly one
per channel with a channel it has no name for left as ""**. Never made up - a product writes "Input 3" itself - and read
from what each API lists, never by opening a stream: no back end opens anything for them that its enumeration did not
open already. They are read once with the probe that already runs, so a repeated listing costs what it did.
`InputChannelName(i)` and `OutputChannelName(i)` answer for one channel and give "" past the list. The arithmetic is
`audio/ChannelNames.h`, held on every platform by `tests/test_channel_names.cpp`; each real API's list is checked for
its shape by the device suite (`tests/test_audio_devices.cpp`, which prints them).

| API | Names | Read from | Unrun here |
|---|---|---|---|
| ASIO | the driver's name for each channel | `ASIOGetChannelInfo`, on the driver the probe has already loaded, no buffers made; the driver's 8-bit text converted from the ANSI code page | compiled by CI only |
| JACK | each port's own name without its client (`capture_1`, `playback_FL`) | the port list the device layer already reads, in the order it connects the ports | the arithmetic is tested; the live graph is the device suite's |
| CoreAudio | the HAL's element name for each channel, what a driver or Audio MIDI Setup set | `kAudioObjectPropertyElementName` per scope and element (channel counted from 1 across the streams), a property read; a channel with none is "" | compiled by CI only |
| ALSA | the speaker **position** the driver declares for each channel: `FL`, `FR`, `FC`, `AUX0` | `snd_pcm_query_chmaps_from_hw` on the card's control, no PCM opened; `hw:` PCMs only, the map of the PCM's channel count; unknown and not-applicable positions are "" | the device suite on CI's loopback card |
| WASAPI | none | an endpoint has no per-channel name in the Core Audio interfaces; the lists stay empty | compiled by CI only |

**ALSA's are positions, not labels, and the difference matters.** A UMC1820 (kernel 7.0, read from `amixer -c UMC1820
contents` and `/proc/asound/UMC1820/stream0`, 2026-11-19) declares a *fixed* map of 12 playback channels, `FL FR FC LFE RL
RR FLC FRC RC SL SR TC`, and of 10 capture channels, `FL FR FC LFE RL RR FLC FRC RC SL`: the speaker layout the
USB driver gives any multichannel interface that does not describe its channels, and nothing of the front panel's
"Mic/Line 3". The query cannot tell a default layout from a real one, so the layer reports what the driver declares and
a product should show an ALSA name only as the position it is. Where a driver (a UAC2 interface with channel clusters)
declares real positions they are the same names.

## Where each API ends up

| Platform | Audio | MIDI |
|---|---|---|
| Windows | **ASIO**, on Steinberg's SDK directly, beside the toolkit's own **WASAPI** in its two modes | **WinMM** |
| macOS | **CoreAudio**, HAL/AUHAL directly | **CoreMIDI** |
| Linux | **ALSA** directly, beside a **JACK** client | **ALSA sequencer** |

Two of those are already written: WASAPI (`audio/win/WasapiDevice.cpp`, the two modes) and the ASIO SDK's host
sources, which this build already compiles for RtAudio (`asio.cpp`, `asiodrivers.cpp`, `asiolist.cpp`).

ASIO is **not** Windows' only back end. Most machines have no ASIO driver - onboard codecs, USB class-compliant
devices, Bluetooth - and WASAPI is what serves them.

On Linux, ALSA and JACK are not alternatives to choose between. **ALSA direct is what gives the control**: explicit
period and buffer size, and `snd_pcm_mmap_begin`/`commit` on `SND_PCM_ACCESS_MMAP_INTERLEAVED` is the DMA ring
itself. JACK is for coexisting with a desktop that owns the card, and one JACK client covers PipeWire too, since
pipewire-jack implements the JACK API.

## Threads of a consumer's own

A DAW renders its mix on many threads: a few **realtime workers** that work beside the device's callback inside its
deadline, and a pool of lower-urgency render-ahead threads. The back ends raise their own callback threads (below) and,
since this section, hand a consumer the same standing for its realtime workers - never for the render-ahead pool,
which is meant to be preempted.

```cpp
void WorkerMain(const reckless::audio::AudioSystem &system)
{
    const reckless::audio::RealtimeThreadScope standing(system); // first thing in the thread; given back at its end
    ShowInSettings(standing.Result().Describe());               // "round-robin realtime priority 70", or why not
    ...
}
```

`AudioSystem::JoinAudioThread(RealtimeThreadMembership &)` is the virtual (`audio/AudioDevice.h`) and
`RealtimeThreadScope` (`audio/RealtimeThreadScope.h`) the RAII that calls it. It is made **by the thread itself**,
once at its start and once at its end, so it may allocate and ask the kernel - it is never made on an audio thread in
steady state. A refusal is a **result with its reason** (`RealtimeThreadResult::refused`), never an abort and never
silent, and the thread runs as it was. With **no stream open** the join is refused ("no stream is open ..."). The caller
joins its workers after `Open()` and lets them leave before `Close()`.

| API | What the callback thread has today (read 2026-11-19) | What a consumer's thread is given |
|---|---|---|
| ALSA | `SCHED_RR` at 70 or the system's maximum, the stream thread's first act, a refusal ignored (`audio/linux/AlsaDevice.cpp`, `Run`) | the same, from the one `audio/linux/RealtimeScheduling.h` (`kStreamThreadPriority`, the constant the stream thread now reads too); refused with `RLIMIT_RTPRIO` named where the user has none - `ulimit -r` is 0 on a stock desktop |
| JACK (and PipeWire's libjack) | the server's: libjack's own thread, at the server's priority | `jack_client_real_time_priority` and `jack_acquire_real_time_scheduling`, looked up by the run-time loader (`audio/linux/JackLoader.cpp`). A libjack without either, or a refused acquire, falls back to the ALSA route and the result says so; a server not running in realtime refuses, since its callback has none either |
| Windows Audio (WASAPI, both modes) | MMCSS class `"Pro Audio"` (`AvSetMmThreadCharacteristicsW`), `THREAD_PRIORITY_TIME_CRITICAL` where MMCSS refuses (`audio/win/WasapiDevice.cpp`, `Run`) | the same, from `audio/win/MmcssThread.h`, reverted on leave |
| ASIO | the driver's own thread, the driver's business | MMCSS `"Pro Audio"` as WASAPI's, on our threads |
| CoreAudio | the HAL's IOProc thread, in the device's audio workgroup | the device's workgroup joined (`kAudioDevicePropertyIOThreadOSWorkgroup`, macOS 11, then `os_workgroup_join`; left with `os_workgroup_leave`), after a time-constraint policy whose period is the stream's block, as Apple's "Understanding Audio Workgroups" says for parallel realtime threads. Without the workgroup Apple silicon schedules a helper on an efficiency core |

What it does **not** do, deliberately: start a workgroup interval (the HAL's thread owns the cycle), lock memory, or
touch a thread it was not called on. The time-constraint policy's computation is half the period, within the whole
period, preemptible; what the HAL's own thread has is not published, so that is a choice to be measured against a real
Mac (see the pull request that added this).

The machine's cores - which logical processors share a core, which are performance and which efficiency - and the
pinning of the calling thread to one are in `core/CpuTopology.h`, documented in [../core/cpu-topology.md](../core/cpu-topology.md).

## MIDI output

Decided and built on 2026-11-28 (issue #102): `MidiSystem` sends as well as receives, on all three platforms.

**The interface** (`audio/AudioDevice.h`). `Outputs()` lists the outputs by name, as `Inputs()` lists the inputs, and
`OpenOutput(name, port)` opens one as a port number from 0 to `kMidiOutputPorts - 1` (63), finding the name as `Open`
finds an input's; several are open at once. `Send(port, time_nanoseconds, bytes, size)` queues one message to leave
at that time on `NowNanoseconds()`'s clock, or at once if the time has passed, and returns false when the port is not
open, its queue is full, or the message is not one whole channel message or system real-time message of the length its
status asks (`IsSendableMidiMessage`). **System exclusive is not sent**, nor running status, nor the system common
messages. `CloseAll()` closes the outputs too, and nothing is sent after it returns: what was queued and not yet sent
is dropped, what a back end had handed its platform ahead of the time with it, and the sender has stopped. `Send` is
for any thread, an audio callback among them, and for several at once on one port; the rest is the owner's thread's. A
device that goes away is not reported, as it is not for an input.

**The queue** (`audio/MidiOutputQueue.h`): one per open output, bounded at 4,096 messages
(`kMidiOutputQueueCapacity`), many producers and one consumer, fixed-size messages (a time, three bytes, a length).
**A producer is wait-free**: a reservation counter apart from the ticket counter means one that finds the queue full
gives its reservation back without taking a ticket - so the consumer never meets a hole - and one that reserves always
finds its ticket's slot already free, so it never waits for the consumer. A slot's sequence number is published only
after its message is written, so the consumer never reads a half-written one. The header carries both arguments. Send
finds a port's queue in a fixed table (`audio/MidiOutputPorts.h`) whose epoch and two counters let `CloseAll` withdraw
a queue and know no `Send` still holds it, with no lock on `Send`'s side: a `Send` racing a close returns false or has
its message dropped, and never touches a closed output.

**The sender** (`audio/MidiOutputSender.h`): one ordinary thread per system, never a realtime one, serving every open
output. It moves each queue into that output's schedule - a heap on its own memory, earliest first and ties in the order
queued, so two messages one thread sent for one time leave in the order it sent them - and hands each message on when
it is due, or a lead ahead where the platform schedules by time. A `Send` cannot wake it (that would be a system call),
so it looks at its queues at least every millisecond (`kMidiSenderPollNanoseconds`): a message sent for a time already
past, or nearer than the lead, leaves up to that much late.

| Platform | How a message leaves | Its timing |
|---|---|---|
| Linux | The ALSA sequencer: a port of ours on a client of its own, subscribed to by the output, with a sequencer queue of its own. The sender hands each message on 20 ms ahead, scheduled on that queue in real time, and **the kernel's timer sends it**. The queue's zero is measured against the steady clock once, at the open; its timer ticks at 4 kHz. Closing frees the queue and every event on it. | Measured through the sequencer's own loop on an x86-64 desktop (Linux 7.0, the high-resolution timer), 2026-11-28, 30 runs of 32 stamped messages: a median of 215 us after its time, at worst 369 us; one sent for a time past, at worst 1.33 ms. The two clocks moved 0.7 to 2.6 us apart over a minute. At the sequencer's default 1 kHz tick a message left up to a millisecond late, which is why the queue's timer is set finer. |
| macOS | CoreMIDI: an output port of ours to the destination. The sender hands each message on 20 ms ahead as one Universal MIDI Packet on MIDI 1.0's protocol (`MIDISendEventList`), stamped with its time in host ticks, and **CoreMIDI's scheduler sends it**. The steady clock and `mach_absolute_time` are read together at each message and only the distance between them is converted (`coremidi::HostTimeAt`). Two messages of one time are stamped a host tick apart, in the order they were sent, because CoreMIDI orders packets of one stamp in an order of its own (the suite saw a note off put before a note on sent ahead of it). Closing unschedules what was handed on (`MIDIFlushOutput`) before the port goes. | On a hosted macOS runner, Apple silicon, 2026-11-28, to a virtual destination of the suite's own (`MidiOutputs.LeaveThroughCoreMidi...`): each of 27 stamped messages left 39 to 151 µs after its time. |
| Windows | WinMM, whose short messages carry no time: **the sender sleeps until each message is due** on a high-resolution waitable timer (`CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`), or on an ordinary one with the multimedia timer's period held at a millisecond while an output is open, and sends it then (`midiOutShortMsg`). Closing closes WinMM's handle and sends no note off. | Not yet measured: no CI runner has a MIDI output to send to, so only the arithmetic and the refusals are tested there. |

## Why neither library

The goals were a clean low-cost abstraction, the lowest latency on all three platforms, as much control over buffer
size, rate and DMA as possible, and Windows, macOS, Ubuntu and Mint. Both libraries exist to hide the platform
differences that control lives in.

- **RtAudio** holds `stream_.mutex` across a blocking `snd_pcm_readi`, so `stopStream` and `abortStream` queue behind
  a thread parked in ALSA's `wait_for_avail`. `Stop()` measured 0.7 s to 48.6 s. See
  [device-tests.md](device-tests.md), "Why a case can take a minute".
- **PortAudio** was considered and rejected. Its ALSA back end is sound - `PaAlsaStream_WaitForFrames` polls with a
  finite timeout and `RealStop` takes no lock - but its WASAPI has no `IAudioClient3`, so no low-latency shared mode
  (its own header: "For the Shared mode latency can not be lower than 20 ms"), and it offers no portable way to
  request a host buffer size, which is the whole of goal three. Its ASIO support needs Steinberg sources compiled in,
  a patch for an SDK bug, and the project built as Multi-Byte rather than Unicode.

The abstraction is already low-cost: `AudioSystem`'s virtuals are per stream operation and `AudioCallback::Process`
is one indirect call per buffer - 375 a second at 128 frames and 48 kHz. Do not template it to chase zero. What costs
in a callback is allocation and locks, and the suite already holds the callback to no allocations.

## The order, and why

1. **ALSA.** First: it is the one that is broken, the loopback rig is already in the suite, and it retires two of the
   three patches. Done, 2026-10-25.
2. **CoreAudio.** Low risk, and the wrappers are thin shims over it.
3. **ASIO.** The SDK is pinned at 2.3.4 and `tests/devices/AsioRig.cpp` already drives its sample driver.
4. **JACK.** Smallest, and `audio/linux/JackLoader.cpp` already dlopens libjack so a machine without it still starts.
5. **MIDI**, each platform's, after the audio of that platform.

Roughly three thousand new lines in all. The device suite is the oracle at every step: it drives only `AudioSystem`,
and asserts first-callback latency, the rate within 3 %, no xruns, the longest callback and no allocations.

## Adding a back end

- A file under `audio/` (or `audio/linux/`, `audio/win/`) implementing `AudioSystem`, with a header exposing only
  `const AudioBackend &<Name>Backend();` - no library's headers in it, so nothing else meets them.
- Its descriptor added to the list in `DeviceBackends.cpp` under the `#if` that compiles it, and its API's name to the
  series order (`kPreference`) where it is a new one.
- Its sources added to `reckless_audio_devices` in the root `CMakeLists.txt`.
- The device suite run against it, and against the back end it replaces where there is one, and the two compared.

## Two things settled by evidence, worth not re-deciding

**A buffer size cannot be chosen in WASAPI's shared mode** but at the driver's small periods (IAudioClient3, at the
endpoint's mix rate), and there only where the driver offers more than one period. On the BEHRINGER UMC 1820 `GetSharedModeEnginePeriod` reports smallest, default, step and
largest all 480 frames, so a block of 256 opens at 480. Exclusive mode and ASIO honour it exactly. Measured, same
binary, block asked 256: low latency 480, exclusive 256, ASIO 256.

**The settings' device names were composed by RtAudio.** A product's settings keep the API name and the device name,
and `FindDevice` matches on the name with the channel layout, ordinals appended where two devices share one. A native
back end will not name the same hardware the same way. Each one must either reproduce that naming or the settings
layer must miss gracefully and fall back to the default device - decide per back end, and note that v1.1.0 shipped on
2026-10-24, so there are settings in the field. WASAPI and ALSA reproduce it, ALSA to the letter ("Default ALSA
Device", "<card name> (<PCM id>)"), and the ordinals are one function every back end calls, `AudioDevice.h`'s
`NameApart`.

**Windows Audio is one API, not two** (decided 2026-10-25): its shared mode opens at the driver's smallest period that
holds the block wherever the stream runs at the endpoint's mix rate and Windows and the driver offer it, and at the
engine's period otherwise, converting - so the block setting works where it can and falls back where it cannot, and
there is no separate mode that reads as broken on a driver of one period. "Windows Audio (Low Latency Mode)" in a
settings file reads as "Windows Audio" (`AudioSettings.h`), and asking for it by name makes Windows Audio
(`WasapiFormats.h`'s `ModeNamed`).

## Open, not yet decided

- Nothing at present. Whether MIDI kept to input only was decided on 2026-11-28: it does not ("MIDI output",
  above).
