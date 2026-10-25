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
binary, nor fetched: the products' notices name neither. macOS has been run on real hardware on Apple silicon, below.
What is left is the hardware a CI runner lacks elsewhere: Windows Audio and ASIO on a real interface, and MIDI from a
real keyboard on Windows and Linux.

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
  of 0.9997 or better), one stream playing bursts with silence between them bringing every burst back within a
  hundredth of a frame of the first.
- **The round trip, measured against what the HAL reports.** Two blocks and the latency reported, and then the
  driver's error: the UMC1820's driver reports its latency and safety offsets as a fixed time (about 1.5 ms each in,
  0.28 ms each out: 74 and 14 frames at 48 kHz, 146 and 26 at 96), where its converters' delay is fixed in samples -
  so the measured round trip is 21 to 37 frames more than reported at 44.1 and 48 kHz and 41 to 70 frames less at
  88.2 and 96, within 0.8 ms either way. RtAudio counted the device latency alone, without the safety offsets, which
  is about 110 frames short at every rate. Between one stream and the next the round trip moves by one USB
  high-speed microframe (125 µs: 6 frames at 48 kHz, 12 at 96) - how the driver lines its input and output packets
  up at a start - and within a stream it never moves.
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

**CoreAudio has no exclusive mode here, on purpose.** Its exclusive access is hog mode
(`kAudioDevicePropertyHogMode`), and it buys nothing a stream of this layer wants: the nominal rate is the hardware's
own and any client may set it, the IOProc's buffer is per process and may be any size in the device's range (15 to
4096 frames on the UMC1820), and no mixer stands between the IOProc and the driver's buffer, as Windows Audio's
shared mode puts one - which is why Windows Audio has an exclusive mode and CoreAudio does not need one. What hog mode
adds is silencing every other program and formats that cannot be mixed (integer, encoded passthrough), at the cost of
taking the device from everything else, and leaving it taken should the program die holding it. The back end refuses a
stream whose buffers are not 32-bit float - the format of every mixable stream - rather than misread one another
program has hogged.

**Linux carries neither library now**: with the ALSA sequencer serving MIDI, RtAudio and RtMidi are not fetched or
compiled there. The sequencer lists every input under the name RtMidi gave it - checked side by side against RtMidi
on this machine, identical - and also finds a name whose client and port numbers have changed, as a USB interface's
do when it is plugged in again. A Linux build without ALSA's headers has no MIDI inputs at all, as it has no audio
devices.

Nothing above the seam has to change: `AudioDevice.h`'s `AudioSystem` (nine pure virtuals) and `MidiSystem` (four)
were already written against no library, and no test names a back end.

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

- Whether MIDI keeps to input only. `MidiSystem` has no outputs, and the suite has a `sequence` plugin that might
  one day want them; adding them later changes the interface and all three implementations.
