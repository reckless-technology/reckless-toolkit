// The GPU back end on Linux, in reckless_window_gpu: a child window drawn by Skia's Ganesh on desktop OpenGL through
// EGL, on the X11 window the child already is. The window's own connection is made an EGL display
// (eglGetPlatformDisplay on EGL_PLATFORM_X11_KHR); a software device is refused there, before any context is made; a
// config is chosen whose native visual is the window's own, since the window was made with its parent's; a GL context
// is made - a robust one, which a driver reset is reported to, where the driver offers it - and its renderer read, a
// software rasteriser refused, before the window is touched; then a window surface on the window, and Skia's GPU
// context on the context. The views draw into a surface of the window's size on the GPU, which keeps its pixels from
// frame to frame as the CPU raster does, and each frame is that surface drawn into the window surface's back buffer and
// the buffers swapped, with a swap interval of 0: the frame callback runs on the host loop's 16 ms timer, and a swap
// that waited for the display would hold that loop - which every window on it shares - for up to a refresh
// (docs/window/gpu-drawing.md).
//
// No GL header is included and no GL library linked: every GL function is EGL's to give (eglGetProcAddress, which
// EGL_KHR_client_get_all_proc_addresses makes good for the core functions too), so the one library this adds to a
// program is libEGL, and only to a program that links reckless_window_gpu.
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/SkColorSpace.h"
#include "include/core/SkSurface.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/GrDirectContext.h"
#include "include/gpu/ganesh/SkSurfaceGanesh.h"
#include "include/gpu/ganesh/gl/GrGLAssembleInterface.h"
#include "include/gpu/ganesh/gl/GrGLBackendSurface.h"
#include "include/gpu/ganesh/gl/GrGLDirectContext.h"
#include "include/gpu/ganesh/gl/GrGLInterface.h"

#include "GpuDrawingBackend.h"
#include "GpuSurfacePresenter.h"

// Xlib and EGL after Skia's headers: Xlib's macros (None, Bool, Status, Success, Always) would reach into them. Xlib
// first, which EGL's own platform header includes only where it is told to (USE_X11).
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>

#include "GpuPresenterLinux.h"

namespace reckless::window
{
namespace
{
// What GL names that its own headers would, which this file does not include.
constexpr unsigned kGlRenderer = 0x1F01; ///< GL_RENDERER
constexpr unsigned kGlVersion  = 0x1F02; ///< GL_VERSION
constexpr unsigned kGlNoError  = 0;      ///< GL_NO_ERROR, which a reset status reads when none
constexpr unsigned kGlRgba8    = 0x8058; ///< GL_RGBA8
constexpr unsigned kGlRgb8     = 0x8051; ///< GL_RGB8
constexpr unsigned kGlBack     = 0x0405; ///< GL_BACK

using GetStringFunction           = const unsigned char *(*)(unsigned); ///< glGetString
using DrawBufferFunction          = void (*)(unsigned);                 ///< glDrawBuffer, glReadBuffer
using GraphicsResetStatusFunction = unsigned (*)();                     ///< glGetGraphicsResetStatus(ARB, KHR)

/** An EGL or GL function by name, as EGL gives it; null where it has none.
    @param name  the function's name
    @return      the function, or null */
template <typename Function> Function EglFunction(const char *name) noexcept
{
    return reinterpret_cast<Function>(eglGetProcAddress(name));
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

/** Whether the EGL display's device is a software one: Mesa's EGL_MESA_device_software, which it names on the device
    it gives a display that has no GPU to draw with - Xvfb's, LIBGL_ALWAYS_SOFTWARE's, an X server without DRI3. Read
    from the display alone, before any context is made, so a machine that has only llvmpipe - CI's runners, which are
    also the machines the chess matches run on - never starts a software rasteriser's threads for a window. Asked
    through the device query, which a driver without it (NVIDIA's names no such extension) answers no to.
    @param display     the initialised EGL display
    @param client_extensions  EGL's client extensions
    @return            true when the device is software */
bool IsSoftwareDevice(EGLDisplay display, const char *client_extensions) noexcept
{
    if (!NamesExtension(client_extensions, "EGL_EXT_device_query") &&
        !NamesExtension(client_extensions, "EGL_EXT_device_base"))
    {
        return false;
    }
    const auto query_display = EglFunction<PFNEGLQUERYDISPLAYATTRIBEXTPROC>("eglQueryDisplayAttribEXT");
    const auto query_device  = EglFunction<PFNEGLQUERYDEVICESTRINGEXTPROC>("eglQueryDeviceStringEXT");
    EGLAttrib  device        = 0;
    if (query_display == nullptr || query_device == nullptr ||
        query_display(display, EGL_DEVICE_EXT, &device) != EGL_TRUE || device == 0)
    {
        return false;
    }
    return NamesExtension(query_device(reinterpret_cast<EGLDeviceEXT>(device), EGL_EXTENSIONS),
                          "EGL_MESA_device_software");
}

/** The config a window of `visual` is drawn to with desktop GL: one whose native visual is the window's own - the
    window was made with its parent's visual, and keeps it - with eight bits of red, green and blue, eight of stencil
    where one has them, and the fewest depth bits and samples of the rest, since Skia draws with neither.
    @param display  the initialised EGL display
    @param visual   the window's visual
    @param config   set to the config chosen
    @return         true when there is one */
bool ChooseConfig(EGLDisplay display, Visual *visual, EGLConfig &config) noexcept
{
    const EGLint wanted[] = {EGL_SURFACE_TYPE,
                             EGL_WINDOW_BIT,
                             EGL_RENDERABLE_TYPE,
                             EGL_OPENGL_BIT,
                             EGL_RED_SIZE,
                             8,
                             EGL_GREEN_SIZE,
                             8,
                             EGL_BLUE_SIZE,
                             8,
                             EGL_NONE};
    EGLint       count    = 0;
    if (eglChooseConfig(display, wanted, nullptr, 0, &count) != EGL_TRUE || count <= 0)
    {
        return false;
    }
    std::vector<EGLConfig> configs(static_cast<std::size_t>(count));
    if (eglChooseConfig(display, wanted, configs.data(), count, &count) != EGL_TRUE)
    {
        return false;
    }
    const auto visual_id = static_cast<EGLint>(XVisualIDFromVisual(visual));
    const auto attribute = [display](EGLConfig of, EGLint name) {
        EGLint value = 0;
        eglGetConfigAttrib(display, of, name, &value);
        return value;
    };
    bool found      = false;
    long best_score = 0;
    for (int index = 0; index < count; ++index)
    {
        const EGLConfig candidate = configs[static_cast<std::size_t>(index)];
        if (attribute(candidate, EGL_NATIVE_VISUAL_ID) != visual_id || attribute(candidate, EGL_RED_SIZE) != 8 ||
            attribute(candidate, EGL_GREEN_SIZE) != 8 || attribute(candidate, EGL_BLUE_SIZE) != 8)
        {
            continue;
        }
        // Lower is better: a stencil first, then no multisampling, then the fewest depth bits.
        const long score = (attribute(candidate, EGL_STENCIL_SIZE) >= 8 ? 0 : 100000) +
                           attribute(candidate, EGL_SAMPLES) * 1000 + attribute(candidate, EGL_DEPTH_SIZE);
        if (!found || score < best_score)
        {
            found      = true;
            best_score = score;
            config     = candidate;
        }
    }
    return found;
}

/** A desktop GL context on `config`: robust - a driver reset reported, not hidden - where the driver makes one, and
    the context EGL makes by itself where not. EGL 1.5's own attributes are asked first and EGL_KHR_create_context's
    flags after: EGL_EXT_create_context_robustness's attributes are for OpenGL ES alone, and every driver measured
    refused them for desktop GL (Mesa 25.2.8 on Intel and llvmpipe, NVIDIA 580.178.04; jonny-precision, 2026-11-08).
    @param display     the initialised EGL display, the GL API bound
    @param config      the config
    @param is_egl_1_5  whether the display is EGL 1.5 or later
    @param extensions  the display's extensions
    @param is_robust   set to whether the context made is a robust one
    @return            the context, or EGL_NO_CONTEXT where none could be made */
EGLContext MakeGlContext(EGLDisplay display, EGLConfig config, bool is_egl_1_5, const char *extensions,
                         bool &is_robust) noexcept
{
    is_robust = true;
    if (is_egl_1_5)
    {
        const EGLint robust[] = {EGL_CONTEXT_OPENGL_ROBUST_ACCESS, EGL_TRUE,
                                 EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY, EGL_LOSE_CONTEXT_ON_RESET, EGL_NONE};
        if (EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, robust); context != EGL_NO_CONTEXT)
        {
            return context;
        }
    }
    if (NamesExtension(extensions, "EGL_KHR_create_context"))
    {
        const EGLint robust[] = {EGL_CONTEXT_FLAGS_KHR, EGL_CONTEXT_OPENGL_ROBUST_ACCESS_BIT_KHR,
                                 EGL_CONTEXT_OPENGL_RESET_NOTIFICATION_STRATEGY_KHR, EGL_LOSE_CONTEXT_ON_RESET_KHR,
                                 EGL_NONE};
        if (EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, robust); context != EGL_NO_CONTEXT)
        {
            return context;
        }
    }
    is_robust = false;
    return eglCreateContext(display, config, EGL_NO_CONTEXT, nullptr);
}

/** A window drawn by Skia's Ganesh on EGL: the platform-neutral half is SkiaGpuPresenter's (the surface the views draw
    into, the copy over the back buffer, the loss), and this is the EGL of it. Made by MakeEglPresenter, which leaves
    it whole or makes nothing. */
class EglPresenter final : public SkiaGpuPresenter
{
  public:
    /** The presenter over what MakeEglPresenter made, which it owns from here.
        @param display       the EGL display on the window's connection, which the presenter terminates
        @param surface       the window surface on the window
        @param gl            the GL context
        @param context       Skia's GPU context on `gl`
        @param reset_status  the GL's reset status function, where the context is a robust one; null where not
        @param stencil_bits  the config's stencil bits
        @param has_alpha     whether the config's buffer has an alpha channel */
    EglPresenter(EGLDisplay display, EGLSurface surface, EGLContext gl, sk_sp<GrDirectContext> context,
                 GraphicsResetStatusFunction reset_status, int stencil_bits, bool has_alpha) noexcept
        : SkiaGpuPresenter(std::move(context)), display_(display), surface_(surface), gl_(gl),
          reset_status_(reset_status), stencil_bits_(stencil_bits), has_alpha_(has_alpha)
    {
    }

    /** Skia's GPU resources let go on the context while it is current - and abandoned, so a surface someone still
        holds makes no GL call after - then the context, the window surface and the display, before the window's
        connection closes. */
    ~EglPresenter() override
    {
        const bool is_current = MakeCurrent();
        back_buffer_.reset();
        ShutDownSkia(is_current);
        eglMakeCurrent(display_, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        eglDestroySurface(display_, surface_);
        eglDestroyContext(display_, gl_);
        eglTerminate(display_);
    }

    EglPresenter(const EglPresenter &)            = delete;
    EglPresenter &operator=(const EglPresenter &) = delete;

  private:
    bool MakeCurrent() const noexcept override
    {
        if (eglGetCurrentContext() == gl_ && eglGetCurrentSurface(EGL_DRAW) == surface_)
        {
            return true;
        }
        if (eglMakeCurrent(display_, surface_, surface_, gl_) == EGL_TRUE)
        {
            return true;
        }
        if (eglGetError() == EGL_CONTEXT_LOST)
        {
            is_context_lost_ = true;
        }
        return false;
    }

    sk_sp<SkSurface> BackBuffer(int width, int height) override
    {
        // The window surface's own size, which follows the X window's: after a resize the server may not yet have
        // given the new size when the first frame comes, and a bottom-up buffer wrapped at another height would put
        // the frame that many rows off. The frame goes at its top left whatever the buffer's size.
        EGLint surface_width  = 0;
        EGLint surface_height = 0;
        if (eglQuerySurface(display_, surface_, EGL_WIDTH, &surface_width) == EGL_TRUE &&
            eglQuerySurface(display_, surface_, EGL_HEIGHT, &surface_height) == EGL_TRUE && surface_width > 0 &&
            surface_height > 0)
        {
            width  = surface_width;
            height = surface_height;
        }
        if (!back_buffer_ || back_buffer_->width() != width || back_buffer_->height() != height)
        {
            // The window's own framebuffer, 0, bottom-up as GL's is, with the config's stencil and its format: RGB8
            // for a window of a 24-bit visual, which has no alpha to keep.
            GrGLFramebufferInfo framebuffer{};
            framebuffer.fFBOID  = 0;
            framebuffer.fFormat = has_alpha_ ? kGlRgba8 : kGlRgb8;
            const GrBackendRenderTarget target =
                GrBackendRenderTargets::MakeGL(width, height, 0, stencil_bits_, framebuffer);
            back_buffer_ = SkSurfaces::WrapBackendRenderTarget(
                Context(), target, kBottomLeft_GrSurfaceOrigin,
                has_alpha_ ? kRGBA_8888_SkColorType : kRGB_888x_SkColorType, nullptr, nullptr);
        }
        return back_buffer_;
    }

    void SwapBuffers() noexcept override
    {
        if (eglSwapBuffers(display_, surface_) != EGL_TRUE && eglGetError() == EGL_CONTEXT_LOST)
        {
            is_context_lost_ = true;
        }
    }

    bool IsDeviceReset() const noexcept override
    {
        return is_context_lost_ || (reset_status_ != nullptr && reset_status_() != kGlNoError);
    }

    EGLDisplay                  display_;                ///< the EGL display on the window's connection.
    EGLSurface                  surface_;                ///< the window surface on the window.
    EGLContext                  gl_;                     ///< the GL context.
    GraphicsResetStatusFunction reset_status_{nullptr};  ///< the reset status, for a robust context; null otherwise.
    int                         stencil_bits_;           ///< the config's stencil bits.
    bool                        has_alpha_;              ///< whether the config's buffer has alpha.
    mutable bool                is_context_lost_{false}; ///< EGL said EGL_CONTEXT_LOST to a make-current or swap.
    sk_sp<SkSurface>            back_buffer_;            ///< the window's framebuffer, wrapped at the surface's size.
};

/** The presenter on `window`, or null and the step that refused: the EGL display on the window's connection, its
    device (refused where it is software), a config of the window's visual, a GL context and its renderer (refused
    where it is a software rasteriser or older than 3.0), and only then a window surface on the window, Skia's
    interface to the GL and Skia's context on it. Nothing is left behind where a step refuses, the window is not
    touched before the renderer has been accepted where EGL can make a context current with no surface, and the context
    current before is current again.
    @param x_display  the window's connection
    @param window     the child window
    @param visual     the window's visual
    @return           the presenter, or null and why */
GpuPresenterMade MakeEglPresenter(Display *x_display, ::Window window, Visual *visual)
{
    GpuPresenterMade  made;
    const char *const client_extensions = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!NamesExtension(client_extensions, "EGL_KHR_platform_x11") &&
        !NamesExtension(client_extensions, "EGL_EXT_platform_x11"))
    {
        made.why = DrawingFallback::kNoContext; // an EGL that cannot be given an X11 display
        return made;
    }
    EGLDisplay display = eglGetPlatformDisplay(EGL_PLATFORM_X11_KHR, x_display, nullptr);
    EGLint     major   = 0;
    EGLint     minor   = 0;
    if (display == EGL_NO_DISPLAY || eglInitialize(display, &major, &minor) != EGL_TRUE)
    {
        made.why = DrawingFallback::kNoContext;
        return made;
    }
    const EGLDisplay previous_display = eglGetCurrentDisplay();
    const EGLContext previous_context = eglGetCurrentContext();
    const EGLSurface previous_draw    = eglGetCurrentSurface(EGL_DRAW);
    const EGLSurface previous_read    = eglGetCurrentSurface(EGL_READ);
    const EGLenum    previous_api     = eglQueryAPI();
    EGLContext       gl               = EGL_NO_CONTEXT;
    EGLSurface       surface          = EGL_NO_SURFACE;
    const auto       give_up          = [&](DrawingFallback why) {
        eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (surface != EGL_NO_SURFACE)
        {
            eglDestroySurface(display, surface);
        }
        if (gl != EGL_NO_CONTEXT)
        {
            eglDestroyContext(display, gl);
        }
        eglTerminate(display);
        if (previous_context != EGL_NO_CONTEXT)
        {
            eglMakeCurrent(previous_display, previous_draw, previous_read, previous_context);
        }
        eglBindAPI(previous_api);
        made.why = why;
        return std::move(made);
    };
    if (IsSoftwareDevice(display, client_extensions))
    {
        return give_up(DrawingFallback::kSoftwareRenderer);
    }
    if (eglBindAPI(EGL_OPENGL_API) != EGL_TRUE)
    {
        return give_up(DrawingFallback::kNoContext);
    }
    EGLConfig config{};
    if (!ChooseConfig(display, visual, config))
    {
        return give_up(DrawingFallback::kNoPixelFormat);
    }
    const char *const extensions = eglQueryString(display, EGL_EXTENSIONS);
    bool              is_robust  = false;
    gl = MakeGlContext(display, config, major > 1 || (major == 1 && minor >= 5), extensions, is_robust);
    if (gl == EGL_NO_CONTEXT)
    {
        return give_up(DrawingFallback::kNoContext);
    }
    // The renderer read with no surface where EGL allows it, so a GL that is refused never had the window.
    const bool is_surfaceless = NamesExtension(extensions, "EGL_KHR_surfaceless_context");
    const auto make_surface   = [&] {
        surface = eglCreateWindowSurface(display, config, static_cast<EGLNativeWindowType>(window), nullptr);
        return surface != EGL_NO_SURFACE;
    };
    if (!is_surfaceless && !make_surface())
    {
        return give_up(DrawingFallback::kNoPixelFormat);
    }
    if (eglMakeCurrent(display, surface, surface, gl) != EGL_TRUE)
    {
        return give_up(DrawingFallback::kNoContext);
    }
    const auto  get_string = EglFunction<GetStringFunction>("glGetString");
    const auto *renderer   = get_string != nullptr ? reinterpret_cast<const char *>(get_string(kGlRenderer)) : nullptr;
    const auto *gl_version = get_string != nullptr ? reinterpret_cast<const char *>(get_string(kGlVersion)) : nullptr;
    made.renderer = std::string(renderer != nullptr ? renderer : "") + ", " + (gl_version != nullptr ? gl_version : "");
    if (IsSoftwareRenderer(renderer))
    {
        return give_up(DrawingFallback::kSoftwareRenderer);
    }
    if (!IsGlNewEnough(gl_version))
    {
        return give_up(DrawingFallback::kGlTooOld);
    }
    if (is_surfaceless)
    {
        if (!make_surface() || eglMakeCurrent(display, surface, surface, gl) != EGL_TRUE)
        {
            return give_up(DrawingFallback::kNoPixelFormat);
        }
        // The window's back buffer drawn to and read from by name: a context first made current with no default
        // framebuffer starts with its draw and read buffers at GL_NONE (OpenGL 4.6 core, 17.4.1 and 18.2.1), and
        // binding the window's surface after does not change them. NVIDIA keeps to that, and every frame went
        // nowhere - the window read back black - until they were set; Mesa starts them at GL_BACK regardless
        // (NVIDIA 580.178.04 and Mesa 25.2.8, jonny-precision, 2026-11-08).
        const auto draw_buffer = EglFunction<DrawBufferFunction>("glDrawBuffer");
        const auto read_buffer = EglFunction<DrawBufferFunction>("glReadBuffer");
        if (draw_buffer == nullptr || read_buffer == nullptr)
        {
            return give_up(DrawingFallback::kNoContext);
        }
        draw_buffer(kGlBack);
        read_buffer(kGlBack);
    }
    const sk_sp<const GrGLInterface> interface = GrGLMakeAssembledInterface(
        nullptr, [](void *, const char *name) -> GrGLFuncPtr { return eglGetProcAddress(name); });
    if (!interface)
    {
        return give_up(DrawingFallback::kNoSkiaInterface);
    }
    sk_sp<GrDirectContext> context = GrDirectContexts::MakeGL(interface);
    if (!context)
    {
        return give_up(DrawingFallback::kNoSkiaContext);
    }
    // Swap interval 0: the frame callback runs on the host loop's timer, and a swap that waited for the blank would
    // hold that loop, and every window on it, up to a refresh (docs/window/gpu-drawing.md, "Pacing").
    eglSwapInterval(display, 0);
    GraphicsResetStatusFunction reset_status = nullptr;
    if (is_robust)
    {
        for (const char *name : {"glGetGraphicsResetStatus", "glGetGraphicsResetStatusARB",
                                 "glGetGraphicsResetStatusKHR", "glGetGraphicsResetStatusEXT"})
        {
            reset_status = EglFunction<GraphicsResetStatusFunction>(name);
            if (reset_status != nullptr)
            {
                break;
            }
        }
    }
    EGLint stencil_bits = 0;
    EGLint alpha_bits   = 0;
    eglGetConfigAttrib(display, config, EGL_STENCIL_SIZE, &stencil_bits);
    eglGetConfigAttrib(display, config, EGL_ALPHA_SIZE, &alpha_bits);
    eglBindAPI(previous_api);
    made.presenter = std::make_unique<EglPresenter>(display, surface, gl, std::move(context), reset_status,
                                                    stencil_bits, alpha_bits > 0);
    made.why       = DrawingFallback::kNone;
    return made;
}

/** The back end this library carries. */
const GpuBackend kEglBackend{&MakeEglPresenter};
} // namespace

const GpuBackend *GpuDrawingBackend() noexcept
{
    return &kEglBackend;
}
} // namespace reckless::window
