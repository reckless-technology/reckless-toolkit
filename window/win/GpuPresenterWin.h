#pragma once

// The seam between the child window on Windows (WindowWin.cpp) and its GPU back end (GpuDrawingWin.cpp, in
// reckless_window_gpu): the one function that makes a window's presenter. The presenter itself is the platform-neutral
// GpuPresenter (GpuPresenter.h). The window names nothing of Skia's GPU or of GL here, so a binary that links the
// window and not the back end - every plug-in, and every app that does not ask - carries none of it. windows.h is the
// includer's, before this.

#include "GpuPresenter.h"

namespace reckless::window
{
/** The GPU back end on Windows (GpuDrawing.h's opaque GpuBackend): the one function that makes a window's presenter. */
struct GpuBackend
{
    /** A presenter on `window`: its pixel format, its GL context and Skia's GPU context on it, or why not.
        @param window  the child window, on whose thread the presenter is made and used ever after
        @return        the presenter, or null and the reason */
    GpuPresenterMade (*make)(HWND window);
};
} // namespace reckless::window
