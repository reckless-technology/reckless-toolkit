// The GPU back end on macOS, in reckless_window_gpu: a child window drawn by Skia's Ganesh on Metal. The device is the
// one driving the display the window is on, not merely the system's default, so a Mac with two GPUs does not draw on
// one and composite on the other; Skia's GPU context is made on it and its own command queue, and a CAMetalLayer is
// added over the view's own layer for each frame to be presented in. The views draw into a surface of the window's size
// on the GPU, which keeps its pixels from frame to frame as the CPU raster does, and each frame is that surface drawn
// into the layer's next drawable, Skia's work submitted, and the drawable presented on the same queue - so it is shown
// only once the frame is drawn into it. The drawable is top-down, as Metal's textures and the views' surface both are,
// so the copy is straight, with no flip (GL's back buffer, on Windows, is bottom-up). Compiled with ARC
// (docs/window/gpu-drawing.md).
#import <AppKit/AppKit.h>
#import <CoreGraphics/CGDirectDisplayMetal.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <atomic>
#include <memory>
#include <string>
#include <utility>

#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendContext.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendSurface.h"
#include "include/gpu/ganesh/mtl/GrMtlDirectContext.h"
#include "include/gpu/ganesh/mtl/GrMtlTypes.h"

#include "GpuDrawingBackend.h"
#include "GpuPresenterMac.h"
#include "GpuSurfacePresenter.h"

namespace reckless::window
{
namespace
{
/** The drawables a layer keeps: three, so one can be drawn into while one waits for the display and one is shown. */
constexpr NSUInteger kDrawablesKept = 3;

/** The watch a suite set (WatchMetalPresents); empty for a product.
    @return  the watch, for the process */
MetalPresentWatch &PresentWatch()
{
    static MetalPresentWatch watch;
    return watch;
}

/** The display `view` is on: its window's screen's, or the main display's for a view in no window or on no screen yet.
    @param view  the view
    @return      the display's id */
CGDirectDisplayID DisplayOfView(NSView *view)
{
    NSScreen *screen = view.window.screen != nil ? view.window.screen : NSScreen.mainScreen;
    NSNumber *number = screen.deviceDescription[@"NSScreenNumber"];
    return number != nil ? static_cast<CGDirectDisplayID>(number.unsignedIntValue) : CGMainDisplayID();
}

/** Window points to pixels for `view`: its window's backing scale, or the main screen's for a view in no window yet.
    @param view  the view
    @return      the scale, more than 0 */
CGFloat BackingScaleOf(NSView *view)
{
    const CGFloat backing =
        view.window != nil ? view.window.backingScaleFactor : NSScreen.mainScreen.backingScaleFactor;
    return backing > 0 ? backing : 1.0;
}

/** Whether a command buffer that finished says the GPU has gone from under it: removed (an external GPU unplugged),
    access revoked (the system took the GPU from the process), or timed out (a hang the driver recovered from by
    resetting it). Metal reports no reset status of GL's kind, so this is its word that the device was reset.
    @param buffer  the command buffer, completed
    @return        true when the GPU was lost */
bool IsTheGpuGoneAfter(id<MTLCommandBuffer> buffer)
{
    if (buffer.status != MTLCommandBufferStatusError || buffer.error == nil)
    {
        return false;
    }
    const NSInteger code = buffer.error.code;
    return code == MTLCommandBufferErrorDeviceRemoved || code == MTLCommandBufferErrorAccessRevoked ||
           code == MTLCommandBufferErrorTimeout;
}

/** A window drawn by Skia's Ganesh on Metal: the platform-neutral half is SkiaGpuPresenter's (the surface the views
    draw into, the copy over the back buffer, the loss), and this is the Metal of it. Made by MakeMetalPresenter, which
    leaves it whole or makes nothing. */
class MetalPresenter final : public SkiaGpuPresenter
{
  public:
    /** The presenter over what MakeMetalPresenter made, which it owns from here.
        @param view     the child window's view, held weakly: it outlives the presenter
        @param layer    the layer added over the view's own, presented in
        @param queue    the command queue Skia draws on, which the drawables are presented on too
        @param context  Skia's GPU context on the device and `queue` */
    MetalPresenter(NSView *view, CAMetalLayer *layer, id<MTLCommandQueue> queue,
                   sk_sp<GrDirectContext> context) noexcept
        : SkiaGpuPresenter(std::move(context)), view_(view), layer_(layer), queue_(queue)
    {
    }

    /** Skia's GPU resources let go - every frame it submitted finished first - and the layer taken off the view, so the
        view shows its own layer again, which the window draws into on the CPU raster. */
    ~MetalPresenter() override
    {
        drawable_ = nil;
        ShutDownSkia(true);
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        [layer_ removeFromSuperlayer];
        [CATransaction commit];
    }

    MetalPresenter(const MetalPresenter &)            = delete;
    MetalPresenter &operator=(const MetalPresenter &) = delete;

  private:
    /** Metal has no context to make current: a device and a queue, usable from any thread at any time.
        @return  true */
    bool MakeCurrent() const noexcept override
    {
        return true;
    }

    /** The layer's next drawable wrapped for Skia, the layer first fitted to the view as it is now: its frame the
       view's bounds, its scale the window's backing scale and its drawable `width` x `height` pixels - read on every
       frame, so a move to a screen of another scale or a change of size is followed as it happens. A new drawable each
       frame: the layer hands them round in turn, so there is no one buffer to wrap once and keep.
        @param width   the views' surface's width, in pixels
        @param height  the views' surface's height, in pixels
        @return        the drawable as a surface; null where the layer had none to give in time */
    sk_sp<SkSurface> BackBuffer(int width, int height) override
    {
        FitTheLayer(width, height);
        id<CAMetalDrawable> drawable = [layer_ nextDrawable];
        if (drawable == nil)
        {
            return nullptr; // none free within the layer's second: this frame is not shown, the next is drawn whole
        }
        id<MTLTexture>   texture = drawable.texture;
        GrMtlTextureInfo info;
        info.fTexture.retain((__bridge GrMTLHandle)texture);
        const GrBackendRenderTarget target =
            GrBackendRenderTargets::MakeMtl(static_cast<int>(texture.width), static_cast<int>(texture.height), info);
        // Top-down, as a Metal texture is and the views' surface is, so the copy needs no flip.
        sk_sp<SkSurface> surface = SkSurfaces::WrapBackendRenderTarget(Context(), target, kTopLeft_GrSurfaceOrigin,
                                                                       kBGRA_8888_SkColorType, nullptr, nullptr);
        if (surface)
        {
            drawable_ = drawable;
        }
        return surface;
    }

    /** The drawable presented on the queue Skia's frame was submitted to, so it is shown only once the frame is drawn
        into it - after the suite's watch, where one is set, has been handed the frame. The display's own blank paces
        the showing (`displaySyncEnabled`); nothing here waits for it. */
    void SwapBuffers() noexcept override
    {
        if (drawable_ == nil)
        {
            return;
        }
        if (const MetalPresentWatch &watch = PresentWatch())
        {
            watch(drawable_, queue_);
        }
        id<MTLCommandBuffer> present = [queue_ commandBuffer];
        if (present == nil)
        {
            drawable_ = nil;
            return;
        }
        const std::shared_ptr<std::atomic<bool>> gone = gone_;
        [present addCompletedHandler:^(id<MTLCommandBuffer> done) {
          if (IsTheGpuGoneAfter(done))
          {
              gone->store(true, std::memory_order_release);
          }
        }];
        [present presentDrawable:drawable_];
        [present commit];
        drawable_ = nil;
    }

    /** Whether a frame presented since the presenter was made found the GPU gone (IsTheGpuGoneAfter).
        @return  true once one has */
    bool IsDeviceReset() const noexcept override
    {
        return gone_->load(std::memory_order_acquire);
    }

    /** The layer fitted to the view: its frame the view's bounds, its contents scale the window's backing scale, and
        its drawables `width` x `height`; each set only where it has changed, and with Core Animation's implicit
        animations off, so a resize is not eased.
        @param width   the drawables' width, in pixels
        @param height  the drawables' height, in pixels */
    void FitTheLayer(int width, int height)
    {
        NSView *view = view_;
        if (view == nil)
        {
            return;
        }
        const CGRect  bounds   = view.layer != nil ? view.layer.bounds : NSRectToCGRect(view.bounds);
        const CGFloat backing  = BackingScaleOf(view);
        const CGSize  drawable = CGSizeMake(width, height);
        if (CGRectEqualToRect(layer_.frame, bounds) && layer_.contentsScale == backing &&
            CGSizeEqualToSize(layer_.drawableSize, drawable))
        {
            return;
        }
        [CATransaction begin];
        [CATransaction setDisableActions:YES];
        layer_.frame         = bounds;
        layer_.contentsScale = backing;
        layer_.drawableSize  = drawable;
        [CATransaction commit];
    }

    __weak NSView      *view_;     ///< the child window's view, which the layer is fitted to.
    CAMetalLayer       *layer_;    ///< the layer over the view's own, which the frames are presented in.
    id<MTLCommandQueue> queue_;    ///< the queue Skia draws on, and the drawables are presented on.
    id<CAMetalDrawable> drawable_; ///< the drawable of the frame being presented; nil between frames.
    /** Set by a presented frame's command buffer, on Metal's thread, where it found the GPU gone. */
    std::shared_ptr<std::atomic<bool>> gone_{std::make_shared<std::atomic<bool>>(false)};
};

/** The layer each frame is presented in, made for `device` and added over `view`'s own layer, filling it: BGRA, the
    layout the CPU raster hands the window too; in the device RGB colour space the CPU raster's image is tagged with, so
    the two paths look alike on a wide-gamut screen; three drawables; shown at the display's blank; and a wait for a
    drawable that gives up after a second rather than holding the window's thread for ever. Its contents set the right
    way up: a flipped view's layer flips its sublayers' contents, which the layer's own flip then undoes.
    @param view    the child window's view, layer-backed
    @param device  the Metal device
    @return        the layer, added; nil where the view has no layer */
CAMetalLayer *AddLayerOver(NSView *view, id<MTLDevice> device)
{
    CALayer *host = view.layer;
    if (host == nil)
    {
        return nil;
    }
    CAMetalLayer *layer             = [CAMetalLayer layer];
    layer.device                    = device;
    layer.pixelFormat               = MTLPixelFormatBGRA8Unorm;
    layer.framebufferOnly           = !PresentWatch(); // readable only for a suite that reads the drawables back
    layer.maximumDrawableCount      = kDrawablesKept;
    layer.displaySyncEnabled        = YES;
    layer.allowsNextDrawableTimeout = YES;
    layer.presentsWithTransaction   = NO;
    layer.opaque                    = YES;
    CGColorSpaceRef device_rgb      = CGColorSpaceCreateDeviceRGB();
    layer.colorspace                = device_rgb;
    CGColorSpaceRelease(device_rgb);
    layer.autoresizingMask = kCALayerWidthSizable | kCALayerHeightSizable;
    const CGFloat backing  = BackingScaleOf(view);
    [CATransaction begin];
    [CATransaction setDisableActions:YES];
    layer.frame         = host.bounds;
    layer.contentsScale = backing;
    layer.drawableSize  = CGSizeMake(host.bounds.size.width * backing, host.bounds.size.height * backing);
    [host addSublayer:layer];
    if ([layer contentsAreFlipped])
    {
        layer.geometryFlipped = !layer.geometryFlipped;
    }
    [CATransaction commit];
    return layer;
}

/** The presenter on `view`, or null and the step that refused: the Metal device driving the view's display, a command
    queue on it, Skia's context on the two, and the layer over the view. Nothing is left behind where a step refuses.
    @param view  the child window's view
    @return      the presenter, or null and why */
GpuPresenterMade MakeMetalPresenter(NSView *view)
{
    GpuPresenterMade made;
    if (view == nil)
    {
        made.why = DrawingFallback::kNoSurface;
        return made;
    }
    id<MTLDevice> device = CGDirectDisplayCopyCurrentMetalDevice(DisplayOfView(view));
    if (device == nil)
    {
        made.why = DrawingFallback::kNoMetalDevice;
        return made;
    }
    made.renderer             = std::string(device.name != nil ? [device.name UTF8String] : "") + ", Metal";
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (queue == nil)
    {
        made.why = DrawingFallback::kNoMetalDevice; // a device that gives no queue is none to draw with
        return made;
    }
    // Skia holds a reference of its own to each, given it here: under ARC, a retained bridge it releases as it goes.
    GrMtlBackendContext backend;
    backend.fDevice.reset((__bridge_retained GrMTLHandle)device);
    backend.fQueue.reset((__bridge_retained GrMTLHandle)queue);
    sk_sp<GrDirectContext> context = GrDirectContexts::MakeMetal(backend);
    if (!context)
    {
        made.why = DrawingFallback::kNoSkiaContext;
        return made;
    }
    CAMetalLayer *layer = AddLayerOver(view, device);
    if (layer == nil)
    {
        context->releaseResourcesAndAbandonContext();
        made.why = DrawingFallback::kNoSurface;
        return made;
    }
    made.presenter = std::make_unique<MetalPresenter>(view, layer, queue, std::move(context));
    made.why       = DrawingFallback::kNone;
    return made;
}

/** The back end this library carries. */
const GpuBackend kMetalBackend{&MakeMetalPresenter};
} // namespace

void WatchMetalPresents(MetalPresentWatch watch)
{
    PresentWatch() = std::move(watch);
}

const GpuBackend *GpuDrawingBackend() noexcept
{
    return &kMetalBackend;
}
} // namespace reckless::window
