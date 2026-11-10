// The child window drawn on the GPU, macOS's half (test_gpu_window.cpp has Windows' and Linux's): Objective-C++,
// because it reaches the Metal layer the window presents in and the drawables it presents. A known frame drawn on the
// GPU and read back twice - from the canvas the views drew into, and from the very drawable handed to the display,
// copied on the queue it was drawn on (window::WatchMetalPresents, which leaves a suite's drawables readable) - then
// drawn again at other scales and sizes, with its GPU lost and made again, and lost until the window gives it up for
// the CPU raster; put back on the CPU raster and on the GPU again; the fallback with RECKLESS_GPU_DRAWING=off; the
// request with no back end; and the suite's own binary read with otool for the Metal it links, the control for the
// window suite's holding that it links none. Where the Mac has no Metal device driving its display - a virtual machine
// with no GPU - the GPU's cases skip with the reason the window gave. Compiled with ARC.
#import <AppKit/AppKit.h>
#import <CoreGraphics/CGDirectDisplayMetal.h>
#import <Metal/Metal.h>
#import <QuartzCore/QuartzCore.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrRecordingContext.h"

#include "FramePacing.h"
#include "GpuDrawingBackend.h"
#include "LinkedLibrariesMac.h"
#include "SkiaCanvas.h"
#include "TestHostWindow.h"
#include "View.h"
#include "Window.h"
#include "mac/GpuPresenterMac.h"

namespace
{
using reckless::gfx::Color;
using reckless::window::Drawing;
using reckless::window::DrawingFallback;
using reckless::window::DrawingState;

/** RECKLESS_GPU_DRAWING set to `value` for a scope, or taken away for an empty one, and put back as it was after. */
class GpuDrawingVariable
{
  public:
    /** The variable set.
        @param value  its value; empty to take it away */
    explicit GpuDrawingVariable(const char *value)
    {
        if (const char *was = std::getenv(reckless::window::kGpuDrawingEnvironmentVariable))
        {
            was_ = was;
        }
        Set(value);
    }

    ~GpuDrawingVariable()
    {
        Set(was_ ? was_->c_str() : "");
    }

    GpuDrawingVariable(const GpuDrawingVariable &)            = delete;
    GpuDrawingVariable &operator=(const GpuDrawingVariable &) = delete;

  private:
    static void Set(const char *value)
    {
        if (*value == '\0')
        {
            unsetenv(reckless::window::kGpuDrawingEnvironmentVariable);
        }
        else
        {
            setenv(reckless::window::kGpuDrawingEnvironmentVariable, value, 1);
        }
    }

    std::optional<std::string> was_; ///< the value it had, where it had one.
};

/** The four colours of the known frame, its quarters from the top left, along and down. */
constexpr Color kQuarterColors[4] = {Color::FromRgb(230, 20, 30), Color::FromRgb(20, 210, 40),
                                     Color::FromRgb(30, 40, 220), Color::FromRgb(240, 230, 20)};

/** Whether `pixel`, 0xAARRGGBB, is `expected` within a few levels a channel.
    @param pixel     the pixel read
    @param expected  the colour drawn
    @return          true when it is that colour */
bool IsColor(std::uint32_t pixel, Color expected)
{
    const auto is_close = [](int a, int b) { return std::abs(a - b) <= 6; };
    return is_close(static_cast<int>((pixel >> 16) & 0xFFu), expected.red()) &&
           is_close(static_cast<int>((pixel >> 8) & 0xFFu), expected.green()) &&
           is_close(static_cast<int>(pixel & 0xFFu), expected.blue());
}

/** Whether every quarter read is the colour drawn there.
    @param quarters  the four pixels read, 0xAARRGGBB
    @return          true when each is its quarter's colour */
bool AreTheKnownFrame(const std::uint32_t *quarters)
{
    for (int quarter = 0; quarter < 4; ++quarter)
    {
        if (!IsColor(quarters[quarter], kQuarterColors[quarter]))
        {
            return false;
        }
    }
    return true;
}

/** The pixels read, for a failure's message.
    @param quarters  the four pixels
    @return          them in hexadecimal */
std::string TextOfPixels(const std::uint32_t *quarters)
{
    char text[64];
    std::snprintf(text, sizeof(text), "%08X %08X %08X %08X", quarters[0], quarters[1], quarters[2], quarters[3]);
    return text;
}

/** The known frame: a quarter of each colour, with what it learnt of the canvas it was painted on - whether the canvas
    is on the GPU, whether its pixels can be written by hand, and the middle of each quarter read back off it - and,
    when asked, the GPU's context abandoned after painting, as a lost device leaves it. */
class KnownFrame final : public reckless::view::View
{
  public:
    void Paint(reckless::gfx::Canvas &canvas) override
    {
        const int half_width  = width() / 2;
        const int half_height = height() / 2;
        for (int quarter = 0; quarter < 4; ++quarter)
        {
            canvas.SetColor(kQuarterColors[quarter]);
            canvas.FillRect(reckless::gfx::RectI{(quarter % 2) * half_width, (quarter / 2) * half_height,
                                                 width() - half_width, height() - half_height});
        }
        ++paints;
        auto *skia_canvas = dynamic_cast<reckless::gfx::SkiaCanvas *>(&canvas);
        if (skia_canvas == nullptr)
        {
            return;
        }
        SkCanvas &sk = skia_canvas->skia();
        is_on_gpu    = sk.recordingContext() != nullptr;
        SkImageInfo info;
        std::size_t row_bytes = 0;
        has_pixels_in_memory  = sk.accessTopLayerPixels(&info, &row_bytes) != nullptr;
        const SkISize device  = sk.getBaseLayerSize();
        for (int quarter = 0; quarter < 4; ++quarter)
        {
            const int         x     = (quarter % 2) * device.width() / 2 + device.width() / 4;
            const int         y     = (quarter / 2) * device.height() / 2 + device.height() / 4;
            std::uint32_t     pixel = 0;
            const SkImageInfo one   = SkImageInfo::Make(1, 1, kBGRA_8888_SkColorType, kPremul_SkAlphaType);
            canvas_pixels[quarter]  = sk.readPixels(one, &pixel, 4, x, y) ? pixel : 0u;
        }
        if (losses_to_cause > 0 && is_on_gpu)
        {
            --losses_to_cause;
            if (GrDirectContext *direct = sk.recordingContext()->asDirectContext())
            {
                direct->abandonContext(); // what a lost device leaves Skia with
            }
        }
    }

    int           paints{0};                   ///< how many times it has been painted.
    bool          is_on_gpu{false};            ///< the last paint's canvas had a GPU context.
    bool          has_pixels_in_memory{false}; ///< the last paint's canvas gave its pixels to be written by hand.
    std::uint32_t canvas_pixels[4]{};          ///< the middle of each quarter as the last paint read it back.
    int           losses_to_cause{0};          ///< paints still to abandon the GPU's context after.
};

/** What the drawables presented while it is in scope gave back: each one copied, on the queue its frame was drawn on,
    into memory the CPU reads, and the middle of each quarter kept. Set before the GPU is asked for, so the window's
    drawables are left readable. */
class PresentedDrawables
{
  public:
    PresentedDrawables()
    {
        reckless::window::WatchMetalPresents(
            [this](id<CAMetalDrawable> drawable, id<MTLCommandQueue> queue) { Read(drawable.texture, queue); });
    }

    ~PresentedDrawables()
    {
        reckless::window::WatchMetalPresents({});
    }

    PresentedDrawables(const PresentedDrawables &)            = delete;
    PresentedDrawables &operator=(const PresentedDrawables &) = delete;

    int           presented{0};    ///< how many drawables have been presented.
    int           width{0};        ///< the last one's width, in pixels.
    int           height{0};       ///< the last one's height, in pixels.
    std::uint32_t quarters[4]{};   ///< the middle of each quarter of the last one, 0xAARRGGBB; 0 where unread.
    bool          was_read{false}; ///< whether the last one could be copied back.

  private:
    void Read(id<MTLTexture> texture, id<MTLCommandQueue> queue)
    {
        ++presented;
        width                        = static_cast<int>(texture.width);
        height                       = static_cast<int>(texture.height);
        was_read                     = false;
        const NSUInteger row_bytes   = texture.width * 4;
        const NSUInteger image_bytes = row_bytes * texture.height;
        id<MTLBuffer>    buffer = [queue.device newBufferWithLength:image_bytes options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer>      copy = [queue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [copy blitCommandEncoder];
        if (buffer == nil || copy == nil || blit == nil)
        {
            return;
        }
        [blit copyFromTexture:texture
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(texture.width, texture.height, 1)
                            toBuffer:buffer
                   destinationOffset:0
              destinationBytesPerRow:row_bytes
            destinationBytesPerImage:image_bytes];
        [blit endEncoding];
        [copy commit];
        [copy waitUntilCompleted];
        if (copy.status != MTLCommandBufferStatusCompleted)
        {
            return;
        }
        // B G R A in memory, which a little-endian word reads as 0xAARRGGBB.
        const auto *pixels = static_cast<const std::uint32_t *>(buffer.contents);
        for (int quarter = 0; quarter < 4; ++quarter)
        {
            const int x       = (quarter % 2) * width / 2 + width / 4;
            const int y       = (quarter / 2) * height / 2 + height / 4;
            quarters[quarter] = pixels[static_cast<std::size_t>(y) * texture.width + static_cast<std::size_t>(x)];
        }
        was_read = true;
    }
};

/** The thread's run loop and AppKit's events run for `seconds`, or until `is_done` says so: the display link, the
    timers, the main queue the window defers its paints to, and the window server's word of what is shown.
    @param seconds  the most to run them for
    @param is_done  asked after each turn; empty to run them for the whole time */
void RunFor(double seconds, const std::function<bool()> &is_done = {})
{
    NSDate *until = [NSDate dateWithTimeIntervalSinceNow:seconds];
    while ([until timeIntervalSinceNow] > 0)
    {
        @autoreleasepool
        {
            NSEvent *event = [NSApp nextEventMatchingMask:NSEventMaskAny
                                                untilDate:[NSDate dateWithTimeIntervalSinceNow:0.01]
                                                   inMode:NSDefaultRunLoopMode
                                                  dequeue:YES];
            if (event != nil)
            {
                [NSApp sendEvent:event];
            }
        }
        if (is_done && is_done())
        {
            return;
        }
    }
}

/** The child window's view in a host window: the one view the window attaches to its content view.
    @param host  the host window
    @return      the view; nil where there is none */
NSView *ChildViewOf(const reckless::test::TestHostWindow &host)
{
    NSView *content = (__bridge NSView *)host.Parent();
    return content.subviews.firstObject;
}

/** The Metal layer over `view`'s own, where the GPU put one.
    @param view  the child window's view
    @return      the layer; nil where there is none */
CAMetalLayer *MetalLayerOf(NSView *view)
{
    for (CALayer *layer in view.layer.sublayers)
    {
        if ([layer isKindOfClass:[CAMetalLayer class]])
        {
            return static_cast<CAMetalLayer *>(layer);
        }
    }
    return nil;
}

/** The state a window reports, said for the log.
    @param state  the state
    @return       it in words */
std::string Described(const DrawingState &state)
{
    return std::string(state.drawing == Drawing::kGpu ? "the GPU" : "the CPU raster") + " (" +
           reckless::window::TextOf(state.why) + "), " + (state.renderer.empty() ? "no device" : state.renderer) +
           ", " + std::to_string(state.contexts_lost) + " lost";
}

/** A host window on the screen, or the reason there is none: a failure where the run says a window is required, a
    skip otherwise. */
#define RECKLESS_SHOWN_HOST(host, width, height)                \
    reckless::test::TestHostWindow host(width, height);         \
    if (!host.Valid())                                          \
    {                                                           \
        if (reckless::test::WindowRequired())                   \
        {                                                       \
            FAIL() << "a window is required and none was made"; \
        }                                                       \
        GTEST_SKIP() << "no window server";                     \
    }                                                           \
    host.Show(true)
} // namespace

TEST(GpuWindow, DrawsAKnownFrameThroughMetalThatTheCanvasAndTheDrawableBothGiveBack)
{
    GpuDrawingVariable unset("");
    PresentedDrawables presented;
    constexpr int      kWidth  = 240;
    constexpr int      kHeight = 160;
    RECKLESS_SHOWN_HOST(host, kWidth, kHeight);
    KnownFrame frame;
    frame.SetBounds({kWidth, kHeight});
    auto child = reckless::window::CreateChildWindow(host.Parent(), frame, kWidth, kHeight, 1.0f);
    ASSERT_NE(child, nullptr);
    const DrawingState state = child->SetDrawing(reckless::window::GpuDrawingRequest());
    std::printf("[ gpu window ] asked for the GPU: %s\n", Described(state).c_str());
    std::fflush(stdout);
    EXPECT_EQ(child->DrawingNow().drawing, state.drawing) << "and says the same when asked again";
    if (state.drawing != Drawing::kGpu)
    {
        EXPECT_NE(state.why, DrawingFallback::kNone);
        EXPECT_NE(state.why, DrawingFallback::kNotAsked) << "it was asked, so it says why it did not";
        GTEST_SKIP() << "no GPU to draw on here: " << reckless::window::TextOf(state.why);
    }
    EXPECT_EQ(state.why, DrawingFallback::kNone);
    EXPECT_NE(state.renderer.find("Metal"), std::string::npos) << "the device is named, for a product's log";

    NSView *view = ChildViewOf(host);
    ASSERT_NE(view, nil);
    CAMetalLayer *layer = MetalLayerOf(view);
    ASSERT_NE(layer, nil) << "the frames are presented in a Metal layer over the view's own";
    EXPECT_EQ(layer.pixelFormat, MTLPixelFormatBGRA8Unorm);
    EXPECT_EQ(layer.maximumDrawableCount, 3u);
    EXPECT_TRUE(layer.displaySyncEnabled) << "shown at the display's blank";
    EXPECT_TRUE(layer.allowsNextDrawableTimeout) << "a wait for a drawable gives up rather than hanging the window";
    EXPECT_FALSE(layer.framebufferOnly) << "readable, for this suite alone";
    EXPECT_FALSE([layer contentsAreFlipped]) << "the frame the right way up over a flipped view";
    CGColorSpaceRef device_rgb = CGColorSpaceCreateDeviceRGB();
    EXPECT_TRUE(layer.colorspace != nullptr && CFEqual(layer.colorspace, device_rgb))
        << "the colour space the CPU raster's image is tagged with, so the two paths look alike";
    CGColorSpaceRelease(device_rgb);
    NSNumber     *display = view.window.screen.deviceDescription[@"NSScreenNumber"];
    id<MTLDevice> driving = CGDirectDisplayCopyCurrentMetalDevice(
        display != nil ? static_cast<CGDirectDisplayID>(display.unsignedIntValue) : CGMainDisplayID());
    EXPECT_TRUE(driving != nil && layer.device.registryID == driving.registryID)
        << "the device driving the window's display, not simply the system's default";

    RunFor(3.0, [&] { return frame.paints > 0 && presented.presented > 0; });
    ASSERT_GT(frame.paints, 0) << "a window on the GPU is painted";
    EXPECT_TRUE(frame.is_on_gpu) << "its canvas has a GPU context, which is how a view asks";
    EXPECT_FALSE(frame.has_pixels_in_memory) << "and gives no pixels to be written by hand";
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels))
        << "the canvas gives back what was drawn on it: " << TextOfPixels(frame.canvas_pixels);
    EXPECT_EQ(view.layer.contents, nil) << "AppKit keeps no backing store beside the GPU's layer";
    if (presented.presented == 0)
    {
        ASSERT_FALSE(view.window.visible) << "a window that is ordered in is presented to, covered or not";
        GTEST_SKIP() << "painted, but never presented: the window is not ordered in here, so the drawable is not held";
    }
    const CGFloat backing = view.window.backingScaleFactor;
    std::printf("[ gpu window ] presented %d: %d x %d at a backing scale of %.2f\n", presented.presented,
                presented.width, presented.height, backing);
    EXPECT_EQ(presented.width, static_cast<int>(std::ceil(view.bounds.size.width * backing)));
    EXPECT_EQ(presented.height, static_cast<int>(std::ceil(view.bounds.size.height * backing)));
    EXPECT_EQ(layer.contentsScale, backing);
    ASSERT_TRUE(presented.was_read) << "the drawable copied back";
    EXPECT_TRUE(AreTheKnownFrame(presented.quarters))
        << "the drawable handed to the display holds the frame, the right way up: " << TextOfPixels(presented.quarters);
    EXPECT_EQ(child->DrawingNow().drawing, Drawing::kGpu);
}

/** When each drawable actually reached the screen, which only the compositor knows: `presentedTime`, taken through
    `addPresentedHandler:`, which is called on a thread of Metal's own and so is guarded. A drawable that was never
    shown reports a presented time of zero, and is counted apart rather than quietly left out - that count is the
    question "was a frame dropped?" asked directly. */
class PresentTimes
{
  public:
    PresentTimes()
    {
        reckless::window::WatchMetalPresents([this](id<CAMetalDrawable> drawable, id<MTLCommandQueue>) {
            int index = 0;
            {
                const std::lock_guard<std::mutex> lock(mutex_);
                index = asked_++;
            }
            [drawable addPresentedHandler:^(id<MTLDrawable> shown) {
              const CFTimeInterval              at = shown.presentedTime;
              const std::lock_guard<std::mutex> lock(mutex_);
              if (at > 0.0)
              {
                  times_.push_back(at);
              }
              else
              {
                  never_shown_.push_back(index);
              }
            }];
        });
    }

    ~PresentTimes()
    {
        reckless::window::WatchMetalPresents({});
    }

    PresentTimes(const PresentTimes &)            = delete;
    PresentTimes &operator=(const PresentTimes &) = delete;

    /** @return How many drawables were handed over to be shown. */
    int asked() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return asked_;
    }

    /** @return Which of them, in the order they were handed over, were never shown. */
    std::vector<int> never_shown() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<int>                  copy = never_shown_;
        std::sort(copy.begin(), copy.end());
        return copy;
    }

    /** @return When each one reached the screen, in the order they did, in seconds. */
    std::vector<CFTimeInterval> times() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        std::vector<CFTimeInterval>       copy = times_;
        std::sort(copy.begin(), copy.end());
        return copy;
    }

  private:
    mutable std::mutex          mutex_;       ///< Metal calls the handler on its own thread.
    std::vector<CFTimeInterval> times_;       ///< When each drawable was shown.
    int                         asked_{0};    ///< How many were handed over.
    std::vector<int>            never_shown_; ///< And which of those never reached the screen.
};

TEST(GpuWindow, ShowsEveryDrawableItIsGivenAndShowsThemOnTheDisplaysOwnBlanks)
{
    // **This is the tearing question asked where it can be answered.** A `CAMetalLayer` with `displaySyncEnabled`
    // cannot tear: it has no torn present to make, so the failure it can have instead is dropping a drawable or
    // showing one late. `presentedTime` is the compositor's own answer to when a frame reached the screen, which
    // neither the frame log nor a screen capture can give, so it is what this asks. Two things are held: that every
    // drawable handed over was shown - none dropped - and that the gaps between showings are whole multiples of one
    // display period, which is what "on the blank" means.
    GpuDrawingVariable unset("");
    PresentTimes       shown;
    constexpr int      kWidth  = 240;
    constexpr int      kHeight = 160;
    RECKLESS_SHOWN_HOST(host, kWidth, kHeight);
    KnownFrame frame;
    frame.SetBounds({kWidth, kHeight});
    auto child = reckless::window::CreateChildWindow(host.Parent(), frame, kWidth, kHeight, 1.0f);
    ASSERT_NE(child, nullptr);
    const DrawingState state = child->SetDrawing(reckless::window::GpuDrawingRequest());
    if (state.drawing != Drawing::kGpu)
    {
        GTEST_SKIP() << "no GPU to draw on here: " << reckless::window::TextOf(state.why);
    }
    // **It has to be asked to draw every frame, or there is no grid to read.** A window paints when something asks
    // it to, so the display's own callback is what asks here - which is also how a product drives it
    // (`SetFrameCallback`, and Sentinel's `GameFrame`). Long enough for a useful run of blanks at any rate a Mac has,
    // and short enough not to hold the suite up.
    // **Paced on the display's own clock, which is the path a product runs on.** Without this a window is on the
    // platform timer (`WindowMac.mm`: the display tick is "empty until a product asks"), and what it presents then is
    // that timer's cadence and not the display's - so a test that left it out would be measuring the wrong clock and
    // reading the answer as the window's. A divisor of 1 asks for a frame every refresh, which is the strictest grid
    // there is to hold the showings to; Sentinel itself asks for 2 at 120 Hz, because its tick is 60 Hz and two
    // refreshes is one new frame (`app/frame_pacing.h`, `RefreshPeriodsFor`).
    const reckless::window::FrameRequest     paced{reckless::window::FramePacing::kDisplayClock, 1, 1, 0};
    const reckless::window::FramePacingState on = child->SetFramePacing(paced);
    std::printf("[ gpu window ] paced on %s, a divisor of %d, the display read as %.3f Hz\n",
                on.pacing == reckless::window::FramePacing::kDisplayClock ? "the display's clock" : "a timer",
                on.refresh_divisor, on.display_hertz);
    child->SetFrameCallback([&frame] { frame.Repaint(); });
    RunFor(4.0, [&] { return shown.times().size() >= 90; });
    child->SetFrameCallback(nullptr);
    // A drawable handed over on the last callback is still on its way when the asking stops, and it is shown a
    // refresh or two later. Waiting for it is the difference between "one was dropped" and "one had not arrived yet",
    // and without the wait this reads as the first when it is the second.
    const int handed_over = shown.asked();
    RunFor(0.5, [&] {
        return static_cast<int>(shown.times().size()) + static_cast<int>(shown.never_shown().size()) >= handed_over;
    });
    const std::vector<CFTimeInterval> at = shown.times();
    if (at.size() < 8)
    {
        GTEST_SKIP() << "the window was shown " << at.size() << " times, too few to read a display's grid from ("
                     << shown.asked() << " drawables handed over, " << shown.never_shown().size() << " never shown, "
                     << frame.paints << " paints)";
    }
    // **The drawables that are never shown are the first ones handed over, and nothing after them.** They are
    // handed over by the paints the window does as it is made, before its layer is on the screen to show them, so the
    // compositor discards them; once it is showing, every drawable reaches the screen. How many there are depends on
    // how fast they are asked for: one at a frame every second refresh, two at a frame every refresh, measured on
    // jonny-macbook 2026-11-09 on a 120 Hz panel. So what is held here is not a count but a shape - that the dropped
    // ones are an unbroken run from the very first, and few - because a drop *after* the window has settled is a
    // frame a player loses and must fail, while the start-up's are the compositor doing what it must.
    const std::vector<int> never = shown.never_shown();
    std::string            dropped;
    for (const int which : never)
    {
        dropped += " " + std::to_string(which);
    }
    std::printf("[ gpu window ] never shown, by the order handed over:%s (of %d)\n",
                dropped.empty() ? " none" : dropped.c_str(), shown.asked());
    std::fflush(stdout);
    EXPECT_LE(never.size(), 4u) << "more than the window's first paints were dropped:" << dropped;
    for (std::size_t place = 0; place < never.size(); ++place)
    {
        EXPECT_EQ(never[place], static_cast<int>(place))
            << "drawable " << never[place] << " of " << shown.asked()
            << " was handed over and never reached the screen, after the window had settled:" << dropped;
    }

    std::vector<double> gaps;
    gaps.reserve(at.size() - 1);
    for (std::size_t index = 1; index < at.size(); ++index)
    {
        gaps.push_back(at[index] - at[index - 1]);
    }
    // **The grid is the display's refresh, not the window's own cadence**, and taking the second for the first is a
    // mistake that reads as a fault. A window need not draw every refresh: this one settles at a frame every second
    // refresh of a 120 Hz panel, so the commonest gap is two periods, and judged against *that* a perfectly legal gap
    // of one refresh or three is half a period out and reads as "shown off the blank". So the period is asked of the
    // screen rather than inferred from the gaps.
    NSView         *view     = ChildViewOf(host);
    NSScreen       *screen   = view != nil && view.window.screen != nil ? view.window.screen : [NSScreen mainScreen];
    const NSInteger a_second = screen != nil ? [screen maximumFramesPerSecond] : 0;
    if (a_second <= 0)
    {
        GTEST_SKIP() << "the screen did not say its refresh rate, so there is no grid to hold the showings to";
    }
    const double period = 1.0 / static_cast<double>(a_second);
    ASSERT_GT(period, 0.001) << "a display period of " << period * 1000.0 << " ms is not one";
    ASSERT_LT(period, 0.1);
    int    off_the_grid = 0;
    double worst        = 0.0;
    for (const double gap : gaps)
    {
        const double periods = gap / period;
        const double stray   = std::fabs(periods - std::round(periods));
        worst                = std::max(worst, stray);
        if (stray > 0.25) // a quarter of a period either way is far more than a blank's own jitter
        {
            ++off_the_grid;
            if (off_the_grid <= 6)
            {
                std::printf("[ gpu window ]   off the grid: %.3f ms = %.3f refreshes\n", gap * 1000.0, periods);
            }
        }
    }
    std::vector<double> sorted = gaps;
    std::sort(sorted.begin(), sorted.end());
    std::printf("[ gpu window ] %zu shown, %d never, a %ld Hz screen (%.3f ms), the commonest gap %.3f ms = %.2f "
                "refreshes, worst stray %.3f of one\n",
                at.size(), static_cast<int>(shown.never_shown().size()), static_cast<long>(a_second), period * 1000.0,
                sorted[sorted.size() / 2] * 1000.0, sorted[sorted.size() / 2] / period, worst);
    std::fflush(stdout);
    // **One stray is allowed and a second is not**, which is where the evidence puts the line. Vsync off would put
    // nearly every gap off the grid, not one, and a dropped drawable is caught above by its index, so one is no
    // weaker a test of either. It is allowed because one appeared once in seven runs here on 2026-11-09 - a gap of
    // half a refresh, on a machine that was also building at the time - and did not come back in the four runs after
    // it. A test that fails one time in seven on sound hardware teaches a reader to ignore it.
    EXPECT_LE(off_the_grid, 1) << off_the_grid << " of " << gaps.size()
                               << " gaps between showings are not a whole number of display periods, the worst "
                               << worst << " of one: frames shown off the display's blank";
}

TEST(GpuWindow, AScaleAndASizeChangeRebuildTheSurfaceAndTheDrawablesAndItGoesOnDrawing)
{
    GpuDrawingVariable unset("");
    PresentedDrawables presented;
    constexpr int      kWidth  = 200;
    constexpr int      kHeight = 120;
    RECKLESS_SHOWN_HOST(host, kWidth * 2, kHeight * 2);
    KnownFrame frame;
    frame.SetBounds({kWidth, kHeight});
    auto child = reckless::window::CreateChildWindow(host.Parent(), frame, kWidth, kHeight, 1.0f);
    ASSERT_NE(child, nullptr);
    const DrawingState state = child->SetDrawing(reckless::window::GpuDrawingRequest());
    if (state.drawing != Drawing::kGpu)
    {
        GTEST_SKIP() << "no GPU to draw on here: " << reckless::window::TextOf(state.why);
    }
    RunFor(3.0, [&] { return frame.paints > 0 && presented.presented > 0; });
    NSView *view = ChildViewOf(host);
    ASSERT_NE(view, nil);
    // A move to a screen of another scale and a full screen reach the window as SetScale - a new scale, or a new size
    // of the root at the same scale (app/AppRun.cpp) - as on Windows.
    for (const auto &[scale, width, height] : {std::tuple{2.0f, kWidth, kHeight}, std::tuple{1.0f, kWidth * 2, kHeight},
                                               std::tuple{1.5f, kWidth, kHeight * 2 / 3}})
    {
        frame.SetBounds({width, height});
        const int painted          = frame.paints;
        const int presented_before = presented.presented;
        child->SetScale(scale);
        RunFor(3.0, [&] { return frame.paints > painted && presented.presented > presented_before; });
        ASSERT_GT(frame.paints, painted) << "the window is drawn again at " << scale;
        EXPECT_EQ(child->DrawingNow().drawing, Drawing::kGpu) << "and is still on the GPU at " << scale;
        EXPECT_TRUE(frame.is_on_gpu);
        EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels))
            << "at " << scale << ", " << width << " x " << height << ": " << TextOfPixels(frame.canvas_pixels);
        if (presented.presented == presented_before)
        {
            continue; // not shown: what the first case says, and skips there
        }
        const CGFloat backing = view.window.backingScaleFactor;
        EXPECT_EQ(presented.width, static_cast<int>(std::ceil(std::lround(width * scale) * backing)))
            << "the drawable made again at the new size, at " << scale;
        EXPECT_EQ(presented.height, static_cast<int>(std::ceil(std::lround(height * scale) * backing)));
        EXPECT_TRUE(presented.was_read && AreTheKnownFrame(presented.quarters))
            << "at " << scale << ": " << TextOfPixels(presented.quarters);
    }
}

TEST(GpuWindow, ALostGpuIsMadeAgainAndOneLostTooOftenIsGivenUpForTheCpuRaster)
{
    GpuDrawingVariable unset("");
    constexpr int      kSize = 160;
    RECKLESS_SHOWN_HOST(host, kSize, kSize);
    KnownFrame frame;
    frame.SetBounds({kSize, kSize});
    auto child = reckless::window::CreateChildWindow(host.Parent(), frame, kSize, kSize, 1.0f);
    ASSERT_NE(child, nullptr);
    if (const DrawingState state = child->SetDrawing(reckless::window::GpuDrawingRequest());
        state.drawing != Drawing::kGpu)
    {
        GTEST_SKIP() << "no GPU to draw on here: " << reckless::window::TextOf(state.why);
    }
    RunFor(3.0, [&] { return frame.paints > 0; });
    ASSERT_GT(frame.paints, 0);
    NSView *view = ChildViewOf(host);
    ASSERT_NE(view, nil);

    // Lost once: made again, and the frame drawn again on the new context.
    frame.losses_to_cause = 1;
    frame.Repaint();
    RunFor(3.0, [&] { return frame.losses_to_cause == 0 && child->DrawingNow().contexts_lost == 1; });
    const int painted_after_one = frame.paints;
    RunFor(3.0, [&] { return frame.paints > painted_after_one; });
    DrawingState now = child->DrawingNow();
    std::printf("[ gpu window ] lost once: %s\n", Described(now).c_str());
    EXPECT_EQ(now.contexts_lost, 1);
    EXPECT_EQ(now.drawing, Drawing::kGpu) << "a GPU lost once is made again";
    EXPECT_EQ(now.why, DrawingFallback::kNone);
    EXPECT_TRUE(frame.is_on_gpu);
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);
    EXPECT_NE(MetalLayerOf(view), nil) << "presented in the new GPU's layer";
    EXPECT_EQ(view.layer.sublayers.count, 1u) << "and the lost one's taken off the view";

    // Lost until it is given up: the CPU raster, said, and the frame drawn on it.
    frame.losses_to_cause = reckless::window::kMostGpuContextsLost;
    for (int loss = 0; loss < reckless::window::kMostGpuContextsLost; ++loss)
    {
        const int lost_before = child->DrawingNow().contexts_lost;
        frame.Repaint();
        // A GPU made again is drawn whole at once, and so lost again at once: the losses may all come in one turn.
        RunFor(3.0, [&] {
            return child->DrawingNow().contexts_lost > lost_before || child->DrawingNow().drawing != Drawing::kGpu;
        });
    }
    const int painted_after_all = frame.paints;
    frame.Repaint();
    RunFor(3.0, [&] { return frame.paints > painted_after_all; });
    now = child->DrawingNow();
    std::printf("[ gpu window ] lost %d times: %s\n", now.contexts_lost, Described(now).c_str());
    EXPECT_EQ(now.contexts_lost, reckless::window::kMostGpuContextsLost + 1);
    EXPECT_EQ(now.drawing, Drawing::kCpuRaster) << "a GPU lost too often is given up";
    EXPECT_EQ(now.why, DrawingFallback::kContextLostTooOften);
    EXPECT_FALSE(frame.is_on_gpu) << "and the views are drawn on the CPU raster, through drawRect: again";
    EXPECT_TRUE(frame.has_pixels_in_memory) << "whose pixels a view may write by hand again";
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);
    EXPECT_EQ(MetalLayerOf(view), nil) << "with no Metal layer left over the view";
}

TEST(GpuWindow, AWindowAskedBackToTheCpuRasterDrawsThereAgain)
{
    GpuDrawingVariable unset("");
    constexpr int      kSize = 160;
    RECKLESS_SHOWN_HOST(host, kSize, kSize);
    KnownFrame frame;
    frame.SetBounds({kSize, kSize});
    auto child = reckless::window::CreateChildWindow(host.Parent(), frame, kSize, kSize, 1.0f);
    ASSERT_NE(child, nullptr);
    if (child->SetDrawing(reckless::window::GpuDrawingRequest()).drawing != Drawing::kGpu)
    {
        GTEST_SKIP() << "no GPU to draw on here";
    }
    RunFor(3.0, [&] { return frame.paints > 0; });
    NSView *view = ChildViewOf(host);
    ASSERT_NE(view, nil);
    const DrawingState back = child->SetDrawing({});
    EXPECT_EQ(back.drawing, Drawing::kCpuRaster);
    EXPECT_EQ(back.why, DrawingFallback::kNotAsked) << "a default request is the CPU raster, not asked for the GPU";
    EXPECT_EQ(MetalLayerOf(view), nil) << "the Metal layer taken off the view";
    const int painted = frame.paints;
    RunFor(3.0, [&] { return frame.paints > painted; });
    EXPECT_GT(frame.paints, painted) << "the whole window drawn again on the CPU raster";
    EXPECT_FALSE(frame.is_on_gpu);
    EXPECT_TRUE(frame.has_pixels_in_memory);
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);
    // And to the GPU again.
    EXPECT_EQ(child->SetDrawing(reckless::window::GpuDrawingRequest()).drawing, Drawing::kGpu);
    EXPECT_NE(MetalLayerOf(view), nil);
    const int painted_again = frame.paints;
    RunFor(3.0, [&] { return frame.paints > painted_again; });
    EXPECT_TRUE(frame.is_on_gpu);
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);
}

TEST(GpuWindow, AProductsDrawablesAreForTheDisplayAlone)
{
    // No watch set, as in every product: the drawables are framebufferOnly, the cheaper kind the display alone reads.
    GpuDrawingVariable unset("");
    constexpr int      kSize = 160;
    RECKLESS_SHOWN_HOST(host, kSize, kSize);
    reckless::view::View root;
    root.SetBounds({kSize, kSize});
    auto child = reckless::window::CreateChildWindow(host.Parent(), root, kSize, kSize, 1.0f);
    ASSERT_NE(child, nullptr);
    if (child->SetDrawing(reckless::window::GpuDrawingRequest()).drawing != Drawing::kGpu)
    {
        GTEST_SKIP() << "no GPU to draw on here";
    }
    CAMetalLayer *layer = MetalLayerOf(ChildViewOf(host));
    ASSERT_NE(layer, nil);
    EXPECT_TRUE(layer.framebufferOnly);
}

TEST(GpuWindow, TheEnvironmentTurnsTheGpuOffAndTheWindowSaysSoAndDrawsOnTheCpuRaster)
{
    GpuDrawingVariable off("off");
    constexpr int      kSize = 160;
    RECKLESS_SHOWN_HOST(host, kSize, kSize);
    KnownFrame frame;
    frame.SetBounds({kSize, kSize});
    auto child = reckless::window::CreateChildWindow(host.Parent(), frame, kSize, kSize, 1.0f);
    ASSERT_NE(child, nullptr);
    const DrawingState state = child->SetDrawing(reckless::window::GpuDrawingRequest());
    EXPECT_EQ(state.drawing, Drawing::kCpuRaster);
    EXPECT_EQ(state.why, DrawingFallback::kForcedOff);
    EXPECT_TRUE(state.renderer.empty()) << "and no Metal device was asked for to find that out";
    EXPECT_EQ(child->DrawingNow().why, DrawingFallback::kForcedOff) << "it says the same when asked again";
    EXPECT_EQ(MetalLayerOf(ChildViewOf(host)), nil) << "and no Metal layer is put over the view";
    RunFor(3.0, [&] { return frame.paints > 0; });
    ASSERT_GT(frame.paints, 0);
    EXPECT_FALSE(frame.is_on_gpu) << "the views are drawn on the CPU raster";
    EXPECT_TRUE(frame.has_pixels_in_memory) << "whose pixels a view may write by hand";
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);
}

TEST(GpuWindow, TheGpuAskedForWithNoBackEndIsAnsweredNotLinked)
{
    GpuDrawingVariable unset("");
    RECKLESS_SHOWN_HOST(host, 240, 160);
    reckless::view::View root;
    root.SetBounds({240, 160});
    auto child = reckless::window::CreateChildWindow(host.Parent(), root, 240, 160, 1.0f);
    ASSERT_NE(child, nullptr);
    const DrawingState state = child->SetDrawing({Drawing::kGpu, nullptr});
    EXPECT_EQ(state.drawing, Drawing::kCpuRaster);
    EXPECT_EQ(state.why, DrawingFallback::kNotLinked);
    EXPECT_NE(reckless::window::GpuDrawingBackend(), nullptr) << "where this suite's own back end is Metal";
}

TEST(GpuWindow, ThisSuiteLinksMetalAsAProgramThatLinksTheBackEndDoes)
{
    // The control for the window suite's holding that a program linking the window alone names no Metal: the same
    // reading, of a program that links the back end, finds it.
    const std::string libraries = reckless::test::LinkedLibrariesOfThisProgram();
    ASSERT_FALSE(libraries.empty()) << "otool read this program's libraries";
    EXPECT_NE(libraries.find("/Metal.framework/"), std::string::npos) << libraries;
}
