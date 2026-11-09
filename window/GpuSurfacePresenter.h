#pragma once

// The platform-neutral half of a GPU presenter (GpuPresenter.h), in reckless_window_gpu: the offscreen surface the
// views draw into, which keeps its pixels from frame to frame as the CPU raster does; its copy over the window's back
// buffer; and the bookkeeping of a GPU lost. What a platform adds is four things - its context made current, its back
// buffer wrapped for Skia, its swap, and its word that the device was reset - and the Windows back end (WGL) is the
// first to, Metal on macOS (GpuDrawingMac.mm) the second. EGL on X11 on Linux is to be written against this too.

#include "include/core/SkRefCnt.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"

#include "GpuPresenter.h"

namespace reckless::window
{
/** A presenter over a Skia GPU context, with everything that does not depend on the platform's API. A back end derives
    from it, hands it the Skia context, and implements MakeCurrent, BackBuffer, SwapBuffers and (where its API reports
    one) IsDeviceReset. Its destructor must call ShutDownSkia while its own context is still there. */
class SkiaGpuPresenter : public GpuPresenter
{
  public:
    sk_sp<SkSurface> SurfaceOfSize(int width, int height) final;
    bool             BeginFrame() final;
    bool             Present() final;

  protected:
    /** @param context  Skia's GPU context on the platform's context; not null */
    explicit SkiaGpuPresenter(sk_sp<GrDirectContext> context) noexcept;

    /** Skia's GPU resources let go while the context is current - and abandoned, so a surface someone still holds makes
        no GPU call after. A derived destructor calls it first, with its back buffer already released, and then releases
        its own context.
        @param is_current  whether the context could be made current */
    void ShutDownSkia(bool is_current) noexcept;

    /** The platform's context made current on this thread, where it is not already.
        @return  true when it is current */
    virtual bool MakeCurrent() const noexcept = 0;

    /** The window's back buffer wrapped as a Skia surface, `width` x `height`, bottom-up or top-down as the platform
        holds it. Called on each Present, so a back end that wraps a buffer once keeps the surface and returns it again
        while the size holds.
        @param width   the offscreen surface's width
        @param height  the offscreen surface's height
        @return        the surface; null where it could not be wrapped */
    virtual sk_sp<SkSurface> BackBuffer(int width, int height) = 0;

    /** The buffers swapped, without waiting for the display. Called after Skia's work on the back buffer is submitted.
     */
    virtual void SwapBuffers() noexcept = 0;

    /** Whether the platform's API says the device was reset. The default, for an API that reports none, is no.
        @return  true when the driver reported a reset */
    virtual bool IsDeviceReset() const noexcept;

    /** Whether the GPU has gone: Skia has abandoned its context, or the platform reports a reset.
        @return  true when it is lost */
    bool IsLost() const noexcept;

    /** @return  Skia's GPU context, null once ShutDownSkia has run */
    GrDirectContext *Context() const noexcept
    {
        return context_.get();
    }

  private:
    sk_sp<GrDirectContext> context_;   ///< Skia's GPU context on the platform's.
    sk_sp<SkSurface>       offscreen_; ///< the surface the views draw into, which keeps its pixels.
};
} // namespace reckless::window
