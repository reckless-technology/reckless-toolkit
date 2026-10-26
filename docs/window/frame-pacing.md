# How a window paces its frame callback

A child window runs its owner's frame callback on a timer of the platform's own, and has done since the toolkit
began: `SetTimer(hwnd, id, 16, nullptr)` on Windows, an `NSTimer` at a sixtieth on macOS, a 16 ms tick on the app
loop on Linux. Every one of those asks for a **period in milliseconds**, and a period in milliseconds has no
relationship to the panel in front of the person. That is the whole fault, and on Windows it is worse than that: the
period asked for is not even the period delivered.

So a window now carries an **opt-in display clock** beside the platform's timer, and a product that needs uniform
motion asks for it. What it asks for is **an integer subdivision of the display's actual refresh rate, not a target
frequency**: a frame on every Nth refresh, so that every frame drawn is shown for exactly N refresh periods.

## What was measured, and why 16 ms was the worst number in the range

Measured by an audit on a Windows desk (an RTX 4070 laptop, then on a **2560 x 1600 panel at 240 Hz**; the harness
below, `tools/frame-pacing-harness.cpp`, retakes all of it, and "What this measured here" is the retaking). Today's
mechanism, `SetTimer` at 16 ms with the raster written and blitted every frame, over 3000 frames; the last column
counts a frame as whole where it is within a tenth of a refresh period of whole, which on a 240 Hz panel is a safe
tolerance and on a 60 Hz one is not (see that section):

| | mean | rate | sd | frames lasting a whole number of refresh periods |
|---|---|---|---|---|
| **`SetTimer(16)` - the platform timer** | **26.861 ms** | **37.23 Hz** | **7.132 ms** | **3.1 %** |
| the same with `timeBeginPeriod(1)` held | 26.700 ms | 37.45 Hz | 7.349 ms | - |
| the same with nothing drawn and nothing invalidated | 26.950 ms | 37.11 Hz | 7.08 ms | - |
| `SetTimer(15)` | 16.806 ms | 59.50 Hz | 4.459 ms | 3.2 % |
| `DwmFlush` every vertical blank | 4.176 ms | 239.47 Hz | 0.327 ms | yes, by construction |
| a high-resolution waitable timer at 16.6667 ms | 16.666 ms | 60.001 Hz | 0.280 ms | 94.5 % |
| **the same, snapped to the vertical blank grid** | **16.666 ms** | **60.002 Hz** | **0.234 ms** | **95.1 %** |

Two shipped products, timed end to end rather than in a harness, agree: **Knect 4 1.1.4 at 39.94 Hz** and **Reckless
Sudoku 1.0.4 at 37.76 Hz**. The third control above is the one that matters for diagnosis - with nothing drawn and
nothing invalidated the rate is the same, so the frame work is not the cause. The timer is.

### The `WM_TIMER` mechanism

Two obvious models of `SetTimer(16)` both predict the wrong answer. A **drift** model - due times advancing 16 ms
against a 15.625 ms clock - predicts about 62.5 Hz. A **re-arm on expiry** model predicts a flat 32 Hz. The
measurement is neither: it is a mixture, 26 % of intervals one clock tick long, 68 % two and 6 % three. Four
measurements settle what is actually happening.

**One: the grid is the system clock's, and `timeBeginPeriod` does not move it.** `GetSystemTimeAdjustment` reports
the clock's increment in force as **15.6252 ms**, and every `WM_TIMER`'s own `GetMessageTime` is exactly
`floor(15.625 k)` - 335597031, 335597062, 335597078, 335597109, 335597125 - so the messages are generated at the
clock's tick instants and nowhere else. `timeBeginPeriod(1)` raises the *timer* resolution, which
`NtQueryTimerResolution` duly reports as 1.0 ms; it leaves the clock's increment at 15.6252 ms, and the measured rate
with the period held is 38.78 Hz against 37.97 Hz without it. **That is why `timeBeginPeriod` does nothing for
`SetTimer`** - the two are different clocks.

**Two: the schedule is re-armed at each expiry, not absolute.** A handler that burns 20 or 25 ms of processor before
the next message is taken leaves the mean at 26.7-27.0 ms; an absolute schedule would have become handler-bound at
20-25 ms. And calling `SetTimer` again by hand from the handler - an explicit re-arm from the retrieval - reproduces
the periodic timer's figure exactly at no burn (27.3 ms against 26.6 ms) while diverging to 43.5 ms at a 20 ms burn,
which places the periodic timer's reference at the **expiry instant**, not at the retrieval and not on an absolute
schedule. **So there is no drift, and 62.5 Hz was never reachable through `SetTimer` at any period.** The most it can
give is one tick a frame: 15.625 ms, 64 Hz.

**Three: the rate is a staircase in the scans a period takes.** Sweeping the period asked for, 300 frames each, gives
the mean interval in units of the 15.625 ms grid:

| asked for | 1-14 ms | **15** | **16** | 17-30 | **31** | **32** | 33-46 | **47** | 48 |
|---|---|---|---|---|---|---|---|---|---|
| grid scans a frame | 1.00-1.02 | 1.09 | **1.74** | 2.00-2.05 | 2.26 | **2.80** | 3.00-3.11 | 3.53 | 4.02 |
| rate | 63-64 Hz | 58.9 Hz | **36.8 Hz** | 31.2-31.9 Hz | 28.3 Hz | 22.9 Hz | 21.1 Hz | 18.1 Hz | 15.9 Hz |

The staircase is `ceil(period / 15.625)` scans, as the re-arm model says. The interesting values are the ones at the
step edges - 15 and 16, 31 and 32, 47 - where the mean is a *mixture* of two step heights rather than either.

**Four: the mixture is a one-millisecond ambiguity band at each step edge, and 16 sits in one.** The due time is
carried in whole milliseconds of tick count while the grid's step is 15.625 ms, so the grid's tick-count values
advance 16, 15, 16, 16, 15, 16, 16, 15 and a request of 16 ms is satisfied in one scan or in two depending on where
in that cycle the expiry fell - and which way the boundary case goes varies. The resulting sequence of one-scan and
two-scan intervals is **not periodic**: its repeat rate at every lag from 1 to 128 is 0.49 to 0.66 against a chance
level of about 0.53. It is a frame-by-frame coin toss.

That is the whole answer, and it has a blunt consequence. **`kFrameMilliseconds = 16` is the worst value in the range
1 to 30.** One millisecond less is 59 Hz, two less is 63 Hz, and two more is a clean 31.7 Hz; 16 is the first value
past the one-scan boundary of 15.625, inside the one-millisecond band where it cannot make up its mind. On a 60 Hz
panel each frame is then shown for one refresh or two, alternating irregularly; on the 240 Hz panel the audit
measured, for six or seven. **That alternation is the stutter, more than the rate is.**

It is also not a thing to fix by changing the number. `kFrameMilliseconds` stays 16, because three products have
shipped on it and because 15 would be a 60 % rise in the rate of per-frame processor work for every one of them -
raster cost here is per pixel, and a 3840 x 2160 window is about 5.4 ms a frame of memory traffic before Skia draws
anything.

## The design: a divisor, not a frequency

The property that matters is **uniformity, not rate**: every frame drawn is shown for the same whole number of
refresh periods. A target frequency cannot express that, because it has no relationship to the panel - 16.6667 ms is
1 refresh period at 60 Hz, 2 at 120, 2.4 at 144 and 4 at 240, and on three of those four it is not whole. A divisor
can express it on every panel at once:

| the product asks for | 60 Hz panel | 120 Hz | 144 Hz | 240 Hz |
|---|---|---|---|---|
| N = 1 | 60 frames a second | 120 | 144 | 240 |
| N = 2 | 30 | 60 | 72 | 120 |
| N = 4 | 15 | 30 | 36 | 60 |

A product that wants half rate for something cheap - a meter, a background pane - asks for twice its divisor and gets
uniform motion at half the cost. A halved *frequency* cannot promise that on a panel whose rate it does not know.

**Every number is the product's.** The toolkit carries no rate, no divisor and no patience of its own beyond the
platform timer it has always run on; `FrameRequest{}` asks for exactly what a window did before any of this existed.
Nothing here knows a product.

## The seam

`window::ChildWindow::SetFramePacing` and `window::ChildWindow::FramePacingNow`
([window/Window.h](../../window/Window.h)) are the pair, both new virtuals with default bodies that answer "the
platform's timer" - so a window that does not implement them, and a product that does not call them, are exactly what
they were. [window/FramePacing.h](../../window/FramePacing.h) is everything they hand over.

An app on the shell reaches its child window through `app::AppRunHooks::on_child`
([app/AppRun.h](../../app/AppRun.h)), as the raw keys do ([raw-keys.md](raw-keys.md)):

```cpp
reckless::app::AppRunHooks hooks;
hooks.on_child = [&](reckless::window::ChildWindow &child) {
    // This product's numbers: a frame every second refresh, and one frame of lateness made up.
    const reckless::window::FramePacingState pacing =
        child.SetFramePacing({reckless::window::FramePacing::kDisplayClock, 2, 1});
    std::printf("paced at %.2f frames a second - %s\n", pacing.frames_a_second,
                reckless::window::TextOf(pacing.why));
};
```

A plug-in's editor, which makes its own child window, calls it on the window `window::CreateChildWindow` gave it.

What a product asks for is a `window::FrameRequest`:

| Member | What it is |
|---|---|
| `pacing` | `kPlatformTimer` (the default: unchanged) or `kDisplayClock` |
| `refresh_divisor` | **N**: the refresh periods each frame drawn is shown for, 1 or more |
| `most_frames_behind` | whole frames of lateness the clock will make up before it starts again |

What it is told is a `window::FramePacingState`: the pacing actually in force, the divisor, the display's rate, the
frames a second that comes to, and `why` - a `window::FramePacingFallback` - where it fell back. `frames_a_second` is
**0 on the platform timer**, because the platform timer's rate is not a number the window knows: it is the
platform's, it is not the period asked for, and on Windows it is not even steady.

## What the window promises

- **Nothing until it is asked.** A window whose owner has not called `SetFramePacing` reads no display clock, starts
  no thread, makes no link and is paced exactly as it always was. On Windows the frame callback runs from `WM_TIMER`
  on `SetTimer(hwnd_, kFrameTimer, kFrameMilliseconds, nullptr)`, on macOS from the `NSTimer` at a sixtieth, on Linux
  from the app loop's own tick - the same calls, in the same order, as before this existed.
- **The callback runs on the window's own thread.** Where the waiting needs a thread, the thread is the window's own
  and it hands each frame to the window's loop - a message on Windows, the main queue on macOS - so nothing a
  callback touches moves to another thread, and a plug-in's editor inside a host's loop is served as well as an app
  on its own loop. `AppPlatform::Run` is untouched.
- **A frame is never queued behind a frame that has not run.** At most one frame is in flight: a window whose thread
  is busy - a modal loop, a file dialog, a window being dragged - loses frames rather than being handed a backlog to
  run in a burst. `WM_TIMER` does this by accident, coalescing rather than queueing; the display clock does it on
  purpose, and `most_frames_behind` says how much of a backlog the product will have made up before the schedule
  simply starts again.
- **The schedule is absolute.** Each frame is due one frame period after the last was *due*, not one period after it
  *ran*, so a frame that runs late does not make every frame after it late too. That is what makes the motion
  uniform rather than merely fast.
- **The divisor is re-derived when the display changes under the window** - a mode change, and the window moved to a
  monitor of another rate. Where the new display has no rate to divide, the window falls back and `FramePacingNow`
  says so.
- **Every fallback says why.** There is no silent degradation: a product that asked for uniform motion and did not
  get it is owed the reason, and `window::TextOf` puts it in words.
- **No tuning number is the toolkit's.** The divisor and the patience are the product's. What is the toolkit's is a
  bound on what a *display* can be - a rate between 20 and 1000 hertz is believed and anything outside it is refused
  - and how far two readings of one display may be apart and still be the same display's. Neither is a rate anything
  here wants.

## Each platform

| | Windows | macOS | Linux (X11) |
|---|---|---|---|
| The platform's timer, unchanged | `SetTimer` at 16 ms, `WM_TIMER` | `NSTimer` at a sixtieth | the app loop's 16 ms tick |
| The display clock | a high-resolution waitable timer snapped to the compositor's vertical blank grid | `CADisplayLink` from macOS 14, `CVDisplayLink` below it | **none** |
| Where it waits | a thread of the window's own, posting each frame to the window | the run loop (Core Animation) or CoreVideo's own thread, hopping to the main queue | - |
| The rate read | `DwmGetCompositionTimingInfo`, cross-checked against the window's monitor's mode | `NSScreen.maximumFramesPerSecond`, cross-checked against `CGDisplayModeGetRefreshRate` | - |
| How the subdivision is done | arithmetic: the due time on the grid | counted: every Nth refresh of the link | - |

**Windows.** There is no present to put on the vertical blank. The view tree is rastered on the CPU into a
`SkSurfaces::WrapPixels` bitmap and blitted with `SetDIBitsToDevice`; there is no GL context, no D3D device and no
swap chain, so **a DXGI waitable swap chain is out of reach** - reaching one means a D3D11 device, a flip-model swap
chain, a per-frame upload of Skia's raster or a move to Skia's D3D backend outright, and redoing the child-window,
popup, DPI and dirty-rectangle handling that `window/win/WindowWin.cpp` is mostly about. It would also change the
pixels three shipped products draw. So the clock is the nearest honest thing to a present: a
`CreateWaitableTimerExW` with `CREATE_WAITABLE_TIMER_HIGH_RESOLUTION`, whose next due time is pulled onto the
compositor's own vertical blank grid every frame. The compositor's calls are looked up from `dwmapi.dll` rather than
linked, as `GetDpiForWindow` already is, so no consumer's link line changes and a Windows without them is simply the
fallback.

The audit this was built on waited with `MsgWaitForMultipleObjects(..., QS_ALLINPUT)`, because its harness waited on
the window's own thread and had to keep pumping messages. In a thread of the window's own there are no messages to
pump, so the wait is `WaitForMultipleObjects` on the timer and a stop event - and the timing is the timer's, not the
wait function's.

**macOS.** This is the clean case: the platform already calls once a refresh, so "shown for exactly N refresh
periods" is a count and there is nothing to wait for and nothing to snap. Core Animation's link, from macOS 14, is
made from the view itself (`-[NSView displayLinkWithTarget:selector:]`), follows that view's own screen as the window
is moved between displays, and calls back on the run loop - the window's own thread, with no hop. CoreVideo's is the
fallback down to the toolkit's floor of 13.3; it calls back on a thread of its own, so each tick is handed to the
main queue, and its calls are deprecated from macOS 15, which is why they are reached only below 14. The schedule is
kept in the ticks' own timestamps rather than by counting callbacks, because a run loop that is busy coalesces a
display link's callbacks and a count would then fall behind the panel.

**Linux has no display clock, and the toolkit says so.** X hands a window's pixels over with `XPutImage`, which is a
copy and not a presentation, so nothing reports a vertical blank. GLX or EGL would need a GL context and a swap,
which this path has not got. The X11 Present extension would need the tree rastered into a Pixmap and handed over
with `PresentPixmap` against a target media stream count - a different present path, a new library on every
consumer's link line, and a rewrite of the dirty-rectangle handling - **for phase alone**, because the rate on Linux
is already right: the app loop keeps its deadline in a steady clock and advances it by the period, which measured
16.000 ms with a standard deviation of 0.306 ms against Windows' 26.9 ms and 7.1 ms. What display synchronisation
would buy on Linux is phase, not rate, and until that is worth its cost `SetFramePacing` there answers
`kNoDisplayClock` rather than faking a phase lock.

## The three things that are actually hard

**The reported refresh rate can be another display's.** On the machine this was written on, `Win32_VideoController`
in WMI reported "59 Hz, 3840 x 2160" while `EnumDisplaySettingsW` and the compositor agreed on 2560 x 1600 at 240 Hz
- and on the same machine a week later all three agreed on 3840 x 2160 at 59.94 Hz, so the earlier disagreement was
a display configuration, not a lie. Either way the lesson holds: a rate is **read**, never assumed, and a reading is
believed only where it is a rate a display could have (`window::IsBelievableRefreshRate`: 20 to 1000 hertz) and
where the compositor and the window's own display agree within a twentieth. The compositor is preferred, being the
clock the frames are actually composited on; the display's mode is the cross-check. Windows' composition timing is
the **desktop's**, so a window on a 60 Hz second monitor of a desktop composited at 240 is caught by that
cross-check and nowhere else - and falling back there is right, because dividing the wrong panel's rate would be
worse than not pacing at all. (Measured on one display; the multi-monitor case is handled by the cross-check and has
not been measured here.)

**A variable-refresh panel has no stable period, and then there is no denominator.** The compositor's reported rate
wandered run to run on this machine - 240.0038, 240.1593, 240.2576 hertz - which is a hundredth of a percent and is
one panel read three times; a panel whose rate is actually varying reads whole multiples apart. So the window keeps
the last eight refresh periods the platform reported (`window::RefreshSteadiness`) and asks whether they are one
period read over again, within a twentieth. Where they are not, **it goes back to the platform's timer and reports
`kRefreshRateUnsteady`** rather than pacing to a number that is not real. One mode change is told from a panel that
varies by waiting: a single jump washes out of the readings being judged within eight of them, while a varying panel
never settles, so the window gives up only when the readings have refused to settle for as many frames as are judged
together.

**`DwmFlush` blocks the thread with no message pump.** It measures almost perfectly - 4.176 ms mean, 0.327 ms
standard deviation against a 4.1622 ms period, phase-locked by construction - and each call blocks the calling
thread for a whole refresh period with no messages pumped. Four of them back to back for 60 Hz on a 240 Hz panel is
16.7 ms in which a window answers nothing. It would be acceptable in a thread of one's own and is wrong anywhere near
a window's, and its period is three times as loose as the waitable timer's, so it is not used.

## What this measured here

Taken on the machine this was written on with `tools/frame-pacing-harness.cpp`, which anyone can run again. The
display at the time was **3840 x 2160 at 59.94 Hz** (the compositor's reading; period 16.6828 ms) and the system
clock's increment 15.6252 ms. 2000 frames for the platform timer and for a divisor of 1, 1000 and 500 for the larger
divisors.

| Mechanism | mean | rate | sd | median | lowest | highest | a whole refresh period, +/- 1/10 | +/- 1/30 |
|---|---|---|---|---|---|---|---|---|
| **`SetTimer(16)` - the platform timer** | 26.855 ms | 37.24 Hz | **7.094 ms** | 30.48 | 14.16 | 46.75 | 28.0 % | **4.4 %** |
| **the display clock, N = 1** | 16.683 ms | **59.942 Hz** | **0.246 ms** | 16.66 | 13.51 | 19.74 | 99.8 % | **99.0 %** |
| the display clock, N = 2 | 33.366 ms | 29.970 Hz | 0.911 ms | 33.36 | 27.04 | 39.84 | 94.4 % | 73.0 % |
| the display clock, N = 4 | 66.730 ms | 14.986 Hz | 1.478 ms | 66.72 | 53.88 | 81.63 | 96.2 % | 90.2 % |

The platform timer's 26.855 ms and 37.24 Hz reproduce the earlier audit's 26.861 ms and 37.23 Hz to three figures,
on a different panel - which is the point: the fault is the system clock's, not the display's.

**The standard deviation falls by a factor of 29** at a divisor of 1, and 89.8 % of frames land in the one
millisecond from 16 to 17 against the platform timer's two clumps of 15.4 % at 15-16 ms and 40.1 % at 30-31 ms. That
bimodality *is* the stutter: on this 60 Hz panel each frame is shown for one refresh or for two, chosen afresh each
frame.

**The tolerance on the last two columns matters, and both are given for that reason.** A tenth of a refresh period is
the tolerance the earlier audit used, and on its 240 Hz panel it was safe: one 15.625 ms clock scan is 3.75 refresh
periods there, nowhere near whole, and the platform timer scored 3.1 %. On a *60 Hz* panel a scan and a refresh are
only 6 % apart, so a tenth of a period cannot tell them apart and flatters the platform timer to 28 %; a thirtieth
can, and gives 4.4 %. The display clock scores 99.0 % on the tighter tolerance either way.

The larger divisors are looser than N = 1, and honestly so: at 15 frames a second the thread sleeps 66 ms at a time,
and a handful of wakings in 500 are late enough that the clock makes one frame up - the lowest and highest intervals
are one refresh period either side of the mean, which is exactly one frame slipped and the next recovered. That is
`most_frames_behind` doing what the product asked for, and a product that would rather re-phase than ever make a
frame up asks for 0.

## What is held, and where

The policy needs no window, no display and no clock, and it is in one place for that reason
([window/FramePacing.h](../../window/FramePacing.h)) - as the raw keys' is ([raw-keys.md](raw-keys.md)) and as
Windows Audio's arithmetic is ([../audio/native-backends.md](../audio/native-backends.md)).
`tests/test_frame_pacing.cpp`, in the `view` suite, holds all of it on **every platform, in the builds with no GUI
too**: the rate chosen from two readings and every reason there is none, every branch of the fallback decision, the
subdivision on six panels, the grid an instant is pulled onto and which tick a frame belongs on, whether a period
holds still, and the schedule with its catching up and its giving up. Two of its cases are simulations of the shapes
the platforms drive it in - a waited schedule against a made-up 240.2576 Hz grid, and a counted display link at 120 Hz
- and each holds the property that matters: **every frame exactly the divisor's refresh periods after the last, over
hundreds of frames, with no drift.** A third shows why the count is kept in the ticks' timestamps: with one callback
in seven dropped, the timestamps stay on the panel while a count of callbacks ends more than twenty refreshes behind.

`tests/test_app_shell.cpp`, in the `app` suite, holds the seam's default body on every platform: a child window that
paces nothing itself answers the platform's timer, with `kNotAsked` until someone asks and `kNoDisplayClock` once
someone has.

`skia/tests/test_child_window.cpp`, the `child_window` CTest (label `window`), holds a **real** window on whatever
display clock the machine has - and **asserts no rate at all**, because a timing distribution is not a pass or a
fail and none of the three CI runners is a machine to measure one on. The Windows runners are services on a window
station that shows nothing, where the compositor's timing is very probably not to be had; the Linux job runs under
Xvfb, which has no vertical blank. So each test holds whichever branch its machine reached - on the display's clock,
a believable rate and the divisor honoured; on the platform's timer, a reason that is neither `kNone` nor `kNotAsked`
and no rate claimed - and **prints the probe**, so one run of the suite says which branch it was on. Each of them runs twice, once having asked for nothing and once having asked for the display's clock, and holds that the frames arrive on the window's own thread either way - which is how the unchanged path is held rather than inferred. macOS is the one
CI platform with a real window server, which the window suites already rely on, so there a display link is genuinely
made and ticked: that test holds that the frames arrive and keep arriving inside a generous window, and still never
a rate.

`tools/frame-pacing-harness.cpp` is the hand-run measurement, built with the suites so that it cannot rot and
asserting nothing. It takes the clocks, the platform timer, the period sweep and the schedule test that settle the
`WM_TIMER` mechanism above, and the display clock at a few divisors. It needs nothing of Skia or the views and builds
in seconds, including from one compiler command on a machine with no configured build tree - the command is in its own
opening comment.
