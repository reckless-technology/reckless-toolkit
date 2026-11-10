# Drawing a window on the GPU

A child window draws its view tree with Skia's CPU raster and hands the pixels to the platform, and has done since the
toolkit began: `SkSurfaces::WrapPixels` and `SetDIBitsToDevice` on Windows, a `CGImage` in `drawRect:` on macOS,
`XPutImage` on Linux. So a window now carries an **opt-in GPU path** beside it, on every platform: Skia's Ganesh on
OpenGL through WGL on Windows, on Metal on macOS and on OpenGL through EGL on Linux, each frame drawn into a surface on
the GPU and presented. A
product asks for it, and the window gives it where it can, says so, and draws with the CPU raster where it cannot and
says why.

## The decision

Jonny, 2026-11-07, of a study of what Sentinel would gain from the GPU: *"Windows should get the GPU window first.
Ignore the raspberry pi for now."* and *"Yes"* to starting. Sentinel's HD look is what asked for it - at 4K its frame
costs 5.1 to 5.4 ms on the CPU raster against a refresh of 16.68 - but the window is the toolkit's, so the path is here
and every product may ask for it.

What the study measured, on `Legion5i` (i9-13900HX, RTX 4070 Laptop, GL 4.6 on NVIDIA 610.62), 2026-11-07, one
Sentinel-like frame drawn the same way through each, 600 frames after 60 to warm up, the medians of three rounds:

| Frame | Ganesh GL, the CPU's side | CPU raster, one core |
|---|---|---|
| 1917 x 1080 | 0.59 - 0.60 ms | 5.22 - 5.30 ms |
| 3834 x 2160 | 0.57 - 0.61 ms | 20.11 - 20.50 ms |

The skin's own frame is lighter than that one, so these are the shape of the gain, not Sentinel's numbers.

## Asking for it

```cpp
#include "GpuDrawingBackend.h" // and link reckless_window_gpu

const reckless::window::DrawingState state = child.SetDrawing(reckless::window::GpuDrawingRequest());
if (state.drawing != reckless::window::Drawing::kGpu)
{
    std::fprintf(stderr, "drawn on the CPU raster: %s\n", reckless::window::TextOf(state.why));
}
```

- **`ChildWindow::SetDrawing(DrawingRequest)` and `ChildWindow::DrawingNow()`**, beside `SetFramePacing` and
  `FramePacingNow` and made the same way: a request whose default asks for what a window always did, an answer that says
  what the window is doing and why where it fell back, and a `Now` that a product reads for its log rather than keeping
  the first answer. A window that has never been asked answers `{Drawing::kCpuRaster, DrawingFallback::kNotAsked}`.
- **It may be called at any time.** An app asks as its window is made (Sentinel's `ChildMade`, the app shell's
  `AppRunHooks::on_child`), before the first frame; it may also ask later, and `SetDrawing({})` puts a window back on the
  CPU raster. Either way the whole window is drawn again on whichever it ends up with.
- **The GPU's code is a library of its own, `reckless_window_gpu`**, and a request names its back end
  (`GpuDrawingBackend()`, which `GpuDrawingRequest()` fills in). That is the one part of the design that is not
  `SetFramePacing`'s, and it is there because of what Ganesh weighs. Measured on `Legion5i`, 2026-11-07, a program that
  rasters one surface was **3,572,224 bytes**, and the same program reaching Ganesh GL **5,402,624** - 1.8 MB more - and
  it gained `OPENGL32.dll` as a library loaded as it starts. Every plug-in compiles the window into its own module and is
  loaded into a host's process, and never asks; with the GPU in `WindowWin.cpp` each would carry the 1.8 MB and load the
  GL into the host. So the window names nothing of Ganesh (`window/GpuPresenter.h` is the seam, and `window/win/GpuPresenterWin.h` Windows' half of it), a binary that
  never links `reckless_window_gpu` carries none of it, and the window's own suite holds that: it links the window alone,
  and asserts `opengl32.dll` is not in its process. The window's suite grew by 15,360 bytes with this change, its tests
  included.
- A request for the GPU with no back end, `{Drawing::kGpu, nullptr}`, is answered `kNotLinked`.
- **Linking.** Ganesh's GL is compiled into the pinned skia-pack's `skia.lib` beside its Direct3D, whose objects it
  shares a link with, so `reckless_window_gpu` puts `d3d12` and `d3dcompiler` on the link line too (`dxgi` is not
  needed). Nothing of either is called, and neither becomes a library the binary loads: the probe above imported
  `OPENGL32.dll` and `KERNEL32.dll` and nothing else. On macOS the pinned pack's `libskia.a` carries Ganesh's Metal in
  both slices, and `reckless_window_gpu` adds `-framework Metal` and `-framework QuartzCore`; the window itself names
  no Metal, and its suite holds that from the binary (below, "Metal on macOS").

## How a frame is drawn

On Windows, through WGL (macOS's is [Metal on macOS](#metal-on-macos), below):

1. **A pixel format, asked of a window of its own first.** A window keeps the first pixel format it is given for life. So
   the back end first gives one to a window of its own that is never shown, makes a GL context there, reads its version
   and asks Skia for its interface to it; only where all of that holds is the child window given a pixel format. A
   machine whose GL refuses - CI's runners, which have no GPU driver and Windows' own software GL 1.1 - leaves the child
   window exactly as it was.
2. **A GL context on the window**, robust where the driver offers `WGL_ARB_create_context_robustness`
   (`WGL_LOSE_CONTEXT_ON_RESET_ARB`, so a reset is reported rather than hidden), and the context WGL makes by itself where
   not. A GL older than 3.0 is refused (`kGlTooOld`): Skia's own floor is lower, but nothing older is a GPU a window
   would be better on than the CPU raster. The device context is held for the presenter's life (`GetDC` once), since
   the window's class is not `CS_OWNDC` and is shared with the popups, which stay on the CPU raster.
3. **Skia's context on it**, `GrDirectContexts::MakeGL(GrGLInterfaces::MakeWin())`.
4. **A surface on the GPU, the window's size**, which the views draw into. It keeps its pixels from frame to frame, as
   the CPU raster does, so the window's paint is unchanged: it draws only the part that was invalidated, with the same
   clip and the same scale, and the rest of the surface still holds the last frame.
5. **The present**: the whole surface drawn into the window's own framebuffer (framebuffer 0, wrapped as a Skia surface,
   bottom-up as GL's is), Skia's work submitted, and `SwapBuffers`. A swap leaves the back buffer's contents undefined, so
   the views' surface is what keeps the frame and the copy is the GPU's.

Several windows on one thread each have their own context, made current before each thing a presenter does, so they
never draw into one another's.

## Pacing, and the swap interval

**The swap interval is 1 in full screen and 0 in a window** (Jonny's brief, 2026-11-09, on the measurement below). The
frame callback is already paced on the display's own clock ([frame-pacing.md](frame-pacing.md)): a waitable timer
snapped to the compositor's vertical blank, with a lead the product chooses, on a thread of the window's own that posts
each frame to the window.

- **Windowed, 0.** DWM composes the window, so a frame is never torn whatever the swap does, and a swap that waited for
  a blank as well would be a second clock beside the first, holding the window's thread for up to a refresh while it
  waited and throwing away the lead, which exists to finish the frame just before the blank rather than a refresh early.
  With 0 the swap does not wait, the frame reaches the compositor as soon as the GPU has drawn it, and the compositor
  shows it at the next blank, as the CPU raster's `SetDIBitsToDevice` has always done.
- **Full screen, 1.** A full-screen window is borderless over its monitor, and PresentMon reports it presented with
  tearing allowed when the swap interval is 0: a frame can reach the display part-way through a refresh, and 10 % of the
  display's changes were not on exactly one refresh (table below). With 1 the swap waits for the blank and every change
  is on exactly one refresh.

The app's window tells its child window when it enters or leaves full screen (`ChildWindow::SetFullScreen`, which
`RunApp` calls from the window's own `full_screen_changed`, so the presenter is told the window's own state, not a poll
of whether it covers its monitor), the child tells its presenter (`GpuPresenter::SetFullScreen`), and the presenter
sets `wglSwapIntervalEXT` before the next swap, only when the interval has changed (`SkiaGpuPresenter::Present`;
`SwapIntervalFor` in `window/GpuSwapInterval.h` is the rule). A window that is full screen when its GPU is made, or made
again after a loss, is told as it is made.

`RECKLESS_GPU_SWAP_INTERVAL` overrides it, for diagnosis: `0` (never wait), `1` (always wait) or `auto`, which is the
default and also what anything unrecognised or unset means.

### Interval 1 in full screen, measured

This **replaces** the earlier text, from 2026-11-08, which kept the interval at 0 on the grounds that a swap that waited
was a second clock, and which recorded an interval of 1 as worse. That test ran during an unexplained stall period on
the machine and does not stand. The A/B below was taken the next day with PresentMon.

`Legion5i` (RTX 4070 Laptop), one 3840 x 2160 display at 59.94 Hz, Sentinel full screen with scripted play, about 30 s a
run, 2026-11-09, PresentMon for the present mode, `AllowsTearing` and the display's changes, and Sentinel's frame log
for the ticks (analyser: `tear-ab.py`, in the session's scratchpad, not in a repository):

| WGL swap interval | Tearing allowed | Display changes on exactly 1 refresh | Stalls > 25 ms | Ticks not 1 | Present-to-display latency p50 |
|---|---|---|---|---|---|
| 0 (until now) | yes | 90 % | 109 (max 550 ms) | 0.7 % | 1.3 ms |
| 1 always | no | 100 % | 0 | 0 % | 31 ms |
| **1 in full screen, 0 windowed** | **no** | **100 %** | **0** | **0 %** | **15 ms** |

Interval 1 removes the tearing and the stalls, at the price of latency in full screen (15 ms at p50, against the 1.3 ms
of a frame let through torn). Interval 1 on a window the compositor already composes without tearing buys nothing and
costs the second clock, so windowed stays at 0.

### The change itself, proved in Sentinel

This toolkit change overlaid on Sentinel `2ec215f2` (a probe build, `-DSENTINEL_LATENCY_PROBE=ON`; full screen from the
tenth frame by `SENTINEL_PROBE_FULL_SCREEN`, scripted play by `SENTINEL_PROBE_PLAY`, a copy of the settings file), same
machine, display and PresentMon, 30 s a run, 2026-11-09 17:19 (`fs-0` and `win-0` are the same binary with
`RECKLESS_GPU_SWAP_INTERVAL=0`, the control):

| Run | Present mode | Tearing allowed | Sync interval | Display changes not on exactly 1 refresh | Stalls > 25 ms | Ticks not 1 | Present-to-display p50 |
|---|---|---|---|---|---|---|---|
| Full screen, auto | Hardware: Legacy Flip | 0 | 1 | 0 of 1,793 | 0 | 0 of 2,019 | 31.3 ms |
| Full screen, `=0` | Hardware: Legacy Flip | 1 | 0 | 206 of 1,796 (11.5 %) | 0 | 0 of 2,022 | 0.8 ms |
| Windowed, auto | Composed: Copy with GPU GDI | 0 | -1 | 0 of 1,793 | 0 | 3 of 2,023 | 31.5 ms |
| Windowed, `=0` | Composed: Copy with GPU GDI | 0 | -1 | 6 of 1,794 | 0 | 0 of 2,031 | 31.5 ms |

- Full screen presents with tearing off and interval 1 under the default, on every one of its frames, and with tearing on
  and interval 0 when the variable asks for 0. Windowed is composed and reads the same under both (PresentMon
  reports no sync interval, -1, for the compositor's copy), so the default does not change a windowed window.
- This run's full-screen latency under interval 1 was 31 ms, not the 15 ms of the experiment's table above, and its
  interval-0 run had no stalls where that one had 109; both are one 30 s run each, and a session that measures latency
  should take more than one.
- The three windowed frames with no tick (3 of 2,023) are not in the control's run; a windowed window's swap is the
  same under both, and one 30 s run each does not say whether they are more than the run's own noise.

### Linux

The EGL window ([below](#egl-on-x11-on-linux)) **keeps an interval of 0, windowed and full screen alike**, on a Linux
session's evaluation of Windows' change (jonny-precision, 2026-11-09, both GPUs, windowed and full screen, Sentinel's
frame log): an interval of 1 did not make `eglSwapBuffers` wait - it returned in about 0.5 ms at 0 and at 1, Mesa and
NVIDIA both queueing the frame - so it gave the loop no clock and took nothing from the judder, which was the timer's
pacing; and Cinnamon's Muffin composites full-screen windows too (`unredirect-fullscreen-windows` false), so nothing
reaches the panel part-way through a refresh whatever the interval. What did take the judder away was pacing the frame
callback on the display's own clock, which Linux now has from the X server's Present extension
([frame-pacing.md](frame-pacing.md#what-this-measured-on-linux)): every frame one tick, its wakes 0.05 ms apart. A desktop
that does unredirect a full-screen window could tear at 0, and is the case to measure before changing it. macOS's
Metal drawable is paced by the display link and is not changed either.

### What it did live (2026-11-07, swap interval 0)

Measured on `Legion5i` (RTX 4070 Laptop, GL 4.6.0 NVIDIA 610.62), 2026-11-07 from 23:24 to 23:29 Pacific time, one
3840 x 2160 display at 59.941 Hz, with Sentinel's live route (its `docs/app/motion.md`: a probed build, scripted play
through `--smoke`, 2,400 frames a run, the frame log's compositor reports and a Desktop Duplication capture of the tick
written into the frame). Three builds of Sentinel `8a5d68d`, each windowed at its 150 % and full screen: as it is, on
its own copy of the toolkit; on this toolkit with the GPU asked for; and that same binary with `RECKLESS_GPU_DRAWING=off`,
the control that differs from the GPU run in nothing but the drawing. One round, no compiler, linker, `ninja`, `ctest` or
`ffmpeg` running throughout, the binaries' hashes the same after as before.

| Run | Refreshes not one tick (the compositor's reports) | Late, shown twice, dropped (the compositor's count) | Woke to handed over: mean, p99 | Paint, p50 | Hand-over, p50 / p99 |
|---|---|---|---|---|---|
| 150 %, today's build | 9 of 2,278, 0.40 % | 0, 0, 0 | 6.64 ms, 13.02 | 5.12 ms | 0.79 / 2.35 ms |
| 150 %, GPU | 15 of 2,282, 0.66 % | 0, 0, 0 | 5.85 ms, 9.57 | 4.73 ms | 0.67 / 2.79 ms |
| 150 %, GPU build, GPU off | 3 of 2,278, 0.13 % | 0, 0, 0 | 5.44 ms, 6.86 | 4.66 ms | 0.68 / 1.15 ms |
| Full screen, today's build | 7 of 2,279, 0.31 % | 0, 0, 0 | 9.18 ms, 13.46 | 8.05 ms | 1.86 / 2.72 ms |
| Full screen, GPU | 8 of 2,280, 0.35 % | 0, 0, 0 | 5.58 ms, 8.90 | 4.64 ms | 0.69 / 1.88 ms |
| Full screen, GPU build, GPU off | 11 of 2,278, 0.48 % | 0, 0, 0 | 11.83 ms, 16.97 | 9.25 ms | 2.28 / 7.07 ms |

- **The display clock still paces every frame.** Every run held the clock (`is_locked` on all 2,280 frames), the
  compositor's blanks read 59.941 Hz in all six, and a swap interval of 0 changed nothing of that.
- **What the screen showed.** On the CPU raster the capture found one refresh in 1,051 that was not one tick at 150 % and
  none in 1,278 full screen for today's build; for the GPU build with the GPU off, nothing at 150 % and one late frame
  full screen, outside five seconds of the game's own banner, whose state does not tick and which is set aside as Sentinel
  sets it aside. **The capture cannot read the GPU runs**: on a GPU canvas Sentinel's HD look draws its frame at 1080
  lines and scales it (its `PaintCapture` fallback, density 1 where the others are 2), and the scaling filters the tick
  the probe writes into the corner, which the capture reads only to the colour. The GPU runs' figures are the frame log's
  alone.
- **One hitch, on the GPU only.** Both GPU runs have one frame of 65 and 82 ms, at the same moment of the scripted play,
  29.8 seconds in, nearly all of it in the present (60.5 and 76.2 ms). Nothing like it is on the CPU raster. It is
  most likely Ganesh compiling the shaders for something drawn for the first time, which Skia does as it first draws it;
  not yet shown. It accounts for the GPU run's extra refreshes that were not one tick at 150 %.
- **The frame's cost on the CPU.** Full screen at 4K the GPU's frame cost 5.58 ms mean against 9.18 and 11.83 on the CPU
  raster, and its hand-over 0.69 ms against 1.86 and 2.28, with no 4K blit through GDI. But this is not like for like:
  on the GPU the HD look drew its 1080-line frame and scaled it, where the CPU raster drew at 2160 lines, and the picture
  is the softer for it. At 150 % the two CPU-raster runs, one path in two builds, differ by 1.2 ms in their means, so one
  round does not separate the GPU from the CPU raster there.
- **Tearing was not measured.** Desktop Duplication hands over whole frames the compositor composed, before they are
  scanned out, so it cannot see a tear; nor can the frame log. PresentMon did, on 2026-11-09, and found it (the table
  above in "Interval 1 in full screen, measured").

## Size, scale, full screen, another monitor

Everything that changes a window's pixels reaches the child window as `ChildWindow::SetScale`: a resize and full
screen as a new size of the root at the same scale, a change of DPI and a move to a monitor of another DPI as a new
scale (`app/AppRun.cpp`, `app/win/AppWin.cpp`'s `WM_DPICHANGED` and `WM_DISPLAYCHANGE`). There the window makes its
surface again at the new size, as it makes its raster again on the CPU, and draws the whole of it. The window's own
framebuffer follows the window's size by itself, and its wrapping is made again whenever the size it is presented at
changes. A move to a monitor of the same DPI changes nothing of the window's, so nothing is made again: the context
stays on the GPU it was made on, and Windows carries a frame to a display on another adapter. An app's full screen on
Windows is borderless, a window over the monitor ([../app/platform.md](../app/platform.md#full-screen)), so there is no
exclusive mode for the GPU path to enter or leave.

## A GPU lost

A driver reset, a driver update, a GPU taken away: the GPU is lost where the robust context's reset status says so,
where Skia has abandoned its context, or where the context can no longer be made current. The window then lets its
surface and its context go and makes them again, and draws the whole window on the new ones; `DrawingState::contexts_lost`
counts it. **A driver that keeps resetting is given up**: after `window::kMostGpuContextsLost` (3) the window draws with
the CPU raster and says `kContextLostTooOften`, and where a context cannot be made again it says
`kContextNotMadeAgain`. The window keeps the GL pixel format it was given, and the CPU raster is handed to it through
`SetDIBitsToDevice` as before; the suite holds that it is shown.

## Metal on macOS

The second back end, on the shared seam below: `window/mac/GpuDrawingMac.mm` and its seam with the window,
`window/mac/GpuPresenterMac.h`, compiled with ARC into `reckless_window_gpu` beside `GpuSurfacePresenter.cpp`. Nothing
of Skia changed for it: the pinned pack (`m144-e2e6623374-4`) has `GrDirectContexts::MakeMetal`,
`GrBackendRenderTargets::MakeMtl` and `SkSurfaces::WrapCAMetalLayer` in both slices, and the library builds universal,
arm64 and x86_64, for the 13.3 floor.

### How a frame is drawn there

1. **The device that drives the window's display** - `CGDirectDisplayCopyCurrentMetalDevice` of the display the
   window's screen is, or the main display's for a view in no window yet - not `MTLCreateSystemDefaultDevice`, so a Mac
   with two GPUs does not draw on one and composite on the other. None is `kNoMetalDevice`, as is a device that will
   give no command queue. The renderer string is the device's name and "Metal" ("Apple M6, Metal").
2. **Skia's context on it**, `GrDirectContexts::MakeMetal` on the device and a command queue of the presenter's own,
   each handed over with a retained bridge, which Skia releases (`GrMtlBackendContext`, under ARC).
3. **A `CAMetalLayer` added over the view's own layer**, filling it and following it (`autoresizingMask`): BGRA8Unorm,
   the layout the CPU raster hands the window too; three drawables; `displaySyncEnabled`, so a drawable is shown at the
   display's blank; `allowsNextDrawableTimeout`, so a wait for a drawable gives up after a second rather than holding the
   window's thread; `framebufferOnly` (a suite alone turns it off, to read the drawables back); and the **device RGB
   colour space** the CPU raster's image is tagged with (`CGColorSpaceCreateDeviceRGB`), so the two paths are handed to
   the compositor alike. A flipped view's layer would flip its sublayers' contents, so the layer's own `geometryFlipped`
   is set where `contentsAreFlipped` says so, and the suite holds that the frame is the right way up.
4. **The views draw into the shared seam's offscreen surface**, which keeps its pixels as the CPU raster does.
5. **The present**: the layer fitted to the view as it is now - its frame the view's bounds, its `contentsScale` the
   window's backing scale and its `drawableSize` the surface's pixels, read on every frame - then `nextDrawable`, its
   texture wrapped with `GrBackendRenderTargets::MakeMtl`, the surface copied over it **top-left to top-left**, with no
   flip (a Metal texture and the views' surface are both top-down, where GL's framebuffer on Windows is bottom-up), Skia's
   work submitted, and `presentDrawable` in a command buffer on the **same queue**, so the drawable is shown only once
   the frame is in it. A layer that has no drawable to give in time is a frame not shown; the next is drawn whole.

### Painted on the tick, not through drawRect:

On the CPU raster the window is drawn by AppKit, which calls `drawRect:` and keeps a backing store the view draws into.
**On the GPU the window paints and presents in the frame callback's own tick** - the timer, or the display link the
product asked to be paced on ([frame-pacing.md](frame-pacing.md)) - straight after the frame callback, so the frame
presented is the one the product's frame asked for and there is one present a frame. Where no frame callback runs, an
invalidation is painted on the main queue once the event in hand is done, however many came before it. The view is
taken off its backing store meanwhile: its redraw policy `NSViewLayerContentsRedrawNever`, its layer's contents let go,
and `wantsUpdateLayer` answering YES, so that should AppKit display the view at all it calls `updateLayer`, which
leaves the contents empty, and makes no backing store - at 4K, 33 MB beside the GPU's own. The suite holds that the
view's layer has no contents while the window is on the GPU. All of it is put back with the CPU raster.

A frame is presented only to a window that is ordered in, not in the Dock and not hidden - what `Showing()` answers -
and the one painted meanwhile is presented as the window's occlusion next changes. A window that is merely covered, or
behind a locked screen, is presented to as usual: its drawables come back at once. Measured on mac-0 (Apple M6, macOS
27.2, its screen locked, 2026-11-08), 120 frames presented one after another took at most 5.1 ms each to be given a
drawable, ordered in or out, so holding frames back from a covered window would gain nothing.

### Scale, a move, full screen

The backing scale is read on every paint and every present, as the CPU raster's draw reads it, so a move to a screen
of another scale - which reaches the view as `viewDidChangeBackingProperties`, and the window as a whole repaint - and
full screen and a resize, which reach it as `SetScale`, make the surface and the drawables again at the new size. A
move to a display that another GPU drives keeps the device it was made on: the drawables are then composited across
GPUs, which works and costs a copy, and the presenter is not made again for it.

### Lost

Metal reports no reset status of GL's kind. The GPU is lost where Skia's context has been abandoned, or where a
presented frame's command buffer finished with `MTLCommandBufferErrorDeviceRemoved` (an external GPU unplugged),
`AccessRevoked` (the system took the GPU from the process) or `Timeout` (a hang the driver reset) - that is the back
end's `IsDeviceReset`. A loss found as a frame is painted for a window that is not shown is acted on then, not at a
present that may be long in coming. What follows is the shared seam's: made again up to `kMostGpuContextsLost` times,
each presenter taking its layer off the view as it goes, and then the CPU raster, drawn through `drawRect:` again.

### What it did

- **mac-0** (Apple M6, macOS 27.2, one 1920 x 1080 display at 60 Hz, backing scale 1, its screen locked), 2026-11-08:
  the GPU suite's eight cases pass and none skips - the known frame read back off the canvas and off the very drawable
  presented, at 1, 2 and 1.5 and three sizes, lost and made again, given up, back to the CPU raster and on again, and
  the fallbacks. Paced on the display clock at a divisor of 1 for five seconds, 300 frames were handed to the display
  at 16.67 ms (p1 15.55, p99 17.62). A locked screen composes nothing, so no drawable reported a time it was shown.
- **jonny-macbook** (M1 Pro, macOS 27.0.1, its 3024 x 1964 Retina XDR display at 120 Hz, backing scale 2),
  2026-11-08: the same eight cases pass, a 240 x 160 view presented as 480 x 320 drawables. Paced on the display clock
  at a divisor of 1 for five seconds, 599 frames were handed over (p50 8.40 ms, p1 4.68, p99 12.57) and 597 were
  shown, by each drawable's `presentedTime`, at 8.33 ms from p1 to p99, one at 16.67 (a refresh missed) and two never.
- **The colours** (the same two machines and day). The drawable holds exactly the bytes drawn (230, 20, 30 and the
  other three quarters), as the CPU raster does, and both are tagged device RGB. On the XDR panel, whose window colour
  space is "Color LCD", AppKit's own drawing of the CPU-raster window (`cacheDisplayInRect:`) gave D3 33 2D, 61 CF 49,
  20 28 D3 and EE E6 4F for the four quarters; ColorSync's conversion of the drawable's bytes from device RGB to the
  window's space gave the same four to the byte. ColorSync takes device RGB as sRGB here: an sRGB tag gave them too.
  On mac-0's sRGB display all three are the bytes drawn. What is not measured is the window server's own conversion of
  the layer's tag, since nothing over ssh may record the screen; the two paths hand it the same bytes in the same
  colour space.
- **The toolkit's whole suite on a Mac**, which mac-0's run was cut off before it finished: on `jonny-macbook`,
  2026-11-08, all 21 of the toolkit's tests pass with the GPU back end built, the window suites among them
  (`gpu_window` in 3.45 s, `child_window` in 6.42 s with its one Linux drop case skipped). The window-only suite's
  binary names no Metal and the GPU suite's names it, read off each with `otool -L`.

## EGL on X11 on Linux

The third back end, on the shared seam below: `window/linux/GpuDrawingLinux.cpp` and its seam with the window,
`window/linux/GpuPresenterLinux.h`, compiled into `reckless_window_gpu` beside `GpuSurfacePresenter.cpp`. The pinned
pack has only GLX's native interface on Linux x64, so Skia is handed an interface assembled from `eglGetProcAddress`
(`GrGLMakeAssembledInterface`), which `EGL_KHR_client_get_all_proc_addresses` makes good for the core functions too:
no GL header is included and libGL is not linked. libGLX is on the link line all the same, because the pack's GL
context maker shares an object with its GLX interface; nothing of it is called. It is linked `--no-as-needed`: under
Ubuntu's default `--as-needed` a shared library is kept only where something before it on the line needs it, CMake puts
it before `libskia.a`, and Sentinel's link dropped it. libEGL's files are found by hand
(`find_path`, `find_library`) rather than through FindOpenGL, which makes its EGL target only where libOpenGL's are
found too (CMake 3.28), and **where they are missing the back end is none**, as it was, and configuring says so: a CI
image needs `libegl-dev` (`.github/actions/cpp/action.yml`, `tools/setup-dev.sh`).

### How a frame is drawn there

1. **An EGL display on the window's own X11 connection**, `eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, ...)`. Each
   child window has a connection of its own, so each has a display of its own, and the presenter terminates it.
2. **A software device refused before any context is made**: the display's device is asked for its extensions
   (`EGL_EXT_device_query`), and one that names `EGL_MESA_device_software` - Xvfb's, `LIBGL_ALWAYS_SOFTWARE`'s, an X
   server with no DRI3 - is `kSoftwareRenderer`. A machine that has only llvmpipe, which CI's runners are and which the
   chess matches run on, so never starts a software rasteriser's threads for a window. NVIDIA's EGL names no such
   extension, and answers no.
3. **A config whose native visual is the window's own** (the window was made with its parent's), eight bits of red,
   green and blue, a stencil where there is one, and the fewest samples and depth bits after that.
4. **A desktop GL context, robust where the driver makes one**: EGL 1.5's own `EGL_CONTEXT_OPENGL_ROBUST_ACCESS` and
   lose-on-reset first, `EGL_KHR_create_context`'s flags after, a plain context last. `EGL_EXT_create_context_robustness`
   is not asked: its attributes are for OpenGL ES alone, and Mesa 25.2.8 and NVIDIA 580.178.04 both refused them for
   desktop GL with `EGL_BAD_ATTRIBUTE`. A robust context's `glGetGraphicsResetStatus` is the back end's `IsDeviceReset`,
   beside `EGL_CONTEXT_LOST` from a make-current or a swap.
5. **The renderer read with no surface, then refused or accepted**: with `EGL_KHR_surfaceless_context` the context is
   made current with no surface, so a GL that is refused - a software renderer `IsSoftwareRenderer` names, a GL older
   than 3.0 - never had the window. Only then is a window surface made on the window and made current, and **the draw
   and read buffers set to `GL_BACK`**: a context first made current with no default framebuffer starts with both at
   `GL_NONE` (OpenGL 4.6 core, 17.4.1 and 18.2.1), and binding a surface after does not change them. NVIDIA keeps to
   that, and until they were set every frame went nowhere and the window read back black; Mesa starts them at
   `GL_BACK` regardless.
6. **Skia's context on the assembled interface, and a swap interval of 0** (the "Linux" note above says why it stays
   0): the frame callback runs on the host's loop - on the display's clock where a product asks for it, on the loop's
   timer otherwise - and a swap that waited for the blank would hold that loop, which every window on it shares.
7. **The present** is the shared seam's: the views' surface copied over the window's framebuffer 0, wrapped bottom-up
   at the window surface's own size (`eglQuerySurface`, which follows the X window's even when the server has not yet
   reported a resize), and `eglSwapBuffers`. A 24-bit visual's buffer has no alpha, so it is wrapped as RGB8.

The popups stay on the CPU raster and `XPutImage`, as do a window that is not asked, a refusal and a loss: the window
falls back to the raster it always had, and the suite holds that it is shown.

### Two GPUs, PRIME and Xvfb

The display is the X server's, so the GPU is the one EGL's vendor library picks for it. On a laptop with PRIME render
offload that is the integrated GPU, and the discrete one where the product is started with
`__NV_PRIME_RENDER_OFFLOAD=1 __GLX_VENDOR_LIBRARY_NAME=nvidia` (glvnd's EGL follows them as GLX does); the toolkit
chooses neither. Under Xvfb, and wherever the user cannot open a render node, Mesa offers only llvmpipe, which step 2
refuses.

### What it did

- **jonny-precision** (Dell Precision 5530, Linux Mint 22.3, Cinnamon on Xorg, Intel UHD 630 on Mesa 25.2.8 driving
  the display and a Quadro P2000 on NVIDIA 580.178.04 by PRIME render offload), 2026-11-08, `RECKLESS_REQUIRE_WINDOW=1
  RECKLESS_REQUIRE_GPU=1`: on the Intel GPU all nine of the GPU suite's cases pass - the known frame read back off the
  canvas and off the window (`00E6141E 0014D228 001E28DC 00F0E614`) at 1 and at 1.5 and other sizes, lost and made
  again, given up, back to the CPU raster and on again, a popup over it, a software GL refused under
  `LIBGL_ALWAYS_SOFTWARE=1`, and the fallbacks. On the P2000 eight pass and the software case skips, since NVIDIA's EGL
  ignores `LIBGL_ALWAYS_SOFTWARE`.
- **Under Xvfb**, as CI runs it, every GPU case is refused at step 2 with no context made, and the toolkit's whole
  suite, all 21 tests, passes. The window-only suite (`child_window`) holds that it loads no EGL or GL.

## Falling back

Every refusal is a reason (`window::DrawingFallback`), said in words by `window::TextOf`:

| Reason | When |
|---|---|
| `kNotAsked` | no one asked for the GPU |
| `kForcedOff` | the environment says `RECKLESS_GPU_DRAWING=off` |
| `kNoGpuOnThisPlatform` | Linux built where libEGL's files were not found, whose back end is none |
| `kNotLinked` | the request named no back end |
| `kNoPixelFormat`, `kNoContext`, `kGlTooOld`, `kNoSkiaInterface`, `kNoSkiaContext` | the step of making the GPU that refused |
| `kSoftwareRenderer` | the GL is a software rasteriser (llvmpipe, softpipe, swrast) or, on Linux, EGL's device is a software one |
| `kNoMetalDevice` | macOS: no Metal device drives the window's display, or it gives no command queue |
| `kNoSurface` | Skia would not make a surface the window's size |
| `kContextLostTooOften`, `kContextNotMadeAgain` | the GPU was lost and given up |

**`RECKLESS_GPU_DRAWING=off` turns the GPU off for a whole run**, as `RECKLESS_UPDATE_CHECK=off` turns the update
check off: any other value, and none, leaves it to the request. It is for a person whose driver misdraws, for a suite
that is to hold the fallback on any machine, and for comparing the two paths with nothing rebuilt. It is said first, on
every platform, so a run with the GPU turned off reports that rather than a platform's reason.

## What a product can ask

`DrawingNow()` says which the window draws with. A view can also ask the canvas it is painting: its `SkCanvas`
(`gfx::SkiaCanvas::skia()`) has a `recordingContext()` on the GPU and none on the CPU raster, and on the GPU
`accessTopLayerPixels` answers null, because the pixels are not in memory. A view that writes pixels by hand needs
another way there. Sentinel's HD skin already has one: where it cannot write the canvas's pixels it paints its frame
into a raster of its own at 1080 lines and draws that scaled (its `hd_skin.cpp`, `PaintIntoPixels` and the
`PaintCapture` fallback), and that path is what a GPU canvas reaches unchanged.

**On Linux built without libEGL's files** the request is accepted and answered with the CPU raster,
`kNoGpuOnThisPlatform`, and `reckless_window_gpu` there is a back end of none (`window/GpuDrawingNone.cpp`), so a
product may link it and ask on every platform alike.

## The shared seam

Every back end is the same two halves, so a new platform writes only the second.

- **`window/GpuPresenter.h`** is what a window asks of a presenter (`SurfaceOfSize`, `BeginFrame`, `Present`) and what a
  back end's attempt came to (`GpuPresenterMade`: the presenter, or null and the `DrawingFallback`, and the renderer
  string). It names nothing of Skia's GPU, GL or Metal, so a window includes it and a binary that never links a back end
  stays as small as it was.
- **`window/GpuSurfacePresenter.h` and `.cpp`**, in `reckless_window_gpu`, are `SkiaGpuPresenter`, the neutral half: the
  offscreen `SkSurfaces::RenderTarget` that keeps its pixels from frame to frame, its copy over the back buffer, Skia's
  shut down while the context is current, and the lost-GPU test (Skia's context abandoned, or the platform's word that
  the device was reset). A back end derives from it and supplies four things: `MakeCurrent`, `BackBuffer` (the window's
  buffer wrapped for Skia), `SwapBuffers`, and `IsDeviceReset` where its API reports a reset.
- **The platform's own file** holds the rest - for Windows `window/win/GpuDrawingWin.cpp`: the pixel format, the WGL
  context, the probe, and `GpuBackend`'s `make` function, whose signature is the platform's (`GpuPresenterWin.h`).
- **`window/GpuDrawing.h`** holds the words, shared by all: `kSoftwareRenderer` and `kNoMetalDevice` joined the reasons,
  with `IsSoftwareRenderer` to read a GL renderer string (llvmpipe, softpipe, swrast, a "software rasterizer"). Where
  the GPU is meant in general the words say so ("the GPU's graphics API"); the GL-specific reasons still say GL.
  `ChosenDrawing` never answers the new two: they are a back end's own, found only when it tries, after the environment,
  the platform and the back end have been checked in the order above.

### The back ends

- **WGL on Windows**, the first, described from [How a frame is drawn](#how-a-frame-is-drawn) on.
- **Metal on macOS (T1)**: [Metal on macOS](#metal-on-macos), above.
- **EGL on X11 on Linux (T2)**: [EGL on X11 on Linux](#egl-on-x11-on-linux), above.

## Reading a GPU window back off the screen

GDI's read of the screen (`BitBlt` from the screen's device context) cannot be relied on to see what a GL window
presented. On `Legion5i`, 2026-11-07, with the display asleep and the compositor composing nothing, it went on
returning the CPU raster's windows - which it answers from their own GDI surfaces - while the GL window read as the host
window's background behind it, so a read that agrees with a CPU window says nothing about a GL one. The suite therefore reads the screen through **DXGI Desktop Duplication**, which hands over what
the compositor composed, and checks its reader first on a window on the CPU raster: where that control reads nothing -
a display that is off, a locked desktop, a service's window station - nothing is held of what the swap presented, and
the case says so and skips.
A window station that paints nothing at all (a runner service in Session 0 with nobody logged in) skips the cases
that wait for a first paint, too.

## What is held, and where

- **The policy, on every platform and in the builds with no GUI**: `tests/test_gpu_drawing.cpp`, in the `view`
  suite - a default request, the order the refusals are checked in, the environment's word, the GL versions drawn on,
  how often a lost GPU is made again, every reason in words of its own (the software-renderer and Metal ones included),
  the software renderers `IsSoftwareRenderer` names, and what a window with no GPU path answers.
- **The window that is not asked**: `skia/tests/test_child_window.cpp`, `ChildWindowDrawing` - on Windows, a window that
  links no back end loads no GL, answers `kNotAsked`, paints on a canvas with no GPU context whose pixels are the raster
  `SetDIBitsToDevice` is handed, and answers `kNotLinked` to a request with no back end. On macOS
  (`skia/tests/test_child_window_mac.mm`) the suite's own binary is read with `otool -L` and names no Metal - read from
  its load commands, not from what is loaded, since AppKit loads Metal into every process that makes a window - and a
  request with no back end is answered `kNotLinked` with no layer put over the view.
- **The GPU window**: `skia/tests/test_gpu_window.cpp`, the `gpu_window` CTest, which links `reckless_window_gpu`. On
  Windows, a known frame drawn on the GPU and read back off the canvas and off the screen; drawn again at three other
  scales and sizes; its GPU lost once and made again, then lost until it is given up for the CPU raster, which is shown;
  put back on the CPU raster and on the GPU again; the fallback with `RECKLESS_GPU_DRAWING=off`; and `kNotLinked`. Each
  GPU case skips with the window's own reason where this machine's GL will not draw. On macOS
  (`skia/tests/test_gpu_window_mac.mm`, Objective-C++): the layer as described above, on the device driving the
  window's display; the known frame read back off the canvas and off the very drawable presented, copied on the queue it
  was drawn on (`window::WatchMetalPresents`, a suite's alone, which leaves its drawables readable), the right way up
  and the backing scale's size; no backing store kept beside it; drawn again at three other scales and sizes, the
  drawables made again at each; lost and made again, then given up for the CPU raster with no layer left over the view;
  put back on the CPU raster and on the GPU again; a product's drawables `framebufferOnly`; the fallback with
  `RECKLESS_GPU_DRAWING=off`; `kNotLinked`; and the suite's own binary naming Metal, the control for the window suite's
  holding that its binary does not. Each GPU case skips where no Metal device drives the display. On Linux, on EGL:
  the known frame read back off the canvas and off the window (`XGetImage`, after a window on the CPU raster read the
  same way as the control), at other scales and sizes, lost and made again and given up, back to the CPU raster and
  on again, a popup over a GPU window kept on the CPU raster, a software GL refused, the fallback with
  `RECKLESS_GPU_DRAWING=off`, `kNotLinked`, and the suite loading libEGL and not libGL. Each GPU case skips with the
  window's own reason where EGL will not draw, as under CI's Xvfb, unless `RECKLESS_REQUIRE_GPU=1` says a GPU is there,
  and then it fails. The window suite (`ChildWindowDrawing`) holds that a window not asked loads no EGL or GL.
