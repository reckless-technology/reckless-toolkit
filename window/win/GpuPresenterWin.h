#pragma once

// The seam between the child window on Windows (WindowWin.cpp) and its GPU back end (GpuDrawingWin.cpp, in
// reckless_window_gpu): what the window asks of a presenter, and the back end that makes one. The window names nothing
// of Skia's GPU or of GL here, so a binary that links the window and not the back end - every plug-in, and every app
// that does not ask - carries none of it. windows.h is the includer's, before this.

#include <memory>
#include <string>

#include "include/core/SkRefCnt.h"

#include "GpuDrawing.h"

class SkSurface;

namespace reckless::window::win
{
/** A window's GPU: its GL context on the window, Skia's GPU context on that, the surface the views draw into and the
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

    /** The GL context made current on this thread, before the views draw: false where it cannot be - a GPU lost.
        @return  true when the window may draw */
    virtual bool BeginFrame() = 0;

    /** What has been drawn into the surface put on the window: drawn into the window's back buffer, Skia's work
        submitted and the buffers swapped, without waiting for the display - the frame callback's pacing is the
        display's clock, not the swap. False where the GPU has been lost: the driver reset, or Skia's context abandoned.
        @return  true when the frame was presented and the GPU is still there */
    virtual bool Present() = 0;
};

/** What a back end's attempt at a presenter came to: the presenter, or null and why not. */
struct GpuPresenterMade
{
    std::unique_ptr<GpuPresenter> presenter;                   ///< the presenter; null where none could be made.
    DrawingFallback               why{DrawingFallback::kNone}; ///< kNone with a presenter, else what refused.
    std::string                   renderer; ///< the GL's renderer and version, where a context was made.
};
} // namespace reckless::window::win

namespace reckless::window
{
/** The GPU back end on Windows (GpuDrawing.h's opaque GpuBackend): the one function that makes a window's presenter. */
struct GpuBackend
{
    /** A presenter on `window`: its pixel format, its GL context and Skia's GPU context on it, or why not.
        @param window  the child window, on whose thread the presenter is made and used ever after
        @return        the presenter, or null and the reason */
    win::GpuPresenterMade (*make)(HWND window);
};
} // namespace reckless::window
