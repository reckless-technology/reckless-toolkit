# The audio layer on the real APIs

The audio layer (`audio/`) reaches the machine's interfaces through the back ends `DeviceBackends.h` chooses between:
the toolkit's own on every API - CoreAudio, Windows Audio (WASAPI, in two modes), ASIO, ALSA and JACK (`native-backends.md`). Two suites hold it. `audio`
(`reckless_audio_test`) runs the engine on the offline device and needs no hardware; `audio_devices`
(`reckless_audio_device_test`, `tests/test_audio_devices.cpp`) opens every API the build carries on a
deterministic test device and holds what really happens there. This note is the second: what it holds, the device it
uses on each API, how the CI runners are set up for it, and how to run it by hand - Windows Audio included, which only a
machine with speakers can test.

## What it holds

For each API the build carries, and any the run requires that it lacks (one parameterised instance per API, named after it):

| test | what it judges |
|---|---|
| `AudioApis.AreTheOnesTheConfigurePromised` | `AudioApis()` is exactly the set the configure promised - the platform's own API, ASIO wherever the configure has the SDK, JACK and ALSA wherever it found their headers (`RECKLESS_AUDIO_PROMISED_APIS`, from the toolkit's `CMakeLists.txt`) - so a build that silently loses one fails; every API the run requires is one of the series' and in the build. |
| `ProbesTheDevicesEachUnderANameOfItsOwn` | The device list, printed: no two devices with channels the same way share a name (a name and a way are what a settings file finds a device by; an input and an output may share one), every device has channels, every rate is a rate, a second probe lists the same, every name is UTF-8, and the test device is there with the channels and the rate its streams open. |
| `ListsADeviceNamedOutsideAsciiInUtf8` | CoreAudio alone: a device of the test's own - an aggregate of the default output, private to the test's process - named with an apostrophe and a letter past ASCII, as macOS names a person's earphones, found by that name in UTF-8, listed after its maker's as RtAudio listed it (RtAudio read CoreAudio's names in UTF-8 only with `UNICODE` defined, and in Mac OS Roman otherwise). |
| `RunsAStreamAtTheRateAndTheBlockItOpenedAt` | A stream opened at a rate the device lists and run for 1.2 s: the rate, the block and the channels it opened; the callbacks counted against what the rate and the block predict from the first callback to the Stop (within the device's rate tolerance and two); the rate read by the clock past the first 200 ms, where an API may call back in a burst to fill its buffers (within the device's tolerance: 3 %, and 10 % for JACK's dummy driver, below); every callback carrying the block; every channel pointer where every back end lays it out (each channel's frames after the one before's); the callbacks on the API's thread, not the caller's; the xruns, at most two plus one per hundred callbacks, and on a device whose clock the machine schedules one in ten more; nothing calling back once `Stop` has returned - which the layer keeps on every API, each callback counted in and out and silence once the stream is stopped, `Stop` waiting until none is under way. The latency the API reports is printed. |
| `CarriesTheSamplesALoopBringsBack` | Where the device loops its output back (below), a known signal out and the samples read back: bit for bit where the path is exact, correlated where a mixer sits in it, exactly the device's own signal where the device makes one. Through a cable a person patched (CoreAudio, "On a Mac with an interface" below), each channel's noise judged through a 64-tap filter fitted from what was played to what came back - the converters band-limit it and delay it by a fraction of a sample, which a sample-for-sample correlation would count against the loop - each fit at least 0.99, and the round trip within a 64th of a second of two blocks and the latency the API reports, never under the two blocks. |
| `KeepsTheCablesDelayThroughSilence` | Through a cable alone: one stream of about 8 s playing bursts of the known noise - 16384 frames of every 65536, silence between - as Reckless IR Capture plays its noise periods, its gap and its sweep in one stream; each burst's delay found to a fraction of a frame by its matched filter's peak, and every burst's within 0.05 frames of the first's, so a stream that lost or gained a frame while it carried silence fails. |
| `PlaysACoreThroughTheEngineAllocatingNothingInTheCallback` | `StandaloneEngine` playing the Gate's core on the device for a second, `Idle` every 10 ms: running throughout, its callbacks as the rate and the block predict (less, for each xrun the run is allowed, the callbacks of the 300 ms a device has to start again), and not one allocation on the audio thread - the engine's callback runs between two counts of the thread's allocations (`CountAllocations.h`, through an `AudioSystem` that relays the callback), so the count is the engine's and the core's, on the API's own thread. |
| `BuffersThePeriodsTheRequestAsksFor` | ALSA alone, the one API that lets a stream choose: streams asking for two, three and four periods (`StreamRequest::periods`) each open with them (`StreamFormat::periods`), report one block less than the buffer as their latency, and bring each frame back through the exact loop exactly that many blocks later, with no xruns. |
| `RefusesADeviceThatIsNotThereAndOpensTheNextOne` | An output and an input that do not exist each refused, the error naming them, and the test device opening and running afterwards. |
| `CountsAnXrunAndPlaysOnAfterIt` | An overrun or underrun reaching the callback - made by a callback that runs two blocks or 30 ms, whichever is longer (ALSA, JACK, CoreAudio), or reported by the device when the rig asks (ASIO) - with none reported before it (on a scheduled clock, at most one in ten callbacks), and the stream playing on after it as a stream from its start does: over the two seconds from 300 ms after the callback told of it - a window long enough that a loaded runner's moment of slowness as it resumes the device is a small part of it, where a stream that never resumes its rate is as far off as over any - the callbacks as the rate and the block predict (within the device's rate tolerance and two), the rate by the clock, the xruns a stream is allowed (at most two plus one per hundred callbacks, one in ten more on a scheduled clock), and the callbacks running until the stream is stopped; through an exact loop (ALSA, JACK), every sample back one that was played and the last 0.3 s of them one frame after another. Windows Audio skips it, saying why. |
| `AStreamWhoseDeviceGoesAwayFailsAndIdleClosesIt` | Where the rig can take the device away under a running engine (JACK, ASIO): the failure reaching the engine from the API's thread, `Idle` closing the stream and saying why, the state failed, no callback after; then the device back and the next `Open` preparing the core again and playing. |
| `MidiInputs.CarryWhatASequencerPortSendsButWhatTheCoresIgnore` | Linux alone, through ALSA's sequencer (`audio/linux/AlsaMidi.cpp`): a sequencer port of the test's own listed under RtMidi's name for it; a name that is nowhere refused, naming it; the port found by its name with other numbers after it, as a device plugged in again comes back; a note on and off, a controller and a pitch bend arriving as their bytes, on the index the input was opened under, stamped in order between their sending and their arrival; system exclusive, timing clock and active sensing left out; nothing after `CloseAll`. Skipped where the kernel has no sequencer, failed there when ALSA is required. |
| `MidiInputs.CarryWhatAPersonPlaysOnAKeyboard` | Any platform, and only where `RECKLESS_MIDI_KEYBOARD` names an input - a person plays it: the inputs listed, that one opened by the name a settings file keeps, and for 20 s (or 64 messages) what the person plays printed as it is read - notes held and let go (a note off sent as a note on of velocity 0 arrives as sent), controllers, pitch bend - at least one note played and let go, every message whole (its status, then data bytes under 0x80), on the index it was opened under, stamped in order. |
| `MidiInputs.CarryWhatACoreMidiSourceSendsButWhatTheCoresIgnore` | macOS alone, through CoreMIDI (`audio/mac/CoreMidi.cpp`): a virtual source of the test's own on MIDI 1.0's protocol (`devices/MidiSourceMac.cpp`) listed by its name, a name that is nowhere refused, and Universal MIDI Packets sent to it - a note on and off, a controller and a pitch bend arriving as their MIDI 1.0 bytes on the index the input was opened under, system exclusive, timing clock and active sensing left out, nothing after `CloseAll`. Skipped where CoreMIDI makes no source, failed there when CoreAudio is required. |

Every test prints the table it judges.

### The signals

The exact loop plays a sample that names its own position: frame *n* of the stream's output is
(*n* mod 2<sup>20</sup> + 1) / 2<sup>21</sup> - exact in a float, never zero, distinct for 21.8 s at 48 kHz - positive on
the first channel and negative on the second. Each sample that comes back says which frame and which channel it was,
and any arithmetic on the way - a gain, a dither, a conversion, a resampler - leaves a value that is no such sample. From
the first frame the loop carries on both channels - JACK's rig wires them one connection at a time, and a period can
fall between the two - every later sample must name the next frame on both; a jump or a silent gap is allowed only
where the device reported an xrun, and a sample that names nothing never. The loop's latency is the
first sample's position less the frame it names.

The correlated loop plays white noise 30 dB under full scale (it comes out of a person's speakers), regenerated for the
analysis from a hash of the frame: the delay is found from a 4096-sample window a quarter of a second in, then the
correlation coefficient over everything after it must reach 0.9, the gain printed.

## The test devices

| API | device | made by | loop | xrun | goes away |
|---|---|---|---|---|---|
| CoreAudio | the default output, written with silence - or, by hand, a person's interface and its cable ("On a Mac with an interface", below) | the machine: on the hosted macOS runner, the virtual machine's "Apple Virtual Sound Device", whose clock the hypervisor schedules, so the stream tests allow it one xrun in ten callbacks and a 10 % rate tolerance as they allow JACK's dummy driver | none: an input would ask the person at the machine for the microphone | a stalled callback | - |
| Windows Audio | the default render endpoint, heard back by WASAPI's loopback capture | the machine (none on CI) | correlated | a stalled callback | - |
| Windows Audio (Exclusive Mode) | the default render endpoint, the stream's alone | the machine (none on CI) | none: no mixer for a loopback to hear | a stalled callback | - |
| ASIO | the SDK's sample driver: 16 in, 16 out, 16-bit, 256 frames, 44.1 or 48 kHz, a sine on its first input and a sawtooth on its second | the suite: built from the fetched SDK and registered by the binary itself | the inputs, exactly as the driver makes them | the driver's resync request | the driver's reset request |
| JACK | `jackd` on the dummy driver, 48 kHz, 1024 frames, 3 capture and 5 playback ports, under a name of its own | the suite: a server for each test | exact: the stream's outputs wired to its own inputs through the server | a stalled callback | the server stopped with SIGTERM, then started again |
| ALSA | the kernel's loopback card (`snd-aloop`): playback on its device 0 comes back as capture on its device 1 | the CI job (`tools/audio-test-devices.sh`) | exact | a stalled callback (below) | - |

**JACK.** `devices/JackRig.cpp` starts `jackd --no-realtime --name reckless-test-<pid>-<n> -d dummy -r 48000 -p 1024 -C
3 -P 5` for each test and stops it after, with `JACK_DEFAULT_SERVER` naming it and `JACK_NO_START_SERVER` set, so every
client of the process reaches that server and none starts another; the child is forked with `PR_SET_PDEATHSIG`, so it
dies with the suite however the suite ends. Three capture and five playback ports are a signature no desktop's server
has: a test that finds them knows it reached this server and not a PipeWire graph. The loop is wired through the same
libjack, loaded by name, that the JACK back end uses (`JackLoader.cpp`). The self-connection makes a feedback edge in the graph,
so a client reads what it wrote a period before - the dummy's capture ports, connected too, carry silence, which adds
nothing. The dummy driver's cycles are timed by a thread of jackd's, which a runner schedules without real-time
priority, and a cycle that comes past its time is an xrun the driver starts its schedule again after, the time lost: the
machine's load, not the layer's. At 256 frames (5.3 ms) a four-core runner's ordinary waits of a dozen milliseconds make
about a dozen a second; 1024 frames (21.3 ms) rides them out, and the device says what its clock is
(`TestDevice::is_scheduler_timed`), so the stream tests allow it one xrun in ten callbacks beside any device's allowance
and a rate tolerance of 10 %, where everything the layer itself must do - the blocks, the channels, the thread, nothing
after the Stop, the loop's samples bit for bit, an xrun made reported - is held as on any device.

**ALSA.** The loopback card's two devices carry one name, `Loopback (Loopback PCM)`, since both PCMs are named so; the
layer lists the second as `Loopback (Loopback PCM) (2)` (a later device sharing a name with an earlier one the same way
carries its ordinal: `AudioDevice.h`'s `NameApart`), so a stream can name playback on one and capture on the other. The capture side of a loopback pair
opens as many channels as its playback side, so the engine's test opens two inputs and no key there.

**Windows Audio.** WASAPI is the suite's own back end (`audio/win/WasapiDevice.h`), two APIs to the suite, one
rig for each (`devices/WasapiRig.cpp`): Windows Audio, shared, event-driven - at the endpoint's mix rate through
`IAudioClient3`'s `InitializeSharedAudioStream` at the driver's smallest period that holds the block, a few
milliseconds where the driver offers small periods, as Microsoft's inbox HD Audio driver does, the engine's ten where it
offers only that, as many vendors' drivers do; at any other rate at the engine's period, the engine converting (until
2026-10-25 the small periods were a third API, "Windows Audio (Low Latency Mode)", which settings now read as Windows
Audio); and its Exclusive Mode, the endpoint the stream's alone,
event-driven at the block asked for, held to the device's minimum period (`GetDevicePeriod`) and its alignment. The
callback is the render endpoint's period in both modes, with no ring or resampler between them. In the shared mode an
input named after a render endpoint is that endpoint's loopback - WASAPI's capture of the mix it plays, opened with
`AUDCLNT_STREAMFLAGS_LOOPBACK` - so its rig hears the stream back through the mixer; an exclusive stream
bypasses the mixer and its rig is output only. WASAPI tells a render stream of no underrun, so the back end reports
an xrun when a period event comes more than two and a half periods after the last - which a stalled callback makes in
every mode - and when the capture ring runs dry or overfills. The rig prints the endpoint's mix format and its
shared-mode periods (the default, the step, the smallest and the largest), so a driver that offers no small period says
so in the table.

The arithmetic that needs no Windows - the encodings and their conversion, the period each mode opens at, the capture
ring and its margin - is `audio/WasapiFormats.h`, which the `audio` suite holds on every platform
(`tests/test_wasapi_formats.cpp`).

## ALSA: an xrun reported, and played on from

ALSA is the toolkit's own back end (`audio/linux/AlsaDevice.cpp`; `AlsaDevice.h` says what it does). Its output buffer
is three periods, its input's at least 4096 frames, and every PCM is opened with its stop threshold at the buffer's
size, ALSA's default: a playback buffer that runs dry, or a capture buffer that fills, stops the device in the `XRUN`
state, and the stream's thread meets it as `-EPIPE` from its next call (or as an error a poll reports). The recovery is
one path whatever the devices are:

- **Both directions are dropped and prepared again**, the playback primed with a buffer of silence and the two started
  - together where they are linked (`snd_pcm_link`: one card's two directions, as the loopback card's are), each on its
  own where they are two cards. A suspend (`-ESTRPIPE`) is resumed where the driver can and recovered so where it
  cannot.
- **The next callback is told** (`xrun` true), and plays on a buffer ahead of the device, as the stream did from its
  start.

The priming is what keeps a stream from reporting xruns it never had. Each pass waits until the input has a block and
the output room for one, and a stream primed full finds room exactly as a block has played, on the input's clock where
the two share one. What a listener hears of an xrun is silence for as long as the device was stopped, then the primed
buffer's silence. The suite holds both ends of it on the loopback card: `CountsAnXrunAndPlaysOnAfterIt` stalls a
callback for 30 ms and asks for the xrun at the next callback, none before it, and the stream at its rate from 300 ms
on, read over two seconds, and through its loop one frame after another to the end.

## ALSA: the stream's thread, and why it never waits inside ALSA

The stream's thread opens its PCMs non-blocking and waits in `poll` - on the side that lacks a block, and on an eventfd
`Stop` writes - for at most two seconds, then reads, calls back and writes without blocking. Nothing holds a lock across
a wait, so `Stop` sets a flag, wakes the poll and joins the thread: it returns as soon as the pass in hand finishes, and
no callback runs after it. A wait that times out is the device gone quiet, and the stream failing. The case against
waiting inside ALSA is "Why a case took a minute, while RtAudio served ALSA", below.

## ASIO: registering the test driver without an administrator

The ASIO back end (`audio/win/AsioDevice.cpp`) lists drivers through the SDK's host code (`host/pc/asiolist.cpp`): each
driver's class from `HKLM\SOFTWARE\ASIO`, its DLL from `HKCR\CLSID\<class>\InprocServer32`, read once as its driver list is built
while the binary loads; a driver is created later with `CoCreateInstance`. A CI runner's account cannot write HKLM, and a
process at the System integrity level - a service's, as the runners' are - is not given COM's per-user classes. So
`devices/AsioRig.cpp` registers the driver for its own process alone:

- **The registry, seen through views.** In the C runtime's library initialisation segment (`#pragma init_seg(lib)`),
  which runs before every ordinary static initialiser and so before the back end's driver list is built, the binary loads an
  application hive (`RegLoadAppKey`: a hive file in the temporary folder, private to the process, needing no privilege),
  writes into it the test driver's entries - and, copied beside them, the machine's own drivers' entries, so a person's
  interface still lists - and points `HKEY_LOCAL_MACHINE` and `HKEY_CLASSES_ROOT` at it for this process with
  `RegOverridePredefKey`. COM is started on the main thread first, so its own start never reads the views. `main` puts the
  two roots back before anything else reads the registry, and the hive's file goes.
- **The class, from an activation context.** Registration-free COM: a manifest naming the DLL and its class
  (`comClass`), activated on the main thread for the process's life, which `CoCreateInstance` consults before the
  registry at any integrity level. The manifest is written beside the DLL, since COM loads a class's DLL from the folder
  of the manifest that names it: a manifest elsewhere sends it looking for the DLL there (`0x8007007E`, module not
  found), whatever directory the context is told its assembly is in. The binary creates the class once through the
  context before any test, and says so in the header it prints.

Nothing is installed, nothing is written to the machine's registry, and no person runs anything on a runner. The
header the suite prints says what the registration did.

The driver is the SDK's sample (`driver/asiosample` with the SDK's COM scaffolding, `common/combase.cpp`,
`common/dllentry.cpp`, `common/register.cpp`), built from the fetched SDK as `reckless_asio_test_driver`, a DLL beside
the suite that only the suite loads - never installed, never shipped, never committed. Two files are the suite's own
(`devices/asio_driver/`): its entry point, handing the C runtime's `DllMain` to the scaffolding's `DllEntryPoint`, and
its clock, in place of the sample's `wintimer.cpp`. The sample's thread sleeps a whole number of milliseconds a block -
5 for 256 frames, whatever the rate - plus the scheduler's quantum, so its blocks come some way off the rate it reports;
the suite's clock keeps an absolute schedule on a high-resolution waitable timer, so the blocks come at the rate the
stream opened at, and the rate by the clock can be judged. The clock also takes the suite's requests
(`reckless_asio_test_driver_request`): a resync request, which the back end reports as an xrun, and a reset request, which a
driver sends when it loses its device or changes its buffer size, and on which the back end tells the stream it failed,
for the engine to close it from the message thread. The sample's waves are computed again by the suite exactly as the driver computes
them - with the sample's own value of pi, 0.3141592654 - and each input sample must be that wave's int16 over 32768.
The driver fills the half of its input buffers it is about to hand over, and the back end reads that half in the same
switch, so the first callback already carries the driver's samples: the check allows no silence before them. (RtAudio
read the half before, and the check allowed it a block.)

What the back end does about the SDK's host code, which RtAudio needed two workarounds for: its driver list is the
SDK's own global, `asioDrivers`, which the SDK's `ASIOExit` releases the current driver through - RtAudio kept a list
of its own and left the global null, so the build's copy of the SDK read through a null pointer on the first probe -
and it closes a stream from the message thread alone, so no close of the driver ever races an open.

## Required, not skipped

A missing test device skips its tests with the reason printed - on a laptop with no JACK, say. A CI job names the APIs
its runner must have in `RECKLESS_AUDIO_REQUIRE` (a comma-separated list of the series' names: `CoreAudio`, `Windows
Audio`, `ASIO`, `JACK`, `ALSA`), and for those a missing device, or an API missing from the build, fails, naming what is
missing. CTest runs the suite alone (`RUN_SERIAL`), since a stream's timing is what it judges.

The toolkit's own workflow (this repository's `.github/workflows/build.yml`) runs it on the three platforms, and
vst-plugins' workflows run it again in its tree, where the toolkit is a subtree:

| job | requires | where |
|---|---|---|
| the toolkit's build.yml `linux` | ALSA, JACK, where the kernel has sound | a hosted runner, after `tools/audio-test-devices.sh` |
| the toolkit's build.yml `windows` | ASIO | either Windows runner |
| the toolkit's build.yml `macos` | CoreAudio | the Mac runner |
| vst-plugins' build.yml `tests` | ALSA, JACK | either Linux runner, after its copy of `tools/audio-test-devices.sh` |
| vst-plugins' build.yml `windows`, `macos` | ASIO, CoreAudio | as above |
| vst-plugins' release.yml `linux`, `windows`, `macos` | the same, on each platform | the package jobs' Test steps |

The toolkit's `coverage` and `minimal` jobs leave the suite out (the coverage floor measures code the suite does not
reach, and `minimal` builds no device layer), and so do vst-plugins' `coverage` and its Linux plugin build, leaving it
to its tests job. Windows Audio is required nowhere: neither Windows runner has a render endpoint.

## No sound in the kernel at all

This repository is public, so its jobs run on GitHub's hosted images, and **an Ubuntu azure kernel is built with no
sound support whatever**: a hosted runner measured zero sound modules in `/lib/modules`, no `/proc/asound`, and no
`CONFIG_SND_ALOOP` in `/boot/config-$(uname -r)`. Launchpad 1970586, "No sound support on Linux AWS/Azure kernels
(but supported on GCP)", is the cause - the option is set for Ubuntu's GCP kernels and not for its AWS and Azure
ones - and no package, `depmod` or module parameter can make up for a module the kernel was never built with.
`linux-modules-extra-$(uname -r)` installs cleanly there and carries nothing matching `aloop`.

So `tools/audio-test-devices.sh` asks what the kernel can do before it asks what is installed. Where there is no
sound at all it says so, sets `RECKLESS_AUDIO_DEVICES=none` and the job leaves the device suites out, running every
other suite as before - Skia, the views, the look, the net and update code, and the window suites under Xvfb. It
installs the ALSA and JACK headers all the same, since compiling needs no sound in the kernel: without them the
configure promises neither API and the toolkit's own ALSA and JACK back ends go uncompiled, a break in either first met
by a product's build - which is how the toolkit's hosted Linux job ran from the repository's going public until
2026-10-25. The
question is about the kernel and not the kind of runner, which keeps the property worth keeping: **a machine whose
kernel has sound and whose loopback card is missing still fails**, because there the provisioning is broken rather
than impossible. vst-plugins is private, runs on the organisation's own machines, and is therefore where the suite
actually exercises a device.

## Why a case took a minute, while RtAudio served ALSA

A case of this suite measures about a second of stream, and under RtAudio's ALSA it could take fifty times that on the
CI machines. Measured on reckless-ci-lin-0, 2026-10-24, with gdb and `/proc/<pid>/task/*/wchan` every eight seconds -
thirteen consecutive samples, ninety-six seconds, identical:

```
37190 waits in futex_do_wait     the main thread, queued on a lock
37310 waits in futex_do_wait     another thread, the same
37328 waits in wait_for_avail    the audio callback thread, inside ALSA
```

RtAudio's `callbackEvent` held `stream_.mutex` across its blocking `snd_pcm_readi` and `snd_pcm_writei`, and
`stopStream` wanted that same mutex: when the loopback card stopped producing or consuming frames, the callback thread
parked in the kernel holding the lock and everything queued behind it. `Stop()` timed at 0.7 s to 48.6 s on one machine
with one binary, the stream it had just measured healthy every time. Ruled out on the way, each with evidence: one bad
machine, `snd_pcm_drain` on stop, dropping the handles before taking the mutex (`snd_pcm_drop` does not wake a thread
already inside `wait_for_avail`), CPU speed, contention between CI jobs, device enumeration, and making or tearing down
the rig. WirePlumber holding the loopback card's control device contributed (stopping it took one case from 48.6 s to
26.3 s) without causing it.

The toolkit's own back end waits in a bounded `poll` instead (above). The same cases on jonny-precision, 2026-10-25,
RtAudio's ALSA first and the toolkit's after, one binary of each:

| case | RtAudio | the toolkit's |
|---|---|---|
| `RunsAStreamAtTheRateAndTheBlockItOpenedAt` | 1.56 s | 1.35 s |
| `CarriesTheSamplesALoopBringsBack` | 14.1 s | 1.23 s |
| `RefusesADeviceThatIsNotThereAndOpensTheNextOne` | 7.52 s | 0.38 s |
| `CountsAnXrunAndPlaysOnAfterIt` | 14.2 s | 2.84 s |
| the loop's latency, 256 frames at 48 kHz | 1280 samples (26.7 ms), 0 reported | 768 (16.0 ms), 512 reported |

A stream on PipeWire's `default` device, duplex, stopped in 9.6 s and 17.9 s under RtAudio and in under a millisecond
under the toolkit's. Keep the sound server off the test card on every runner all the same, and measure the way this was
measured: the suite is `RUN_SERIAL`, so run it with nothing else on the machine, and check
`/proc/asound/card*/pcm*/sub0/status` reads `closed` before starting - a run that was killed leaves a substream behind
and the next run inherits it.

**The loopback card's own clock** is the kernel's timer tick - 1 ms at `CONFIG_HZ=1000`, 48 frames at 48 kHz - so the
card cannot carry a period much under two ticks: at 32 frames it runs at a few percent of its rate, and at 48 and 64 it
underruns once a tick falls late. aplay shows the same on it (an I/O error at 32, underruns at 64), so a small period
is measured on a real card or PipeWire's `default`, never here; the suite's 256 frames sit well clear.

## What each runner needs

- **reckless-ci-lin-0 and reckless-ci-lin-1**: the ALSA and JACK headers, `jackd` (`jackd2`), `setfacl` (`acl`), the
  loopback card loaded and its nodes open to the job's user. `tools/audio-test-devices.sh` checks each by name and
  provides it with the runner's passwordless sudo - lin-0's `runner` is not in the `audio` group, so it gives that user
  an ACL entry on the loopback card's own nodes, each run, since the nodes are made afresh when the module loads - or
  stops with the command for a person. The card stays loaded until the machine restarts.
- **reckless-ci-win-0 and reckless-ci-win-1**: nothing - the ASIO test driver registers itself in-process.
- **reckless-ci-mac-0**: nothing - its default output, the Mac mini's speakers, written with silence.

## Running it by hand

Any platform, in the toolkit configured on its own (the `tests` preset, nothing windowed):

```
cmake --preset tests
cmake --build --preset tests --target reckless_audio_device_test
build/tests/reckless_audio_device_test                  # what this machine has; the rest skip, saying why
RECKLESS_AUDIO_REQUIRE=CoreAudio build/tests/reckless_audio_device_test   # a missing device fails instead
```

In vst-plugins' tree the suite is `build/tests/external/reckless-toolkit/reckless_audio_device_test`, the paths below
moved the same way.

- **macOS**: the default output plays silence for a few seconds; nothing asks for the microphone. With an interface,
  see the next section.
- **Linux**: run `tools/audio-test-devices.sh` once (it loads the loopback card and opens its nodes to you) and the
  suite with `RECKLESS_AUDIO_REQUIRE=ALSA,JACK`. On a desktop whose libjack is PipeWire's (`pipewire-jack` with its
  `ld.so.conf` entry), the suite's own server is out of reach and the JACK tests say so - "a server that is not this
  test's"; run them with JACK's own library first on the path,
  `LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu build/tests/reckless_audio_device_test`. PipeWire, where it runs,
  lists the loopback card among its devices too, and opens it only while something plays to it.
- **Windows**, inside the Visual Studio developer environment (`CLAUDE.md`'s `VsDevCmd.bat` line): ASIO needs
  nothing. **Windows Audio is tested here and nowhere else**, in both of its modes - on a machine with speakers,
  headphones or a virtual cable as its default playback device:

  ```
  set RECKLESS_AUDIO_REQUIRE=Windows Audio,Windows Audio (Exclusive Mode)
  build\tests\reckless_audio_device_test.exe --gtest_filter=*WindowsAudio*
  ```

  It plays a second or two of quiet noise (30 dB under full scale) through the default playback device in each mode,
  the exclusive one taking the device from every other program for its tests. Look for: the test device's table naming
  the endpoint, its mix format and its shared-mode periods; each stream's block asked and opened - the driver's period
  that holds it in the shared mode at the mix rate (the engine's where the driver offers no smaller), the block asked
  (or the device's minimum) in the exclusive mode - and its rate by the clock within 3 % of the mix rate; the loop's
  correlation at least 0.9 in the shared mode (a muted endpoint, or another sound playing over the test, fails it); a stall reported as an xrun; and nothing
  allocated in the engine's callbacks. vst-plugins' command-line player shows a mode on a device of one's choosing -
  its block, latency and xruns, a duplex stream's among them: `reckless_standalone_cli gate --api "Windows Audio
  (Exclusive Mode)" --output "<a render endpoint>" --input "<a capture endpoint>|none" --block 128`.

## On a Mac with an interface

CI's Mac is a virtual machine with one virtual output, so a Mac's inputs, a duplex stream, an aggregate, a real
converter and a real keyboard are tested by a person, with three variables the CoreAudio rig reads (CI sets none of
them):

| variable | what it asks for |
|---|---|
| `RECKLESS_COREAUDIO_DEVICE` | the output device, by its name as CoreAudio lists it (`BEHRINGER: UMC1820`), in place of the default output |
| `RECKLESS_COREAUDIO_LOOP` | `<output>:<input>`, counted from 1: the first of two outputs patched by cable to two inputs of the device - `3:1` for outputs 3 and 4 into inputs 1 and 2 - which makes the test device's loop a cable's, so the stream, loop, silence and xrun tests run duplex through it |
| `RECKLESS_COREAUDIO_INPUT` | an input device of another name (`miniDSP: UMIK-2`): the stream duplex across the two, joined in the back end's private aggregate |

Any platform's rig takes two more, a rate and a block over the ones it chose, so an interface is held at each it
offers: `RECKLESS_AUDIO_TEST_RATE` and `RECKLESS_AUDIO_TEST_BLOCK`. And `RECKLESS_MIDI_KEYBOARD`, on any platform,
names a MIDI input for a person to play (`MidiInputs.CarryWhatAPersonPlaysOnAKeyboard`).

```
export RECKLESS_AUDIO_REQUIRE=CoreAudio RECKLESS_COREAUDIO_DEVICE="BEHRINGER: UMC1820" RECKLESS_COREAUDIO_LOOP=3:1
build/tests/reckless_audio_device_test --gtest_filter='*CoreAudio*'
for rate in 44100 48000 88200 96000; do for block in 32 64 128 256 512 1024; do
    RECKLESS_AUDIO_TEST_RATE=$rate RECKLESS_AUDIO_TEST_BLOCK=$block \
        build/tests/reckless_audio_device_test --gtest_filter='*CoreAudio*' | tee "umc-$rate-$block.log"
done; done
RECKLESS_COREAUDIO_INPUT="miniDSP: UMIK-2" build/tests/reckless_audio_device_test --gtest_filter='*CoreAudio*'
RECKLESS_MIDI_KEYBOARD="Impact LX88+ MIDI1" build/tests/reckless_audio_device_test --gtest_filter='MidiInputs.*Person*'
```

The cable carries quiet noise, 30 dB under full scale, on the patched outputs alone; set the inputs' gain so it comes
back well over the noise floor and well under clipping (about -2 dB through a UMC1820 at its lowest gain). The
terminal the suite runs in needs the microphone (System Settings, Privacy and Security): an input it is refused records
exact silence rather than failing, which the loop then fails on. A Mac with its lid shut and a display attached hands
the built-in microphone's input over as silence too.

Look for, in each run: the loop's delay through the cable against two blocks and the latency the API reports; both
channels' 64-tap fit at 0.9997 or better; every burst through silence at the first one's delay to a hundredth of a
frame; no xruns, but for the one the xrun test makes; and the stream test's input level that of the device asked
for - a measurement microphone's room, not an idle input's hiss.

What it found on 2026-10-25 - an M1 Pro on macOS 27, a UMC1820 cabled from outputs 3 and 4 to inputs 1 and 2, a
UMIK-2 and a Nektar Impact LX88+ - is in [native-backends.md](native-backends.md): every rate and block clean, the round
trip within 0.8 ms of what the driver reports, the aggregate's input the UMIK-2's, every name RtAudio's and RtMidi's,
and three faults the run found and the back end fixed.

