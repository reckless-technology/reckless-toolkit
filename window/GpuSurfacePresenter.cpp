// The platform-neutral half of a GPU presenter (GpuSurfacePresenter.h): see there.
#include "GpuSurfacePresenter.h"

#include <utility>

#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColor.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSamplingOptions.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/ganesh/GrTypes.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"

namespace reckless::window
{
SkiaGpuPresenter::SkiaGpuPresenter(sk_sp<GrDirectContext> context, SwapIntervalRequest request) noexcept
    : context_(std::move(context)), swap_request_(request)
{
}

void SkiaGpuPresenter::SetFullScreen(bool is_full_screen)
{
    early_back_buffer_.Release();     // the surface is remade either way; a buffer of the old one cannot be shown
    is_full_screen_ = is_full_screen; // applied at the next Present, where the context is current
}

void SkiaGpuPresenter::SetSwapInterval(int) noexcept
{
}

void SkiaGpuPresenter::ApplySwapInterval() noexcept
{
    const int wanted = SwapIntervalWanted();
    if (wanted != swap_interval_)
    {
        SetSwapInterval(wanted);
        swap_interval_ = wanted;
    }
}

bool SkiaGpuPresenter::IsDeviceReset() const noexcept
{
    return false;
}

bool SkiaGpuPresenter::IsLost() const noexcept
{
    return context_->abandoned() || IsDeviceReset();
}

void SkiaGpuPresenter::ShutDownSkia(bool is_current) noexcept
{
    early_back_buffer_.Release(); // before the context goes, so the back end has its drawable back
    offscreen_.reset();
    if (context_)
    {
        if (is_current && !context_->abandoned())
        {
            context_->flushAndSubmit(GrSyncCpu::kYes);
        }
        context_->releaseResourcesAndAbandonContext();
        context_.reset();
    }
}

sk_sp<SkSurface> SkiaGpuPresenter::SurfaceOfSize(int width, int height)
{
    if (!MakeCurrent() || context_->abandoned())
    {
        return nullptr;
    }
    if (!offscreen_ || offscreen_->width() != width || offscreen_->height() != height)
    {
        offscreen_.reset();
        early_back_buffer_.Release(); // one taken at the old size can no longer be presented
        // RGBA, as the GPU holds it, premultiplied as the CPU raster is; drawn top-down, as the views lay it out.
        offscreen_ =
            SkSurfaces::RenderTarget(context_.get(), skgpu::Budgeted::kNo,
                                     SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType), 0,
                                     kTopLeft_GrSurfaceOrigin, nullptr);
        if (!offscreen_)
        {
            return nullptr;
        }
        offscreen_->getCanvas()->clear(SK_ColorTRANSPARENT);
    }
    return offscreen_;
}

void SkiaGpuPresenter::AcquireBackBuffer()
{
    if (!offscreen_ || !MakeCurrent() || context_->abandoned())
    {
        return;
    }
    const gfx::PointI size{offscreen_->width(), offscreen_->height()};
    early_back_buffer_.TakeNow(size, [this, size] { return BackBuffer(size.x, size.y); });
}

bool SkiaGpuPresenter::BeginFrame()
{
    return MakeCurrent() && !context_->abandoned();
}

bool SkiaGpuPresenter::Present()
{
    if (!offscreen_ || !MakeCurrent() || context_->abandoned())
    {
        early_back_buffer_.Release();
        return !IsLost();
    }
    const gfx::PointI size{offscreen_->width(), offscreen_->height()};
    sk_sp<SkSurface>  back_buffer = early_back_buffer_.ForPresent(size);
    if (!back_buffer)
    {
        back_buffer = BackBuffer(size.x, size.y); // none taken early, or one of another size let go
    }
    if (!back_buffer)
    {
        return !IsLost();
    }
    // The whole frame, as it stands, over the back buffer: the swap leaves the back buffer's pixels undefined, so only
    // the views' own surface keeps them, and the copy is the GPU's, a fraction of a millisecond at 4K.
    SkPaint copy;
    copy.setBlendMode(SkBlendMode::kSrc);
    offscreen_->draw(back_buffer->getCanvas(), 0, 0, SkSamplingOptions(), &copy);
    context_->flushAndSubmit(back_buffer.get(), GrSyncCpu::kNo);
    ApplySwapInterval();
    SwapBuffers();
    return !IsLost();
}
} // namespace reckless::window
