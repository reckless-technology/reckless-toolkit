#pragma once

// The seam between a platform's child window and its GPU back end (reckless_window_gpu): what the window asks of a
// presenter, and what a back end's attempt at one comes to. Nothing here names Skia's GPU, GL or Metal, so a window
// that includes it - and a binary that links the window and not the back end, as every plug-in and every app that does
// not ask does - carries none of it. The platform-neutral half of a presenter, the surface the views draw into and the
// loss bookkeeping, is SkiaGpuPresenter (GpuSurfacePresenter.h); each platform's back end derives from it and adds
// only its own context and present (window/win/GpuDrawingWin.cpp today).

#include <memory>
#include <string>

#include "include/core/SkRefCnt.h"

#include "GpuDrawing.h"

class SkSurface;

namespace reckless::window
{
/** A window's GPU: its context on the window, Skia's GPU context on that, the surface the views draw into and the
    present. One per window, on the window's own thread, made current before each thing it does - so several windows on
    one thread, each with its own, do not draw into one another's. */
class GpuPresenter
{
  public:
    virtual ~GpuPresenter() = default;

    /** The surface the views draw into, `width` x `height` pixels on the GPU: made again where the size has changed,
        cleared, and the one there was kept where it has not. It keeps what is drawn into it from one present to the
        next, as the CPU raster does, so a paint that draws only the part that changed leaves the rest as it was.
        @param width   the width, in the window's pixels, 1 or more
        @param height  the height, in the window's pixels, 1 or more
        @return        the surface; null where Skia would not make one, or the GPU has been lost */
    virtual sk_sp<SkSurface> SurfaceOfSize(int width, int height) = 0;

    /** The GPU context made current on this thread, before the views draw: false where it cannot be - a GPU lost.
        @return  true when the window may draw */
    virtual bool BeginFrame() = 0;

    /** What has been drawn into the surface put on the window: drawn into the window's back buffer, Skia's work
        submitted and the buffers swapped, without waiting for the display - the frame callback's pacing is the
        display's clock, not the swap. False where the GPU has been lost: the driver reset, or Skia's context abandoned.
        @return  true when the frame was presented and the GPU is still there */
    virtual bool Present() = 0;

    /** Told whenever the window's full screen changes, and once the window is made if it is full screen already: a
        back end with a swap interval presents with 1 while the window is full screen, where the compositor would
        otherwise leave a tear, and 0 while it is windowed, where the compositor has already composed the frame
        (docs/window/gpu-drawing.md, "Pacing, and the swap interval"). Takes effect at the next Present. Nothing for a
        back end with no swap interval to set, which is the default.
        @param is_full_screen  whether the window is the app's full screen */
    virtual void SetFullScreen(bool is_full_screen)
    {
        static_cast<void>(is_full_screen);
    }
};

/** What a back end's attempt at a presenter came to: the presenter, or null and why not. */
struct GpuPresenterMade
{
    std::unique_ptr<GpuPresenter> presenter;                   ///< the presenter; null where none could be made.
    DrawingFallback               why{DrawingFallback::kNone}; ///< kNone with a presenter, else what refused.
    std::string                   renderer; ///< the GPU's renderer and version, where a context was made.
};
} // namespace reckless::window
