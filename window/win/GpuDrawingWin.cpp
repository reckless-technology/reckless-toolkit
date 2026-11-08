// The GPU back end on Windows, in reckless_window_gpu: a child window drawn by Skia's Ganesh on OpenGL through WGL. The
// window is given a double-buffered pixel format, a GL context is made on it - a robust one, which a driver reset is
// reported to, where the driver offers that - and Skia's GPU context on the context. The views draw into a surface of
// the window's size on the GPU, which keeps its pixels from frame to frame as the CPU raster does, and each frame is
// that surface drawn into the window's back buffer and the buffers swapped, with a swap interval of 0: the swap never
// waits for the display, because the frame callback is already paced on the display's own clock
// (ChildWindow::SetFramePacing) and a swap that waited too would be a second clock fighting the first
// (docs/window/gpu-drawing.md).
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "include/core/SkBlendMode.h"
#include "include/core/SkCanvas.h"
#include "include/core/SkColorSpace.h"
#include "include/core/SkImageInfo.h"
#include "include/core/SkPaint.h"
#include "include/core/SkSamplingOptions.h"
#include "include/core/SkSurface.h"
#include "include/gpu/GpuTypes.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/GrTypes.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"
#include "include/gpu/ganesh/gl/win/GrGLMakeWinInterface.h"

#include "GpuDrawingBackend.h"

// windows.h after Skia's headers: it defines macros (DrawText, min, max) that would reach into them.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <GL/gl.h>

#include "GpuPresenterWin.h"

namespace reckless::window
{
namespace
{
// What WGL's and GL's extensions name, which the system's own headers (GL 1.1) do not.
constexpr int    kWglContextFlags                     = 0x2094; ///< WGL_CONTEXT_FLAGS_ARB
constexpr int    kWglContextRobustAccessBit           = 0x0004; ///< WGL_CONTEXT_ROBUST_ACCESS_BIT_ARB
constexpr int    kWglContextResetNotificationStrategy = 0x8256; ///< WGL_CONTEXT_RESET_NOTIFICATION_STRATEGY_ARB
constexpr int    kWglLoseContextOnReset               = 0x8252; ///< WGL_LOSE_CONTEXT_ON_RESET_ARB
constexpr GLenum kGlRgba8                             = 0x8058; ///< GL_RGBA8
constexpr GLenum kGlNoError                           = 0;      ///< GL_NO_ERROR, which a reset status reads when none

using CreateContextAttribsFunction = HGLRC(WINAPI *)(HDC, HGLRC, const int *); ///< wglCreateContextAttribsARB
using ExtensionsStringFunction     = const char *(WINAPI *)(HDC);              ///< wglGetExtensionsStringARB
using SwapIntervalFunction         = BOOL(WINAPI *)(int);                      ///< wglSwapIntervalEXT
using GraphicsResetStatusFunction  = GLenum(APIENTRY *)();                     ///< glGetGraphicsResetStatus(ARB)

/** A WGL or GL extension function by name, from the context current on this thread; null where it has none. WGL answers
    some failures with 1, 2, 3 or -1 rather than null, and those are none too.
    @param name  the function's name
    @return      the function, or null */
template <typename Function> Function ContextFunction(const char *name) noexcept
{
    PROC       found = wglGetProcAddress(name);
    const auto value = reinterpret_cast<std::intptr_t>(found);
    if (value == 0 || value == 1 || value == 2 || value == 3 || value == -1)
    {
        return nullptr;
    }
    return reinterpret_cast<Function>(reinterpret_cast<void *>(found));
}

/** Whether the space-separated list `extensions` names `name` whole.
    @param extensions  the list; null for none
    @param name        the extension
    @return            true when it is there */
bool NamesExtension(const char *extensions, const char *name) noexcept
{
    if (extensions == nullptr)
    {
        return false;
    }
    const std::size_t length = std::strlen(name);
    for (const char *at = std::strstr(extensions, name); at != nullptr; at = std::strstr(at + 1, name))
    {
        const bool is_start = at == extensions || at[-1] == ' ';
        const bool is_end   = at[length] == '\0' || at[length] == ' ';
        if (is_start && is_end)
        {
            return true;
        }
    }
    return false;
}

/** A window drawn by Skia's Ganesh on WGL. Made by MakeWglPresenter, which leaves it whole or makes nothing. */
class WglPresenter final : public win::GpuPresenter
{
  public:
    /** The presenter over what MakeWglPresenter made, which it owns from here.
        @param window   the window
        @param dc       the window's device context, held for the presenter's life
        @param gl       the GL context on it
        @param context  Skia's GPU context on `gl`
        @param reset_status  the GL's reset status function, where the context is a robust one; null where not */
    WglPresenter(HWND window, HDC dc, HGLRC gl, sk_sp<GrDirectContext> context,
                 GraphicsResetStatusFunction reset_status) noexcept
        : window_(window), dc_(dc), gl_(gl), context_(std::move(context)), reset_status_(reset_status)
    {
    }

    /** Skia's GPU resources let go on the context while it is current - and abandoned, so a surface someone still
        holds makes no GL call after - and the GL context and the device context released. */
    ~WglPresenter() override
    {
        const bool is_current = MakeCurrent();
        offscreen_.reset();
        back_buffer_.reset();
        if (context_)
        {
            if (is_current && !context_->abandoned())
            {
                context_->flushAndSubmit(GrSyncCpu::kYes);
            }
            context_->releaseResourcesAndAbandonContext();
            context_.reset();
        }
        wglMakeCurrent(nullptr, nullptr);
        wglDeleteContext(gl_);
        ReleaseDC(window_, dc_);
    }

    WglPresenter(const WglPresenter &)            = delete;
    WglPresenter &operator=(const WglPresenter &) = delete;

    sk_sp<SkSurface> SurfaceOfSize(int width, int height) override
    {
        if (!MakeCurrent() || context_->abandoned())
        {
            return nullptr;
        }
        if (!offscreen_ || offscreen_->width() != width || offscreen_->height() != height)
        {
            offscreen_.reset();
            back_buffer_.reset();
            // RGBA, as GL holds it, premultiplied as the CPU raster is; drawn top-down, as the views lay it out.
            offscreen_ =
                SkSurfaces::RenderTarget(context_.get(), skgpu::Budgeted::kNo,
                                         SkImageInfo::Make(width, height, kRGBA_8888_SkColorType, kPremul_SkAlphaType),
                                         0, kTopLeft_GrSurfaceOrigin, nullptr);
            if (!offscreen_)
            {
                return nullptr;
            }
            offscreen_->getCanvas()->clear(SK_ColorTRANSPARENT);
        }
        return offscreen_;
    }

    bool BeginFrame() override
    {
        return MakeCurrent() && !context_->abandoned();
    }

    bool Present() override
    {
        if (!offscreen_ || !MakeCurrent() || context_->abandoned())
        {
            return !IsLost();
        }
        const int width  = offscreen_->width();
        const int height = offscreen_->height();
        if (!back_buffer_ || back_buffer_->width() != width || back_buffer_->height() != height)
        {
            // The window's own framebuffer, 0, at the surface's size - which is the window's, the window having been
            // sized to it - bottom-up as GL's is, with the pixel format's stencil.
            GrGLFramebufferInfo framebuffer{};
            framebuffer.fFBOID                 = 0;
            framebuffer.fFormat                = kGlRgba8;
            const GrBackendRenderTarget target = GrBackendRenderTargets::MakeGL(width, height, 0, 8, framebuffer);
            back_buffer_ = SkSurfaces::WrapBackendRenderTarget(context_.get(), target, kBottomLeft_GrSurfaceOrigin,
                                                               kRGBA_8888_SkColorType, nullptr, nullptr);
            if (!back_buffer_)
            {
                return !IsLost();
            }
        }
        // The whole frame, as it stands, over the back buffer: the swap leaves the back buffer's pixels undefined, so
        // only the views' own surface keeps them, and the copy is the GPU's, a fraction of a millisecond at 4K.
        SkPaint copy;
        copy.setBlendMode(SkBlendMode::kSrc);
        offscreen_->draw(back_buffer_->getCanvas(), 0, 0, SkSamplingOptions(), &copy);
        context_->flushAndSubmit(back_buffer_.get(), GrSyncCpu::kNo);
        SwapBuffers(dc_);
        return !IsLost();
    }

  private:
    /** The GL context current on this thread, where it is not already.
        @return  true when it is current */
    bool MakeCurrent() const noexcept
    {
        return wglGetCurrentContext() == gl_ && wglGetCurrentDC() == dc_ ? true : wglMakeCurrent(dc_, gl_) != FALSE;
    }

    /** Whether the GPU has gone: the driver said the context was reset, or Skia has abandoned its context.
        @return  true when it is lost */
    bool IsLost() const noexcept
    {
        if (context_->abandoned())
        {
            return true;
        }
        return reset_status_ != nullptr && reset_status_() != kGlNoError;
    }

    HWND                        window_;                ///< the window drawn in.
    HDC                         dc_;                    ///< its device context, held for the presenter's life.
    HGLRC                       gl_;                    ///< the GL context on it.
    sk_sp<GrDirectContext>      context_;               ///< Skia's GPU context on the GL.
    GraphicsResetStatusFunction reset_status_{nullptr}; ///< the reset status, for a robust context; null otherwise.
    sk_sp<SkSurface>            offscreen_;             ///< the surface the views draw into, which keeps its pixels.
    sk_sp<SkSurface>            back_buffer_;           ///< the window's framebuffer, wrapped at the surface's size.
};

/** A GL context on `dc`, robust - a driver reset reported, not hidden - where the driver offers that, and the one WGL
    makes by itself where not; current on this thread when made.
    @param dc            the window's device context, its pixel format set
    @param reset_status  set to the reset status function where the context is robust, null where not
    @return              the context, or null where none could be made */
HGLRC MakeGlContext(HDC dc, GraphicsResetStatusFunction &reset_status) noexcept
{
    reset_status = nullptr;
    HGLRC plain  = wglCreateContext(dc);
    if (plain == nullptr || wglMakeCurrent(dc, plain) == FALSE)
    {
        if (plain != nullptr)
        {
            wglDeleteContext(plain);
        }
        return nullptr;
    }
    // A robust context needs WGL_ARB_create_context_robustness, which only a current context can be asked about.
    const auto        extensions_string = ContextFunction<ExtensionsStringFunction>("wglGetExtensionsStringARB");
    const auto        create_context    = ContextFunction<CreateContextAttribsFunction>("wglCreateContextAttribsARB");
    const char *const extensions        = extensions_string != nullptr ? extensions_string(dc) : nullptr;
    if (create_context == nullptr || !NamesExtension(extensions, "WGL_ARB_create_context_robustness"))
    {
        return plain;
    }
    const int attributes[] = {kWglContextFlags, kWglContextRobustAccessBit, kWglContextResetNotificationStrategy,
                              kWglLoseContextOnReset, 0};
    HGLRC     robust       = create_context(dc, nullptr, attributes);
    if (robust == nullptr || wglMakeCurrent(dc, robust) == FALSE)
    {
        if (robust != nullptr)
        {
            wglDeleteContext(robust);
        }
        wglMakeCurrent(dc, plain);
        return plain;
    }
    wglDeleteContext(plain);
    reset_status = ContextFunction<GraphicsResetStatusFunction>("glGetGraphicsResetStatusARB");
    if (reset_status == nullptr)
    {
        reset_status = ContextFunction<GraphicsResetStatusFunction>("glGetGraphicsResetStatus");
    }
    return robust;
}

/** The window's pixel format, set where it has none yet: double-buffered RGBA with eight bits of alpha and eight of
    stencil and no depth, drawn to the window by the GL. A window keeps the first pixel format it is given for life, so
   a window asked for the GPU again keeps the one it was given before.
    @param dc  the window's device context
    @return    true when the window has a pixel format the GL draws to */
bool SetPixelFormatOf(HDC dc) noexcept
{
    if (GetPixelFormat(dc) != 0)
    {
        return true;
    }
    PIXELFORMATDESCRIPTOR wanted{};
    wanted.nSize        = sizeof(wanted);
    wanted.nVersion     = 1;
    wanted.dwFlags      = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    wanted.iPixelType   = PFD_TYPE_RGBA;
    wanted.cColorBits   = 32;
    wanted.cAlphaBits   = 8;
    wanted.cStencilBits = 8;
    wanted.iLayerType   = PFD_MAIN_PLANE;
    const int format    = ChoosePixelFormat(dc, &wanted);
    return format != 0 && SetPixelFormat(dc, format, &wanted) != FALSE;
}

/** This code's own module, which the probe's window class is registered under.
    @return  the module's handle */
HINSTANCE ThisModule() noexcept
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ThisModule), &module);
    return module;
}

/** Whether this machine's GL will draw a window at all, asked of a window of the probe's own that is never shown: a
    pixel format, a GL context, a version of 3.0 or more and Skia's interface to it. A window keeps the first pixel
    format it is given for life, so it is the probe's that is set, and a machine whose GL refuses - Windows' own
   software GL, 1.1, on a machine with no GPU driver, as CI's runners are - leaves the child window exactly as it was,
   drawing with the CPU raster through GDI.
    @param renderer  set to the GL's renderer and version where a context was made
    @return          kNone where the GL will draw, else the step that refused */
DrawingFallback ProbeTheGl(std::string &renderer)
{
    static const wchar_t kProbeClass[] = L"RecklessGpuDrawingProbe";
    WNDCLASSEXW          probe_class{};
    probe_class.cbSize        = sizeof(probe_class);
    probe_class.style         = CS_OWNDC;
    probe_class.lpfnWndProc   = DefWindowProcW;
    probe_class.hInstance     = ThisModule();
    probe_class.lpszClassName = kProbeClass;
    RegisterClassExW(&probe_class); // refused, harmlessly, where an earlier probe registered it
    HWND probe =
        CreateWindowExW(0, kProbeClass, L"", WS_POPUP, 0, 0, 1, 1, nullptr, nullptr, probe_class.hInstance, nullptr);
    if (probe == nullptr)
    {
        return DrawingFallback::kNoPixelFormat;
    }
    DrawingFallback why = DrawingFallback::kNone;
    HDC             dc  = GetDC(probe);
    HGLRC           gl  = nullptr;
    if (dc == nullptr || !SetPixelFormatOf(dc))
    {
        why = DrawingFallback::kNoPixelFormat;
    }
    else
    {
        GraphicsResetStatusFunction unused{nullptr};
        gl = MakeGlContext(dc, unused);
        if (gl == nullptr)
        {
            why = DrawingFallback::kNoContext;
        }
        else
        {
            const auto *name    = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
            const auto *version = reinterpret_cast<const char *>(glGetString(GL_VERSION));
            renderer            = std::string(name != nullptr ? name : "") + ", " + (version != nullptr ? version : "");
            if (!IsGlNewEnough(version))
            {
                why = DrawingFallback::kGlTooOld;
            }
            else if (!GrGLInterfaces::MakeWin())
            {
                why = DrawingFallback::kNoSkiaInterface;
            }
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(gl);
        }
    }
    if (dc != nullptr)
    {
        ReleaseDC(probe, dc);
    }
    DestroyWindow(probe);
    return why;
}

/** The presenter on `window`, or null and the step that refused: the GL asked first on a window of the probe's own
    (ProbeTheGl), and then the window's pixel format, its GL context, its version, Skia's interface to it and Skia's
    context on it. Nothing is left behind where a step refuses, and no context is current.
    @param window  the child window
    @return        the presenter, or null and why */
win::GpuPresenterMade MakeWglPresenter(HWND window)
{
    win::GpuPresenterMade made;
    made.why = ProbeTheGl(made.renderer);
    if (made.why != DrawingFallback::kNone)
    {
        return made;
    }
    HDC dc = GetDC(window);
    if (dc == nullptr || !SetPixelFormatOf(dc))
    {
        if (dc != nullptr)
        {
            ReleaseDC(window, dc);
        }
        made.why = DrawingFallback::kNoPixelFormat;
        return made;
    }
    const HDC                   previous_dc = wglGetCurrentDC();
    const HGLRC                 previous_gl = wglGetCurrentContext();
    GraphicsResetStatusFunction reset_status{nullptr};
    HGLRC                       gl      = MakeGlContext(dc, reset_status);
    const auto                  give_up = [&](DrawingFallback why) {
        if (gl != nullptr)
        {
            wglMakeCurrent(nullptr, nullptr);
            wglDeleteContext(gl);
        }
        ReleaseDC(window, dc);
        if (previous_gl != nullptr)
        {
            wglMakeCurrent(previous_dc, previous_gl);
        }
        made.why = why;
        return std::move(made);
    };
    if (gl == nullptr)
    {
        return give_up(DrawingFallback::kNoContext);
    }
    const auto *renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    const auto *version  = reinterpret_cast<const char *>(glGetString(GL_VERSION));
    made.renderer = std::string(renderer != nullptr ? renderer : "") + ", " + (version != nullptr ? version : "");
    if (!IsGlNewEnough(version))
    {
        return give_up(DrawingFallback::kGlTooOld);
    }
    const sk_sp<const GrGLInterface> interface = GrGLInterfaces::MakeWin();
    if (!interface)
    {
        return give_up(DrawingFallback::kNoSkiaInterface);
    }
    sk_sp<GrDirectContext> context = GrDirectContexts::MakeGL(interface);
    if (!context)
    {
        return give_up(DrawingFallback::kNoSkiaContext);
    }
    // Swap interval 0: the frame callback is paced on the display's clock, and a swap that waited for the blank as well
    // would hold the window's thread up to a refresh beside it (docs/window/gpu-drawing.md, "Pacing").
    if (const auto swap_interval = ContextFunction<SwapIntervalFunction>("wglSwapIntervalEXT"))
    {
        swap_interval(0);
    }
    made.presenter = std::make_unique<WglPresenter>(window, dc, gl, std::move(context), reset_status);
    made.why       = DrawingFallback::kNone;
    return made;
}

/** The back end this library carries. */
const GpuBackend kWglBackend{&MakeWglPresenter};
} // namespace

const GpuBackend *GpuDrawingBackend() noexcept
{
    return &kWglBackend;
}
} // namespace reckless::window
