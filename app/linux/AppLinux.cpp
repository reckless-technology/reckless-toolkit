// The Standalone's platform on Linux (app/AppPlatform.h): top-level X11 windows on a connection of the app's
// own, sized within the window manager's hints - a dialog fixed, the main window between the least and the most its
// owner's constraint allows and at its aspect over the bar, the hints moved with the content, and a size the person
// drags it to told back (ConfigureNotify) - closed through the window manager's WM_DELETE_WINDOW, a dialog marked as
// one and kept over its owner, each carrying the series' app icon as _NET_WM_ICON (decoded by Skia from the module's
// own copy), which the task bar and the switcher show, and each of a visual whose pixels the child window's raster is
// laid out in, whatever the server's default; and the event loop - a poll over the app's connection, the descriptors
// and timers the child windows ask for (window/linux/WindowLinux.cpp runs on the loop a plug-in's host would
// give it, and here the app gives it), a pipe the termination signals write to, so a session that ends saves what the
// app keeps, the calls after the current event, and a pipe a post from another thread writes to, which wakes the poll
// for the calls posted. A window's position is its client area's top left on the root
// window, its gravity static, so it comes back where it stood however tall the window manager's title bar is - wholly
// inside the desktop's work area (WindowPlace.h). The app answers X errors itself, as JUCE does, with a line on
// stderr: Xlib's own handler ends the process, and every knob turned since the start with it.
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/SkBitmap.h"
#include "include/core/SkData.h"
#include "include/core/SkImage.h"

#include "AppPlatform.h"
#include "WindowPlace.h"

// Xlib last: it defines macros (None, Bool, Status, Success, Always) that would reach into the headers above.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/keysym.h>

#include "../../window/linux/X11Clipboard.h" // the child window's too: not on an include path

namespace reckless::app
{
namespace
{
/** The pipe a termination signal writes a byte to, for the loop to read: a handler may do little else. */
int g_signal_pipe[2] = {-1, -1};

/** The termination signals' handler (SIGTERM, SIGINT, SIGHUP): one byte into the pipe, for the loop to read. The
    signal's number is not used. */
void OnSignal(int)
{
    const char byte    = 1;
    const auto written = write(g_signal_pipe[1], &byte, 1);
    (void)written;
}

/** The clock the loop's timers run on: monotonic, so a change of the wall clock moves none. */
using Clock = std::chrono::steady_clock;

/** The series' app icon as _NET_WM_ICON holds one: its width, its height, then each pixel as 0xAARRGGBB in a long,
    straight alpha, row by row; empty for bytes that are not an image.
    @param png  the icon's PNG file, as the module embeds it; empty for no icon
    @return     the property's values, 2 + width x height longs, or an empty vector for none */
std::vector<unsigned long> IconOf(std::span<const std::uint8_t> png)
{
    if (png.empty())
    {
        return {};
    }
    const auto image = SkImages::DeferredFromEncodedData(SkData::MakeWithCopy(png.data(), png.size()));
    if (!image)
    {
        return {};
    }
    SkBitmap bitmap;
    bitmap.allocPixels(
        SkImageInfo::Make(image->width(), image->height(), kBGRA_8888_SkColorType, kUnpremul_SkAlphaType));
    if (!image->readPixels(nullptr, bitmap.pixmap(), 0, 0))
    {
        return {};
    }
    std::vector<unsigned long> icon;
    icon.reserve(2 + static_cast<std::size_t>(image->width() * image->height()));
    icon.push_back(static_cast<unsigned long>(image->width()));
    icon.push_back(static_cast<unsigned long>(image->height()));
    for (int y = 0; y < image->height(); ++y)
    {
        const auto *row = static_cast<const std::uint8_t *>(bitmap.pixmap().addr(0, y));
        for (int x = 0; x < image->width(); ++x)
        {
            const std::uint8_t *p = row + 4 * x; // B, G, R, A
            icon.push_back((static_cast<unsigned long>(p[3]) << 24) | (static_cast<unsigned long>(p[2]) << 16) |
                           (static_cast<unsigned long>(p[1]) << 8) | static_cast<unsigned long>(p[0]));
        }
    }
    return icon;
}

/** Every X error, the app's connection's and its child windows': said on stderr, and the app carries on. A request
    that fails - the focus asked for a window that went out of view a moment before - is no reason to end the process,
    which Xlib's own handler does.
    @param display  the connection the error arrived on
    @param error    the error event: its code, the failed request's major and minor codes and the resource
    @return         0, as a handler returns */
int SayXError(Display *display, XErrorEvent *error)
{
    char text[256] = {};
    XGetErrorText(display, error->error_code, text, static_cast<int>(sizeof(text)));
    std::fprintf(stderr, "X error %d (%s), request %d.%d on resource 0x%lx\n", static_cast<int>(error->error_code),
                 text, static_cast<int>(error->request_code), static_cast<int>(error->minor_code), error->resourceid);
    return 0;
}

/** The key a key press names, as the shortcuts match it: its key symbol unshifted (the press's first, with no
    modifier applied) - letters, digits and the other Latin-1 characters by the character, the function keys and the
    editing and moving keys by name - and Control as the primary modifier, Shift, and Mod1 as Alt.
    @param key  the key press
    @return     the key pressed; empty for a key that names none (a modifier alone, a key of another script) */
Shortcut StrokeOf(XKeyEvent &key)
{
    Shortcut stroke;
    stroke.primary      = (key.state & ControlMask) != 0;
    stroke.shift        = (key.state & ShiftMask) != 0;
    stroke.alt          = (key.state & Mod1Mask) != 0;
    const KeySym symbol = XLookupKeysym(&key, 0);
    if (symbol >= XK_F1 && symbol <= XK_F12)
    {
        stroke.key = static_cast<ShortcutKey>(static_cast<int>(ShortcutKey::kF1) + static_cast<int>(symbol - XK_F1));
        return stroke;
    }
    switch (symbol)
    {
    case XK_Escape:
        stroke.key = ShortcutKey::kEscape;
        return stroke;
    case XK_Return:
    case XK_KP_Enter:
        stroke.key = ShortcutKey::kReturn;
        return stroke;
    case XK_Tab:
    case XK_ISO_Left_Tab:
        stroke.key = ShortcutKey::kTab;
        return stroke;
    case XK_space:
        stroke.key = ShortcutKey::kSpace;
        return stroke;
    case XK_BackSpace:
        stroke.key = ShortcutKey::kBackspace;
        return stroke;
    case XK_Delete:
        stroke.key = ShortcutKey::kDelete;
        return stroke;
    case XK_Left:
        stroke.key = ShortcutKey::kLeft;
        return stroke;
    case XK_Right:
        stroke.key = ShortcutKey::kRight;
        return stroke;
    case XK_Up:
        stroke.key = ShortcutKey::kUp;
        return stroke;
    case XK_Down:
        stroke.key = ShortcutKey::kDown;
        return stroke;
    case XK_Home:
        stroke.key = ShortcutKey::kHome;
        return stroke;
    case XK_End:
        stroke.key = ShortcutKey::kEnd;
        return stroke;
    case XK_Page_Up:
        stroke.key = ShortcutKey::kPageUp;
        return stroke;
    case XK_Page_Down:
        stroke.key = ShortcutKey::kPageDown;
        return stroke;
    default:
        break;
    }
    // Latin-1's printable key symbols are their characters.
    if ((symbol > XK_space && symbol <= XK_asciitilde) || (symbol >= XK_nobreakspace && symbol <= XK_ydiaeresis))
    {
        stroke.key       = ShortcutKey::kCharacter;
        stroke.character = Shortcut::Lowered(static_cast<char32_t>(symbol));
    }
    return stroke;
}

/** Whether `info` lays its pixels out as the child window's raster does: TrueColor, 24 bits, B G R in memory.
    @param info  a visual the server offers
    @return      true when the visual is TrueColor with the red, green and blue masks of B G R in memory */
bool LaysOutBgra(const XVisualInfo &info) noexcept
{
    return info.c_class == TrueColor && info.red_mask == 0xff0000 && info.green_mask == 0x00ff00 &&
           info.blue_mask == 0x0000ff;
}

/** The desktop's work area - what the panels and docks leave of the screen, as the window manager publishes it on the
    current desktop (_NET_WORKAREA) - else the whole screen.
    @param display  the app's connection
    @return         the area, in pixels from the screen's top left; the whole screen where the property is missing or
                    empty */
gfx::RectI WorkArea(Display *display)
{
    const int        screen = DefaultScreen(display);
    const gfx::RectI whole{0, 0, DisplayWidth(display, screen), DisplayHeight(display, screen)};
    const ::Window   root = RootWindow(display, screen);
    const auto       read = [display, root](const char *name, long count, std::vector<long> &values) {
        Atom           type   = 0;
        int            format = 0;
        unsigned long  items  = 0;
        unsigned long  after  = 0;
        unsigned char *data   = nullptr;
        const bool     got    = XGetWindowProperty(display, root, XInternAtom(display, name, False), 0, count, False,
                                                   XA_CARDINAL, &type, &format, &items, &after, &data) == Success &&
                                data != nullptr && format == 32;
        if (got)
        {
            const auto *longs = reinterpret_cast<const long *>(data); // format 32 arrives as longs
            values.assign(longs, longs + items);
        }
        if (data != nullptr)
        {
            XFree(data);
        }
        return got;
    };
    std::vector<long> desktop;
    std::vector<long> areas;
    const long        current = read("_NET_CURRENT_DESKTOP", 1, desktop) && !desktop.empty() ? desktop[0] : 0;
    if (!read("_NET_WORKAREA", 4 * 64, areas) || areas.size() < 4)
    {
        return whole;
    }
    const auto at =
        static_cast<std::size_t>(4 * current + 3) < areas.size() ? static_cast<std::size_t>(4 * current) : 0u;
    const gfx::RectI area{static_cast<int>(areas[at]), static_cast<int>(areas[at + 1]), static_cast<int>(areas[at + 2]),
                          static_cast<int>(areas[at + 3])};
    return area.IsEmpty() ? whole : area;
}

/** The frame a window manager draws round a window's client area, each side in pixels. */
struct FrameExtents
{
    int left{0};   ///< the frame's width at the left
    int right{0};  ///< at the right
    int top{0};    ///< its height at the top, the title bar's
    int bottom{0}; ///< at the bottom
};

/** The frame round `window`'s client area as the window manager publishes it (_NET_FRAME_EXTENTS), once the window is
    mapped; else a typical title bar and border, scaled by `scale`.
    @param display  the app's connection
    @param window   the top-level window
    @param scale    pixels per design pixel, for the typical frame
    @return         the frame's four sides, in pixels */
FrameExtents ExtentsOf(Display *display, ::Window window, float scale)
{
    FrameExtents   extents{static_cast<int>(std::lround(4.0f * scale)), static_cast<int>(std::lround(4.0f * scale)),
                           static_cast<int>(std::lround(36.0f * scale)), static_cast<int>(std::lround(4.0f * scale))};
    Atom           type   = 0;
    int            format = 0;
    unsigned long  items  = 0;
    unsigned long  after  = 0;
    unsigned char *data   = nullptr;
    if (XGetWindowProperty(display, window, XInternAtom(display, "_NET_FRAME_EXTENTS", False), 0, 4, False, XA_CARDINAL,
                           &type, &format, &items, &after, &data) == Success &&
        data != nullptr && format == 32 && items == 4)
    {
        const auto *longs = reinterpret_cast<const long *>(data); // format 32 arrives as longs
        extents           = {static_cast<int>(longs[0]), static_cast<int>(longs[1]), static_cast<int>(longs[2]),
                             static_cast<int>(longs[3])};
    }
    if (data != nullptr)
    {
        XFree(data);
    }
    return extents;
}
} // namespace

class LinuxPlatform;

/** A top-level X11 window of the app: the content a child window is made in, sized within the window manager's hints
    and closed through WM_DELETE_WINDOW. Made by LinuxPlatform::OpenWindow, which holds it in its table by id so the
    loop hands it its events. Main thread. */
class LinuxWindow final : public AppWindow
{
  public:
    /** Create the window, place it and map it: where it stood, else centred over its owner, else on the screen, in a
        visual of the raster's layout. valid() is false where the server refused the window.
        @param platform  the platform whose connection the window is on and whose table holds it; outlives the window
        @param options   the title, the content's design size, the owner and the constraint, as the app asks */
    LinuxWindow(LinuxPlatform &platform, const WindowOptions &options);

    /** Take the window out of the platform's table and destroy it, with its colormap. */
    ~LinuxWindow() override;

    void *Handle() const override
    {
        return reinterpret_cast<void *>(static_cast<std::uintptr_t>(window_));
    }

    float Scale() const override
    {
        return scale_;
    }

    std::pair<int, int> Position() const override;
    void                BringToFront() override;
    void                SetContentSize(int width, int height) override;
    void                SetContentSizeOnScreen(int width, int height) override;
    gfx::PointI         LargestContent() const override;
    void                SetTitle(const std::string &title) override;
    std::string         Title() const override;

    /** The X window's id, the key the platform finds the window by.
        @return the id; 0 when the window was not made */
    ::Window id() const noexcept
    {
        return window_;
    }

    /** Whether the server made the window.
        @return true when the window exists */
    bool valid() const noexcept
    {
        return window_ != 0;
    }

    /** An event for this window: the window manager's close, Escape on a dialog, a resize the person dragged.
        @param event  the event the platform took off the connection, for this window */
    void Event(const XEvent &event);

  private:
    /** Tell the app the person closed the window, when it asked to be told. */
    void Closing()
    {
        if (closing_)
        {
            const auto f = closing_;
            f();
        }
    }

    /** The window manager told the window's size, which no one may change but the app, and where it goes.
        @param x       the client area's left on the root window, in pixels
        @param y       the client area's top on the root window, in pixels
        @param placed  true when the position is one the app asked for (USPosition), false when it is a default */
    void SetSizeHints(int x, int y, bool placed);

    LinuxPlatform &platform_;    ///< the platform this window is in the table of.
    Display       *display_;     ///< the platform's connection.
    ::Window       window_{0};   ///< the X window; 0 when it was not made.
    Colormap       colormap_{0}; ///< the colormap of the raster's visual, freed with the window; 0 for the default's.
    int            pixel_width_{0};  ///< the content's width, in pixels.
    int            pixel_height_{0}; ///< the content's height, in pixels.
    float          scale_{1.0f};     ///< pixels per design pixel, the desktop's (window::DisplayScaleOf).
    std::function<void()> closing_;  ///< what the app does when the person closes the window; may be empty.
    /** The least and the most the owner's constraint allows, asked of it for the size hints; empty for a window of one
        size. */
    std::function<gfx::PointF(gfx::PointF)> constrain_;
    std::function<gfx::PointI()>  aspect_; ///< the panel's design size as the aspect to hold, or {0, 0}; may be empty.
    int                           fixed_height_{0}; ///< the bar's height, in design pixels, over what scales.
    std::function<void(int, int)> resized_; ///< told the content's design size after the person dragged the frame.
    bool                          dialog_;  ///< a dialog: typed as one, Escape closes it.
};

/** The Standalone's platform on Linux: the app's X11 connection, its windows by id, and the loop - a poll over the
    connection, the signal pipe and the child windows' descriptors, with their timers and the calls after the current
    event. Made by AppPlatform::Create. Main thread. */
class LinuxPlatform final : public AppPlatform
{
  public:
    /** Open the connection, install the X error handler and the termination signals' handlers (which write to the
        pipe). With no display the platform makes no window and Run returns at once.
        @param name      the app's name, the windows' class
        @param icon_png  the window icon's PNG file; empty for none */
    LinuxPlatform(const std::string &name, std::span<const std::uint8_t> icon_png)
        : name_(name), icon_(IconOf(icon_png))
    {
        XSetErrorHandler(&SayXError);
        display_ = XOpenDisplay(nullptr);
        // The pipe a post that finds the queue empty writes a byte to, which wakes the loop's poll: written under the
        // queue's lock, so never once Close has returned and the pipe is closed. Non-blocking both ends, so a full
        // pipe - a loop that has not run for a while - drops the byte, and the batch waiting still runs.
        if (pipe(post_pipe_) == 0)
        {
            for (const int fd : post_pipe_)
            {
                fcntl(fd, F_SETFL, O_NONBLOCK);
                fcntl(fd, F_SETFD, FD_CLOEXEC);
            }
            posts_->SetWake([fd = post_pipe_[1]] {
                const char byte    = 1;
                const auto written = write(fd, &byte, 1);
                (void)written;
            });
        }
        else
        {
            post_pipe_[0] = post_pipe_[1] = -1;
        }
        if (pipe(g_signal_pipe) == 0)
        {
            fcntl(g_signal_pipe[0], F_SETFL, O_NONBLOCK);
            fcntl(g_signal_pipe[1], F_SETFL, O_NONBLOCK);
            struct sigaction action{};
            action.sa_handler = OnSignal;
            sigemptyset(&action.sa_mask);
            sigaction(SIGTERM, &action, nullptr);
            sigaction(SIGINT, &action, nullptr);
            sigaction(SIGHUP, &action, nullptr);
        }
    }

    /** Close the posts' queue and its pipe, the windows and the connection. */
    ~LinuxPlatform() override
    {
        posts_->Close();
        for (const int fd : post_pipe_)
        {
            if (fd >= 0)
            {
                close(fd);
            }
        }
        windows_.clear();
        clipboard_.reset(); // on the connection, so before it closes
        if (display_ != nullptr)
        {
            XCloseDisplay(display_);
        }
    }

    /** The app's connection.
        @return the display; nullptr where none could be opened */
    Display *display() const noexcept
    {
        return display_;
    }

    /** The app's name.
        @return the name, which is the window class */
    const std::string &name() const noexcept
    {
        return name_;
    }

    /** The window icon, as _NET_WM_ICON holds it.
        @return the property's values; empty for no icon */
    const std::vector<unsigned long> &icon() const noexcept
    {
        return icon_;
    }

    /** Enter a window in the table the loop hands events through.
        @param window  the window; stays valid until Remove */
    void Add(LinuxWindow &window)
    {
        windows_[window.id()] = &window;
    }

    /** Take a window out of the table.
        @param window  the window being destroyed */
    void Remove(const LinuxWindow &window)
    {
        windows_.erase(window.id());
    }

    /** The window with an X id.
        @param id  the X window id of an event
        @return    the window, or nullptr for an id the app has no window for */
    LinuxWindow *Find(::Window id) const
    {
        const auto it = windows_.find(id);
        return it != windows_.end() ? it->second : nullptr;
    }

    // ---- The loop's registrations, which a child window's run loop makes and takes back -----------------------------

    /** Watch a descriptor: `ready` runs on the loop's thread whenever it is readable, hung up or in error.
        @param fd     the descriptor, which the caller keeps open until it unregisters
        @param ready  what runs
        @return       a token for Unregister */
    int Watch(int fd, std::function<void()> ready)
    {
        const int token = next_token_++;
        watches_[token] = {fd, std::move(ready)};
        return token;
    }

    /** Start a repeating timer, first due one interval from now.
        @param milliseconds  the interval, in milliseconds, at least 1
        @param tick          what runs each interval, on the loop's thread
        @return              a token for Unregister */
    int AddTimer(int milliseconds, std::function<void()> tick)
    {
        const int  token    = next_token_++;
        const auto interval = std::chrono::milliseconds(std::max(milliseconds, 1));
        timers_[token]      = {interval, Clock::now() + interval, std::move(tick)};
        return token;
    }

    /** Take back a descriptor's watch or a timer, whichever the token names; one already gone is ignored.
        @param token  a token Watch or AddTimer gave */
    void Unregister(int token)
    {
        watches_.erase(token);
        timers_.erase(token);
    }

    // ---- AppPlatform -----------------------------------------------------------------------------------------------

    gfx::PointI LargestContent(const WindowOptions &options) const override
    {
        // The window manager says a frame's extents (_NET_FRAME_EXTENTS) only once a window is mapped: a typical title
        // bar and border are allowed for, in design pixels at the desktop's scale.
        constexpr int     kAssumedFrameWidth  = 8;
        constexpr int     kAssumedFrameHeight = 40;
        const float       scale               = window::DisplayScaleOf(nullptr);
        const gfx::RectI  area                = WorkArea(display_);
        const gfx::PointI content{static_cast<int>(std::lround(static_cast<float>(options.width) * scale)),
                                  static_cast<int>(std::lround(static_cast<float>(options.height) * scale))};
        const gfx::PointI fitted = FittedContent(content,
                                                 {static_cast<int>(std::lround(kAssumedFrameWidth * scale)),
                                                  static_cast<int>(std::lround(kAssumedFrameHeight * scale))},
                                                 area);
        return {static_cast<int>(static_cast<float>(fitted.x) / scale),
                static_cast<int>(static_cast<float>(fitted.y) / scale)};
    }

    std::unique_ptr<AppWindow> OpenWindow(const WindowOptions &options) override
    {
        if (display_ == nullptr)
        {
            return nullptr;
        }
        auto window = std::make_unique<LinuxWindow>(*this, options);
        if (!window->valid())
        {
            return nullptr;
        }
        return window;
    }

    /** Hands the child window a run loop on this loop (ChildRunLoop), which the app gives it where a plug-in's host
        would.
        @return the run loop, whose registrations end with it */
    std::unique_ptr<window::RunLoop> ChildLoop() override;

    /** A timer for the app's own use, which lives as long as the platform.
        @param milliseconds  the interval, in milliseconds
        @param tick          what runs each interval, on the loop's thread */
    void StartTimer(int milliseconds, std::function<void()> tick) override
    {
        AddTimer(milliseconds, std::move(tick));
    }

    /** Queue `call` to run once the events the loop holds are handled; calls run in the order they were queued.
        @param call  what runs, on the loop's thread */
    void CallSoon(std::function<void()> call) override
    {
        calls_.push_back(std::move(call));
    }

    /** Nothing: Linux has no platform menu, and the window's bar holds the app's commands. */
    void SetMenu(const std::vector<view::MenuItem> &, std::function<void(int)>) override
    {
        // The window's bar holds the app's commands.
    }

    /** The bar set: shown by the drawn bars MakeMenuBarView made, which hear of it, and its shortcuts taken by every
        window's keys.
        @param bar     the menus
        @param chosen  told each chosen entry's id */
    void SetMenuBar(const MenuBar &bar, std::function<void(int id)> chosen) override
    {
        menu_->Set(bar, std::move(chosen));
    }

    /** A drawn bar on the platform's menus: X has no menu bar of its own.
        @param style  the bar's look
        @return       the bar */
    std::unique_ptr<MenuBarView> MakeMenuBarView(const MenuBarStyle &style) override
    {
        return std::make_unique<MenuBarView>(menu_, style);
    }

    /** A key a window had, or its child window's views did not take, offered to the bar's shortcuts.
        @param pressed  the key and its modifiers
        @return         true when it chose an entry */
    bool PressShortcut(const Shortcut &pressed)
    {
        return !pressed.empty() && menu_->PressShortcut(pressed);
    }

    /** The loop: drain the connection, run the queued calls and the due timers, then poll the connection, the signal
        pipe and the watched descriptors until the next timer is due. A termination signal runs the quit handler,
        or ends the loop where there is none.
        @return 0 */
    int Run() override
    {
        running_ = display_ != nullptr;
        while (running_)
        {
            Drain();
            RunCalls();
            RunDueTimers();
            if (!running_)
            {
                break;
            }
            // Every descriptor the loop answers for: the app's connection, the signal pipe, the posts' pipe, the child
            // windows'.
            constexpr std::size_t kWatched = 3; // the descriptors before the child windows'
            std::vector<pollfd>   fds;
            std::vector<int>      tokens;
            fds.push_back({ConnectionNumber(display_), POLLIN, 0});
            fds.push_back({g_signal_pipe[0], POLLIN, 0});
            fds.push_back({post_pipe_[0], POLLIN, 0});
            for (const auto &[token, watch] : watches_)
            {
                fds.push_back({watch.fd, POLLIN, 0});
                tokens.push_back(token);
            }
            XFlush(display_);
            poll(fds.data(), static_cast<nfds_t>(fds.size()), Timeout());
            if ((fds[2].revents & POLLIN) != 0)
            {
                char bytes[64];
                while (read(post_pipe_[0], bytes, sizeof(bytes)) > 0)
                {
                }
                posts_->RunPending();
            }
            if ((fds[1].revents & POLLIN) != 0 && g_signal_pipe[0] >= 0)
            {
                char      bytes[16];
                const int read_count = static_cast<int>(read(g_signal_pipe[0], bytes, sizeof(bytes)));
                (void)read_count;
                if (quit_handler_)
                {
                    quit_handler_(); // the app's ending, which ends the loop
                }
                else
                {
                    running_ = false;
                }
            }
            for (std::size_t i = kWatched; i < fds.size(); ++i)
            {
                if ((fds[i].revents & (POLLIN | POLLHUP | POLLERR)) != 0)
                {
                    const auto it = watches_.find(tokens[i - kWatched]); // a callback before may have taken it back
                    if (it != watches_.end())
                    {
                        const auto ready = it->second.ready;
                        ready();
                    }
                }
            }
        }
        posts_->Close(); // the app has quit: what other threads post from here on never runs
        return 0;
    }

    std::shared_ptr<core::PostQueue> Posts() override
    {
        return posts_;
    }

    /** `text` on the CLIPBOARD selection, held by an owner on the platform's connection and answered from the loop
        until another copy takes it.
        @param text  the text, UTF-8
        @return      true when the selection was taken; false with no display */
    bool SetClipboardText(const std::string &text) override
    {
        if (display_ == nullptr)
        {
            return false;
        }
        if (!clipboard_)
        {
            clipboard_ = std::make_unique<window::x11::ClipboardOwner>(display_);
        }
        return clipboard_->Take(text);
    }

    /** The CLIPBOARD selection's text: this process's own copy where it holds the selection, else asked of its owner
        (window::x11::ReadClipboardText).
        @return  the text, UTF-8, or empty */
    std::string ClipboardText() override
    {
        return display_ != nullptr ? window::x11::ReadClipboardText() : std::string();
    }

    /** End the loop after the event being handled. */
    void Quit() override
    {
        running_ = false;
    }

    /** The app's ending, run on a termination signal in place of ending the loop at once.
        @param handler  what runs; it ends by calling Quit */
    void SetQuitHandler(std::function<void()> handler) override
    {
        quit_handler_ = std::move(handler);
    }

  private:
    /** A descriptor a child window asked the loop to watch. */
    struct WatchEntry
    {
        int                   fd{-1}; ///< the descriptor, polled for input.
        std::function<void()> ready;  ///< what runs when it is readable, hung up or in error.
    };

    /** A timer a child window or the app asked for. */
    struct TimerEntry
    {
        Clock::duration       interval{}; ///< the time between ticks.
        Clock::time_point     due{};      ///< when the next tick is due.
        std::function<void()> tick;       ///< what runs each tick.
    };

    /** Every event the app's connection holds, each to its window. */
    void Drain()
    {
        while (XPending(display_) > 0)
        {
            XEvent event{};
            XNextEvent(display_, &event);
            if (clipboard_ && clipboard_->Handle(event))
            {
                continue; // a paste asking the copy the platform holds, or word that another copy took it
            }
            if (LinuxWindow *window = Find(event.xany.window))
            {
                window->Event(event);
            }
        }
    }

    /** Run the queued calls, in order, including any a call queues. */
    void RunCalls()
    {
        while (!calls_.empty())
        {
            auto call = std::move(calls_.front());
            calls_.pop_front();
            call();
        }
    }

    /** Run the timers that are due, once each, and set each one's next tick a whole interval on - or from now, where
        the loop fell behind. A timer a callback took back is skipped. */
    void RunDueTimers()
    {
        const auto       now = Clock::now();
        std::vector<int> due;
        for (const auto &[token, timer] : timers_)
        {
            if (now >= timer.due)
            {
                due.push_back(token);
            }
        }
        for (const int token : due)
        {
            const auto it = timers_.find(token);
            if (it == timers_.end())
            {
                continue; // taken back by a callback before it
            }
            // The next tick a whole interval on, or from now where the loop fell behind.
            it->second.due  = std::max(it->second.due + it->second.interval, now);
            const auto tick = it->second.tick;
            tick();
        }
    }

    /** The milliseconds until the next timer is due, 0 with calls waiting, -1 with neither.
        @return the poll timeout, in milliseconds, at most 1000 */
    int Timeout() const
    {
        if (!calls_.empty())
        {
            return 0;
        }
        if (timers_.empty())
        {
            return -1;
        }
        auto next = timers_.begin()->second.due;
        for (const auto &[token, timer] : timers_)
        {
            next = std::min(next, timer.due);
        }
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(next - Clock::now()).count();
        return static_cast<int>(std::clamp<long long>(wait, 0, 1000));
    }

    std::string                       name_;             ///< the app's name, the windows' class.
    std::vector<unsigned long>        icon_;             ///< the window icon as _NET_WM_ICON holds it; empty for none.
    Display                          *display_{nullptr}; ///< the app's connection; nullptr with no display.
    std::map<::Window, LinuxWindow *> windows_;          ///< the open windows by X id; not owned.
    std::map<int, WatchEntry>         watches_;          ///< the watched descriptors by token.
    std::map<int, TimerEntry>         timers_;           ///< the timers by token.
    int                               next_token_{1};    ///< the token the next registration takes.
    std::deque<std::function<void()>> calls_;            ///< the calls CallSoon queued, in order.
    std::function<void()>             quit_handler_;   ///< the app's ending, run on a termination signal; may be empty.
    bool                              running_{false}; ///< true while Run loops.
    /** The calls other threads post (Posts): woken through post_pipe_, closed when Run returns. */
    std::shared_ptr<core::PostQueue> posts_{std::make_shared<core::PostQueue>()};
    int                              post_pipe_[2]{-1, -1}; ///< the posts' wake: read by the loop, written by a post.
    /** The CLIPBOARD selection's owner on the platform's connection, made by the first copy; null until then. */
    std::unique_ptr<window::x11::ClipboardOwner> clipboard_;
    /** The menu bar, its choosing and its shortcuts, shared with the drawn bars. */
    std::shared_ptr<MenuBarController> menu_{std::make_shared<MenuBarController>()};
};

/** A child window's run loop: its descriptors and timers on the app's loop, taken back when it goes. */
class ChildRunLoop final : public window::RunLoop
{
  public:
    /** A run loop on `platform`'s loop.
        @param platform  the platform whose loop runs the registrations; outlives this object */
    explicit ChildRunLoop(LinuxPlatform &platform) : platform_(platform)
    {
    }

    /** Take back every descriptor and timer registered through this loop. */
    ~ChildRunLoop() override
    {
        for (const int token : tokens_)
        {
            platform_.Unregister(token);
        }
    }

    /** Watch `fd` on the app's loop until this loop goes.
        @param fd     the descriptor, which the caller keeps open until this loop goes
        @param ready  what runs, on the loop's thread, when the descriptor is readable, hung up or in error
        @return       true: always taken */
    bool WatchFd(int fd, std::function<void()> ready) override
    {
        tokens_.push_back(platform_.Watch(fd, std::move(ready)));
        return true;
    }

    /** Start a timer on the app's loop until this loop goes.
        @param milliseconds  the interval, in milliseconds
        @param tick          what runs each interval, on the loop's thread
        @return              true: always taken */
    bool StartTimer(int milliseconds, std::function<void()> tick) override
    {
        tokens_.push_back(platform_.AddTimer(milliseconds, std::move(tick)));
        return true;
    }

    /** A copy held by the platform's connection, which outlives every window, rather than the window's.
        @param text  the text, UTF-8
        @return      true when the platform took the selection */
    bool SetClipboardText(const std::string &text) override
    {
        return platform_.SetClipboardText(text);
    }

  private:
    LinuxPlatform   &platform_; ///< the platform whose loop holds the registrations.
    std::vector<int> tokens_;   ///< the tokens of what this loop registered, for the destructor.
};

std::unique_ptr<window::RunLoop> LinuxPlatform::ChildLoop()
{
    return std::make_unique<ChildRunLoop>(*this);
}

LinuxWindow::LinuxWindow(LinuxPlatform &platform, const WindowOptions &options)
    : platform_(platform), display_(platform.display()), closing_(options.closing), constrain_(options.constrain),
      aspect_(options.aspect), fixed_height_(options.fixed_height), resized_(options.resized), dialog_(options.dialog)
{
    const int      screen = DefaultScreen(display_);
    const ::Window root   = RootWindow(display_, screen);
    scale_                = window::DisplayScaleOf(nullptr);
    pixel_width_          = std::max(1, static_cast<int>(std::lround(static_cast<float>(options.width) * scale_)));
    pixel_height_         = std::max(1, static_cast<int>(std::lround(static_cast<float>(options.height) * scale_)));

    // Where it stood, wholly inside the desktop's work area; else centred over its owner and inside the work area;
    // else centred on the screen.
    const gfx::RectI area = WorkArea(display_);
    const gfx::RectI areas[1]{area};
    const auto      *owner  = dynamic_cast<const LinuxWindow *>(options.owner);
    const bool       placed = options.position.has_value();
    gfx::RectI       frame{(DisplayWidth(display_, screen) - pixel_width_) / 2,
                           (DisplayHeight(display_, screen) - pixel_height_) / 2, pixel_width_, pixel_height_};
    if (placed)
    {
        frame = PlaceWithin(frame.WithPosition(options.position->first, options.position->second), areas);
    }
    else if (owner != nullptr)
    {
        const auto [ox, oy] = owner->Position();
        frame = CentredOver({ox, oy, owner->pixel_width_, owner->pixel_height_}, {pixel_width_, pixel_height_}, areas);
    }

    // A visual of the raster's layout - TrueColor, B G R in memory, 24 bits or 32 with alpha over them - which the
    // child window inherits and draws in: the server's default is none at a depth of 30 or 16.
    XVisualInfo visual{};
    const bool  own_visual = (XMatchVisualInfo(display_, screen, 24, TrueColor, &visual) != 0 && LaysOutBgra(visual)) ||
                             (XMatchVisualInfo(display_, screen, 32, TrueColor, &visual) != 0 && LaysOutBgra(visual));
    XSetWindowAttributes attributes{};
    unsigned long        mask = CWEventMask | CWBackPixel | CWBorderPixel;
    attributes.event_mask     = StructureNotifyMask | KeyPressMask | FocusChangeMask;
    // Black, opaque: in a TrueColor visual its pixel is the alpha alone.
    attributes.background_pixel = own_visual ? (visual.depth == 32 ? 0xff000000ul : 0ul) : BlackPixel(display_, screen);
    attributes.border_pixel     = 0;
    if (own_visual)
    {
        colormap_           = XCreateColormap(display_, root, visual.visual, AllocNone);
        attributes.colormap = colormap_;
        mask |= CWColormap;
    }
    window_ = XCreateWindow(display_, root, frame.x(), frame.y(), static_cast<unsigned>(pixel_width_),
                            static_cast<unsigned>(pixel_height_), 0,
                            own_visual ? visual.depth : static_cast<int>(CopyFromParent), InputOutput,
                            own_visual ? visual.visual : nullptr, mask, &attributes);
    if (window_ == 0)
    {
        return;
    }

    // Its name, both ways a window manager reads one; its class; its fixed size, placed by its client area.
    SetTitle(options.title);
    std::string class_name = platform.name();
    std::string instance   = class_name;
    XClassHint  class_hint{instance.data(), class_name.data()};
    XSetClassHint(display_, window_, &class_hint);
    SetSizeHints(frame.x(), frame.y(), placed);
    Atom close_atom = XInternAtom(display_, "WM_DELETE_WINDOW", False);
    XSetWMProtocols(display_, window_, &close_atom, 1);
    const std::vector<unsigned long> &icon = platform.icon();
    if (!icon.empty())
    {
        XChangeProperty(display_, window_, XInternAtom(display_, "_NET_WM_ICON", False), XA_CARDINAL, 32,
                        PropModeReplace, reinterpret_cast<const unsigned char *>(icon.data()),
                        static_cast<int>(icon.size()));
    }
    if (options.dialog)
    {
        if (owner != nullptr)
        {
            XSetTransientForHint(display_, window_, owner->window_);
        }
        Atom dialog_type = XInternAtom(display_, "_NET_WM_WINDOW_TYPE_DIALOG", False);
        XChangeProperty(display_, window_, XInternAtom(display_, "_NET_WM_WINDOW_TYPE", False), XA_ATOM, 32,
                        PropModeReplace, reinterpret_cast<const unsigned char *>(&dialog_type), 1);
    }
    platform_.Add(*this);
    XMapWindow(display_, window_);
    XFlush(display_);
}

LinuxWindow::~LinuxWindow()
{
    if (window_ != 0)
    {
        platform_.Remove(*this);
        XDestroyWindow(display_, window_);
    }
    if (colormap_ != 0)
    {
        XFreeColormap(display_, colormap_);
    }
    XFlush(display_);
}

void LinuxWindow::SetSizeHints(int x, int y, bool placed)
{
    if (XSizeHints *size = XAllocSizeHints())
    {
        size->flags       = PMinSize | PMaxSize | PWinGravity | PPosition | (placed ? USPosition : 0);
        size->x           = x;
        size->y           = y;
        size->min_width   = pixel_width_;
        size->max_width   = pixel_width_;
        size->min_height  = pixel_height_;
        size->max_height  = pixel_height_;
        size->win_gravity = StaticGravity;
        if (constrain_)
        {
            // Resizable: the least and the most the owner's constraint allows, and its aspect over what scales - the
            // fixed height a base size the window manager takes off before it holds the aspect.
            const gfx::PointF least = constrain_({0.0f, static_cast<float>(fixed_height_)});
            const gfx::PointF most  = constrain_({1.0e5f, 1.0e5f});
            size->min_width         = std::max(1, static_cast<int>(std::ceil(least.x * scale_)));
            size->min_height        = std::max(1, static_cast<int>(std::ceil(least.y * scale_)));
            size->max_width         = std::max(size->min_width, static_cast<int>(std::floor(most.x * scale_)));
            size->max_height        = std::max(size->min_height, static_cast<int>(std::floor(most.y * scale_)));
            size->flags |= PBaseSize;
            size->base_width  = 0;
            size->base_height = static_cast<int>(std::lround(static_cast<float>(fixed_height_) * scale_));
            if (const gfx::PointI aspect = aspect_ ? aspect_() : gfx::PointI{0, 0}; aspect.x > 0 && aspect.y > 0)
            {
                size->flags |= PAspect;
                size->min_aspect.x = size->max_aspect.x = aspect.x;
                size->min_aspect.y = size->max_aspect.y = aspect.y;
            }
        }
        XSetWMNormalHints(display_, window_, size);
        XFree(size);
    }
}

std::pair<int, int> LinuxWindow::Position() const
{
    int      x     = 0;
    int      y     = 0;
    ::Window child = 0;
    XTranslateCoordinates(display_, window_, DefaultRootWindow(display_), 0, 0, &x, &y, &child);
    return {x, y};
}

void LinuxWindow::BringToFront()
{
    // Mapped again where the window manager had it iconic, and activated the way a pager asks (_NET_ACTIVE_WINDOW),
    // which restores a minimised or shaded window and fetches one from another desktop; the server's focus asked for
    // directly only for a window in view, since asking for it for one out of view is an error.
    XMapRaised(display_, window_);
    XEvent activate{};
    activate.xclient.type         = ClientMessage;
    activate.xclient.window       = window_;
    activate.xclient.message_type = XInternAtom(display_, "_NET_ACTIVE_WINDOW", False);
    activate.xclient.format       = 32;
    activate.xclient.data.l[0]    = 1; // asked by an application
    activate.xclient.data.l[1]    = CurrentTime;
    XSendEvent(display_, DefaultRootWindow(display_), False, SubstructureRedirectMask | SubstructureNotifyMask,
               &activate);
    XWindowAttributes attributes{};
    if (XGetWindowAttributes(display_, window_, &attributes) != 0 && attributes.map_state == IsViewable)
    {
        XSetInputFocus(display_, window_, RevertToParent, CurrentTime);
    }
    XFlush(display_);
}

void LinuxWindow::SetContentSize(int width, int height)
{
    // The size hints first, which hold the window to its size, then the size; the gravity static, so the client area's
    // top left stays where it stands.
    pixel_width_      = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale_)));
    pixel_height_     = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale_)));
    const auto [x, y] = Position();
    SetSizeHints(x, y, false);
    XResizeWindow(display_, window_, static_cast<unsigned>(pixel_width_), static_cast<unsigned>(pixel_height_));
    XFlush(display_);
}

void LinuxWindow::SetContentSizeOnScreen(int width, int height)
{
    // The frame at the new size, its client area's top left kept, moved onto the desktop's work area in from any edge
    // it would cross; the size hints first, then the client area moved and sized - the gravity static, so the place is
    // the client area's. A size the app asks for draws no ConfigureNotify the owner hears (Event), so it is told here.
    pixel_width_               = std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale_)));
    pixel_height_              = std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale_)));
    const auto [x, y]          = Position();
    const FrameExtents extents = ExtentsOf(display_, window_, scale_);
    const gfx::RectI frame = MovedOnto({x - extents.left, y - extents.top, pixel_width_ + extents.left + extents.right,
                                        pixel_height_ + extents.top + extents.bottom},
                                       WorkArea(display_));
    const int        client_x = frame.x() + extents.left;
    const int        client_y = frame.y() + extents.top;
    SetSizeHints(client_x, client_y, true);
    XMoveResizeWindow(display_, window_, client_x, client_y, static_cast<unsigned>(pixel_width_),
                      static_cast<unsigned>(pixel_height_));
    XFlush(display_);
    if (resized_)
    {
        const auto f = resized_;
        f(width, height);
    }
}

gfx::PointI LinuxWindow::LargestContent() const
{
    // The desktop's work area less the window manager's frame round the window, in design units, rounded down.
    const gfx::RectI   area    = WorkArea(display_);
    const FrameExtents extents = ExtentsOf(display_, window_, scale_);
    return {std::max(1, static_cast<int>(
                            std::floor(static_cast<float>(area.width() - extents.left - extents.right) / scale_))),
            std::max(1, static_cast<int>(
                            std::floor(static_cast<float>(area.height() - extents.top - extents.bottom) / scale_)))};
}

void LinuxWindow::SetTitle(const std::string &title)
{
    // Both ways a window manager reads a name: WM_NAME, which an old one shows, and _NET_WM_NAME in UTF-8.
    XStoreName(display_, window_, title.c_str());
    const Atom utf8 = XInternAtom(display_, "UTF8_STRING", False);
    XChangeProperty(display_, window_, XInternAtom(display_, "_NET_WM_NAME", False), utf8, 8, PropModeReplace,
                    reinterpret_cast<const unsigned char *>(title.data()), static_cast<int>(title.size()));
    XFlush(display_);
}

std::string LinuxWindow::Title() const
{
    // _NET_WM_NAME, the name in UTF-8 the window was given, as the server holds it.
    Atom           type   = None;
    int            format = 0;
    unsigned long  count  = 0;
    unsigned long  after  = 0;
    unsigned char *data   = nullptr;
    std::string    title;
    if (XGetWindowProperty(display_, window_, XInternAtom(display_, "_NET_WM_NAME", False), 0, 1 << 16, False,
                           XInternAtom(display_, "UTF8_STRING", False), &type, &format, &count, &after,
                           &data) == Success &&
        data != nullptr && format == 8)
    {
        title.assign(reinterpret_cast<const char *>(data), count);
    }
    if (data != nullptr)
    {
        XFree(data);
    }
    return title;
}

void LinuxWindow::Event(const XEvent &event)
{
    if (event.type == ClientMessage &&
        static_cast<Atom>(event.xclient.data.l[0]) == XInternAtom(display_, "WM_DELETE_WINDOW", False))
    {
        Closing();
    }
    else if (event.type == KeyPress)
    {
        // The window's own keys, and those its child window's views did not take, which the child sends here: Escape
        // closing a dialog, else the menu bar's shortcuts.
        XKeyEvent key = event.xkey;
        if (dialog_ && XLookupKeysym(&key, 0) == XK_Escape)
        {
            Closing();
        }
        else
        {
            platform_.PressShortcut(StrokeOf(key));
        }
    }
    else if (event.type == ConfigureNotify &&
             (event.xconfigure.width != pixel_width_ || event.xconfigure.height != pixel_height_))
    {
        // The window manager resized the window - the person dragged its frame - to a size the app did not ask for.
        pixel_width_  = event.xconfigure.width;
        pixel_height_ = event.xconfigure.height;
        if (resized_)
        {
            const auto f = resized_;
            f(std::max(1, static_cast<int>(std::lround(static_cast<float>(pixel_width_) / scale_))),
              std::max(1, static_cast<int>(std::lround(static_cast<float>(pixel_height_) / scale_))));
        }
    }
}

std::unique_ptr<AppPlatform> AppPlatform::Create(const std::string &name, std::span<const std::uint8_t> icon_png)
{
    return std::make_unique<LinuxPlatform>(name, icon_png);
}
} // namespace reckless::app
