# Drawing a window on the GPU

A child window draws its view tree with Skia's CPU raster and hands the pixels to the platform, and has done since the
toolkit began: `SkSurfaces::WrapPixels` and `SetDIBitsToDevice` on Windows, a `CGImage` in `drawRect:` on macOS,
`XPutImage` on Linux. So a window now carries an **opt-in GPU path** beside it, on Windows first: Skia's Ganesh on
OpenGL through WGL, each frame drawn into a surface on the GPU and presented with a swap. A product asks for it, and the
window gives it where it can, says so, and draws with the CPU raster where it cannot and says why.

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
  GL into the host. So the window names nothing of Ganesh (`window/win/GpuPresenterWin.h` is the seam), a binary that
  never links `reckless_window_gpu` carries none of it, and the window's own suite holds that: it links the window alone,
  and asserts `opengl32.dll` is not in its process. The window's suite grew by 15,360 bytes with this change, its tests
  included.
- A request for the GPU with no back end, `{Drawing::kGpu, nullptr}`, is answered `kNotLinked`.
- **Linking.** Ganesh's GL is compiled into the pinned skia-pack's `skia.lib` beside its Direct3D, whose objects it
  shares a link with, so `reckless_window_gpu` puts `d3d12` and `d3dcompiler` on the link line too (`dxgi` is not
  needed). Nothing of either is called, and neither becomes a library the binary loads: the probe above imported
  `OPENGL32.dll` and `KERNEL32.dll` and nothing else.

## How a frame is drawn

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

**The swap interval is 0.** The frame callback is already paced on the display's own clock
([frame-pacing.md](frame-pacing.md)): a waitable timer snapped to the compositor's vertical blank, with a lead the
product chooses, on a thread of the window's own that posts each frame to the window. A swap interval of 1 would make
`SwapBuffers` wait for a blank as well - a second clock beside the first, holding the window's thread for up to a
refresh while it waited, and throwing away the lead, which exists to finish the frame just before the blank rather than
a refresh early. With 0 the swap does not wait, the frame reaches the compositor as soon as the GPU has drawn it, and
the compositor shows it at the next blank, which is what the CPU raster's `SetDIBitsToDevice` has always done.
`wglSwapIntervalEXT` is asked for 0 where the driver has it.

### What it did live

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
  scanned out, so it cannot see a tear; nor can the frame log. Whether a full-screen GL window with a swap interval of 0
  tears is for an eye or a camera, or for PresentMon, whose present mode says whether the window was composed or flipped
  independently.

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

## Falling back

Every refusal is a reason (`window::DrawingFallback`), said in words by `window::TextOf`:

| Reason | When |
|---|---|
| `kNotAsked` | no one asked for the GPU |
| `kForcedOff` | the environment says `RECKLESS_GPU_DRAWING=off` |
| `kNoGpuOnThisPlatform` | macOS and Linux, for now |
| `kNotLinked` | the request named no back end |
| `kNoPixelFormat`, `kNoContext`, `kGlTooOld`, `kNoSkiaInterface`, `kNoSkiaContext` | the step of making the GPU that refused |
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

**On macOS and Linux** the request is accepted and answered with the CPU raster, `kNoGpuOnThisPlatform`, and
`reckless_window_gpu` there is a back end of none (`window/GpuDrawingNone.cpp`), so a product may link it and ask on
every platform alike. The pinned skia-pack has Ganesh GL and Metal on macOS, GLX's GL on Linux x64 and EGL's GLES on
Linux arm64, for when those platforms are done.

## Reading a GPU window back off the screen

GDI's read of the screen (`BitBlt` from the screen's device context) cannot be relied on to see what a GL window
presented. On `Legion5i`, 2026-11-07, with the display asleep and the compositor composing nothing, it went on
returning the CPU raster's windows - which it answers from their own GDI surfaces - while the GL window read as the host
window's background behind it, so a read that agrees with a CPU window says nothing about a GL one. The suite therefore reads the screen through **DXGI Desktop Duplication**, which hands over what
the compositor composed, and checks its reader first on a window on the CPU raster: where that control reads nothing -
a display that is off, a locked desktop, a service's window station - nothing is held of what the swap presented, and
the case says so and skips.

## What is held, and where

- **The policy, on every platform and in the builds with no GUI**: `tests/test_gpu_drawing.cpp`, in the `view`
  suite - a default request, the order the refusals are checked in, the environment's word, the GL versions drawn on,
  how often a lost GPU is made again, every reason in words of its own, and what a window with no GPU path answers.
- **The window that is not asked**: `skia/tests/test_child_window.cpp`, `ChildWindowDrawing` - on Windows, a window that
  links no back end loads no GL, answers `kNotAsked`, paints on a canvas with no GPU context whose pixels are the raster
  `SetDIBitsToDevice` is handed, and answers `kNotLinked` to a request with no back end.
- **The GPU window**: `skia/tests/test_gpu_window.cpp`, the `gpu_window` CTest, which links `reckless_window_gpu`. On
  Windows, a known frame drawn on the GPU and read back off the canvas and off the screen; drawn again at three other
  scales and sizes; its GPU lost once and made again, then lost until it is given up for the CPU raster, which is shown;
  put back on the CPU raster and on the GPU again; the fallback with `RECKLESS_GPU_DRAWING=off`; and `kNotLinked`. Each
  GPU case skips with the window's own reason where this machine's GL will not draw. Elsewhere, the request answered
  with the CPU raster.
