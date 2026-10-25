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
its name**, so one API moves at a time and the device suite can be run against either. RtAudio's descriptor goes when
the last name it serves has moved, and `cmake/PatchRtAudio.cmake`, `cmake/PatchRtAudioAlsaXruns.cmake` and
`cmake/PatchRtAudioAlsaThreads.cmake` go with it.

Nothing above the seam has to change: `AudioDevice.h`'s `AudioSystem` (nine pure virtuals) and `MidiSystem` (four)
were already written against no library, and no test names a back end.

## Where each API ends up

| Platform | Audio | MIDI |
|---|---|---|
| Windows | **ASIO**, on Steinberg's SDK directly, beside the toolkit's own **WASAPI** in its three modes | **WinMM** |
| macOS | **CoreAudio**, HAL/AUHAL directly | **CoreMIDI** |
| Linux | **ALSA** directly, beside a **JACK** client | **ALSA sequencer** |

Two of those are already written: WASAPI (`audio/win/WasapiDevice.cpp`, the three modes) and the ASIO SDK's host
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
   three patches.
2. **CoreAudio.** Low risk, and the wrappers are thin shims over it.
3. **ASIO.** The SDK is pinned at 2.3.4 and `tests/devices/AsioRig.cpp` already drives its sample driver.
4. **JACK.** Smallest, and `audio/linux/JackLoader.cpp` already dlopens libjack so a machine without it still starts.
5. **MIDI**, each platform's, after the audio of that platform.

Roughly three thousand new lines in all. The device suite is the oracle at every step: it drives only `AudioSystem`,
and asserts first-callback latency, the rate within 3 %, no xruns, the longest callback and no allocations.

## Adding a back end

- A file under `audio/` (or `audio/linux/`, `audio/win/`) implementing `AudioSystem`, with a header exposing only
  `const AudioBackend &<Name>Backend();` - no library's headers in it, so nothing else meets them.
- Its descriptor added to the list in `DeviceBackends.cpp`, **ahead of RtAudio's**, under the `#if` that compiles it.
- Its sources added to `reckless_audio_devices` in the root `CMakeLists.txt`.
- The device suite run against it, and against RtAudio's for the same API, and the two compared.

## Two things settled by evidence, worth not re-deciding

**A buffer size cannot be chosen in WASAPI's shared mode**, and in the low-latency mode only where the driver offers
more than one period. On the BEHRINGER UMC 1820 `GetSharedModeEnginePeriod` reports smallest, default, step and
largest all 480 frames, so a block of 256 opens at 480. Exclusive mode and ASIO honour it exactly. Measured, same
binary, block asked 256: low latency 480, exclusive 256, ASIO 256.

**The settings' device names were composed by RtAudio.** A product's settings keep the API name and the device name,
and `FindDevice` matches on the name with the channel layout, ordinals appended where two devices share one. A native
back end will not name the same hardware the same way. Each one must either reproduce that naming or the settings
layer must miss gracefully and fall back to the default device - decide per back end, and note that v1.1.0 shipped on
2026-10-24, so there are settings in the field.

## Open, not yet decided

- Whether to collapse "Windows Audio" and "Windows Audio (Low Latency Mode)" into one API whose block dropdown works
  and falls back to the engine's period. It would remove a mode that reads as broken, but the names are stored in
  settings as the `api` field, so it needs a migration.
- Whether MIDI keeps to input only. `MidiSystem` has no outputs, and the suite has a `sequence` plugin that might
  one day want them; adding them later changes the interface and all three implementations.
