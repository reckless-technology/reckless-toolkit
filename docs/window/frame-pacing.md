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
measurement is neither: it is a mixture - of 2000 intervals on an idle machine, **27 % one clock tick long, 71 %
two and 1.5 % three**, and the three-tick tail grows with whatever else the machine is doing, being a tick the window
did not retrieve in time. Four measurements settle what is actually happening, and the harness runs all four.

**One: the grid is the system clock's, and `timeBeginPeriod` does not move it.** `GetSystemTimeAdjustment` reports
the clock's increment in force as **15.6252 ms**, and every `WM_TIMER`'s own `GetMessageTime` - the tick count the
message was generated at - sits on that grid and nowhere else. The harness's `platform-timer-grid` run prints them;
2000 of them came back stepping 16, 31, 32 and 47 milliseconds and nothing between, which is one, two, two and three
scans of a 15.6252 ms clock:

```
339172531(+31) 339172562(+16) 339172578(+31) 339172609(+16) 339172625(+31) 339172656(+31)
339172687(+31) 339172718(+16) 339172734(+31) 339172765(+16) 339172781(+31) 339172812(+31)
339172843(+32) 339172875(+31) 339172906(+31) 339172937(+16) 339172953(+31) 339172984(+31)
```

`timeBeginPeriod(1)` raises the *timer* resolution, which `NtQueryTimerResolution` duly reports as 1.0 ms; it leaves
the clock's increment at 15.6252 ms, and the measured rate with the period held is 38.78 Hz against 37.97 Hz without
it. **That is why `timeBeginPeriod` does nothing for `SetTimer`** - the two are different clocks.

**Two: the schedule is re-armed at each expiry, not absolute.** The harness's `platform-timer-schedule` run makes the
handler burn processor before the next message is taken, 400 frames each, and prints a control beside it - the same
timer re-armed by hand from the handler, which is an absolute schedule from the retrieval by construction:

| the handler burns | 0 ms | 8 ms | 20 ms | 25 ms | 40 ms |
|---|---|---|---|---|---|
| `SetTimer(16)`, the periodic timer | 27.274 ms | 27.078 ms | **27.314 ms** | **27.356 ms** | 40.035 ms |
| re-armed by hand, absolute from the retrieval | 27.390 ms | - | **43.273 ms** | - | - |

The periodic timer's mean is **flat through burns of 20 and 25 ms**, where an absolute schedule would already have
become handler-bound; the control, which is absolute by construction, matches it at no burn and diverges to 43.273 ms
at a 20 ms burn. That places the periodic timer's reference at the **expiry instant** - not at the retrieval, and not
on an absolute schedule. It becomes handler-bound only at a burn of 40 ms, which is past two scans of the grid.

**So there is no drift, and 62.5 Hz was never reachable through `SetTimer` at any period.** The most it can give is
one scan a frame - 15.6252 ms and 64 Hz in principle, 63.5 Hz as the sweep below measures it.

**Three: the rate is a staircase in the scans a period takes.** The `platform-timer-sweep` run asks for each period
in turn, 400 frames each, and gives the mean interval in units of the 15.6252 ms grid:

| asked for | 1 | 10 | 14 | **15** | **16** | 17 | 20 | 30 | **31** | **32** | 33 | 46 | **47** | 48 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| grid scans a frame | 1.01 | 1.01 | 1.03 | **1.12** | **1.68** | 2.01 | 2.02 | 2.05 | **2.29** | **2.76** | 3.01 | 3.16 | **3.47** | 4.01 |
| rate | 63.3 Hz | 63.5 Hz | 61.9 Hz | 57.2 Hz | **38.0 Hz** | 31.9 Hz | 31.7 Hz | 31.3 Hz | 28.0 Hz | 23.2 Hz | 21.3 Hz | 20.3 Hz | 18.4 Hz | 16.0 Hz |

The staircase is `ceil(period / 15.6252)` scans, as the re-arm model says. The **bold** values are the ones at the
step edges, where the mean is a *mixture* of two step heights rather than either of them.

**Four: the mixture is a one-millisecond ambiguity band at each step edge, and 16 sits in one.** The due time is
carried in whole milliseconds of tick count while the grid's step is 15.625 ms, so the grid's tick-count values
advance 16, 15, 16, 16, 15, 16, 16, 15 and a request of 16 ms is satisfied in one scan or in two depending on where
in that cycle the expiry fell - and which way the boundary case goes varies. The resulting sequence of one-scan and
two-scan intervals is **not periodic**: over 2000 frames its repeat rate at every lag from 1 to 128 is **0.45 to
0.59**, against a chance level of **0.55** for this mixture, which the same run computes. It is a frame-by-frame coin
toss, and there is no beat anything could be designed around.

```
2121222121222221221222221221212212211221221222221212222221221222
2221122212122222212212212122221211221212221122222212122122121212
```

That is the whole answer, and it has a blunt consequence. **`kFrameMilliseconds = 16` is the worst value in the range
1 to 30.** One millisecond less is 57 Hz, two less is 62 Hz, and one more is a clean 31.9 Hz; 16 is the first value
past the one-scan boundary of 15.6252, inside the one-millisecond band where it cannot make up its mind. On a 60 Hz
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
| `lead_microseconds` | how long before the vertical blank each frame is woken, so it is finished as it is composed: 0 (the default) wakes at the blank, as a window always has ([below](#leading-the-vertical-blank)) |

What it is told is a `window::FramePacingState`: the pacing actually in force, the divisor, the display's rate, the
frames a second that comes to, `why` - a `window::FramePacingFallback` - where it fell back, and the lead in force
(`lead_microseconds`), which is 0 wherever the platform cannot lead. `frames_a_second` is
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
- **A frame waits its turn.** Where a thread holds several windows, or a host's beside them, a frame goes behind every
  paint and timer waiting on the thread, as the platform's timer's always has, so a thread with more to paint than a
  refresh has room for shares it round its windows and starves none of them, and none of its host's
  ([below](#several-windows-on-one-thread)).
- **The schedule is absolute.** Each frame is due one frame period after the last was *due*, not one period after it
  *ran*, so a frame that runs late does not make every frame after it late too. That is what makes the motion
  uniform rather than merely fast.
- **The divisor is re-derived when the display changes under the window** - a mode change, and the window moved to a
  monitor of another rate. Where the new display has no rate to divide, the window falls back and `FramePacingNow`
  says so.
- **A window that stops showing says so.** A clock that stops as a window hides is noticed, and a product that asks goes
  on being handed frames ([below](#a-window-that-does-not-show)).
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

## Leading the vertical blank

A frame paced on the display's clock is woken **at** a vertical blank. It polls its input, ticks and paints in a few
milliseconds, and then the finished frame waits for the compositor, which takes it at the *next* blank: the input it
sampled is most of a refresh older than it need be by the time it is composed. Sentinel measured that wait at 14.10 ms
of a 16.67 ms refresh windowed, and 9.28 ms full screen at 2160 lines (Legion5i, 2026-11-04, at 60 Hz). A lead
(`FrameRequest::lead_microseconds`) wakes each frame that long **before** the blank it is due at instead, so a product
that knows what a frame costs it can ask to be woken just early enough to finish - and samples its input that much
later.

```cpp
// This product's numbers: its paint's 99th percentile and a margin. Too short a lead costs a whole refresh.
child.SetFramePacing({reckless::window::FramePacing::kDisplayClock, 1, 1, /*lead_microseconds*/ 5000});
```

- **The schedule stays on the blanks; only the waking moves.** The clock is kept on the vertical blanks themselves and
  told the time as *now plus the lead* (`window::LedWakeSeconds`), so its absolute schedule, its catching up and its
  giving up are exactly those of a clock with no lead. Moving each due time back by the lead instead would not work:
  the next due time is pulled onto the blank *nearest* it, which for a lead over half a refresh period is the blank
  before, and the schedule would run backwards.
- **The first frame is never already late.** Starting the clock a frame period ahead of *now plus the lead* puts the
  first waking at least half a refresh period ahead, whatever the lead.
- **The lead is held to the frame period** (`window::LeadInForceMicroseconds`): none for a lead under 0, and at most
  the frame period - the refresh period times the divisor - less a millisecond. A lead longer than one refresh is
  fine where the divisor leaves room for it. The lead in force moves with the display under the window, as the rate
  does, and `FramePacingNow` reports it.
- **A frame that misses the blank it led is shown a whole refresh later**, which is worse than no lead at all. The
  margin is the product's to choose: the window's own wake is not exact (below), and a frame's cost has a tail.

**Windows** leads. **macOS does not yet**, and its state says 0: a Mac's display link runs the frame callback when it
ticks, and whether a lead would gain anything there depends on *when* it ticks, which nothing in the code can show.
Core Animation's link hands over `timestamp` (the blank just gone) and `targetTimestamp` (the one the frame will be
shown at); CoreVideo's an `output` time for the frame. If the callback runs at `timestamp`, a whole refresh before
`targetTimestamp`, the frame waits as Windows' did and a lead is worth the same - schedule the frame at
`targetTimestamp - lead` on the main queue rather than in the callback. If the link is already run ahead of the blank
by about a frame's cost, a lead gains nothing. **What to measure on a Mac**, on Apple Silicon and on Intel:
`CACurrentMediaTime()` on entry to the callback against `timestamp` and `targetTimestamp`, and then a frame's paint
against `targetTimestamp`. **Linux** has no display clock to lead.

What the lead does, measured on Legion5i on 2026-11-04 (3840 x 2160 at 59.94 Hz, the compositor's period 16.683 ms;
a Release build, one frame a refresh, a frame that fills the window with one colour) by a probe that stamps the frame
callback against the compositor's own last vertical blank (`DwmGetCompositionTimingInfo(nullptr).qpcVBlank`), 590
frames each:

| Lead in force | wake to the blank it led, mean | the callback's start after its target: mean, sd, p99 | intervals over 1.5 refreshes | paints past the blank they led | DWM frames missed |
|---|---|---|---|---|---|
| 0 | 16.254 ms (0.43 ms after the blank) | - | 0 | 0 | 0 |
| 2 ms | 1.622 ms | 0.378 ms, 1.091 ms, 1.051 ms | 0 | **3 of 589** | 0 |
| 5 ms | 4.525 ms | 0.475 ms, 0.257 ms, 1.221 ms | 0 | 0 | 0 |
| 10 ms | 9.513 ms | 0.487 ms, 0.296 ms, 1.644 ms | 0 | 0 | 0 |
| 15 ms | 14.467 ms | 0.533 ms, 0.342 ms, 2.032 ms | 0 | 0 | 0 |
| 15.683 ms (20 ms asked, held) | 15.225 ms | 0.458 ms, 0.235 ms, 1.100 ms | 0 | 0 | 0 |

The callback starts about **half a millisecond after its target**, with a standard deviation of 0.2 to 0.35 ms: that
is the hop from the clock's thread to the window's (a posted message) and the waitable timer's own lateness, and it
is there at a lead of 0 too - a frame woken at the blank begins 0.43 ms after it. A product's lead absorbs it. A cheap
frame missed nothing at any lead, but at 2 ms three paints in 589 finished after the blank they were woken for: a
2 ms lead leaves too little for a half-millisecond wake with a millisecond's tail.

## Several windows on one thread

A plug-in's editors all live on the host's one UI thread, so a thread with a dozen panels on it is an ordinary thing to
have, and on Windows each is a window with a clock of its own posting frames to it. What decides how they share the
thread is the order its queue hands messages over, which `tools/frame-pacing-harness.cpp`'s `paint-rank` run shows on
any machine (Legion5i, 2026-11-05):

```
a timer set, then the window invalidated, then a message posted:
  posted, paint w0, timer,
a message posted, then the window invalidated, then a timer set:
  posted, paint w0, timer,
four windows invalidated in the order w0 w3 w1 w2:
  paint w3, paint w2, paint w1, paint w0,
w0 raised to the top, then all four invalidated in the order w1 w2 w3 w0:
  paint w0, paint w3, paint w2, paint w1,
two windows invalid, each invalidated again as soon as it paints, the first eight taken:
  paint w0, paint w0, paint w0, paint w0, paint w0, paint w0, paint w0, paint w0,
```

**A posted message outranks every paint, a paint outranks every timer, and of several invalid windows the one nearest
the top of the z-order is painted first, however long the others have waited.** On the platform's timer that is harmless
by accident: a frame is the last message a thread takes, so every paint the last round of frames asked for is done before
the next frame runs. The display's clock posts its frames, and a posted frame invalidates its window again ahead of paints
still owed. While the panels fit a refresh nobody sees it. Past that point a thread gives its paints to the top of the
z-order and nothing to the bottom, and a host's own window on the thread, with a paint and a timer of its own, gets
neither.

Measured on Legion5i (i9-13900HX) on 2026-11-05 on its panel at 2560 x 1600, 240 Hz and 150 %, a frame every fourth
refresh, 60 a second, with real Tape panels at 125 %, 1115 x 810, on one thread, pinned to one logical processor of each
P-core, and each window's picture read after every compositor frame. Pictures a second, window by window, the top of the
z-order first, and in brackets the thread's total:

| | 1 window | 4 windows | 6 windows | 8 windows |
|---|---|---|---|---|
| the platform's timer | 36.9 | 37.5 37.4 37.4 37.3 (150) | 34.1 each (205) | 30.8 each (247) |
| the clock, every frame posted and run | 58.7 | 60.0 59.9 59.9 59.9 (240) | 59.9 59.7 58.9 54.0 28.5 0.6 (261) | 59.3 59.2 58.1 53.1 26.7 0.1 0.0 0.0 (256) |
| a frame waits for its own window's paint | | 59.8 59.8 59.1 55.8 (235) | 58.5 58.3 57.8 56.6 40.3 6.4 (278) | 59.0 59.5 57.5 51.6 19.6 0.3 0.0 0.0 (247) |
| the paint made inside the frame | | 58.0 58.3 57.9 58.8 (233) | 42.3 41.0 43.5 40.4 44.1 42.1 (253) | 33.4 39.4 39.2 1.6 43.2 40.5 40.1 27.7 (265) |
| a frame waits for the thread's paints and timers, in one pass | | 59.7 59.6 59.9 59.9 (239) | 41.4 36.7 42.8 42.0 46.1 44.8 (254) | 25.7 26.4 23.8 23.6 31.9 33.1 36.6 31.1 (232) |
| **the same in two passes: what a window does** | **59.8** | **59.4 59.5 59.5 59.5 (238)** | **43.8 each (263)** | **30.6 each (244)** |
| the same, 30 frames a second | 30.5 | 30.0 each (120) | 30.0 each (180) | 30.0 each (240) |

The same shape was measured on 2026-11-04 on 3840 x 2160 at 59.94 Hz and 125 %, a frame every refresh: 54 54 54 53 38
2.1 0 0 at eight windows, and which windows starve is not fixed.

A window on that thread with a timer of its own every 20 ms and a paint of 1 ms, beside eight panels:

| | its timers a second | its paints a second | the longest it waited |
|---|---|---|---|
| the clock, every frame posted and run, the window under the panels | 0.4 | 0.4 | 1685 ms |
| the same, the window over the panels | 0.0 | 0.0 | the whole run |
| a frame waits for its own window's paint | 0.0 | 0.0 | the whole run |
| the paint made inside the frame | 0.0 | 0.0 | the whole run |
| **a frame waits for the thread's paints and timers** | **30.0** | **30.0** | **69 ms** |
| the same, the window over the panels | 29.6 | 29.6 | 71 ms |
| the platform's timer | 28.8 | 28.8 | 70 ms |

**A frame that waits only for its own window's paint does not do it.** It still invalidates the window nearest the top the
moment that one has painted, so the bottom of the z-order goes on starving. **Nor does painting inside the frame**, which
is even at six windows and not at eight, leaves a host nothing, and loses the order: the clocks' threads wake in no fixed
order, so a window's paint falls first in one refresh and last in the next.

**So a frame waits its turn behind the thread's paints and timers** (`window::DoesFrameStandBack`), as the timer's did, in
two passes through the queue:

- **The first pass reads the queue.** `GetQueueStatus`' high word says whether a paint or an expired timer is waiting on
  the thread, of any window. It has to be the queue that is read rather than anything of the toolkit's own: every
  plug-in carries its own copy of this code, so a count kept here would see that plug-in's windows and no others, and
  nothing here sees a host's. With work ahead the frame stands back, still in flight so no other is posted over it, and
  the clock asks again shortly (`window::FrameAsksAgainAfter`), its schedule going on underneath.
- **The second pass runs the callback**, in a posted message of its own, so that every frame of one refresh has read the
  queue before any of them has invalidated a window. Otherwise each would take the last one's paint for work ahead of
  it, the frames of a refresh would go one at a time, and their order would be whichever clock woke first: with four
  panels that have room, 218 of 478 paints came a round after the same panel's with one pass, against 421 of 477 with
  two (`ChildWindowFramePacing.PanelsThatFitARefreshPaintInOneOrderEveryRefresh`, whose printout this is).
- **The wait is bounded.** A queue whose paint never comes - a host pumping only some messages, a window whose paint
  never validates it - costs a frame `window::kFrameStandsBackAtMostSeconds` and not the window's frames for good.
- **Asking again is itself a posted message**, which outranks the paint it stood back for. A millisecond's wait left a
  thread idle once its last paint was done; no wait at all kept it answering, and six panels fell to 9 pictures a second
  each. So the wait starts at a tenth of a millisecond and doubles only while the window answers within a moment of being
  asked, which a thread that is painting cannot do.

A thread with room is close to unchanged: a frame is two posted messages where it was one, and stands back only for a
paint or a timer it happens to meet, which the table's four windows show. A saturated thread goes round its windows in
turn, each round starting as the last finishes, so its frames no longer land on whole refresh periods: a window whose
motion must stay uniform past that point asks for a larger divisor, which is what the 30 frames a second row shows. Beside
a host's window that keeps a timer of its own, a thread near its limit gives some of its frames up to it: four panels read
56.4 56.4 56.3 56.8 with the host's window above (226), against 59.9 59.9 59.4 58.1 (237) with every frame posted and run,
whose host waited 396 ms at worst where this one waits 141 ms. A plug-in built on a toolkit from before this still posts
ahead of the rest.

## A window that does not show

macOS stops a display link made from a view whose window is hidden or on no screen, and the frame callback with it. A
window paced on nothing else then has no frame at all, so nothing runs to tell its panel it no longer shows, and a product
that stands its pacing down as a window hides never gets the chance. So a link is watched (`window::IsDisplayLinkSilent`):
silent for `window::kDisplayLinkSilentSeconds`, the window says `kDisplayLinkSilent`, and its next tick takes it back onto
the clock with no one asking.

**Whether frames go on meanwhile is the product's** (`FrameRequest::keeps_frames_off_screen`). Asked for, the window runs
on the timer it has always used, as a window never paced at all does. Not asked for, it has no frames until it shows,
which a product may be relying on: one that hands a window that has left the clock to a frame source of its own, for good,
should not take a window hidden for a moment as a display it cannot pace on. Windows' clock does not stop, so nothing
changes there: a hidden window goes on being handed frames, and a product stands down as it always could.

Measured on an M1 Pro on 2026-11-05, 1920 x 1080 at 60 Hz, two seconds each, a product that stands down as a window hides
and asks again as it shows: shown, 60.0 frames a second on the clock; hidden, 52.5 a second over the two seconds, the first
quarter of a second being the silence, and on the platform's timer; shown again, 60.0 on the clock.

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
holds still, the schedule with its catching up and its giving up, and the lead - held to the frame period, reported
only on the display's clock, and a led loop simulated on seven panels and leads, every frame woken the lead before a
blank and the divisor's refreshes after the last, with a stall caught up exactly as an unled loop catches it up. Two of its cases are simulations of the shapes
the platforms drive it in - a waited schedule against a made-up 240.2576 Hz grid, and a counted display link at 120 Hz
- and each holds the property that matters: **every frame exactly the divisor's refresh periods after the last, over
hundreds of frames, with no drift.** A third shows why the count is kept in the ticks' timestamps: with one callback
in seven dropped, the timestamps stay on the panel while a count of callbacks ends more than twenty refreshes behind.

The same suite holds a frame's waiting its turn, with no window: `FrameStandsBack` holds the bound on the wait, how soon a
frame asks again, and a model of the queue's three rules, in which frames that run as they ask leave the bottom of the
z-order and a host's timer with nothing and frames that wait their turn leave every window its share at about the same
total; `DisplayLinkSilence` holds what counts as a link that has stopped.

`tests/test_app_shell.cpp`, in the `app` suite, holds the seam's default body on every platform: a child window that
paces nothing itself answers the platform's timer, with `kNotAsked` until someone asks and `kNoDisplayClock` once
someone has.

`skia/tests/test_child_window.cpp` and `skia/tests/test_child_window_mac.mm`, the `child_window` CTest (label
`window`), hold a **real** window on whatever display clock the machine has - macOS's half is Objective-C++ because
it reaches AppKit's run loop, and `tests/FramePacingRule.h` is the assertion both halves make - and **asserts no rate at all**, because a timing distribution is not a pass or a
fail and none of the three CI runners is a machine to measure one on. The Windows runners are services on a window
station that shows nothing, where the compositor's timing is very probably not to be had; the Linux job runs under
Xvfb, which has no vertical blank. So each test holds whichever branch its machine reached - on the display's clock,
a believable rate and the divisor honoured; on the platform's timer, a reason that is neither `kNone` nor `kNotAsked`
and no rate claimed - and **prints the probe**, so one run says which branch it was on. CTest keeps a passing test's output to itself, so the probe is read with `ctest --preset windows -V -R child_window`, or by running `reckless_child_window_test` itself. Each of them runs twice, once having asked for nothing and once having asked for the display's clock, and holds that the frames arrive on the window's own thread either way - which is how the unchanged path is held rather than inferred. macOS is the one
CI platform with a real window server, which the window suites already rely on, so there a display link is genuinely
made and ticked: that test holds that the frames arrive and keep arriving inside a generous window, and still never
a rate. On a Mac the same suite holds a hidden window, both ways: asking for frames off a screen it goes on having them, says
`kDisplayLinkSilent`, and goes back to the clock as it shows with the timer stopped; not asking it has none, and says the
same. On Windows, where a window that shows reaches the clock, it holds six panels at a paint too long for the thread with a
host-like window beside them, every panel painted and the host's timer and paint running, and four that have room painting
in one order, each paint a round after the same panel's. Both print what they measured, and neither claims a rate.

`tools/frame-pacing-harness.cpp` is the hand-run measurement, built with the suites so that it cannot rot and
asserting nothing. Its runs are the clocks and the displays attached to the desktop, the platform timer, the
period sweep, the schedule test and the grid test that settle the `WM_TIMER` mechanism above, the display clock at
a few divisors, and the order a thread's queue hands over a posted message, a paint and a timer - the claims the mechanism
rests on. The figures for panels on one thread were taken with real panels and the pictures read off the screen, which
needs a product's panel and so is not the harness's. It needs nothing of Skia or the views and builds
in seconds, including from one compiler command on a machine with no configured build tree - the command is in its own
opening comment.
