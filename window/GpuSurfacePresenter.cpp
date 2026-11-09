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
SkiaGpuPresenter::SkiaGpuPresenter(sk_sp<GrDirectContext> context) noexcept : context_(std::move(context))
{
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

bool SkiaGpuPresenter::BeginFrame()
{
    return MakeCurrent() && !context_->abandoned();
}

bool SkiaGpuPresenter::Present()
{
    if (!offscreen_ || !MakeCurrent() || context_->abandoned())
    {
        return !IsLost();
    }
    const sk_sp<SkSurface> back_buffer = BackBuffer(offscreen_->width(), offscreen_->height());
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
    SwapBuffers();
    return !IsLost();
}
} // namespace reckless::window
