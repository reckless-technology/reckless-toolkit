#pragma once

// The seam between the child window on Linux (WindowLinux.cpp) and its GPU back end (GpuDrawingLinux.cpp, in
// reckless_window_gpu): the one function that makes a window's presenter. The presenter itself is the platform-neutral
// GpuPresenter (GpuPresenter.h). The window names nothing of Skia's GPU, of EGL or of GL here, so a binary that links
// the window and not the back end - every plug-in, and every app that does not ask - carries none of it and loads no
// libEGL. X11/Xlib.h is the includer's, before this.

#include "GpuPresenter.h"

namespace reckless::window
{
/** The GPU back end on Linux (GpuDrawing.h's opaque GpuBackend): the one function that makes a window's presenter. */
struct GpuBackend
{
    /** A presenter on `window`: an EGL display on `display`'s connection, a config of the window's own visual, a GL
        context and a window surface on it, and Skia's GPU context on those - or why not.
        @param display  the window's connection to the X server, which outlives the presenter
        @param window   the child window, on whose thread the presenter is made and used ever after
        @param visual   the window's visual, which the EGL config is chosen to match
        @return         the presenter, or null and the reason */
    GpuPresenterMade (*make)(Display *display, ::Window window, Visual *visual);
};
} // namespace reckless::window
