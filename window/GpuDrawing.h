#pragma once

// How a child window draws (ChildWindow::SetDrawing): Skia's CPU raster, rastered into memory and handed to the
// platform, which is what every window here has done since the toolkit began, or the GPU - Skia's Ganesh on the
// platform's own GL, the frame drawn into a GPU surface and presented with a swap. A product asks for the GPU, and the
// window gives it where it can and says so, and gives the CPU raster where it cannot and says why: a window that was
// asked and could not is never left drawing nothing.
//
// The GPU's code is a library of its own, reckless_window_gpu, which a product links when it asks for it: Ganesh adds
// about 1.8 MB to a binary and makes the platform's GL a library it loads as it starts (Legion5i, Windows, 2026-11-07),
// and the plug-ins, which never ask, are loaded into a host's process. So a request names the back end it wants drawn
// with (GpuDrawingBackend, in GpuDrawingBackend.h), and a binary that never links one carries none of it.
//
// Everything a window decides about which it draws with is here, and none of it needs a window, a GPU or a display:
// whether the GPU was asked for, whether the person's environment has turned it off, what a back end that is missing
// means, and how many times a lost GPU is made again before the window gives it up. So the suites hold all of it on
// every platform, as window/FramePacing.h holds the frame pacing's. What is left to each platform is making its
// context and presenting through it (window/win/GpuDrawingWin.cpp on Windows).

#include <optional>
#include <string>

namespace reckless::window
{
/** What a window draws its view tree with. */
enum class Drawing
{
    kCpuRaster, ///< Skia's CPU raster, handed to the platform: what every window here has drawn with from the start
    kGpu,       ///< Skia's Ganesh on the platform's GL, presented with a swap; kCpuRaster wherever it cannot be had
};

/** Why a window draws with the CPU raster. Everything but kNone and kNotAsked is a window that was asked for the GPU
    and could not give it, and every one of them is reported rather than passed over, as frame pacing's are: a product
    that asked for the GPU and did not get it is owed the reason. */
enum class DrawingFallback
{
    kNone,                ///< nothing fell back: the window draws on the GPU
    kNotAsked,            ///< no one asked for the GPU - a window's state until a product does
    kForcedOff,           ///< the environment turned the GPU off (RECKLESS_GPU_DRAWING=off)
    kNotLinked,           ///< the request named no GPU back end: the product did not link reckless_window_gpu
    kNoGpuOnThisPlatform, ///< this platform's window has no GPU path yet: macOS and Linux, for now
    kNoPixelFormat,       ///< the window could not be given a pixel format the GL draws to
    kNoContext,           ///< the platform would not make a GL context on the window
    kGlTooOld,            ///< the GL the platform gave is older than the 3.0 Skia's Ganesh is asked to draw on
    kNoSkiaInterface,     ///< Skia could not read the GL's functions off the context
    kNoSkiaContext,       ///< Skia would not make its GPU context on that GL
    kNoSurface,           ///< Skia would not make a surface the window's size on the GPU
    kContextLostTooOften, ///< the GPU was lost more times than kMostGpuContextsLost, and is not asked again
    kContextNotMadeAgain, ///< the GPU was lost, and a context could not be made again in its place
};

/** `why` in words, for a product to report or log.
    @param why  the reason
    @return     the words, a phrase in lower case, never null and never empty */
inline const char *TextOf(DrawingFallback why) noexcept
{
    switch (why)
    {
    case DrawingFallback::kNone:
        return "drawn on the GPU";
    case DrawingFallback::kNotAsked:
        return "the GPU was not asked for";
    case DrawingFallback::kForcedOff:
        return "the environment turned the GPU off (RECKLESS_GPU_DRAWING=off)";
    case DrawingFallback::kNotLinked:
        return "no GPU back end was linked into this program";
    case DrawingFallback::kNoGpuOnThisPlatform:
        return "this platform's window has no GPU path yet";
    case DrawingFallback::kNoPixelFormat:
        return "the window could not be given a pixel format the GL draws to";
    case DrawingFallback::kNoContext:
        return "the platform would not make a GL context on the window";
    case DrawingFallback::kGlTooOld:
        return "the GL given is older than 3.0";
    case DrawingFallback::kNoSkiaInterface:
        return "Skia could not read the GL's functions";
    case DrawingFallback::kNoSkiaContext:
        return "Skia would not make its GPU context on the GL";
    case DrawingFallback::kNoSurface:
        return "Skia would not make a surface the window's size on the GPU";
    case DrawingFallback::kContextLostTooOften:
        return "the GPU was lost too many times to be asked again";
    case DrawingFallback::kContextNotMadeAgain:
        return "the GPU was lost and could not be made again";
    }
    return "unknown";
}

/** The GPU back end a build links in: opaque here, and defined by the platform's own code alone (window/win's on
    Windows). A product names it in its request through GpuDrawingBackend (GpuDrawingBackend.h), which only the
    reckless_window_gpu library defines, so a program that never links that library carries none of the GPU's code. */
struct GpuBackend;

/**
    What a product asks of its window's drawing. A default-made DrawingRequest asks for exactly what a window did before
    any of this existed: the CPU raster, with nothing of a GPU touched.

    A product that wants the GPU links reckless_window_gpu and asks with GpuDrawingRequest() (GpuDrawingBackend.h),
   which fills both members in; `{Drawing::kGpu, nullptr}` - the GPU asked for with no back end - is answered
   kNotLinked.
*/
struct DrawingRequest
{
    /** What to draw with. The default is the CPU raster: what every window did before this existed. */
    Drawing drawing{Drawing::kCpuRaster};

    /** The GPU back end to draw with, from GpuDrawingBackend(); null for none. Read only where `drawing` asks for the
        GPU. */
    const GpuBackend *gpu_backend{nullptr};
};

/** What a window ended up drawing with, for a product to report, log or adapt to (ChildWindow::SetDrawing,
    ChildWindow::DrawingNow). A product that would draw differently on a GPU canvas - a picture uploaded once rather
   than its pixels written by hand every frame - can also ask the canvas itself: a view's SkCanvas
    (gfx::SkiaCanvas::skia()) has a `recordingContext()` on the GPU and none on the CPU raster, and its
    `accessTopLayerPixels` answers null on the GPU, whose pixels are not in memory. */
struct DrawingState
{
    /** What the window draws with: kCpuRaster wherever the GPU could not be had, and `why` then says which of the
        reasons it was. */
    Drawing drawing{Drawing::kCpuRaster};

    /** Why the window draws with the CPU raster; kNone where it draws on the GPU. */
    DrawingFallback why{DrawingFallback::kNotAsked};

    /** The GL's renderer and version as the driver names them ("NVIDIA GeForce RTX 4070 Laptop GPU/PCIe/SSE2, 4.6.0
        NVIDIA 610.62"), for a product to log; empty where no GL context was made. */
    std::string renderer;

    /** How many times the window has lost its GPU context - a driver reset, a driver update, a GPU taken away - since
        the GPU was asked for, each made again in its place (kMostGpuContextsLost). 0 where none was lost. */
    int contexts_lost{0};
};

/** The environment variable that turns the GPU off for a whole run: set to `off`, every window asked for the GPU draws
    with the CPU raster instead and says kForcedOff. For a person whose driver misdraws, a suite that is to hold the
    fallback, and a comparison of the two paths with nothing rebuilt. */
inline constexpr const char *kGpuDrawingEnvironmentVariable = "RECKLESS_GPU_DRAWING";

/** Whether the environment turns the GPU off: RECKLESS_GPU_DRAWING set to `off`, as RECKLESS_UPDATE_CHECK=off turns
    the update check off. Any other value, and none, leaves it to the request.
    @param value  the variable's value, as core::EnvironmentVariable reads it; empty where it is not set
    @return       true when the GPU is turned off */
inline bool IsGpuDrawingForcedOff(const std::optional<std::string> &value)
{
    return value == std::optional<std::string>("off");
}

/** The most times a window makes its GPU context again after losing it. A driver that resets once - an update, a
    recovery from a hang - is drawn on again; one that keeps resetting is given up for the CPU raster, which goes on
    drawing whatever the driver does, rather than making a context to lose for ever. A bound on what is worth trying,
   not a tuning number. */
inline constexpr int kMostGpuContextsLost = 3;

/** Whether a window that has just lost its GPU context, for the `contexts_lost`th time, makes one again.
    @param contexts_lost  the times it has been lost, this one included: 1 or more
    @return               true to make it again; false to give it up for the CPU raster (kContextLostTooOften) */
constexpr bool MayMakeTheGpuContextAgain(int contexts_lost) noexcept
{
    return contexts_lost <= kMostGpuContextsLost;
}

/** What a request decides before any GPU is touched: the CPU raster with kNotAsked for a request that does not ask,
    kForcedOff where the environment has turned the GPU off, kNoGpuOnThisPlatform where the window has no GPU path,
    kNotLinked where the request names no back end - each checked in that order, so a run with the GPU turned off says
    so on every platform - and otherwise the GPU, which the platform then tries to make and may still refuse for a
    reason of its own.
    @param request              what the product asked for
    @param is_forced_off        whether the environment turns the GPU off (IsGpuDrawingForcedOff)
    @param has_platform_gpu     whether this platform's window has a GPU path at all
    @return                     the state: kGpu with kNone where the platform is to try, else the CPU raster and why */
inline DrawingState ChosenDrawing(const DrawingRequest &request, bool is_forced_off, bool has_platform_gpu)
{
    DrawingState state;
    if (request.drawing != Drawing::kGpu)
    {
        return state; // the CPU raster, not asked
    }
    if (is_forced_off)
    {
        state.why = DrawingFallback::kForcedOff;
    }
    else if (!has_platform_gpu)
    {
        state.why = DrawingFallback::kNoGpuOnThisPlatform;
    }
    else if (request.gpu_backend == nullptr)
    {
        state.why = DrawingFallback::kNotLinked;
    }
    else
    {
        state.drawing = Drawing::kGpu;
        state.why     = DrawingFallback::kNone;
    }
    return state;
}

/** The oldest GL a window draws on with Skia's Ganesh, as a major version: 3. Skia's own floor is lower, but nothing
    below 3.0 is a GPU a window would be better on than the CPU raster - Windows' own software GL is 1.1 - so a GL older
    than this is refused (kGlTooOld) rather than drawn on. */
inline constexpr int kOldestGlMajorVersion = 3;

/** Whether a GL whose version string reads `version` (glGetString(GL_VERSION): "4.6.0 NVIDIA 610.62", "1.1.0", "OpenGL
    ES 3.2 ...") is one a window draws on: a major version of kOldestGlMajorVersion or more, read from the first digits
    in it.
    @param version  the version string; null or empty for none
    @return         true for a GL of 3.0 or later */
inline bool IsGlNewEnough(const char *version) noexcept
{
    if (version == nullptr)
    {
        return false;
    }
    while (*version != '\0' && (*version < '0' || *version > '9'))
    {
        ++version; // past an "OpenGL ES " or any other words before the number
    }
    int major = 0;
    for (; *version >= '0' && *version <= '9'; ++version)
    {
        major = major * 10 + (*version - '0');
        if (major > 1000)
        {
            return true; // a number past any GL's: no overflow, and new enough
        }
    }
    return major >= kOldestGlMajorVersion;
}
} // namespace reckless::window
