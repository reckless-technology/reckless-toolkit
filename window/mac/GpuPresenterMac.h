#pragma once

// The seam between the child window on macOS (WindowMac.mm) and its GPU back end (GpuDrawingMac.mm, in
// reckless_window_gpu): the one function that makes a window's presenter, and a watch a suite sets to read back what a
// presenter hands the display. The presenter itself is the platform-neutral GpuPresenter (GpuPresenter.h). The window
// names nothing of Skia's GPU or of Metal here - the protocols below are only declared - so a binary that links the
// window and not the back end, every plug-in and every app that does not ask, links no Metal. Objective-C++ only:
// AppKit's headers are the includer's, before this.

#include <functional>

#include "GpuPresenter.h"

@protocol CAMetalDrawable;
@protocol MTLCommandQueue;

namespace reckless::window
{
/** The GPU back end on macOS (GpuDrawing.h's opaque GpuBackend): the one function that makes a window's presenter. */
struct GpuBackend
{
    /** A presenter on `view`: a Metal device - the one driving the display the view is on - Skia's GPU context on it,
        and a CAMetalLayer added over the view's own layer that each frame is presented in; or why not, with nothing
        left behind.
        @param view  the child window's view, layer-backed, on whose thread (the main one) the presenter is made and
                     used ever after; outlives the presenter
        @return      the presenter, or null and the reason */
    GpuPresenterMade (*make)(NSView *view);
};

/** What a suite is told of each frame a Metal presenter is about to put on the display: the drawable, with the frame
    drawn into its texture and submitted on `queue`, and the queue - so a copy encoded on that queue reads exactly the
    frame the display is handed, and a handler added to the drawable hears when the display showed it. */
using MetalPresentWatch = std::function<void(id<CAMetalDrawable> drawable, id<MTLCommandQueue> queue)>;

/** For a suite, never a product: `watch` told of every frame each Metal presenter made from here on presents, and those
    presenters' drawables left readable (`framebufferOnly` off), which a product's never are - a drawable the display
    alone reads is the cheaper one. Set before the GPU is asked for; empty, as it starts, for none. Main thread only.
    @param watch  told of each frame presented; empty to watch none */
void WatchMetalPresents(MetalPresentWatch watch);
} // namespace reckless::window
