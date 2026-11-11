// The child window drawn on the GPU (ChildWindow::SetDrawing, window/GpuDrawing.h), in a suite of its own because it
// links the GPU's back end (reckless_window_gpu), which the window's own suite does not, so that one holds a window
// that carries none of it. On Windows: a known frame drawn on the GPU and read back twice - from the canvas the views
// drew into, and off the screen after the swap, against a window on the CPU raster drawn the same way as the control
// that says whether this screen can be read at all - then drawn again at another scale, then with its GPU lost and made
// again, and lost until the window gives it up for the CPU raster; and the fallback taken and said where the
// environment turns the GPU off. Where this machine's GL will not draw - CI's runners, which have no GPU driver and
// Windows' own software GL 1.1 - the GPU's cases skip with the reason the window gave, and the fallback's run. macOS's
// half, on Metal, is test_gpu_window_mac.mm. On Linux the request is accepted and answered with the CPU raster, which
// is what its window draws with until it has a GPU path of its own.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <tuple>

#include "include/core/SkCanvas.h"
#include "include/core/SkImageInfo.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrRecordingContext.h"

#include "GpuPresenter.h"

#include "GpuDrawingBackend.h"
#include "GpuSurfacePresenter.h"
#include "GpuSwapInterval.h"
#include "SkiaCanvas.h"
#include "View.h"
#include "Window.h"

using reckless::window::Drawing;
using reckless::window::DrawingFallback;
using reckless::window::DrawingState;

namespace
{
/** RECKLESS_GPU_DRAWING set to `value` for a scope, or taken away for an empty one, and put back as it was after. */
class GpuDrawingVariable
{
  public:
    /** The variable set.
        @param value  its value; empty to take it away */
    explicit GpuDrawingVariable(const char *value)
    {
        if (const auto was = reckless::core::EnvironmentVariable(reckless::window::kGpuDrawingEnvironmentVariable))
        {
            was_ = *was;
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
#if defined(_WIN32)
        _putenv_s(reckless::window::kGpuDrawingEnvironmentVariable, value);
#else
        if (*value == '\0')
        {
            unsetenv(reckless::window::kGpuDrawingEnvironmentVariable);
        }
        else
        {
            setenv(reckless::window::kGpuDrawingEnvironmentVariable, value, 1);
        }
#endif
    }

    std::optional<std::string> was_; ///< the value it had, where it had one.
};

using reckless::gfx::Color;

/** The four colours of the known frame, its quarters from the top left, along and down. */
constexpr Color kQuarterColors[4] = {Color::FromRgb(230, 20, 30), Color::FromRgb(20, 210, 40),
                                     Color::FromRgb(30, 40, 220), Color::FromRgb(240, 230, 20)};

/** Whether `pixel`, 0xAARRGGBB, is `expected` within a few levels a channel: what a compositor that leaves the colour
    alone gives back, with room for one that dithers.
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

/** The known frame: a quarter of each colour, with what it learnt of the canvas it was painted on as it was - whether
    the canvas is on the GPU, whether its pixels can be written by hand, and the middle of each quarter read back off it
    - and, when asked, the GPU's context abandoned after painting, as a driver reset leaves it. */
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
        // The middle of each quarter in the canvas's own pixels, which are the window's: the device's size halved.
        const SkISize device = sk.getBaseLayerSize();
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
                direct->abandonContext(); // what a driver reset leaves Skia with
            }
        }
    }

    int           paints{0};                   ///< how many times it has been painted.
    bool          is_on_gpu{false};            ///< the last paint's canvas had a GPU context.
    bool          has_pixels_in_memory{false}; ///< the last paint's canvas gave its pixels to be written by hand.
    std::uint32_t canvas_pixels[4]{};          ///< the middle of each quarter as the last paint read it back.
    int           losses_to_cause{0};          ///< paints still to abandon the GPU's context after.
};

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

/** The state a window drawn on the GPU reports, said for the log.
    @param state  the state
    @return       it in words */
std::string Described(const DrawingState &state)
{
    return std::string(state.drawing == Drawing::kGpu ? "the GPU" : "the CPU raster") + " (" +
           reckless::window::TextOf(state.why) + "), " + (state.renderer.empty() ? "no GL" : state.renderer) + ", " +
           std::to_string(state.contexts_lost) + " lost";
}
} // namespace

#if defined(_WIN32)
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <d3d11.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

namespace
{
/** A window on the screen for a child window to sit in: a borderless, never-activated tool window at the top of the
    z-order, so the screen under it is its own and the focus stays where it was; on a thread aware of each monitor's
   DPI, so its pixels are the screen's. */
class ShownHost
{
  public:
    /** The window, `width` x `height` pixels, shown.
        @param width   the width, in pixels
        @param height  the height, in pixels */
    ShownHost(int width, int height)
    {
        was_aware_ = SetThreadDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        window_    = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_TOOLWINDOW, L"STATIC", L"", WS_POPUP, 120,
                                     120, width, height, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        if (window_ != nullptr)
        {
            ShowWindow(window_, SW_SHOWNOACTIVATE);
        }
    }

    ~ShownHost()
    {
        if (window_ != nullptr)
        {
            DestroyWindow(window_);
        }
        if (was_aware_ != nullptr)
        {
            SetThreadDpiAwarenessContext(was_aware_);
        }
    }

    ShownHost(const ShownHost &)            = delete;
    ShownHost &operator=(const ShownHost &) = delete;

    /** @return the window; null where Windows refused it */
    HWND window() const noexcept
    {
        return window_;
    }

  private:
    HWND                  window_{nullptr};    ///< the window.
    DPI_AWARENESS_CONTEXT was_aware_{nullptr}; ///< the thread's awareness before, put back after.
};

/** The thread's messages run for `milliseconds`, or until `is_done` says so.
    @param milliseconds  the most to run them for
    @param is_done       asked after each batch; empty to run them for the whole time */
void RunMessages(int milliseconds, const std::function<bool()> &is_done = {})
{
    const ULONGLONG until = GetTickCount64() + static_cast<ULONGLONG>(milliseconds);
    MSG             message{};
    while (GetTickCount64() < until)
    {
        MsgWaitForMultipleObjects(0, nullptr, FALSE, 10, QS_ALLINPUT);
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        if (is_done && is_done())
        {
            return;
        }
    }
}

/** The middle of each quarter of `window`'s client area as the display is shown it: read through DXGI Desktop
    Duplication, which hands over what the compositor composed - a GL window's swapped frame among it. A read of the
    screen's device context does not do here: GDI answers it from the windows' own GDI surfaces, which a GL window's
    frame is not drawn into, and does so even with the display off and nothing composed at all. The frame is drawn
    again before each frame is waited for, since the duplication hands over only what has changed, and the read ends
    once a composed frame shows the known frame or two seconds have gone. Nothing is read where the desktop cannot be
    duplicated (a locked desktop, a service's window station) or composes nothing (a display that is off).
    @param window  the window
    @param width   its client width, in pixels
    @param height  its client height, in pixels
    @param frame   the frame drawn there, repainted while the screen is read
    @return        the four pixels, 0xAARRGGBB, from the last composed frame; empty where none could be read */
std::optional<std::vector<std::uint32_t>> QuartersOnScreen(HWND window, int width, int height, KnownFrame &frame)
{
    using Microsoft::WRL::ComPtr;
    POINT origin{0, 0};
    ClientToScreen(window, &origin);
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))))
    {
        return std::nullopt;
    }
    ComPtr<IDXGIAdapter1> adapter;
    ComPtr<IDXGIOutput>   output;
    DXGI_OUTPUT_DESC      output_desc{};
    for (UINT a = 0; !output && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        ComPtr<IDXGIOutput> candidate;
        for (UINT o = 0; adapter->EnumOutputs(o, &candidate) != DXGI_ERROR_NOT_FOUND; ++o)
        {
            candidate->GetDesc(&output_desc);
            const RECT &r = output_desc.DesktopCoordinates;
            if (origin.x >= r.left && origin.x + width <= r.right && origin.y >= r.top && origin.y + height <= r.bottom)
            {
                output = candidate;
                break;
            }
        }
    }
    ComPtr<IDXGIOutput1>           output1;
    ComPtr<ID3D11Device>           device;
    ComPtr<ID3D11DeviceContext>    context;
    ComPtr<IDXGIOutputDuplication> duplication;
    if (!output || output_desc.Rotation > DXGI_MODE_ROTATION_IDENTITY || FAILED(output.As(&output1)) ||
        FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                 &device, nullptr, &context)) ||
        FAILED(output1->DuplicateOutput(device.Get(), &duplication)))
    {
        return std::nullopt;
    }
    std::optional<std::vector<std::uint32_t>> read;
    const ULONGLONG                           until = GetTickCount64() + 2000;
    while (GetTickCount64() < until && !(read && AreTheKnownFrame(read->data())))
    {
        frame.Repaint();
        RunMessages(20);
        DXGI_OUTDUPL_FRAME_INFO info{};
        ComPtr<IDXGIResource>   resource;
        if (FAILED(duplication->AcquireNextFrame(50, &info, &resource)))
        {
            continue; // nothing composed yet, or nothing at all
        }
        ComPtr<ID3D11Texture2D> desktop;
        D3D11_TEXTURE2D_DESC    desc{};
        if (info.LastPresentTime.QuadPart != 0 && SUCCEEDED(resource.As(&desktop)))
        {
            desktop->GetDesc(&desc);
            desc.Usage          = D3D11_USAGE_STAGING;
            desc.BindFlags      = 0;
            desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
            desc.MiscFlags      = 0;
            ComPtr<ID3D11Texture2D>  staging;
            D3D11_MAPPED_SUBRESOURCE mapped{};
            if (desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM &&
                SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)))
            {
                context->CopyResource(staging.Get(), desktop.Get());
                if (SUCCEEDED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped)))
                {
                    std::vector<std::uint32_t> quarters(4, 0u);
                    for (int quarter = 0; quarter < 4; ++quarter)
                    {
                        const int x =
                            origin.x - output_desc.DesktopCoordinates.left + (quarter % 2) * width / 2 + width / 4;
                        const int y =
                            origin.y - output_desc.DesktopCoordinates.top + (quarter / 2) * height / 2 + height / 4;
                        std::memcpy(&quarters[quarter],
                                    static_cast<const unsigned char *>(mapped.pData) +
                                        static_cast<std::size_t>(y) * mapped.RowPitch +
                                        static_cast<std::size_t>(x) * 4u,
                                    4);
                    }
                    context->Unmap(staging.Get(), 0);
                    read = std::move(quarters);
                }
            }
        }
        duplication->ReleaseFrame();
    }
    return read;
}

/** The known frame drawn in a shown window on the CPU raster, and whether the screen gave it back: the control that
   says whether a window's pixels can be read off this screen at all - a locked desktop, or a window station that shows
    nothing, gives none back, and then nothing is held of what the GPU's window shows either.
    @return  true when the screen gives a window's pixels back */
bool CanTheScreenBeRead()
{
    constexpr int kSize = 160;
    ShownHost     host(kSize, kSize);
    KnownFrame    frame;
    frame.SetBounds({kSize, kSize});
    auto child = reckless::window::CreateChildWindow(host.window(), frame, kSize, kSize, 1.0f);
    if (child == nullptr)
    {
        return false;
    }
    RunMessages(1500, [&] { return frame.paints > 0; });
    const auto quarters = QuartersOnScreen(host.window(), kSize, kSize, frame);
    std::printf("[ gpu window ] the control, on the CPU raster: %d paints, %s on the screen\n", frame.paints,
                quarters ? TextOfPixels(quarters->data()).c_str() : "nothing composed");
    return frame.paints > 0 && quarters && AreTheKnownFrame(quarters->data());
}

/** The known frame as the display is shown it, checked: what a test that has seen its control read holds of a window.
    @param window  the window
    @param width   its client width, in pixels
    @param height  its client height, in pixels
    @param frame   the frame drawn there
    @param what    what is being held, for the log and a failure's message */
void HoldsTheKnownFrameOnScreen(HWND window, int width, int height, KnownFrame &frame, const char *what)
{
    const auto quarters = QuartersOnScreen(window, width, height, frame);
    std::printf("[ gpu window ] %s: %s on the screen\n", what,
                quarters ? TextOfPixels(quarters->data()).c_str() : "nothing composed");
    ASSERT_TRUE(quarters.has_value()) << what << ": the screen gave nothing back, though the control's read";
    EXPECT_TRUE(AreTheKnownFrame(quarters->data())) << what << ": " << TextOfPixels(quarters->data());
}

} // namespace

TEST(GpuWindow, DrawsAKnownFrameOnTheGpuThatTheCanvasAndTheScreenBothGiveBack)
{
    GpuDrawingVariable unset("");
    constexpr int      kWidth  = 240;
    constexpr int      kHeight = 160;
    ShownHost          host(kWidth, kHeight);
    ASSERT_NE(host.window(), nullptr);
    KnownFrame frame;
    frame.SetBounds({kWidth, kHeight});
    auto child = reckless::window::CreateChildWindow(host.window(), frame, kWidth, kHeight, 1.0f);
    ASSERT_NE(child, nullptr);
    const DrawingState state = child->SetDrawing(reckless::window::GpuDrawingRequest());
    std::printf("[ gpu window ] asked for the GPU: %s\n", Described(state).c_str());
    std::fflush(stdout);
    EXPECT_EQ(child->DrawingNow().drawing, state.drawing) << "and says the same when asked again";
    if (state.drawing != Drawing::kGpu)
    {
        EXPECT_NE(state.why, DrawingFallback::kNone);
        EXPECT_NE(state.why, DrawingFallback::kNotAsked) << "it was asked, so it says why it did not";
        GTEST_SKIP() << "no GPU to draw on here: " << reckless::window::TextOf(state.why) << " ("
                     << (state.renderer.empty() ? "no GL context" : state.renderer) << ")";
    }
    EXPECT_EQ(state.why, DrawingFallback::kNone);
    EXPECT_FALSE(state.renderer.empty()) << "the GL is named, for a product's log";

    RunMessages(1500, [&] { return frame.paints > 0; });
    if (frame.paints == 0)
    {
        GTEST_SKIP() << "nothing painted at all: a window station that shows nothing";
    }
    EXPECT_TRUE(frame.is_on_gpu) << "its canvas has a GPU context, which is how a view asks";
    EXPECT_FALSE(frame.has_pixels_in_memory) << "and gives no pixels to be written by hand";
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels))
        << "the canvas gives back what was drawn on it: " << TextOfPixels(frame.canvas_pixels);
    if (!CanTheScreenBeRead())
    {
        GTEST_SKIP() << "the display composes nothing a window's pixels can be read back from here - the control on "
                        "the CPU raster read nothing or read wrong: a display that is off, a locked desktop, a "
                        "service's window station - so what the swap presented is not held";
    }
    // The swap put the frame on the screen, the right way up.
    HoldsTheKnownFrameOnScreen(host.window(), kWidth, kHeight, frame, "on the GPU");
    EXPECT_EQ(child->DrawingNow().drawing, Drawing::kGpu);
}

TEST(GpuWindow, AScaleAndASizeChangeRebuildTheSurfaceAndItGoesOnDrawing)
{
    GpuDrawingVariable unset("");
    constexpr int      kWidth  = 200;
    constexpr int      kHeight = 120;
    ShownHost          host(kWidth * 2, kHeight * 2);
    ASSERT_NE(host.window(), nullptr);
    KnownFrame frame;
    frame.SetBounds({kWidth, kHeight});
    auto child = reckless::window::CreateChildWindow(host.window(), frame, kWidth, kHeight, 1.0f);
    ASSERT_NE(child, nullptr);
    const DrawingState state = child->SetDrawing(reckless::window::GpuDrawingRequest());
    if (state.drawing != Drawing::kGpu)
    {
        GTEST_SKIP() << "no GPU to draw on here: " << reckless::window::TextOf(state.why);
    }
    RunMessages(1500, [&] { return frame.paints > 0; });
    if (frame.paints == 0)
    {
        GTEST_SKIP() << "nothing painted at all: a window station that shows nothing";
    }
    // A DPI change and a move to another monitor reach the window as a new scale, full screen and a resize as a new
    // size of the root at the same scale (app/AppRun.cpp): both come here as SetScale.
    for (const auto &[scale, width, height] : {std::tuple{2.0f, kWidth, kHeight}, std::tuple{1.0f, kWidth * 2, kHeight},
                                               std::tuple{1.5f, kWidth, kHeight * 2 / 3}})
    {
        frame.SetBounds({width, height});
        const int painted = frame.paints;
        child->SetScale(scale);
        RunMessages(1500, [&] { return frame.paints > painted; });
        ASSERT_GT(frame.paints, painted) << "the window is drawn again at " << scale;
        EXPECT_EQ(child->DrawingNow().drawing, Drawing::kGpu) << "and is still on the GPU at " << scale;
        EXPECT_TRUE(frame.is_on_gpu);
        EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels))
            << "at " << scale << ", " << width << " x " << height << ": " << TextOfPixels(frame.canvas_pixels);
    }
    if (CanTheScreenBeRead())
    {
        HoldsTheKnownFrameOnScreen(host.window(), static_cast<int>(std::lround(frame.width() * 1.5f)),
                                   static_cast<int>(std::lround(frame.height() * 1.5f)), frame, "on the GPU at 1.5");
    }
}

TEST(GpuWindow, ALostGpuIsMadeAgainAndOneLostTooOftenIsGivenUpForTheCpuRaster)
{
    GpuDrawingVariable unset("");
    constexpr int      kSize = 160;
    ShownHost          host(kSize, kSize);
    ASSERT_NE(host.window(), nullptr);
    KnownFrame frame;
    frame.SetBounds({kSize, kSize});
    auto child = reckless::window::CreateChildWindow(host.window(), frame, kSize, kSize, 1.0f);
    ASSERT_NE(child, nullptr);
    if (const DrawingState state = child->SetDrawing(reckless::window::GpuDrawingRequest());
        state.drawing != Drawing::kGpu)
    {
        GTEST_SKIP() << "no GPU to draw on here: " << reckless::window::TextOf(state.why);
    }
    RunMessages(1500, [&] { return frame.paints > 0; });
    if (frame.paints == 0)
    {
        GTEST_SKIP() << "nothing painted at all: a window station that shows nothing";
    }

    // Lost once: made again, and the frame drawn again on the new context.
    frame.losses_to_cause = 1;
    frame.Repaint();
    RunMessages(1500, [&] { return frame.losses_to_cause == 0 && child->DrawingNow().contexts_lost == 1; });
    const int painted_after_one = frame.paints;
    RunMessages(1500, [&] { return frame.paints > painted_after_one; });
    DrawingState now = child->DrawingNow();
    std::printf("[ gpu window ] lost once: %s\n", Described(now).c_str());
    EXPECT_EQ(now.contexts_lost, 1);
    EXPECT_EQ(now.drawing, Drawing::kGpu) << "a GPU lost once is made again";
    EXPECT_EQ(now.why, DrawingFallback::kNone);
    EXPECT_TRUE(frame.is_on_gpu);
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);

    // Lost until it is given up: the CPU raster, said, and the frame drawn on it.
    frame.losses_to_cause = reckless::window::kMostGpuContextsLost;
    for (int loss = 0; loss < reckless::window::kMostGpuContextsLost; ++loss)
    {
        const int lost_before = child->DrawingNow().contexts_lost;
        frame.Repaint();
        RunMessages(1500, [&] { return child->DrawingNow().contexts_lost > lost_before; });
    }
    const int painted_after_all = frame.paints;
    frame.Repaint();
    RunMessages(1500, [&] { return frame.paints > painted_after_all; });
    now = child->DrawingNow();
    std::printf("[ gpu window ] lost %d times: %s\n", now.contexts_lost, Described(now).c_str());
    EXPECT_EQ(now.contexts_lost, reckless::window::kMostGpuContextsLost + 1);
    EXPECT_EQ(now.drawing, Drawing::kCpuRaster) << "a GPU lost too often is given up";
    EXPECT_EQ(now.why, DrawingFallback::kContextLostTooOften);
    EXPECT_FALSE(frame.is_on_gpu) << "and the views are drawn on the CPU raster";
    EXPECT_TRUE(frame.has_pixels_in_memory) << "whose pixels a view may write by hand again";
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);
    if (CanTheScreenBeRead())
    {
        HoldsTheKnownFrameOnScreen(host.window(), kSize, kSize, frame,
                                   "the CPU raster on a window that had a GL pixel format");
    }
}

TEST(GpuWindow, AWindowAskedBackToTheCpuRasterDrawsThereAgain)
{
    GpuDrawingVariable unset("");
    constexpr int      kSize = 160;
    ShownHost          host(kSize, kSize);
    ASSERT_NE(host.window(), nullptr);
    KnownFrame frame;
    frame.SetBounds({kSize, kSize});
    auto child = reckless::window::CreateChildWindow(host.window(), frame, kSize, kSize, 1.0f);
    ASSERT_NE(child, nullptr);
    if (child->SetDrawing(reckless::window::GpuDrawingRequest()).drawing != Drawing::kGpu)
    {
        GTEST_SKIP() << "no GPU to draw on here";
    }
    RunMessages(1500, [&] { return frame.paints > 0; });
    if (frame.paints == 0)
    {
        GTEST_SKIP() << "nothing painted at all: a window station that shows nothing";
    }
    const DrawingState back = child->SetDrawing({});
    EXPECT_EQ(back.drawing, Drawing::kCpuRaster);
    EXPECT_EQ(back.why, DrawingFallback::kNotAsked) << "a default request is the CPU raster, not asked for the GPU";
    const int painted = frame.paints;
    RunMessages(1500, [&] { return frame.paints > painted; });
    EXPECT_GT(frame.paints, painted) << "the whole window drawn again on the CPU raster";
    EXPECT_FALSE(frame.is_on_gpu);
    EXPECT_TRUE(frame.has_pixels_in_memory);
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);
    // And to the GPU again, on the pixel format the window kept from the first time.
    EXPECT_EQ(child->SetDrawing(reckless::window::GpuDrawingRequest()).drawing, Drawing::kGpu);
    const int painted_again = frame.paints;
    RunMessages(1500, [&] { return frame.paints > painted_again; });
    EXPECT_TRUE(frame.is_on_gpu);
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);
}

TEST(GpuWindow, TheEnvironmentTurnsTheGpuOffAndTheWindowSaysSoAndDrawsOnTheCpuRaster)
{
    // RECKLESS_GPU_DRAWING=off: what a person whose driver misdraws sets, and what holds the fallback on any machine,
    // CI's among them.
    GpuDrawingVariable off("off");
    constexpr int      kSize = 160;
    ShownHost          host(kSize, kSize);
    ASSERT_NE(host.window(), nullptr);
    KnownFrame frame;
    frame.SetBounds({kSize, kSize});
    auto child = reckless::window::CreateChildWindow(host.window(), frame, kSize, kSize, 1.0f);
    ASSERT_NE(child, nullptr);
    const DrawingState state = child->SetDrawing(reckless::window::GpuDrawingRequest());
    EXPECT_EQ(state.drawing, Drawing::kCpuRaster);
    EXPECT_EQ(state.why, DrawingFallback::kForcedOff);
    EXPECT_TRUE(state.renderer.empty()) << "and no GL context was made to find that out";
    EXPECT_EQ(child->DrawingNow().why, DrawingFallback::kForcedOff) << "it says the same when asked again";
    RunMessages(1500, [&] { return frame.paints > 0; });
    if (frame.paints == 0)
    {
        GTEST_SKIP() << "nothing painted at all: a window station that shows nothing";
    }
    EXPECT_FALSE(frame.is_on_gpu) << "the views are drawn on the CPU raster";
    EXPECT_TRUE(frame.has_pixels_in_memory) << "whose pixels a view may write by hand";
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);
}

namespace
{
/** A presenter with no GL behind it, to see what interval the shared half asks its back end for: every
    SetSwapInterval is recorded, in the order made. */
class RecordingPresenter final : public reckless::window::SkiaGpuPresenter
{
  public:
    explicit RecordingPresenter(reckless::window::SwapIntervalRequest request) : SkiaGpuPresenter(nullptr, request)
    {
    }

    /** What Present does before it swaps. */
    void Apply()
    {
        ApplySwapInterval();
    }

    std::vector<int> intervals; ///< each interval set, in order.

  private:
    bool MakeCurrent() const noexcept override
    {
        return true;
    }

    sk_sp<SkSurface> BackBuffer(int, int) override
    {
        return nullptr;
    }

    void SwapBuffers() noexcept override
    {
    }

    void SetSwapInterval(int interval) noexcept override
    {
        intervals.push_back(interval);
    }
};
} // namespace

TEST(GpuWindow, ThePresenterAsksForIntervalOneInFullScreenAndZeroWindowed)
{
    using reckless::window::SwapIntervalRequest;
    RecordingPresenter presenter(SwapIntervalRequest::kAuto);
    presenter.Apply();
    EXPECT_TRUE(presenter.intervals.empty()) << "windowed is the 0 the context starts with: nothing to set";
    presenter.SetFullScreen(true);
    presenter.Apply();
    presenter.Apply();
    EXPECT_EQ(presenter.intervals, std::vector<int>({1})) << "set once, when full screen began";
    presenter.SetFullScreen(false);
    presenter.Apply();
    EXPECT_EQ(presenter.intervals, std::vector<int>({1, 0})) << "and back when it ended";
    presenter.SetFullScreen(false);
    presenter.Apply();
    EXPECT_EQ(presenter.intervals.size(), 2u) << "told the same again, nothing set again";
}

TEST(GpuWindow, TheEnvironmentOverridesTheIntervalWhateverTheWindowDoes)
{
    using reckless::window::SwapIntervalRequest;
    RecordingPresenter always_one(SwapIntervalRequest::kEveryBlank);
    always_one.Apply();
    always_one.SetFullScreen(true);
    always_one.Apply();
    always_one.SetFullScreen(false);
    always_one.Apply();
    EXPECT_EQ(always_one.intervals, std::vector<int>({1})) << "1 windowed too, set once";

    RecordingPresenter always_zero(SwapIntervalRequest::kNoWait);
    always_zero.SetFullScreen(true);
    always_zero.Apply();
    EXPECT_TRUE(always_zero.intervals.empty()) << "0 in full screen, which the context already has";
}

TEST(GpuWindow, TheGpuAskedForWithNoBackEndIsAnsweredNotLinked)
{
    GpuDrawingVariable unset("");
    HWND               top = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 240, 160, nullptr, nullptr,
                                             GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(top, nullptr);
    reckless::view::View root;
    root.SetBounds({240, 160});
    auto child = reckless::window::CreateChildWindow(top, root, 240, 160, 1.0f);
    ASSERT_NE(child, nullptr);
    const DrawingState state = child->SetDrawing({Drawing::kGpu, nullptr});
    EXPECT_EQ(state.drawing, Drawing::kCpuRaster);
    EXPECT_EQ(state.why, DrawingFallback::kNotLinked);
    EXPECT_NE(reckless::window::GpuDrawingBackend(), nullptr) << "where this suite's own back end is Windows'";
    child.reset();
    DestroyWindow(top);
}
#elif defined(__linux__) // macOS's half is test_gpu_window_mac.mm, Objective-C++ to reach the Metal layer
// Linux, on EGL: the known frame drawn on the GPU and read back off the canvas and off the window (XGetImage, against
// a window on the CPU raster read the same way as the control), at other scales and sizes, through a lost GPU and into
// the fallback, back to the CPU raster and to the GPU again, with the environment turning the GPU off, with no back
// end, a popup over a GPU window kept on the CPU raster, and a software GL refused. Each GPU case skips with the
// window's own reason where this machine's EGL will not draw - CI's Xvfb, which has only llvmpipe - unless the run
// says a GPU is there (RECKLESS_REQUIRE_GPU), and then it fails. The software refusal is held everywhere: on CI as it
// is, and on a machine with a GPU with Mesa told to rasterise in software.
#include <dirent.h>

#include <chrono>
#include <cstring>
#include <fstream>
#include <functional>
#include <memory>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "LoadedLibrariesLinux.h"
#include "TestHostWindow.h"

// Xlib last: its macros (None, Bool, Status) would reach into the headers above.
#include <X11/Xlib.h>
#include <X11/Xutil.h>

namespace
{
/** A host's loop the suite turns by hand: the connection's events and the frame tick, each time it is turned. */
class TurnedLoop final : public reckless::window::RunLoop
{
  public:
    bool WatchFd(int, std::function<void()> ready) override
    {
        ready_ = std::move(ready);
        return true;
    }

    bool StartTimer(int, std::function<void()> tick) override
    {
        tick_ = std::move(tick);
        return true;
    }

    /** The connection's events read and the frame tick run - which paints what was invalidated - once. */
    void Turn()
    {
        if (ready_)
        {
            ready_();
        }
        if (tick_)
        {
            tick_();
        }
    }

  private:
    std::function<void()> ready_; ///< the connection's watch.
    std::function<void()> tick_;  ///< the frame tick.
};

/** An environment variable set for a scope, or taken away for a null value, and put back as it was after. */
class EnvironmentVariable
{
  public:
    /** The variable set.
        @param name   its name
        @param value  its value; null to take it away */
    EnvironmentVariable(const char *name, const char *value) : name_(name)
    {
        if (const char *was = std::getenv(name))
        {
            was_ = was;
        }
        Set(value);
    }

    ~EnvironmentVariable()
    {
        Set(was_ ? was_->c_str() : nullptr);
    }

    EnvironmentVariable(const EnvironmentVariable &)            = delete;
    EnvironmentVariable &operator=(const EnvironmentVariable &) = delete;

  private:
    void Set(const char *value)
    {
        if (value == nullptr)
        {
            unsetenv(name_.c_str());
        }
        else
        {
            setenv(name_.c_str(), value, 1);
        }
    }

    std::string                name_; ///< the variable's name.
    std::optional<std::string> was_;  ///< the value it had, where it had one.
};

/** Whether the run says this machine has a GPU its EGL draws with (RECKLESS_REQUIRE_GPU set, and not to 0): a GPU case
    that would skip then fails, so a hardware run whose GPU cases all skipped does not pass with nothing held.
    @return  true when the GPU is required */
bool GpuRequired()
{
    const char *required = std::getenv("RECKLESS_REQUIRE_GPU");
    return required != nullptr && *required != '\0' && std::string_view(required) != "0";
}

/** How many of this process's threads are a software rasteriser's: Mesa names llvmpipe's "llvmpipe-0" and on, one a
    core. What a GL that was refused before it drew must not have started.
    @return  the count */
int SoftwareRasteriserThreads()
{
    int  count = 0;
    DIR *tasks = opendir("/proc/self/task");
    if (tasks == nullptr)
    {
        return 0;
    }
    while (const dirent *task = readdir(tasks))
    {
        if (task->d_name[0] == '.')
        {
            continue;
        }
        std::ifstream comm(std::string("/proc/self/task/") + task->d_name + "/comm");
        std::string   name;
        std::getline(comm, name);
        if (name.rfind("llvmpipe", 0) == 0 || name.rfind("softpipe", 0) == 0)
        {
            ++count;
        }
    }
    closedir(tasks);
    return count;
}

/** A shown window for a child window to sit in, with a connection of the suite's own to read it back off the server
    by. */
class ShownHost
{
  public:
    /** The window, `width` x `height` pixels, mapped.
        @param width   the width, in pixels
        @param height  the height, in pixels */
    ShownHost(int width, int height) : host_(width, height)
    {
        if (host_.Valid())
        {
            host_.Show(true);
            display_ = XOpenDisplay(nullptr);
            // Mapped once the window manager has put it up: a read of a window that is not viewable is an X error.
            const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
            while (display_ != nullptr && !IsShown() && std::chrono::steady_clock::now() < until)
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        }
    }

    ~ShownHost()
    {
        if (display_ != nullptr)
        {
            XCloseDisplay(display_);
        }
    }

    ShownHost(const ShownHost &)            = delete;
    ShownHost &operator=(const ShownHost &) = delete;

    /** @return whether there is a window and a connection to read it by */
    bool Valid() const noexcept
    {
        return host_.Valid() && display_ != nullptr;
    }

    /** @return the window's handle, for CreateChildWindow */
    void *Parent() const noexcept
    {
        return host_.Parent();
    }

    /** The middle of each quarter of the child window in this one, `width` x `height` of its pixels, as the server
        holds them: XGetImage of the child, which on a compositing desktop is its own pixmap - what an EGL swap
        presented into - and on Xvfb the screen's. Empty where there is no child yet or the read fails.
        @param width   the child's width to read the quarters of, in pixels
        @param height  its height, in pixels
        @return        the four pixels, 0x00RRGGBB */
    std::optional<std::vector<std::uint32_t>> Quarters(int width, int height) const
    {
        ::Window   root = 0, parent = 0, *children = nullptr;
        unsigned   count = 0;
        const auto host  = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(host_.Parent()));
        if (XQueryTree(display_, host, &root, &parent, &children, &count) == 0 || count == 0)
        {
            return std::nullopt;
        }
        const ::Window child = children[count - 1];
        XFree(children);
        // A window off the screen, or one the server will not read, is an X error, which Xlib's own handler would end
        // the suite for: it is no read instead.
        XSync(display_, False);
        XErrorHandler const was = XSetErrorHandler(IgnoreXErrors);
        XImage *image = XGetImage(display_, child, 0, 0, static_cast<unsigned>(width), static_cast<unsigned>(height),
                                  AllPlanes, ZPixmap);
        XSync(display_, False);
        XSetErrorHandler(was);
        if (image == nullptr)
        {
            return std::nullopt;
        }
        std::vector<std::uint32_t> quarters(4, 0u);
        for (int quarter = 0; quarter < 4; ++quarter)
        {
            quarters[static_cast<std::size_t>(quarter)] = static_cast<std::uint32_t>(
                XGetPixel(image, (quarter % 2) * width / 2 + width / 4, (quarter / 2) * height / 2 + height / 4) &
                0xFFFFFFu);
        }
        XDestroyImage(image);
        return quarters;
    }

  private:
    /** An X error passed over, for a read that may be refused.
        @return  zero, always */
    static int IgnoreXErrors(Display *, XErrorEvent *)
    {
        return 0;
    }

    /** @return whether the window is mapped with every window above it mapped */
    bool IsShown() const
    {
        XWindowAttributes attributes{};
        const auto        window = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(host_.Parent()));
        return XGetWindowAttributes(display_, window, &attributes) != 0 && attributes.map_state == IsViewable;
    }

    reckless::test::TestHostWindow host_;             ///< the window.
    Display                       *display_{nullptr}; ///< the suite's own connection, for reading the window back.
};

/** The loop turned for `milliseconds`, or until `is_done` says so, a few milliseconds apart: the child's connection is
    its own, so what it asked of the server is waited for.
    @param loop          the window's loop
    @param milliseconds  the most to turn it for
    @param is_done       asked after each turn; empty to turn it for the whole time */
void Turn(TurnedLoop &loop, int milliseconds, const std::function<bool()> &is_done = {})
{
    const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (std::chrono::steady_clock::now() < until)
    {
        loop.Turn();
        if (is_done && is_done())
        {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

/** The child's quarters read off the server until they are the known frame or two seconds have gone, the frame drawn
    again before each read, as a swap may reach the server a frame after it was asked for.
    @param host    the window the child is in
    @param loop    the child's loop
    @param frame   the frame drawn
    @param width   the child's width, in pixels
    @param height  its height, in pixels
    @return        the last quarters read; empty where none could be */
std::optional<std::vector<std::uint32_t>> QuartersOnScreen(const ShownHost &host, TurnedLoop &loop, KnownFrame &frame,
                                                           int width, int height)
{
    std::optional<std::vector<std::uint32_t>> read;
    const auto                                until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < until && !(read && AreTheKnownFrame(read->data())))
    {
        frame.Repaint();
        Turn(loop, 30);
        read = host.Quarters(width, height);
    }
    return read;
}

/** A child window on its own turned loop, in a shown host. */
struct GpuChild
{
    /** The host, the frame and the child window on it, `width` x `height` design pixels at `scale`, in a host of
        `host_width` x `host_height`.
        @param width        the frame's width
        @param height       its height
        @param host_width   the host's width, in pixels
        @param host_height  its height, in pixels */
    GpuChild(int width, int height, int host_width, int host_height) : host(host_width, host_height)
    {
        frame.SetBounds({width, height});
        if (host.Valid())
        {
            auto made = std::make_unique<TurnedLoop>();
            loop      = made.get();
            child     = reckless::window::CreateChildWindow(host.Parent(), frame, width, height, 1.0f, std::move(made));
        }
    }

    ~GpuChild()
    {
        child.reset(); // before the window it sits in
    }

    GpuChild(const GpuChild &)            = delete;
    GpuChild &operator=(const GpuChild &) = delete;

    ShownHost                                      host;          ///< the shown window.
    KnownFrame                                     frame;         ///< the frame drawn.
    TurnedLoop                                    *loop{nullptr}; ///< the child's loop, owned by the child.
    std::unique_ptr<reckless::window::ChildWindow> child;         ///< the child window; null where none was made.
};

/** Whether a window could be made at all, failing where the run promised one (RECKLESS_REQUIRE_WINDOW).
    @param made  the child and its host
    @return      true to go on; false where the case is to skip */
bool HasAWindow(const GpuChild &made)
{
    if (!made.host.Valid())
    {
        EXPECT_FALSE(reckless::test::WindowRequired()) << "a window is required and none was made";
        return false;
    }
    EXPECT_NE(made.child, nullptr);
    return made.child != nullptr;
}

/** The known frame drawn in a shown window on the CPU raster, and whether the server gave it back: the control that
    says whether a window's pixels can be read off this display at all.
    @return  true when the display gives a window's pixels back */
bool CanTheScreenBeRead()
{
    constexpr int kSize = 160;
    GpuChild      made(kSize, kSize, kSize, kSize);
    if (made.child == nullptr)
    {
        return false;
    }
    Turn(*made.loop, 1500, [&] { return made.frame.paints > 0; });
    const auto quarters = QuartersOnScreen(made.host, *made.loop, made.frame, kSize, kSize);
    std::printf("[ gpu window ] the control, on the CPU raster: %d paints, %s read back\n", made.frame.paints,
                quarters ? TextOfPixels(quarters->data()).c_str() : "nothing");
    return made.frame.paints > 0 && quarters && AreTheKnownFrame(quarters->data());
}

/** The known frame as the server holds the child's pixels, checked.
    @param made    the child and its host
    @param width   the child's width, in pixels
    @param height  its height, in pixels
    @param what    what is being held, for the log and a failure's message */
void HoldsTheKnownFrameOnScreen(GpuChild &made, int width, int height, const char *what)
{
    const auto quarters = QuartersOnScreen(made.host, *made.loop, made.frame, width, height);
    std::printf("[ gpu window ] %s: %s read back\n", what,
                quarters ? TextOfPixels(quarters->data()).c_str() : "nothing");
    ASSERT_TRUE(quarters.has_value()) << what << ": the server gave nothing back, though the control's read";
    EXPECT_TRUE(AreTheKnownFrame(quarters->data())) << what << ": " << TextOfPixels(quarters->data());
}

/** The GPU asked for, and whether the case goes on: true on the GPU; false, the reason said, where it fell back - a
    failure where the run requires a GPU, a skip for the caller to make otherwise.
    @param made   the child
    @param state  set to what the window answered
    @return       true when the window draws on the GPU */
bool AskedForTheGpu(GpuChild &made, DrawingState &state)
{
    state = made.child->SetDrawing(reckless::window::GpuDrawingRequest());
    std::printf("[ gpu window ] asked for the GPU: %s\n", Described(state).c_str());
    std::fflush(stdout);
    if (state.drawing != Drawing::kGpu)
    {
        EXPECT_NE(state.why, DrawingFallback::kNone);
        EXPECT_NE(state.why, DrawingFallback::kNotAsked) << "it was asked, so it says why it did not";
        EXPECT_FALSE(GpuRequired()) << "the run requires a GPU (RECKLESS_REQUIRE_GPU), and the window fell back: "
                                    << Described(state);
        return false;
    }
    return true;
}
} // namespace

#define RECKLESS_SKIP_WITHOUT_THE_GPU(made, state)                                                \
    if (!AskedForTheGpu(made, state))                                                             \
    {                                                                                             \
        GTEST_SKIP() << "no GPU to draw on here: " << reckless::window::TextOf(state.why) << " (" \
                     << (state.renderer.empty() ? "no GL context" : state.renderer) << ")";       \
    }

TEST(GpuWindow, DrawsAKnownFrameOnTheGpuThatTheCanvasAndTheWindowBothGiveBack)
{
    GpuDrawingVariable unset("");
    constexpr int      kWidth  = 240;
    constexpr int      kHeight = 160;
    GpuChild           made(kWidth, kHeight, kWidth, kHeight);
    if (!HasAWindow(made))
    {
        GTEST_SKIP() << "no display";
    }
    DrawingState state;
    RECKLESS_SKIP_WITHOUT_THE_GPU(made, state);
    EXPECT_EQ(made.child->DrawingNow().drawing, Drawing::kGpu) << "and says the same when asked again";
    EXPECT_EQ(state.why, DrawingFallback::kNone);
    EXPECT_FALSE(state.renderer.empty()) << "the GL is named, for a product's log";
    EXPECT_FALSE(reckless::window::IsSoftwareRenderer(state.renderer.c_str())) << state.renderer;

    Turn(*made.loop, 1500, [&] { return made.frame.paints > 0; });
    ASSERT_GT(made.frame.paints, 0) << "a window on the GPU is painted";
    EXPECT_TRUE(made.frame.is_on_gpu) << "its canvas has a GPU context, which is how a view asks";
    EXPECT_FALSE(made.frame.has_pixels_in_memory) << "and gives no pixels to be written by hand";
    EXPECT_TRUE(AreTheKnownFrame(made.frame.canvas_pixels))
        << "the canvas gives back what was drawn on it: " << TextOfPixels(made.frame.canvas_pixels);
    if (!CanTheScreenBeRead())
    {
        GTEST_SKIP() << "the display gives back no window's pixels - the control on the CPU raster read nothing or "
                        "read wrong - so what the swap presented is not held";
    }
    // The swap put the frame on the window, the right way up: GL's bottom-left origin turned over.
    HoldsTheKnownFrameOnScreen(made, kWidth, kHeight, "on the GPU");
    EXPECT_EQ(made.child->DrawingNow().drawing, Drawing::kGpu);
}

TEST(GpuWindow, AScaleAndASizeChangeRebuildTheSurfaceAndItGoesOnDrawing)
{
    GpuDrawingVariable unset("");
    constexpr int      kWidth  = 200;
    constexpr int      kHeight = 120;
    GpuChild           made(kWidth, kHeight, kWidth * 2, kHeight * 2);
    if (!HasAWindow(made))
    {
        GTEST_SKIP() << "no display";
    }
    DrawingState state;
    RECKLESS_SKIP_WITHOUT_THE_GPU(made, state);
    Turn(*made.loop, 1500, [&] { return made.frame.paints > 0; });
    // A change of the desktop's scale reaches the window as a new scale, full screen and a resize as a new size of the
    // root at the same scale (app/AppRun.cpp): both come here as SetScale.
    for (const auto &[scale, width, height] : {std::tuple{2.0f, kWidth, kHeight}, std::tuple{1.0f, kWidth * 2, kHeight},
                                               std::tuple{1.5f, kWidth, kHeight * 2 / 3}})
    {
        made.frame.SetBounds({width, height});
        const int painted = made.frame.paints;
        made.child->SetScale(scale);
        Turn(*made.loop, 1500, [&] { return made.frame.paints > painted; });
        ASSERT_GT(made.frame.paints, painted) << "the window is drawn again at " << scale;
        EXPECT_EQ(made.child->DrawingNow().drawing, Drawing::kGpu) << "and is still on the GPU at " << scale;
        EXPECT_TRUE(made.frame.is_on_gpu);
        EXPECT_TRUE(AreTheKnownFrame(made.frame.canvas_pixels))
            << "at " << scale << ", " << width << " x " << height << ": " << TextOfPixels(made.frame.canvas_pixels);
    }
    if (CanTheScreenBeRead())
    {
        // The window surface follows the X window's size, which the server may give a frame late: what is held is
        // that the frame lands where it should once it has.
        HoldsTheKnownFrameOnScreen(made, static_cast<int>(std::lround(made.frame.width() * 1.5f)),
                                   static_cast<int>(std::lround(made.frame.height() * 1.5f)), "on the GPU at 1.5");
    }
}

TEST(GpuWindow, ALostGpuIsMadeAgainAndOneLostTooOftenIsGivenUpForTheCpuRaster)
{
    GpuDrawingVariable unset("");
    constexpr int      kSize = 160;
    GpuChild           made(kSize, kSize, kSize, kSize);
    if (!HasAWindow(made))
    {
        GTEST_SKIP() << "no display";
    }
    DrawingState state;
    RECKLESS_SKIP_WITHOUT_THE_GPU(made, state);
    KnownFrame &frame = made.frame;
    Turn(*made.loop, 1500, [&] { return frame.paints > 0; });
    ASSERT_GT(frame.paints, 0);

    // Lost once: made again, and the frame drawn again on the new context.
    frame.losses_to_cause = 1;
    frame.Repaint();
    Turn(*made.loop, 1500, [&] { return frame.losses_to_cause == 0 && made.child->DrawingNow().contexts_lost == 1; });
    const int painted_after_one = frame.paints;
    Turn(*made.loop, 1500, [&] { return frame.paints > painted_after_one; });
    DrawingState now = made.child->DrawingNow();
    std::printf("[ gpu window ] lost once: %s\n", Described(now).c_str());
    EXPECT_EQ(now.contexts_lost, 1);
    EXPECT_EQ(now.drawing, Drawing::kGpu) << "a GPU lost once is made again";
    EXPECT_EQ(now.why, DrawingFallback::kNone);
    EXPECT_TRUE(frame.is_on_gpu);
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);

    // Lost until it is given up: the CPU raster, said, and the frame drawn on it.
    frame.losses_to_cause = reckless::window::kMostGpuContextsLost;
    for (int loss = 0; loss < reckless::window::kMostGpuContextsLost; ++loss)
    {
        const int lost_before = made.child->DrawingNow().contexts_lost;
        frame.Repaint();
        Turn(*made.loop, 1500, [&] { return made.child->DrawingNow().contexts_lost > lost_before; });
    }
    const int painted_after_all = frame.paints;
    frame.Repaint();
    Turn(*made.loop, 1500, [&] { return frame.paints > painted_after_all; });
    now = made.child->DrawingNow();
    std::printf("[ gpu window ] lost %d times: %s\n", now.contexts_lost, Described(now).c_str());
    EXPECT_EQ(now.contexts_lost, reckless::window::kMostGpuContextsLost + 1);
    EXPECT_EQ(now.drawing, Drawing::kCpuRaster) << "a GPU lost too often is given up";
    EXPECT_EQ(now.why, DrawingFallback::kContextLostTooOften);
    EXPECT_FALSE(frame.is_on_gpu) << "and the views are drawn on the CPU raster";
    EXPECT_TRUE(frame.has_pixels_in_memory) << "whose pixels a view may write by hand again";
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);
    if (CanTheScreenBeRead())
    {
        HoldsTheKnownFrameOnScreen(made, kSize, kSize, "the CPU raster through XPutImage on a window that had EGL's");
    }
}

TEST(GpuWindow, AWindowAskedBackToTheCpuRasterDrawsThereAgain)
{
    GpuDrawingVariable unset("");
    constexpr int      kSize = 160;
    GpuChild           made(kSize, kSize, kSize, kSize);
    if (!HasAWindow(made))
    {
        GTEST_SKIP() << "no display";
    }
    DrawingState state;
    RECKLESS_SKIP_WITHOUT_THE_GPU(made, state);
    KnownFrame &frame = made.frame;
    Turn(*made.loop, 1500, [&] { return frame.paints > 0; });
    const DrawingState back = made.child->SetDrawing({});
    EXPECT_EQ(back.drawing, Drawing::kCpuRaster);
    EXPECT_EQ(back.why, DrawingFallback::kNotAsked) << "a default request is the CPU raster, not asked for the GPU";
    const int painted = frame.paints;
    Turn(*made.loop, 1500, [&] { return frame.paints > painted; });
    EXPECT_GT(frame.paints, painted) << "the whole window drawn again on the CPU raster";
    EXPECT_FALSE(frame.is_on_gpu);
    EXPECT_TRUE(frame.has_pixels_in_memory);
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);
    // And to the GPU again, on a window surface made again on the same window.
    EXPECT_EQ(made.child->SetDrawing(reckless::window::GpuDrawingRequest()).drawing, Drawing::kGpu);
    const int painted_again = frame.paints;
    Turn(*made.loop, 1500, [&] { return frame.paints > painted_again; });
    EXPECT_TRUE(frame.is_on_gpu);
    EXPECT_TRUE(AreTheKnownFrame(frame.canvas_pixels)) << TextOfPixels(frame.canvas_pixels);
}

TEST(GpuWindow, APopupOverAWindowOnTheGpuStaysOnTheCpuRaster)
{
    GpuDrawingVariable unset("");
    constexpr int      kSize = 160;
    GpuChild           made(kSize, kSize, kSize, kSize);
    if (!HasAWindow(made))
    {
        GTEST_SKIP() << "no display";
    }
    DrawingState state;
    RECKLESS_SKIP_WITHOUT_THE_GPU(made, state);
    Turn(*made.loop, 1500, [&] { return made.frame.paints > 0; });
    KnownFrame menu;
    menu.SetBounds({60, 40});
    auto popup = made.frame.OpenPopup(menu, {10, 10}, [](reckless::gfx::PointI) {}, false);
    ASSERT_NE(popup, nullptr);
    Turn(*made.loop, 1500, [&] { return menu.paints > 0; });
    ASSERT_GT(menu.paints, 0) << "the popup is painted";
    EXPECT_FALSE(menu.is_on_gpu) << "on the CPU raster: a popup is never asked for the GPU";
    EXPECT_TRUE(menu.has_pixels_in_memory);
    EXPECT_TRUE(AreTheKnownFrame(menu.canvas_pixels)) << TextOfPixels(menu.canvas_pixels);
    EXPECT_EQ(made.child->DrawingNow().drawing, Drawing::kGpu) << "while the window it is over stays on the GPU";
    popup.reset();
}

TEST(GpuWindow, ASoftwareGlIsRefusedAndTheWindowDrawsOnTheCpuRaster)
{
    // What keeps a machine with only a CPU rasteriser - CI's Xvfb, whose EGL has only llvmpipe, on runners that are
    // also the chess matches' - from spinning one up across its cores for a window: refused as kSoftwareRenderer, the
    // window left on the CPU raster, and no rasteriser's thread started. On a machine with a GPU, Mesa is told to
    // rasterise in software for this case; a driver that is not Mesa's does not hear that, and the case skips there.
    GpuDrawingVariable  unset("");
    EnvironmentVariable software("LIBGL_ALWAYS_SOFTWARE", "1");
    EnvironmentVariable llvmpipe("GALLIUM_DRIVER", "llvmpipe"); // over a GALLIUM_DRIVER that names a GPU (WSLg's d3d12)
    constexpr int       kSize = 160;
    GpuChild            made(kSize, kSize, kSize, kSize);
    if (!HasAWindow(made))
    {
        GTEST_SKIP() << "no display";
    }
    const int          threads_before = SoftwareRasteriserThreads();
    const DrawingState state          = made.child->SetDrawing(reckless::window::GpuDrawingRequest());
    std::printf("[ gpu window ] asked for the GPU with software GL: %s\n", Described(state).c_str());
    if (state.drawing == Drawing::kGpu && !reckless::window::IsSoftwareRenderer(state.renderer.c_str()))
    {
        GTEST_SKIP() << "this EGL is not Mesa's, and draws on its GPU whatever LIBGL_ALWAYS_SOFTWARE says: "
                     << state.renderer;
    }
    EXPECT_EQ(state.drawing, Drawing::kCpuRaster);
    EXPECT_EQ(state.why, DrawingFallback::kSoftwareRenderer) << reckless::window::TextOf(state.why);
    EXPECT_EQ(made.child->DrawingNow().why, DrawingFallback::kSoftwareRenderer) << "it says the same when asked again";
    EXPECT_EQ(SoftwareRasteriserThreads(), threads_before)
        << "refused before a context was made: no software rasteriser's threads were started";
    Turn(*made.loop, 1500, [&] { return made.frame.paints > 0; });
    ASSERT_GT(made.frame.paints, 0);
    EXPECT_FALSE(made.frame.is_on_gpu) << "the views are drawn on the CPU raster";
    EXPECT_TRUE(made.frame.has_pixels_in_memory);
    EXPECT_TRUE(AreTheKnownFrame(made.frame.canvas_pixels)) << TextOfPixels(made.frame.canvas_pixels);
}

TEST(GpuWindow, TheEnvironmentTurnsTheGpuOffAndTheWindowSaysSoAndDrawsOnTheCpuRaster)
{
    GpuDrawingVariable off("off");
    constexpr int      kSize = 160;
    GpuChild           made(kSize, kSize, kSize, kSize);
    if (!HasAWindow(made))
    {
        GTEST_SKIP() << "no display";
    }
    const DrawingState state = made.child->SetDrawing(reckless::window::GpuDrawingRequest());
    EXPECT_EQ(state.drawing, Drawing::kCpuRaster);
    EXPECT_EQ(state.why, DrawingFallback::kForcedOff);
    EXPECT_TRUE(state.renderer.empty()) << "and no GL context was made to find that out";
    EXPECT_EQ(made.child->DrawingNow().why, DrawingFallback::kForcedOff) << "it says the same when asked again";
    Turn(*made.loop, 1500, [&] { return made.frame.paints > 0; });
    ASSERT_GT(made.frame.paints, 0);
    EXPECT_FALSE(made.frame.is_on_gpu) << "the views are drawn on the CPU raster";
    EXPECT_TRUE(made.frame.has_pixels_in_memory) << "whose pixels a view may write by hand";
    EXPECT_TRUE(AreTheKnownFrame(made.frame.canvas_pixels)) << TextOfPixels(made.frame.canvas_pixels);
}

TEST(GpuWindow, TheGpuAskedForWithNoBackEndIsAnsweredNotLinked)
{
    GpuDrawingVariable unset("");
    GpuChild           made(240, 160, 240, 160);
    if (!HasAWindow(made))
    {
        GTEST_SKIP() << "no display";
    }
    EXPECT_EQ(made.child->DrawingNow().why, DrawingFallback::kNotAsked)
        << "a window no one has asked draws as it always did";
    const DrawingState state = made.child->SetDrawing({Drawing::kGpu, nullptr});
    EXPECT_EQ(state.drawing, Drawing::kCpuRaster);
    EXPECT_EQ(state.why, DrawingFallback::kNotLinked);
}

TEST(GpuWindow, ThisSuiteLinksTheEglBackEndAndLoadsLibEgl)
{
    // The control for the window's own suite, which links no back end and holds that libEGL is not loaded: the same
    // read finds it here, where the back end is linked.
#if defined(RECKLESS_GPU_EGL)
    EXPECT_NE(reckless::window::GpuDrawingBackend(), nullptr) << "the back end is EGL's";
    const std::string loaded = reckless::test::LoadedLibrariesOfThisProcess();
    EXPECT_TRUE(reckless::test::NamesLibrary(loaded, "libEGL.so")) << loaded;
    // libGLX is on the link line for the pack's sake (skia/CMakeLists.txt) and is never called; libGL is not linked.
    EXPECT_FALSE(reckless::test::NamesLibrary(loaded, "libGL.so")) << "and libGL is not linked: " << loaded;
#else
    EXPECT_EQ(reckless::window::GpuDrawingBackend(), nullptr);
    GTEST_SKIP() << "built where no EGL was found (libegl-dev): the back end is none";
#endif
}
#endif

// The interface's own default (window/GpuPresenter.h): a back end that takes no back buffer early is asked on every
// frame and does nothing, so Windows and Linux are unchanged until each is measured and a product that calls it is
// no worse off than one that does not. Needs no GPU: the presenter here is a stub.
TEST(GpuPresenterDefault, APresenterThatTakesNoBackBufferEarlyIsAskedAndDoesNothing)
{
    class TakesNoneEarly final : public reckless::window::GpuPresenter
    {
      public:
        sk_sp<SkSurface> SurfaceOfSize(int, int) override
        {
            return nullptr;
        }

        bool BeginFrame() override
        {
            return true;
        }

        bool Present() override
        {
            ++presents;
            return true;
        }

        int presents = 0;
    };

    TakesNoneEarly presenter;
    presenter.AcquireBackBuffer();
    presenter.AcquireBackBuffer();
    EXPECT_EQ(presenter.presents, 0) << "asking for one early is not a present";
    EXPECT_TRUE(presenter.Present());
    EXPECT_EQ(presenter.presents, 1) << "and the present takes its own back buffer, as it always did";
}
