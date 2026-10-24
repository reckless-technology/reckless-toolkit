# The audio layer on the real APIs

The audio layer (`audio/`) reaches the machine's interfaces through RtAudio: `RtAudioDevice.cpp` is the suite's
`AudioSystem` on CoreAudio, Windows Audio (WASAPI, the suite's own in three modes), ASIO, JACK and ALSA. Two suites hold it. `audio`
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
| `ListsADeviceNamedOutsideAsciiInUtf8` | CoreAudio alone: a device of the test's own - an aggregate of the default output, private to the test's process - named with an apostrophe and a letter past ASCII, as macOS names a person's earphones, found by that name in UTF-8 (RtAudio lists it after its maker's); RtAudio reads CoreAudio's names into UTF-8 only with `UNICODE` defined, and in Mac OS Roman otherwise. |
| `RunsAStreamAtTheRateAndTheBlockItOpenedAt` | A stream opened at a rate the device lists and run for 1.2 s: the rate, the block and the channels it opened; the callbacks counted against what the rate and the block predict from the first callback to the Stop (within the device's rate tolerance and two); the rate read by the clock past the first 200 ms, where an API may call back in a burst to fill its buffers (within the device's tolerance: 3 %, and 10 % for JACK's dummy driver, below); every callback carrying the block; every channel pointer where `RtAudioDevice` lays it out (each channel's frames after the one before's); the callbacks on the API's thread, not the caller's; the xruns, at most two plus one per hundred callbacks, and on a device whose clock the machine schedules one in ten more; nothing calling back once `Stop` has returned - which the layer keeps on every API, each callback counted in and out and silence once the stream is stopped, `Stop` waiting until none is under way. The latency the API reports is printed. |
| `CarriesTheSamplesALoopBringsBack` | Where the device loops its output back (below), a known signal out and the samples read back: bit for bit where the path is exact, correlated where a mixer sits in it, exactly the device's own signal where the device makes one. |
| `PlaysACoreThroughTheEngineAllocatingNothingInTheCallback` | `StandaloneEngine` playing the Gate's core on the device for a second, `Idle` every 10 ms: running throughout, its callbacks as the rate and the block predict (less, for each xrun the run is allowed, the callbacks of the 300 ms a device has to start again), and not one allocation on the audio thread - the engine's callback runs between two counts of the thread's allocations (`CountAllocations.h`, through an `AudioSystem` that relays the callback), so the count is the engine's and the core's, on the API's own thread. |
| `RefusesADeviceThatIsNotThereAndOpensTheNextOne` | An output and an input that do not exist each refused, the error naming them, and the test device opening and running afterwards. |
| `CountsAnXrunAndPlaysOnAfterIt` | An overrun or underrun reaching the callback - made by a callback that runs two blocks or 30 ms, whichever is longer (ALSA, JACK, CoreAudio), or reported by the device when the rig asks (ASIO) - with none reported before it (on a scheduled clock, at most one in ten callbacks), and the stream playing on after it as a stream from its start does: over the two seconds from 300 ms after the callback told of it - a window long enough that a loaded runner's moment of slowness as it resumes the device is a small part of it, where a stream that never resumes its rate is as far off as over any - the callbacks as the rate and the block predict (within the device's rate tolerance and two), the rate by the clock, the xruns a stream is allowed (at most two plus one per hundred callbacks, one in ten more on a scheduled clock), and the callbacks running until the stream is stopped; through an exact loop (ALSA, JACK), every sample back one that was played and the last 0.3 s of them one frame after another. Windows Audio skips it, saying why. |
| `AStreamWhoseDeviceGoesAwayFailsAndIdleClosesIt` | Where the rig can take the device away under a running engine (JACK, ASIO): the failure reaching the engine from the API's thread, `Idle` closing the stream and saying why, the state failed, no callback after; then the device back and the next `Open` preparing the core again and playing. |

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
| CoreAudio | the default output, written with silence | the machine (the Mac runner's speakers) | none: an input would ask the person at the machine for the microphone | a stalled callback | - |
| Windows Audio, and its Low Latency Mode | the default render endpoint, heard back by WASAPI's loopback capture | the machine (none on CI) | correlated | a stalled callback | - |
| Windows Audio (Exclusive Mode) | the default render endpoint, the stream's alone | the machine (none on CI) | none: no mixer for a loopback to hear | a stalled callback | - |
| ASIO | the SDK's sample driver: 16 in, 16 out, 16-bit, 256 frames, 44.1 or 48 kHz, a sine on its first input and a sawtooth on its second | the suite: built from the fetched SDK and registered by the binary itself | the inputs, exactly as the driver makes them | the driver's resync request | the driver's reset request |
| JACK | `jackd` on the dummy driver, 48 kHz, 1024 frames, 3 capture and 5 playback ports, under a name of its own | the suite: a server for each test | exact: the stream's outputs wired to its own inputs through the server | a stalled callback | the server stopped with SIGTERM, then started again |
| ALSA | the kernel's loopback card (`snd-aloop`): playback on its device 0 comes back as capture on its device 1 | the CI job (`tools/audio-test-devices.sh`) | exact | a stalled callback, RtAudio patched to report it (below) | - |

**JACK.** `devices/JackRig.cpp` starts `jackd --no-realtime --name reckless-test-<pid>-<n> -d dummy -r 48000 -p 1024 -C
3 -P 5` for each test and stops it after, with `JACK_DEFAULT_SERVER` naming it and `JACK_NO_START_SERVER` set, so every
client of the process reaches that server and none starts another; the child is forked with `PR_SET_PDEATHSIG`, so it
dies with the suite however the suite ends. Three capture and five playback ports are a signature no desktop's server
has: a test that finds them knows it reached this server and not a PipeWire graph. The loop is wired through the same
libjack, loaded by name, that RtAudio uses (`JackLoader.cpp`). The self-connection makes a feedback edge in the graph,
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
carries its ordinal), so a stream can name playback on one and capture on the other. The capture side of a loopback pair
opens as many channels as its playback side, so the engine's test opens two inputs and no key there.

**Windows Audio.** WASAPI is the suite's own back end (`audio/win/WasapiDevice.h`), three APIs to the suite, one
rig for each (`devices/WasapiRig.cpp`): Windows Audio, shared, event-driven at the engine's period; its Low Latency Mode,
shared through `IAudioClient3`'s `InitializeSharedAudioStream` at the driver's smallest period that holds the block -
a few milliseconds where the driver offers small periods, as Microsoft's inbox HD Audio driver does, the engine's ten
where it offers only that, as many vendors' drivers do; and its Exclusive Mode, the endpoint the stream's alone,
event-driven at the block asked for, held to the device's minimum period (`GetDevicePeriod`) and its alignment. The
callback is the render endpoint's period in every mode, with no ring or resampler between them. In the two shared modes
an input named after a render endpoint is that endpoint's loopback - WASAPI's capture of the mix it plays, opened with
`AUDCLNT_STREAMFLAGS_LOOPBACK` - so those two rigs hear their stream back through the mixer; an exclusive stream
bypasses the mixer and its rig is output only. WASAPI tells a render stream of no underrun, so the back end reports
an xrun when a period event comes more than two and a half periods after the last - which a stalled callback makes in
every mode - and when the capture ring runs dry or overfills. The rig prints the endpoint's mix format and its
shared-mode periods (the default, the step, the smallest and the largest), so a driver that offers no small period says
so in the table.

The arithmetic that needs no Windows - the encodings and their conversion, the period each mode opens at, the capture
ring and its margin - is `audio/WasapiFormats.h`, which the `audio` suite holds on every platform
(`tests/test_wasapi_formats.cpp`).

## ALSA: an xrun reported, and played on from

RtAudio 6.0.1 opens every ALSA PCM with its stop threshold at `ULONG_MAX`, past the boundary: a playback buffer that runs
dry plays the silence RtAudio has ALSA fill it with, a capture buffer that fills is written over, and the device runs on
as if nothing had happened. RtAudio's own handling of an xrun - the `-EPIPE` branches of its read and write, which flag
`RTAUDIO_INPUT_OVERFLOW` or `RTAUDIO_OUTPUT_UNDERFLOW` to the next callback and prepare the device again - never ran, and
a callback stalled for 30 ms, past the 21 ms of a 4 x 256-frame buffer at 48 kHz, was reported as nothing. The build
patches it as the tarball unpacks, beside the ASIO fix (`cmake/PatchRtAudioAlsaXruns.cmake`, each change marked in the
code; the next section's patch follows it):

- **The stop threshold is the buffer's size**, ALSA's default and the one aplay keeps: a playback buffer that runs dry,
  or a capture buffer that fills, stops the device in the `XRUN` state; the next write or read fails with `-EPIPE`; and
  RtAudio's existing branches flag the xrun to the next callback and prepare the device. The silence filling stays: it
  keeps what a playback has played silent behind it.
- **A duplex stream's playback is primed with a buffer of silence** before the stream starts, and again whenever an xrun
  has stopped and prepared it. A stream that reads and writes is paced by its capture - each pass reads a period, which
  waits for one, and writes a period, which finds room - so its playback keeps the level it started at. Started on its
  first write's one period, as RtAudio starts a playback, it would run dry at the next period's end, and every period
  after: an xrun a period. Primed, it starts a buffer ahead of the device, the latency the release had, whose
  free-running playback landed its late writes a buffer on.
- With the priming, **a capture linked to its playback** (`snd_pcm_link`: one card's two directions started and stopped
  as one - the loopback card's are) finds the pair running at its first read. Its read would otherwise try to start the
  pair with nothing to play, which a stop threshold under the boundary refuses with `-EPIPE`.
- **An overrun is named**, as an underrun already was: RtAudio's warning for it carried whatever error came before -
  which `RtAudioDevice`, reading a warning's text for a disconnection ("No such device"), could take for one.

The recovery, path by path - the same on the loopback card and on a real one, the card's clock in place of the
loopback's timer:

- **Playback alone:** the write that finds the device stopped fails, its block is dropped, the device is prepared, and
  the next write starts it again (RtAudio's start threshold is one period); the writes after find room and refill the
  buffer at once, nothing pacing them but the device. ALSA's ordinary recovery, as aplay's.
- **Capture alone:** the read that finds the device stopped fails, the device is prepared, and the next read starts it
  again.
- **Duplex, linked:** an xrun either way stops both directions. The next read fails, both are prepared, the playback is
  primed - which starts both - and the next pass runs a buffer ahead of the device, as the stream did from its start.
  The block read last before the xrun is handed to the callback once more, the read that would have replaced it having
  failed.
- **Duplex, two cards:** each direction stops and starts on its own; the playback is primed whenever it is prepared.

On a real card a stop, a prepare and a start are a few register writes, a USB interface's a restart of its streams, a
few milliseconds more. What a listener hears is silence for as long as the device was stopped, and on a duplex stream the
primed buffer's silence after it. The suite holds both ends of it on the loopback card: `CountsAnXrunAndPlaysOnAfterIt`
stalls a callback for 30 ms and asks for the xrun at the next callback, none before it - a playback started on one
period would report one at its first periods - and the stream at its rate from 300 ms on, read over two seconds, and
through its loop one frame after another to the end.

## ALSA: the stream's state shared between its threads

RtAudio 6.0.1's ALSA back end reads the stream's state on its callback thread without the stream's mutex - at the top of
every pass, to decide whether to wait for a start - while `RtApi::openStream`, after the thread has started, writes it,
and `stopStream` and `abortStream` write it just before they take the mutex; `closeStream` clears the flag the thread
loops on as a plain `bool` the thread reads unlocked. ThreadSanitizer reports the first as a data race in
`RtApiAlsa::callbackEvent`. The build patches it as the tarball unpacks, after the xrun patch
(`cmake/PatchRtAudioAlsaThreads.cmake`, each change marked in the code):

- **The callback thread reads the state under the mutex** every pass, waiting on the condition variable as before while
  the stream is stopped, and unlocks before the program's callback runs, as the release does.
- **Every write of the state holds the mutex** once the thread runs: `openStream`'s, `stopStream`'s and
  `abortStream`'s now take it first; `startStream` already did; `closeStream` writes after joining the thread.
- **The run flag is atomic:** `closeStream` clears it with an atomic store, and the thread's loop reads it with an atomic
  load.

`openStream` is the one change common to every back end: its write takes a mutex `RtApi`'s constructor initialises
before any stream is opened. A product's ThreadSanitizer build needs no suppression for RtAudio.

## ASIO: registering the test driver without an administrator

RtAudio lists ASIO drivers through the SDK's host code (`host/pc/asiolist.cpp`): each driver's class from
`HKLM\SOFTWARE\ASIO`, its DLL from `HKCR\CLSID\<class>\InprocServer32`, read once as RtAudio's static driver list is built
while the binary loads; a driver is created later with `CoCreateInstance`. A CI runner's account cannot write HKLM, and a
process at the System integrity level - a service's, as the runners' are - is not given COM's per-user classes. So
`devices/AsioRig.cpp` registers the driver for its own process alone:

- **The registry, seen through views.** In the C runtime's library initialisation segment (`#pragma init_seg(lib)`),
  which runs before every ordinary static initialiser and so before RtAudio's driver list is built, the binary loads an
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
(`reckless_asio_test_driver_request`): a resync request, which RtAudio reports as an xrun, and a reset request, which a
driver sends when it loses its device or changes its buffer size, and on which RtAudio closes the stream on a thread of
its own and reports the disconnection. The sample's waves are computed again by the suite exactly as the driver computes
them - with the sample's own value of pi, 0.3141592654 - and each input sample must be that wave's int16 over 32768.
RtAudio hands a callback the input of the block before, so the first callback's input is silent: the check allows that
one block, and no more.

Two things `RtAudioDevice.cpp` does on ASIO, without which a real driver does not work through the layer:

- **It gives the SDK's `ASIOExit` a driver list to ask.** `ASIOExit` releases the current driver through the SDK's
  global driver list, `asioDrivers`, which only the SDK's `loadAsioDriver` sets - RtAudio keeps a list of its own and
  never calls it - and it does not check the pointer. RtAudio's bundled copy of the host code checks it; the build
  compiles the SDK's own, so without a list the first probe of any ASIO driver reads through a null pointer and the
  process dies of an access violation. The layer gives the global an empty list of its own as it makes the ASIO system:
  `ASIOExit` asks it to release no driver, and the release is RtAudio's, through its own list, as with RtAudio's copy.
- **It lets RtAudio finish with a driver before the next open.** RtAudio's ASIO back end marks a stream closed before it
  has released the driver - its closing thread then disposes of the buffers, exits the driver and removes it - so after
  a stream the API closed itself, `RtAudioDevice` asks RtAudio to probe its drivers until it no longer answers that a
  stream is open, before it lets the next open begin.

## Required, not skipped

A missing test device skips its tests with the reason printed - on a laptop with no JACK, say. A CI job names the APIs
its runner must have in `RECKLESS_AUDIO_REQUIRE` (a comma-separated list of the series' names: `CoreAudio`, `Windows
Audio`, `ASIO`, `JACK`, `ALSA`), and for those a missing device, or an API missing from the build, fails, naming what is
missing. CTest runs the suite alone (`RUN_SERIAL`), since a stream's timing is what it judges.

The toolkit's own workflow (this repository's `.github/workflows/build.yml`) runs it on the three platforms, and
vst-plugins' workflows run it again in its tree, where the toolkit is a submodule:

| job | requires | where |
|---|---|---|
| the toolkit's build.yml `linux` | ALSA, JACK | either Linux runner, after `tools/audio-test-devices.sh` |
| the toolkit's build.yml `windows` | ASIO | either Windows runner |
| the toolkit's build.yml `macos` | CoreAudio | the Mac runner |
| vst-plugins' build.yml `tests` | ALSA, JACK | either Linux runner, after its copy of `tools/audio-test-devices.sh` |
| vst-plugins' build.yml `windows`, `macos` | ASIO, CoreAudio | as above |
| vst-plugins' release.yml `linux`, `windows`, `macos` | the same, on each platform | the package jobs' Test steps |

The toolkit's `coverage` and `minimal` jobs leave the suite out (the coverage floor measures code the suite does not
reach, and `minimal` builds no device layer), and so do vst-plugins' `coverage` and its Linux plugin build, leaving it
to its tests job. Windows Audio is required nowhere: neither Windows runner has a render endpoint.

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

- **macOS**: the default output plays silence for a few seconds; nothing asks for the microphone.
- **Linux**: run `tools/audio-test-devices.sh` once (it loads the loopback card and opens its nodes to you) and the
  suite with `RECKLESS_AUDIO_REQUIRE=ALSA,JACK`. On a desktop whose libjack is PipeWire's (`pipewire-jack` with its
  `ld.so.conf` entry), the suite's own server is out of reach and the JACK tests say so - "a server that is not this
  test's"; run them with JACK's own library first on the path,
  `LD_LIBRARY_PATH=/usr/lib/x86_64-linux-gnu build/tests/reckless_audio_device_test`. PipeWire, where it runs,
  lists the loopback card among its devices too, and opens it only while something plays to it.
- **Windows**, inside the Visual Studio developer environment (`CLAUDE.md`'s `VsDevCmd.bat` line): ASIO needs
  nothing. **Windows Audio is tested here and nowhere else**, in all three of its modes - on a machine with speakers,
  headphones or a virtual cable as its default playback device:

  ```
  set RECKLESS_AUDIO_REQUIRE=Windows Audio,Windows Audio (Low Latency Mode),Windows Audio (Exclusive Mode)
  build\tests\reckless_audio_device_test.exe --gtest_filter=*WindowsAudio*
  ```

  It plays a second or two of quiet noise (30 dB under full scale) through the default playback device in each mode,
  the exclusive one taking the device from every other program for its tests. Look for: the test device's table naming
  the endpoint, its mix format and its shared-mode periods; each stream's block asked and opened - the engine's period
  in the shared mode, the driver's in the low-latency mode, the block asked (or the device's minimum) in the exclusive
  mode - and its rate by the clock within 3 % of the mix rate; the loop's correlation at least 0.9 in the two shared
  modes (a muted endpoint, or another sound playing over the test, fails it); a stall reported as an xrun; and nothing
  allocated in the engine's callbacks. vst-plugins' command-line player shows a mode on a device of one's choosing -
  its block, latency and xruns, a duplex stream's among them: `reckless_standalone_cli gate --api "Windows Audio
  (Exclusive Mode)" --output "<a render endpoint>" --input "<a capture endpoint>|none" --block 128`.
