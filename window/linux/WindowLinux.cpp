// The child window on Linux: an X11 window inside the host's, drawing a view tree through Skia into a raster the size
// of the window and putting it on screen with XPutImage, on the host's event loop - the display connection's
// descriptor watched for events and a timer for the frame callback, the repaint and the calls deferred to it - since a
// plug-in runs no loop of its own in the host's process. The child window has a connection of its own to the display,
// which its popups share, so every event on it is one of theirs, sent to the window it names. A popup is the same
// surface in an override-redirect window over everything, under a pointer grab that shows every press outside the
// connection's windows to the popups. The file dialog is the desktop's zenity, or kdialog, read without blocking;
// files dragged from the desktop arrive by XDND.
// Screen points are the root window's, opaque to the views.
#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

#include "SkiaCanvas.h"
#include "UriList.h"
#include "Window.h"

// Xlib last: it defines macros (None, Bool, Status, Success, Always) that would reach into the headers above.
#include <X11/Xlib.h>
#include <X11/Xresource.h>
#include <X11/Xutil.h>
#include <X11/cursorfont.h>
#include <X11/keysym.h>

#include "X11Clipboard.h"

namespace reckless::window
{
namespace
{
constexpr int   kFrameMilliseconds = 16;    ///< the frame callback's and the repaint's tick, about 60 Hz
constexpr Time  kDoubleClickMs     = 400;   ///< a second press this soon after the first, near it, is a double click
constexpr float kDoubleClickSlop   = 4.0f;  ///< how far a second press may land from the first, in pixels
constexpr float kNotchPixels       = 20.0f; ///< one click of a wheel, in the views' pixels

/** Xlib's visual for the raster's layout: 32 bits a pixel in memory as B, G, R, A, which a TrueColor visual of 24 or
    32 bits reads with these masks on a little-endian machine.
    @param visual  the visual to test; may be null
    @param depth   the window's depth, in bits
    @return        true when the raster can be put on a window of that visual as it lies */
bool TakesBgra(const Visual *visual, int depth) noexcept
{
    return visual != nullptr && visual->c_class == TrueColor && visual->red_mask == 0xff0000 &&
           visual->green_mask == 0x00ff00 && visual->blue_mask == 0x0000ff && (depth == 24 || depth == 32);
}

/** The modifier keys held in an X event's state.
    @param state  the event's `state` mask
    @return       Shift, Control and Alt (Mod1) as the views read them */
view::Modifiers ModifiersOf(unsigned state) noexcept
{
    return {(state & ShiftMask) != 0, (state & ControlMask) != 0, (state & Mod1Mask) != 0};
}

/** The cursor font's shape for a view's cursor.
    @param cursor  the view's cursor
    @return        an XC_ shape from X11/cursorfont.h; the left pointer for the arrow */
unsigned CursorShapeFor(view::Cursor cursor)
{
    switch (cursor)
    {
    case view::Cursor::kPointingHand:
        return XC_hand2;
    case view::Cursor::kIBeam:
        return XC_xterm;
    case view::Cursor::kGrab:
    case view::Cursor::kGrabbing:
        return XC_fleur;
    case view::Cursor::kUpDown:
        return XC_sb_v_double_arrow;
    case view::Cursor::kLeftRight:
        return XC_sb_h_double_arrow;
    case view::Cursor::kCrosshair:
        return XC_crosshair;
    case view::Cursor::kArrow:
        break;
    }
    return XC_left_ptr;
}

/** A key event as the views' key: the editing and moving keys by their key symbols, the rest as the character it
    types (Latin-1, as XLookupString gives it with no input method).
    @param event  the key press, which XLookupString reads
    @return       the key, with its modifiers; kCharacter with no character when the key is neither of those */
view::KeyEvent KeyOf(XKeyEvent &event)
{
    char           text[8]{};
    KeySym         symbol = 0;
    const int      length = XLookupString(&event, text, sizeof(text), &symbol, nullptr);
    view::KeyEvent key;
    key.modifiers = ModifiersOf(event.state);
    switch (symbol)
    {
    case XK_Escape:
        key.key = view::Key::kEscape;
        return key;
    case XK_Return:
    case XK_KP_Enter:
        key.key = view::Key::kReturn;
        return key;
    case XK_Tab:
        key.key = view::Key::kTab;
        return key;
    case XK_BackSpace:
        key.key = view::Key::kBackspace;
        return key;
    case XK_Delete:
        key.key = view::Key::kDelete;
        return key;
    case XK_Left:
        key.key = view::Key::kLeft;
        return key;
    case XK_Right:
        key.key = view::Key::kRight;
        return key;
    case XK_Up:
        key.key = view::Key::kUp;
        return key;
    case XK_Down:
        key.key = view::Key::kDown;
        return key;
    case XK_Home:
        key.key = view::Key::kHome;
        return key;
    case XK_End:
        key.key = view::Key::kEnd;
        return key;
    case XK_Page_Up:
        key.key = view::Key::kPageUp;
        return key;
    case XK_Page_Down:
        key.key = view::Key::kPageDown;
        return key;
    default:
        break;
    }
    // Ctrl with a letter as the letter with the primary modifier, as a Mac's Command-letter reaches the views:
    // XLookupString gives a control byte for it, which no view reads. Not with Alt, as on Windows.
    if (const KeySym base = XLookupKeysym(&event, 0);
        key.modifiers.command && !key.modifiers.alt && base >= XK_a && base <= XK_z)
    {
        key.key       = view::Key::kCharacter;
        key.character = static_cast<char32_t>(U'a' + (base - XK_a));
        return key;
    }
    if (length > 0 && static_cast<unsigned char>(text[0]) >= 32)
    {
        key.key       = view::Key::kCharacter;
        key.character = static_cast<char32_t>(static_cast<unsigned char>(text[0]));
    }
    return key;
}

/** Whether `program` is on the PATH.
    @param program  an executable's name, with no directory
    @return         true when a directory on the PATH holds an executable of that name */
bool OnPath(const std::string &program)
{
    const char *path = std::getenv("PATH");
    if (path == nullptr)
    {
        return false;
    }
    std::string all(path);
    std::size_t start = 0;
    while (start <= all.size())
    {
        const std::size_t end = all.find(':', start);
        const std::string dir = all.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!dir.empty() && access((dir + "/" + program).c_str(), X_OK) == 0)
        {
            return true;
        }
        if (end == std::string::npos)
        {
            break;
        }
        start = end + 1;
    }
    return false;
}
} // namespace

class LinuxSurface;
class LinuxPopup;

/** A child window's connection to the display, shared by its popups: the events read and sent to the window they name,
    the presses shown to the open popups, the calls deferred to the next tick, the cursors, and a file dialog in
    flight. The deferred calls and the dialog's answer are the connection's, run on its loop's timer, so neither
    outlives the window: as it goes the loop stops, the calls are dropped and the dialog is killed unanswered. */
class LinuxConnection
{
  public:
    /** A connection on `display` and the host's loop.
        @param display  the display connection, which the connection closes at its destruction
        @param loop     the host's event loop, owned; its callbacks stop when the connection goes */
    LinuxConnection(Display *display, std::unique_ptr<RunLoop> loop) : display_(display), loop_(std::move(loop))
    {
    }

    /** The host's loop released so no callback follows, a file dialog still open stopped and reaped, the clipboard let
        go where this connection held it, then the cursors, colormaps and display connection freed. */
    ~LinuxConnection()
    {
        loop_.reset(); // no callback after this
        clipboard_.reset();
        if (dialog_pid_ > 0)
        {
            kill(dialog_pid_, SIGTERM);
            waitpid(dialog_pid_, nullptr, 0);
            close(dialog_fd_);
        }
        for (const auto &[shape, cursor] : cursors_)
        {
            XFreeCursor(display_, cursor);
        }
        for (const auto &[visual, colormap] : colormaps_)
        {
            XFreeColormap(display_, colormap);
        }
        XCloseDisplay(display_);
    }

    /** Watches the display's descriptor and starts the frame timer.
        @param tick  run on every frame tick, before the deferred calls; the window's frame callback
        @return      true when the host's loop took both the watch and the timer */
    bool Start(std::function<void()> tick)
    {
        tick_ = std::move(tick);
        return loop_->WatchFd(ConnectionNumber(display_), [this] { Pump(); }) &&
               loop_->StartTimer(kFrameMilliseconds, [this] { Tick(); });
    }

    /** The display connection.
        @return  the display, owned by the connection */
    Display *display() const noexcept
    {
        return display_;
    }

    /** A surface's window listed, so the events that name it reach the surface.
        @param window   the surface's X window
        @param surface  the surface, which stays listed until Remove */
    void Add(::Window window, LinuxSurface *surface)
    {
        surfaces_[window] = surface;
    }

    /** A window taken off the list, and any top-level it was the proxy of.
        @param window  the surface's X window, as it was given to Add */
    void Remove(::Window window)
    {
        const auto it = surfaces_.find(window);
        if (it != surfaces_.end())
        {
            std::erase_if(proxied_, [surface = it->second](const auto &entry) { return entry.second == surface; });
            surfaces_.erase(it);
        }
    }

    /** `surface` takes the drops dragged over `top_level`, a window of another's that names it its XDND proxy: the
        messages a drag source addresses to the top-level - sent to the proxy, the top-level named in them - handed to
        it, as the messages addressed to it are.
        @param top_level  the top-level window the drag source addresses
        @param surface    the surface that takes the messages, listed already with Add */
    void ProxyFor(::Window top_level, LinuxSurface *surface)
    {
        proxied_[top_level] = surface;
    }

    /** A popup listed, and the pointer grabbed for it where nothing holds the grab, so a press anywhere reaches the
        popups.
        @param popup   the popup, which stays listed until RemovePopup
        @param window  the popup's X window */
    void AddPopup(LinuxPopup *popup, ::Window window);

    /** A popup taken off the list; the pointer released when no popup is left.
        @param popup  the popup, as it was given to AddPopup */
    void RemovePopup(LinuxPopup *popup);

    /** A call deferred to the next tick, on the loop's thread; dropped if the connection goes first.
        @param call  what to run */
    void CallSoon(std::function<void()> call)
    {
        calls_.push_back(std::move(call));
    }

    /** The X cursor for a view's cursor, made the first time it is asked for and freed with the connection.
        @param cursor  the view's cursor
        @return        the cursor, owned by the connection */
    ::Cursor CursorFor(view::Cursor cursor)
    {
        const unsigned shape = CursorShapeFor(cursor);
        auto           it    = cursors_.find(shape);
        if (it == cursors_.end())
        {
            it = cursors_.emplace(shape, XCreateFontCursor(display_, shape)).first;
        }
        return it->second;
    }

    /** The colormap a popup's window of `visual` is made with: one a visual, made the first time a popup asks and
        shared by every popup after - a window's colormap outlives the window, so one made a popup would stand in the
        server until the connection closed, a menu's worth each time one opened.
        @param visual  the visual the popup's window is made with
        @return        the colormap, owned by the connection */
    Colormap ColormapFor(Visual *visual)
    {
        const VisualID id = XVisualIDFromVisual(visual);
        auto           it = colormaps_.find(id);
        if (it == colormaps_.end())
        {
            it =
                colormaps_.emplace(id, XCreateColormap(display_, DefaultRootWindow(display_), visual, AllocNone)).first;
        }
        return it->second;
    }

    /** zenity's or kdialog's answer to `request`, given to `picked` on a later tick; a dialog already open is
        refused (the path empty).
        @param request  the dialog's title, folder, file patterns and whether it saves
        @param picked   run on a later tick with the path chosen, UTF-8; empty when the person cancels, a dialog is
                        already open or the desktop has neither dialog program */
    void ChooseFile(const view::FileRequest &request, std::function<void(const std::string &)> picked);

    /** `text` on the CLIPBOARD selection, for a copy: held by the loop where it keeps the clipboard for the windows on
        it (an app's, RunLoop::SetClipboardText), else by an owner on this connection, answering pastes while the
        connection lives (a plug-in's window in a host).
        @param text  the text, UTF-8
        @return      true when the selection was taken */
    bool SetClipboardText(const std::string &text)
    {
        if (loop_->SetClipboardText(text))
        {
            return true;
        }
        if (!clipboard_)
        {
            clipboard_ = std::make_unique<x11::ClipboardOwner>(display_);
        }
        return clipboard_->Take(text);
    }

    /** Every event the connection holds, each to its window, then the windows' repaints. */
    void Pump();

  private:
    /** One frame tick: the window's frame callback, the calls deferred to it, the dialog's answer, the events. */
    void Tick();

    /** Reads what the file dialog has written; once it has closed, its path is given to the callback. */
    void PollDialog();

    Display                           *display_;  ///< the display connection, closed at destruction
    std::unique_ptr<RunLoop>           loop_;     ///< the host's loop, reset first so no callback follows
    std::function<void()>              tick_;     ///< the window's frame callback, run on every tick
    std::map<::Window, LinuxSurface *> surfaces_; ///< every surface by its window, the child window and its popups
    std::map<::Window, LinuxSurface *> proxied_;  ///< top-level windows whose drops a surface takes (ProxyFor)
    std::vector<LinuxPopup *>          popups_;   ///< the open popups, oldest first
    ::Window                           grab_window_{0};      ///< the window holding the pointer grab; 0 for none
    std::deque<std::function<void()>>  calls_;               ///< the calls deferred to the next tick
    std::map<unsigned, ::Cursor>       cursors_;             ///< the cursors made, by shape
    std::map<VisualID, Colormap>       colormaps_;           ///< the popups' (ColormapFor)
    pid_t                              dialog_pid_{-1};      ///< the file dialog's process; -1 for none open
    int                                dialog_fd_{-1};       ///< the read end of the dialog's output; -1 for none
    std::string                        dialog_output_;       ///< what the dialog has written so far
    std::function<void(const std::string &)> dialog_picked_; ///< given the path when the dialog closes
    /** The CLIPBOARD selection's owner on this connection, made by the first copy the loop does not keep; null until
        then. */
    std::unique_ptr<x11::ClipboardOwner> clipboard_;
};

/** A view tree drawn in an X window on a shared connection: the raster, the drawing, the mouse and keys to a router,
    and the host the tree lives in. The child window and every popup are one. */
class LinuxSurface : public view::Host
{
  public:
    /** A surface on an X window, drawing `root` and listing itself with the connection.
        @param connection  the connection the window is on, which outlives the surface
        @param window      the X window to draw in; destroyed with the surface
        @param visual      the window's visual, a TrueColor one that TakesBgra
        @param depth       the window's depth, in bits
        @param root        the root of the view tree to draw, which outlives the surface
        @param scale       window pixels per design pixel */
    LinuxSurface(LinuxConnection &connection, ::Window window, Visual *visual, int depth, view::View &root, float scale)
        : connection_(connection), display_(connection.display()), window_(window), visual_(visual), depth_(depth),
          root_(root), router_(root, this), scale_(scale)
    {
        gc_ = XCreateGC(display_, window_, 0, nullptr);
        connection_.Add(window_, this);
        root_.SetHost(this);
        ResizeRaster();
        dirty_ = {pixel_width_, pixel_height_};
    }

    /** The surface released (Release): its popups closed, the tree's host dropped, the X window and its drawing gone.
     */
    ~LinuxSurface() override
    {
        Release();
    }

    /** Every popup opened over the surface closed, and theirs, each `closed_by_window` gathered into `heard` for
        whoever let the surface go to run once everything is closed.
        @param heard  the callbacks to run afterwards, appended to */
    void EndPopups(std::vector<std::function<void()>> &heard);

    /** `popup`, going on its own, off the list of those opened over the surface.
        @param popup  the popup, which is no longer listed after this */
    void Forget(const LinuxPopup *popup)
    {
        popups_.erase(std::remove(popups_.begin(), popups_.end(), popup), popups_.end());
    }

    // ---- view::Host -----------------------------------------------------------------------------------------------

    /** The part of the root to paint again, kept as pixels for the next frame.
        @param area  the part, in the root's coordinates */
    void Invalidate(gfx::RectI area) override
    {
        dirty_ = dirty_.Union(ToPixels(area));
    }

    float TextWidth(const gfx::Font &font, std::string_view text) const override
    {
        return gfx::MeasureText(font, text);
    }

    /** The display's scale, which the window draws its root at: the zoom a JUCE editor's DPI transform gives it.
        @return  window pixels per design pixel */
    float Zoom() const override
    {
        return scale_;
    }

    // A surface let go reaches nothing through its host: a popup closed with the window it opened over outlives the
    // connection, which the child window's going takes with it.

    void CallSoon(std::function<void()> call) override
    {
        if (!released_)
        {
            connection_.CallSoon(std::move(call));
        }
    }

    void SetCursor(view::Cursor cursor) override
    {
        if (released_)
        {
            return;
        }
        XDefineCursor(display_, window_, connection_.CursorFor(cursor));
        XFlush(display_);
    }

    void RequestKeyboardFocus(view::View *view) override
    {
        router_.Focus(view);
    }

    void KeyboardFocusChanged(bool a_view_has_it) override
    {
        if (released_)
        {
            return;
        }
        if (a_view_has_it)
        {
            int revert = 0;
            XGetInputFocus(display_, &previous_focus_, &revert);
            focus_taken_ = true;
            XSetInputFocus(display_, window_, RevertToParent, CurrentTime);
        }
        else if (focus_taken_)
        {
            focus_taken_ = false;
            XSetInputFocus(display_, previous_focus_ != 0 ? previous_focus_ : PointerRoot, RevertToParent, CurrentTime);
        }
        XFlush(display_);
    }

    void ViewLeaving(const view::View &view) override
    {
        router_.Forget(view);
    }

    /** A popup as an override-redirect X window over everything, kept whole on the screen, under the connection's
        pointer grab while it takes the mouse.
        @param content          the popup's root, sized by its bounds, which outlives the popup
        @param at               the popup's top left, in this surface's coordinates
        @param pressed_outside  run with the screen point of a press outside the popup's windows
        @param takes_mouse      false for a popup that only shows
        @return                 the popup, closed when it is dropped; null once the surface is let go or no window was
       made */
    std::unique_ptr<view::Popup> OpenPopup(view::View &content, gfx::PointI at,
                                           std::function<void(gfx::PointI)> pressed_outside, bool takes_mouse) override;

    /** The CLIPBOARD selection's text, for a paste (x11::ReadClipboardText): this process's own copy where it holds
        the selection, else asked of its owner.
        @return  the text in UTF-8, or empty */
    std::string ClipboardText() override
    {
        return released_ ? std::string() : x11::ReadClipboardText();
    }

    /** `text` on the CLIPBOARD selection, for a copy (LinuxConnection::SetClipboardText); nothing once the surface is
        let go.
        @param text  the text, UTF-8
        @return      true when the selection was taken */
    bool SetClipboardText(const std::string &text) override
    {
        return !released_ && connection_.SetClipboardText(text);
    }

    /** The connection's file dialog; nothing once the surface is let go.
        @param request  the dialog's title, folder, file patterns and whether it saves
        @param picked   run on a later tick with the path chosen; empty when cancelled */
    void ChooseFile(const view::FileRequest &request, std::function<void(const std::string &)> picked) override
    {
        if (!released_)
        {
            connection_.ChooseFile(request, std::move(picked));
        }
    }

    // ---- Events ---------------------------------------------------------------------------------------------------

    /** The window takes files dragged from the desktop (XDND, version 5): what the child window does. */
    void AcceptDrops()
    {
        atoms_.aware       = XInternAtom(display_, "XdndAware", False);
        atoms_.proxy       = XInternAtom(display_, "XdndProxy", False);
        atoms_.enter       = XInternAtom(display_, "XdndEnter", False);
        atoms_.position    = XInternAtom(display_, "XdndPosition", False);
        atoms_.status      = XInternAtom(display_, "XdndStatus", False);
        atoms_.leave       = XInternAtom(display_, "XdndLeave", False);
        atoms_.drop        = XInternAtom(display_, "XdndDrop", False);
        atoms_.finished    = XInternAtom(display_, "XdndFinished", False);
        atoms_.selection   = XInternAtom(display_, "XdndSelection", False);
        atoms_.copy        = XInternAtom(display_, "XdndActionCopy", False);
        atoms_.uri_list    = XInternAtom(display_, "text/uri-list", False);
        atoms_.property    = XInternAtom(display_, "RECKLESS_DROP", False);
        const long version = 5;
        XChangeProperty(display_, window_, atoms_.aware, 4 /* XA_ATOM */, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(&version), 1);
        XFlush(display_);
    }

    /** Files dragged over `top_level` - a window this one fills, whose owner hands it its drops - taken here, by XDND's
        proxy: a drag source asks the top-level window under the pointer whether it takes files, never the windows
        inside it, so the top-level is marked as taking them (XdndAware) with this window as its proxy (XdndProxy),
        this window naming itself its own proxy as the protocol has a proxy prove it is one, and the connection hands
        it the messages a source addresses to the top-level.
        @param top_level  the X window id of the top-level window; 0 does nothing */
    void ProxyDropsFor(::Window top_level)
    {
        if (atoms_.aware == 0 || top_level == 0)
        {
            return;
        }
        const long version = 5;
        XChangeProperty(display_, top_level, atoms_.aware, 4 /* XA_ATOM */, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(&version), 1);
        XChangeProperty(display_, top_level, atoms_.proxy, 33 /* XA_WINDOW */, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(&window_), 1);
        XChangeProperty(display_, window_, atoms_.proxy, 33 /* XA_WINDOW */, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(&window_), 1);
        XFlush(display_);
        connection_.ProxyFor(top_level, this);
    }

    /** One X event on this surface's window: a repaint, the mouse, a key, the focus, or a step of a file drag.
        @param event  the event; a run of moves is read ahead into it */
    void Handle(XEvent &event)
    {
        if (atoms_.aware != 0 && (event.type == ClientMessage || event.type == SelectionNotify))
        {
            HandleDrag(event);
            return;
        }
        switch (event.type)
        {
        case Expose:
            dirty_ = dirty_.Union({event.xexpose.x, event.xexpose.y, event.xexpose.width, event.xexpose.height});
            break;
        case ButtonPress:
            Press(event.xbutton);
            break;
        case ButtonRelease:
            if (event.xbutton.button == Button1 || event.xbutton.button == Button3)
            {
                pressed_ = false;
                router_.Up(ToDesign(event.xbutton.x, event.xbutton.y), ModifiersOf(event.xbutton.state));
            }
            break;
        case MotionNotify: {
            // Only the latest of a run of moves matters.
            while (XCheckTypedWindowEvent(display_, window_, MotionNotify, &event))
            {
            }
            const gfx::PointF point = ToDesign(event.xmotion.x, event.xmotion.y);
            if (pressed_)
            {
                router_.Drag(point, ModifiersOf(event.xmotion.state));
            }
            else
            {
                router_.Move(point, ModifiersOf(event.xmotion.state));
            }
            break;
        }
        case EnterNotify:
            if (!pressed_)
            {
                router_.Move(ToDesign(event.xcrossing.x, event.xcrossing.y), ModifiersOf(event.xcrossing.state));
            }
            break;
        case LeaveNotify:
            if (!pressed_)
            {
                router_.Exit();
            }
            break;
        case KeyPress:
            if (!router_.Key(KeyOf(event.xkey)))
            {
                HandOnKey(event);
            }
            break;
        case FocusOut:
            if (focus_taken_ && event.xfocus.mode == NotifyNormal)
            {
                focus_taken_ = false;
                router_.Focus(nullptr); // the keyboard went elsewhere
            }
            break;
        default:
            break;
        }
    }

    /** The tree drawn over what was invalidated, and that part put on screen. */
    void Paint()
    {
        const gfx::RectI window_area{pixel_width_, pixel_height_};
        const gfx::RectI dirty = dirty_.Intersection(window_area);
        dirty_                 = {};
        if (dirty.IsEmpty() || !surface_ || image_ == nullptr)
        {
            return;
        }
        // Only the dirty part is drawn again: the rest of the raster still holds the last frame. The design pixels
        // that cover it are drawn whole, and all of them put on screen.
        const gfx::RectI design =
            gfx::RectF{dirty.x() / scale_, dirty.y() / scale_, dirty.width() / scale_, dirty.height() / scale_}
                .SmallestIntegerContainer();
        SkCanvas *sk = surface_->getCanvas();
        sk->save();
        sk->resetMatrix();
        sk->scale(scale_, scale_);
        {
            gfx::SkiaCanvas canvas(*sk, 1.0f);
            canvas.ClipToRect(design);
            view::PaintTree(root_, canvas);
        }
        sk->restore();
        const gfx::RectI put = ToPixels(design).Intersection(window_area);
        XPutImage(display_, window_, gc_, image_, put.x(), put.y(), put.x(), put.y(),
                  static_cast<unsigned>(put.width()), static_cast<unsigned>(put.height()));
        XFlush(display_);
    }

    /** A point in the root's coordinates as a point on the screen (the root window's).
        @param at  the point, in the root's coordinates
        @return    the point, in screen pixels */
    gfx::PointI ToScreen(gfx::PointI at) const
    {
        int      x = 0, y = 0;
        ::Window child = 0;
        XTranslateCoordinates(display_, window_, DefaultRootWindow(display_),
                              static_cast<int>(std::lround(at.x * scale_)),
                              static_cast<int>(std::lround(at.y * scale_)), &x, &y, &child);
        return {x, y};
    }

    /** The surface's X window.
        @return  the X window id */
    ::Window window() const noexcept
    {
        return window_;
    }

  protected:
    /** The surface let go, once - what its destruction does, and what a popup's closing with the window it opened
        over does early, while the connection still lives: the popups opened over it close, and theirs; the tree loses
        its host; the X window and what draws it go. Then each closed popup's `closed_by_window` runs - a menu ends
        dismissed, its owner free to drop it there - when nothing of the surface is left for it to reach. */
    void Release()
    {
        std::vector<std::function<void()>> heard;
        ReleaseInto(heard);
        for (const auto &closed : heard)
        {
            closed();
        }
    }

    /** Release's letting go, the closed popups' `closed_by_window` gathered into `heard` for the caller to run.
        @param heard  the callbacks to run afterwards, appended to */
    void ReleaseInto(std::vector<std::function<void()>> &heard)
    {
        if (released_)
        {
            return;
        }
        released_ = true;
        EndPopups(heard);
        root_.SetHost(nullptr);
        if (focus_taken_)
        {
            XSetInputFocus(display_, previous_focus_ != 0 ? previous_focus_ : PointerRoot, RevertToParent, CurrentTime);
        }
        connection_.Remove(window_);
        DestroyImage();
        XFreeGC(display_, gc_);
        XDestroyWindow(display_, window_);
        XFlush(display_);
    }

    /** A key no view took (MouseRouter::Key's false, "for the window to hand on"): a popup has nowhere to hand it.
        @param event  the key press; the base does nothing with it */
    virtual void HandOnKey(XEvent &event)
    {
        (void)event;
    }

    /** The raster and its XImage made again at the root's size times the scale. */
    void ResizeRaster()
    {
        pixel_width_  = std::max(1, static_cast<int>(std::lround(root_.width() * scale_)));
        pixel_height_ = std::max(1, static_cast<int>(std::lround(root_.height() * scale_)));
        DestroyImage();
        pixels_.assign(static_cast<std::size_t>(pixel_width_) * static_cast<std::size_t>(pixel_height_), 0u);
        const std::size_t row_bytes = static_cast<std::size_t>(pixel_width_) * 4u;
        surface_                    = SkSurfaces::WrapPixels(
            SkImageInfo::Make(pixel_width_, pixel_height_, kBGRA_8888_SkColorType, kPremul_SkAlphaType), pixels_.data(),
            row_bytes);
        image_ = XCreateImage(display_, visual_, static_cast<unsigned>(depth_), ZPixmap, 0,
                              reinterpret_cast<char *>(pixels_.data()), static_cast<unsigned>(pixel_width_),
                              static_cast<unsigned>(pixel_height_), 32, static_cast<int>(row_bytes));
        if (image_ != nullptr)
        {
            image_->byte_order = LSBFirst; // the raster's bytes as they lie, whatever the server's order
        }
    }

    LinuxConnection  &connection_;      ///< the connection the window is on
    Display          *display_;         ///< the connection's display
    ::Window          window_;          ///< the X window drawn in
    Visual           *visual_;          ///< the window's visual
    int               depth_;           ///< the window's depth, in bits
    view::View       &root_;            ///< the root of the tree drawn
    view::MouseRouter router_;          ///< hands the mouse and keys to the tree
    float             scale_;           ///< window pixels per design pixel
    int               pixel_width_{1};  ///< the raster's width, in pixels
    int               pixel_height_{1}; ///< the raster's height, in pixels
    gfx::RectI        dirty_;           ///< the part of the raster to draw and put on screen again, in pixels
    bool              released_{false}; ///< true once Release ran: nothing of the surface reaches the display

  private:
    /** The atoms XDND names its messages and its selection by, interned once by AcceptDrops. */
    struct DragAtoms
    {
        Atom aware{0},    ///< XdndAware, the window property that says it takes drops
            proxy{0},     ///< XdndProxy, the window property that names another window to send the messages to
            enter{0},     ///< XdndEnter, a drag came over the window
            position{0},  ///< XdndPosition, the drag moved
            status{0},    ///< XdndStatus, the answer to a position
            leave{0},     ///< XdndLeave, the drag went away
            drop{0},      ///< XdndDrop, the drag was dropped
            finished{0},  ///< XdndFinished, the answer to a drop
            selection{0}, ///< XdndSelection, the selection that holds the dragged data
            copy{0},      ///< XdndActionCopy, the one action taken
            uri_list{0},  ///< text/uri-list, the data's type
            property{0};  ///< RECKLESS_DROP, the property the selection is converted into
    };

    /** A drag from the desktop: the paths fetched once (the selection converted at the first position), each
        position answered with whether the view under it takes them, the drop handed to the router.
        @param event  a ClientMessage of the XDND protocol, or the SelectionNotify that brings the paths */
    void HandleDrag(XEvent &event)
    {
        if (event.type == SelectionNotify)
        {
            if (event.xselection.property == atoms_.property)
            {
                Atom           type   = 0;
                int            format = 0;
                unsigned long  count = 0, after = 0;
                unsigned char *data = nullptr;
                XGetWindowProperty(display_, window_, atoms_.property, 0, 1 << 20, True, AnyPropertyType, &type,
                                   &format, &count, &after, &data);
                drag_paths_ = data != nullptr ? PathsOfUriList(std::string(reinterpret_cast<char *>(data), count))
                                              : std::vector<std::string>{};
                if (data != nullptr)
                {
                    XFree(data);
                }
                have_paths_ = true;
                if (drop_waiting_)
                {
                    FinishDrop();
                }
                else
                {
                    router_.DragFiles(drag_point_, drag_paths_); // the view under the drag hears of it at once
                }
            }
            return;
        }
        const XClientMessageEvent &message = event.xclient;
        // The window the source addressed - this one, or the top-level it is the proxy of - which the answers name.
        if (message.message_type == atoms_.enter || message.message_type == atoms_.position ||
            message.message_type == atoms_.drop)
        {
            drag_target_ = message.window;
        }
        if (message.message_type == atoms_.enter)
        {
            drag_source_  = static_cast<::Window>(message.data.l[0]);
            drag_paths_   = {};
            have_paths_   = false;
            requested_    = false;
            drop_waiting_ = false;
        }
        else if (message.message_type == atoms_.position)
        {
            drag_source_ = static_cast<::Window>(message.data.l[0]);
            const int rx = static_cast<int>((message.data.l[2] >> 16) & 0xffff);
            const int ry = static_cast<int>(message.data.l[2] & 0xffff);
            int       x = 0, y = 0;
            ::Window  child = 0;
            XTranslateCoordinates(display_, DefaultRootWindow(display_), window_, rx, ry, &x, &y, &child);
            drag_point_ = {static_cast<float>(x) / scale_, static_cast<float>(y) / scale_};
            if (!requested_)
            {
                requested_ = true;
                XConvertSelection(display_, atoms_.selection, atoms_.uri_list, atoms_.property, window_,
                                  static_cast<Time>(message.data.l[3]));
            }
            // Until the paths arrive, accept: the drop waits for them and the router decides then.
            const bool accept = !have_paths_ || router_.DragFiles(drag_point_, drag_paths_);
            SendToSource(atoms_.status, {static_cast<long>(drag_target_), accept ? 3L : 2L, 0, 0,
                                         accept ? static_cast<long>(atoms_.copy) : 0L});
        }
        else if (message.message_type == atoms_.leave)
        {
            drag_source_ = 0;
            router_.EndFileDrag();
        }
        else if (message.message_type == atoms_.drop)
        {
            drag_source_  = static_cast<::Window>(message.data.l[0]);
            drop_waiting_ = true;
            if (have_paths_)
            {
                FinishDrop();
            }
            else if (!requested_)
            {
                requested_ = true;
                XConvertSelection(display_, atoms_.selection, atoms_.uri_list, atoms_.property, window_,
                                  static_cast<Time>(message.data.l[2]));
            }
        }
    }

    /** The drop handed to the router and the source told whether it was taken. */
    void FinishDrop()
    {
        drop_waiting_       = false;
        const bool accepted = router_.DropFiles(drag_point_, drag_paths_);
        SendToSource(atoms_.finished, {static_cast<long>(drag_target_), accepted ? 1L : 0L,
                                       accepted ? static_cast<long>(atoms_.copy) : 0L, 0, 0});
        drag_source_ = 0;
    }

    /** A client message to the drag source; nothing when there is none.
        @param type  the message type, an XDND atom
        @param data  the five longs of the message's data */
    void SendToSource(Atom type, std::array<long, 5> data)
    {
        if (drag_source_ == 0)
        {
            return;
        }
        XEvent reply{};
        reply.xclient.type         = ClientMessage;
        reply.xclient.window       = drag_source_;
        reply.xclient.message_type = type;
        reply.xclient.format       = 32;
        for (std::size_t i = 0; i < data.size(); ++i)
        {
            reply.xclient.data.l[i] = data[i];
        }
        XSendEvent(display_, drag_source_, False, NoEventMask, &reply);
        XFlush(display_);
    }

    /** The XImage destroyed, leaving the raster's pixels to the vector. */
    void DestroyImage()
    {
        if (image_ != nullptr)
        {
            image_->data = nullptr; // the pixels are the vector's, not Xlib's to free
            XDestroyImage(image_);
            image_ = nullptr;
        }
    }

    /** The design pixels `area` covers, in the window's pixels, rounded out.
        @param area  a rectangle, in design pixels
        @return      the smallest pixel rectangle that holds it */
    gfx::RectI ToPixels(gfx::RectI area) const noexcept
    {
        return gfx::RectF{area.x() * scale_, area.y() * scale_, area.width() * scale_, area.height() * scale_}
            .SmallestIntegerContainer();
    }

    /** A point in the window's pixels as a point in design pixels.
        @param x  the x, in window pixels
        @param y  the y, in window pixels
        @return   the point, in design pixels */
    gfx::PointF ToDesign(int x, int y) const noexcept
    {
        return {static_cast<float>(x) / scale_, static_cast<float>(y) / scale_};
    }

    /** A button press: a click with a double click's count, a right press, or a notch of the wheel.
        @param press  the event; buttons 4 to 7 are the wheel and its tilt */
    void Press(const XButtonEvent &press)
    {
        const gfx::PointF     point     = ToDesign(press.x, press.y);
        const view::Modifiers modifiers = ModifiersOf(press.state);
        switch (press.button)
        {
        case Button1: {
            // X has no double click: a second press soon after the first and near it is one.
            const bool second = press.time - last_press_time_ <= kDoubleClickMs &&
                                std::abs(static_cast<float>(press.x) - last_press_x_) <= kDoubleClickSlop &&
                                std::abs(static_cast<float>(press.y) - last_press_y_) <= kDoubleClickSlop;
            last_press_time_  = second ? 0 : press.time; // a third press starts again
            last_press_x_     = static_cast<float>(press.x);
            last_press_y_     = static_cast<float>(press.y);
            pressed_          = true;
            router_.Down(point, second ? 2 : 1, false, modifiers);
            break;
        }
        case Button3:
            pressed_ = true;
            router_.Down(point, 1, true, modifiers);
            break;
        case Button4: // the wheel away from the player
            router_.Wheel(point, 0.0f, kNotchPixels, modifiers);
            break;
        case Button5:
            router_.Wheel(point, 0.0f, -kNotchPixels, modifiers);
            break;
        case 6: // a tilt to the left
            router_.Wheel(point, kNotchPixels, 0.0f, modifiers);
            break;
        case 7:
            router_.Wheel(point, -kNotchPixels, 0.0f, modifiers);
            break;
        default:
            break;
        }
    }

    GC                         gc_{};           ///< the graphics context the raster is put on screen with
    std::vector<std::uint32_t> pixels_;         ///< the raster: B, G, R, A in memory, premultiplied, a row after a row
    sk_sp<SkSurface>           surface_;        ///< Skia's surface on pixels_
    XImage                    *image_{nullptr}; ///< the XImage on pixels_, which XPutImage reads
    bool                       pressed_{false}; ///< a button is down, so moves are drags
    Time                       last_press_time_{0};  ///< the server time of the last press; 0 after a double click
    float                      last_press_x_{0.0f};  ///< the last press's x, in window pixels
    float                      last_press_y_{0.0f};  ///< the last press's y, in window pixels
    bool                       focus_taken_{false};  ///< a view has the keyboard and this window holds X's focus
    ::Window                   previous_focus_{0};   ///< the window that held X's focus before this one took it
    DragAtoms                  atoms_;               ///< the XDND atoms; zero until AcceptDrops
    ::Window                   drag_source_{0};      ///< the window the drag comes from; 0 for none
    ::Window                   drag_target_{0};      ///< the window the source addressed: this one, or its top-level
    std::vector<std::string>   drag_paths_;          ///< the paths being dragged, once fetched
    gfx::PointF                drag_point_{};        ///< the drag's last position, in design pixels
    bool                       have_paths_{false};   ///< drag_paths_ has been fetched
    bool                       requested_{false};    ///< the selection has been asked for
    bool                       drop_waiting_{false}; ///< a drop came before the paths and waits for them
    std::vector<LinuxPopup *>  popups_;              ///< the popups opened over it, which go with it
};

/** A popup: the surface in an override-redirect window at a point on the screen, its presses outside shown to it by
    the connection. It goes with the surface it opened over (CloseInto) if that goes first - as the child window
    does, the connection with it, so a popup its owner drops afterwards must reach nothing. */
class LinuxPopup final : public view::Popup, public LinuxSurface
{
  public:
    /** A popup in an X window already made.
        @param opener      the surface it opened over, which tells it to close if it goes first
        @param connection  the connection the window is on
        @param window      the override-redirect X window; destroyed with the popup
        @param visual      the window's visual
        @param depth       the window's depth, in bits
        @param content     the popup's root, which outlives the popup
        @param scale       window pixels per design pixel
        @param screen      the window's top left, in screen pixels
        @param outside     run with the screen point of a press outside the popup */
    LinuxPopup(LinuxSurface &opener, LinuxConnection &connection, ::Window window, Visual *visual, int depth,
               view::View &content, float scale, gfx::PointI screen, std::function<void(gfx::PointI)> outside)
        : LinuxSurface(connection, window, visual, depth, content, scale), opener_(&opener), screen_(screen),
          outside_(std::move(outside))
    {
        connection_.AddPopup(this, window_);
    }

    /** The popup taken off its opener's list and, if it has not closed, off the connection's. */
    ~LinuxPopup() override
    {
        if (opener_ != nullptr)
        {
            opener_->Forget(this);
        }
        if (!closed_)
        {
            connection_.RemovePopup(this);
        }
    }

    bool Contains(gfx::PointI screen) const override
    {
        return !closed_ && gfx::RectI{screen_.x, screen_.y, pixel_width_, pixel_height_}.Contains(screen);
    }

    /** Closed with the surface it opened over, while the connection lives: off the connection's popups, then the
        surface let go - its own popups with it - and its `closed_by_window` gathered into `heard` after theirs, the
        handle left inert for its owner to drop, its destruction then reaching nothing.
        @param heard  the callbacks to run afterwards, appended to */
    void CloseInto(std::vector<std::function<void()>> &heard)
    {
        if (closed_)
        {
            return;
        }
        closed_ = true;
        opener_ = nullptr;
        connection_.RemovePopup(this);
        ReleaseInto(heard);
        if (closed_by_window)
        {
            heard.push_back(closed_by_window);
        }
    }

    /** A press on the screen, which the popup tells its owner of when it lands outside it.
        @param screen  the press, in screen pixels */
    void PressedAt(gfx::PointI screen)
    {
        if (!Contains(screen) && outside_)
        {
            outside_(screen);
        }
    }

  private:
    LinuxSurface                    *opener_;        ///< the surface it opened over; null once that went first
    gfx::PointI                      screen_;        ///< the window's top left, in screen pixels
    std::function<void(gfx::PointI)> outside_;       ///< run with the screen point of a press outside the popup
    bool                             closed_{false}; ///< the connection has dropped it: it contains nothing
};

void LinuxSurface::EndPopups(std::vector<std::function<void()>> &heard)
{
    for (LinuxPopup *popup : std::exchange(popups_, {}))
    {
        popup->CloseInto(heard);
    }
}

void LinuxConnection::AddPopup(LinuxPopup *popup, ::Window window)
{
    popups_.push_back(popup);
    if (grab_window_ == 0)
    {
        // Every press anywhere comes to this client while a popup is open: on its own windows as usual, elsewhere to
        // the first popup, so a press outside them all can close them.
        if (XGrabPointer(display_, window, True, ButtonPressMask | ButtonReleaseMask | PointerMotionMask, GrabModeAsync,
                         GrabModeAsync, 0, 0, CurrentTime) == GrabSuccess)
        {
            grab_window_ = window;
        }
    }
}

void LinuxConnection::RemovePopup(LinuxPopup *popup)
{
    const auto listed = std::find(popups_.begin(), popups_.end(), popup);
    if (listed != popups_.end())
    {
        popups_.erase(listed);
    }
    if (popups_.empty() && grab_window_ != 0)
    {
        XUngrabPointer(display_, CurrentTime);
        grab_window_ = 0;
        XFlush(display_);
    }
}

void LinuxConnection::Pump()
{
    while (XPending(display_) > 0)
    {
        XEvent event;
        XNextEvent(display_, &event);
        if (clipboard_ && clipboard_->Handle(event))
        {
            continue; // a paste asking the copy this connection holds, or word that another copy took it
        }
        if (event.type == ButtonPress && !popups_.empty())
        {
            const gfx::PointI               at{event.xbutton.x_root, event.xbutton.y_root};
            const std::vector<LinuxPopup *> now = popups_; // a popup may close as it hears
            for (LinuxPopup *popup : now)
            {
                if (std::find(popups_.begin(), popups_.end(), popup) != popups_.end())
                {
                    popup->PressedAt(at);
                }
            }
        }
        LinuxSurface *surface = nullptr;
        if (const auto it = surfaces_.find(event.xany.window); it != surfaces_.end())
        {
            surface = it->second;
        }
        else if (const auto proxied = proxied_.find(event.xany.window);
                 event.type == ClientMessage && proxied != proxied_.end())
        {
            surface = proxied->second; // a drag source's word to a top-level whose drops it takes
        }
        if (surface != nullptr)
        {
            const bool outside = event.type == ButtonPress && event.xany.window == grab_window_ &&
                                 (event.xbutton.x < 0 || event.xbutton.y < 0); // a grabbed press elsewhere
            if (!outside)
            {
                surface->Handle(event);
            }
        }
    }
    for (const auto &[window, surface] : surfaces_)
    {
        surface->Paint();
    }
}

void LinuxConnection::Tick()
{
    if (tick_)
    {
        tick_();
    }
    // The calls deferred to this tick, and any they defer in turn to the next.
    std::deque<std::function<void()>> due;
    due.swap(calls_);
    for (auto &call : due)
    {
        call();
    }
    PollDialog();
    Pump();
}

void LinuxConnection::ChooseFile(const view::FileRequest &request, std::function<void(const std::string &)> picked)
{
    const bool zenity = OnPath("zenity");
    if (dialog_pid_ > 0 || (!zenity && !OnPath("kdialog")))
    {
        CallSoon([picked] { picked({}); });
        return;
    }
    std::vector<std::string> args;
    std::string              start = request.folder.empty() ? std::string() : request.folder + "/";
    if (request.save)
    {
        start += request.file_name;
    }
    std::string patterns;
    for (const auto &p : request.patterns)
    {
        patterns += (patterns.empty() ? "" : " ") + p;
    }
    if (zenity)
    {
        args = {"zenity", "--file-selection", "--title=" + request.title};
        if (request.save)
        {
            args.emplace_back("--save");
            args.emplace_back("--confirm-overwrite");
        }
        if (!start.empty())
        {
            args.push_back("--filename=" + start);
        }
        if (!patterns.empty())
        {
            args.push_back("--file-filter=Files | " + patterns);
        }
    }
    else
    {
        args = {"kdialog", "--title", request.title, request.save ? "--getsavefilename" : "--getopenfilename",
                start.empty() ? std::string(".") : start};
        if (!patterns.empty())
        {
            args.push_back(patterns + "|Files");
        }
    }
    int fds[2];
    if (pipe(fds) != 0)
    {
        CallSoon([picked] { picked({}); });
        return;
    }
    const pid_t pid = fork();
    if (pid == 0)
    {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        std::vector<char *> argv;
        for (auto &a : args)
        {
            argv.push_back(a.data());
        }
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0)
    {
        close(fds[0]);
        CallSoon([picked] { picked({}); });
        return;
    }
    fcntl(fds[0], F_SETFL, O_NONBLOCK);
    dialog_pid_    = pid;
    dialog_fd_     = fds[0];
    dialog_output_ = {};
    dialog_picked_ = std::move(picked);
}

void LinuxConnection::PollDialog()
{
    if (dialog_pid_ <= 0)
    {
        return;
    }
    char buffer[512];
    for (;;)
    {
        const ssize_t n = read(dialog_fd_, buffer, sizeof(buffer));
        if (n > 0)
        {
            dialog_output_.append(buffer, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0)
        {
            break; // the dialog has closed its end: its answer is all here
        }
        return; // nothing more yet
    }
    close(dialog_fd_);
    int status = 0;
    waitpid(dialog_pid_, &status, 0);
    dialog_pid_      = -1;
    dialog_fd_       = -1;
    std::string path = dialog_output_;
    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
    {
        path.pop_back();
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
    {
        path.clear(); // cancelled, or no dialog after all
    }
    auto picked = std::move(dialog_picked_);
    picked(path);
}

std::unique_ptr<view::Popup> LinuxSurface::OpenPopup(view::View &content, gfx::PointI at,
                                                     std::function<void(gfx::PointI)> pressed_outside, bool takes_mouse)
{
    if (released_)
    {
        return nullptr;
    }
    gfx::PointI screen = ToScreen(at);
    {
        // Kept whole on the screen: moved in from an edge it would cross.
        const int width  = static_cast<int>(std::lround(content.width() * scale_));
        const int height = static_cast<int>(std::lround(content.height() * scale_));
        const int right  = DisplayWidth(display_, DefaultScreen(display_));
        const int bottom = DisplayHeight(display_, DefaultScreen(display_));
        screen = {std::max(0, std::min(screen.x, right - width)), std::max(0, std::min(screen.y, bottom - height))};
    }
    XSetWindowAttributes attributes{};
    attributes.override_redirect = True; // no frame, no window manager between it and the screen
    attributes.event_mask        = ExposureMask | EnterWindowMask | LeaveWindowMask |
                                   (takes_mouse ? ButtonPressMask | ButtonReleaseMask | PointerMotionMask : 0);
    attributes.background_pixmap = 0;
    attributes.border_pixel      = 0;
    attributes.colormap          = connection_.ColormapFor(visual_);
    const auto width  = static_cast<unsigned>(std::max(1, static_cast<int>(std::lround(content.width() * scale_))));
    const auto height = static_cast<unsigned>(std::max(1, static_cast<int>(std::lround(content.height() * scale_))));
    const ::Window window = XCreateWindow(
        display_, DefaultRootWindow(display_), screen.x, screen.y, width, height, 0, depth_, InputOutput, visual_,
        CWOverrideRedirect | CWEventMask | CWBackPixmap | CWBorderPixel | CWColormap, &attributes);
    if (window == 0)
    {
        return nullptr;
    }
    XMapRaised(display_, window);
    XFlush(display_);
    auto popup = std::make_unique<LinuxPopup>(*this, connection_, window, visual_, depth_, content, scale_, screen,
                                              std::move(pressed_outside));
    popups_.push_back(popup.get());
    return popup;
}

/** The child window inside the host's editor: the surface as a child of the host's window, owning the connection it
    and its popups share. */
class LinuxChildWindow final : public ChildWindow
{
  public:
    /** The child window on a window already made, which it takes under the host's.
        @param connection  the connection, owned
        @param parent      the host's X window the window sits in
        @param window      the child's X window
        @param visual      the parent's visual
        @param depth       the parent's depth, in bits
        @param root        the root of the tree to show, which outlives the window
        @param scale       window pixels per design pixel */
    LinuxChildWindow(std::unique_ptr<LinuxConnection> connection, ::Window parent, ::Window window, Visual *visual,
                     int depth, view::View &root, float scale)
        : connection_(std::move(connection))
    {
        surface_ = std::make_unique<ChildSurface>(*connection_, window, visual, depth, root, scale);
        surface_->SetParent(parent);
        surface_->AcceptDrops();
    }

    /** The surface released, then the connection it draws on. */
    ~LinuxChildWindow() override
    {
        surface_.reset(); // before the connection it draws on
        connection_.reset();
    }

    /** Hands the connection to the host's loop, the frame callback on its timer.
        @return  true when the loop took the watch and the timer */
    bool Start()
    {
        return connection_->Start([this] {
            if (frame_)
            {
                frame_();
            }
        });
    }

    void SetScale(float scale) override
    {
        surface_->SetScale(scale);
    }

    void SetFrameCallback(std::function<void()> frame) override
    {
        frame_ = std::move(frame);
    }

    /** Viewable: mapped, with every window above it mapped - a window manager unmaps a minimised top-level window,
        which leaves it unviewable. A round trip to the server a frame.
        @return  true when the window is viewable */
    bool Showing() const override
    {
        return surface_->Viewable();
    }

    void TakeDropsFor(void *top_level) override
    {
        surface_->ProxyDropsFor(static_cast<::Window>(reinterpret_cast<std::uintptr_t>(top_level)));
    }

  private:
    /** The child window's surface: the window resized to its raster, the keys it does not take passed up. */
    class ChildSurface final : public LinuxSurface
    {
      public:
        /** The constructor of LinuxSurface.
            @param connection  the connection the window is on, which outlives the surface
            @param window      the X window to draw in; destroyed with the surface
            @param visual      the window's visual
            @param depth       the window's depth, in bits
            @param root        the root of the view tree to draw, which outlives the surface
            @param scale       window pixels per design pixel */
        using LinuxSurface::LinuxSurface;

        /** The window drawn at a new scale and resized to its raster.
            @param scale  window pixels per design pixel */
        void SetScale(float scale)
        {
            scale_ = scale;
            ResizeRaster();
            XResizeWindow(display_, window_, static_cast<unsigned>(pixel_width_), static_cast<unsigned>(pixel_height_));
            dirty_ = {pixel_width_, pixel_height_};
            XFlush(display_);
        }

        /** Whether the window is mapped with every window above it mapped.
            @return  true when X reports it viewable */
        bool Viewable() const
        {
            XWindowAttributes attributes{};
            return XGetWindowAttributes(display_, window_, &attributes) != 0 && attributes.map_state == IsViewable;
        }

        /** The window a key no view took is handed on to.
            @param parent  the host's X window the child sits in */
        void SetParent(::Window parent) noexcept
        {
            parent_ = parent;
        }

      protected:
        /** A key no view took handed to the window this one sits in - a host's editor, the Standalone's own window - as
            the server would have handed it had this window not asked for keys: X gives a key to the deepest window
            under the pointer that asks, so the parent's shortcuts, and Escape on the Standalone's dialog, would
            otherwise go dead while the pointer is over the panel. The Mac's child view hands such a key up its
            responder chain the same way. Sent with propagation, so it reaches the nearest ancestor that asks for keys;
            nothing once the surface is let go, which reaches nothing through its connection after.
            @param event  the key press, copied and addressed to the parent */
        void HandOnKey(XEvent &event) override
        {
            if (parent_ == 0 || released_)
            {
                return;
            }
            XEvent handed           = event;
            handed.xkey.window      = parent_;
            handed.xkey.subwindow   = 0;
            handed.xkey.same_screen = True;
            XSendEvent(display_, parent_, True, KeyPressMask, &handed);
            XFlush(display_);
        }

      private:
        ::Window parent_{0}; ///< the host's window the child sits in; 0 until SetParent
    };

    std::unique_ptr<LinuxConnection> connection_; ///< the connection, shared with the popups
    std::unique_ptr<ChildSurface>    surface_;    ///< the child's surface, dropped before the connection
    std::function<void()>            frame_;      ///< the frame callback; empty for none
};

namespace
{
/** `target` handed to the desktop's xdg-open, in a process of its own started from a child that leaves at once, so the
    host waits for nothing and keeps no child behind.
    @param target  a folder's path or a URL */
void OpenWithDesktop(std::string target)
{
    const pid_t child = fork();
    if (child == 0)
    {
        setsid();
        if (fork() == 0)
        {
            char  opener[] = "xdg-open";
            char *argv[]   = {opener, target.data(), nullptr};
            execvp(argv[0], argv);
        }
        _exit(0);
    }
    if (child > 0)
    {
        waitpid(child, nullptr, 0);
    }
}
} // namespace

void RevealInFileManager(const std::string &path)
{
    // As JUCE's File::revealToUser on Linux: the folder opened by the desktop's xdg-open.
    OpenWithDesktop(path);
}

void OpenInBrowser(const std::string &url)
{
    if (url.rfind("https://", 0) == 0 || url.rfind("http://", 0) == 0)
    {
        OpenWithDesktop(url);
    }
}

float DisplayScaleOf(void *)
{
    // The desktop's scale, as the X resources carry it (Xft.dpi, which every desktop sets when it scales).
    Display *display = XOpenDisplay(nullptr);
    if (display == nullptr)
    {
        return 1.0f;
    }
    float       scale     = 1.0f;
    const char *resources = XResourceManagerString(display);
    if (resources != nullptr)
    {
        XrmInitialize();
        XrmDatabase database = XrmGetStringDatabase(resources);
        char       *type     = nullptr;
        XrmValue    value{};
        if (database != nullptr && XrmGetResource(database, "Xft.dpi", "Xft.Dpi", &type, &value) && value.addr)
        {
            const double dpi = std::atof(value.addr);
            if (dpi >= 48.0 && dpi <= 960.0)
            {
                scale = static_cast<float>(dpi / 96.0);
            }
        }
        if (database != nullptr)
        {
            XrmDestroyDatabase(database);
        }
    }
    XCloseDisplay(display);
    return scale;
}

std::unique_ptr<ChildWindow> CreateChildWindow(void *parent, view::View &root, int width, int height, float scale,
                                               std::unique_ptr<RunLoop> loop)
{
    const auto parent_window = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(parent));
    if (parent_window == 0 || !loop)
    {
        return nullptr;
    }
    Display *display = XOpenDisplay(nullptr);
    if (display == nullptr)
    {
        return nullptr;
    }
    XWindowAttributes parent_attributes{};
    if (XGetWindowAttributes(display, parent_window, &parent_attributes) == 0 ||
        !TakesBgra(parent_attributes.visual, parent_attributes.depth))
    {
        XCloseDisplay(display);
        return nullptr;
    }
    root.SetBounds({width, height});
    XSetWindowAttributes attributes{};
    attributes.event_mask = ExposureMask | ButtonPressMask | ButtonReleaseMask | PointerMotionMask | EnterWindowMask |
                            LeaveWindowMask | StructureNotifyMask | KeyPressMask | FocusChangeMask;
    attributes.background_pixmap = 0; // nothing cleared before an expose: the raster covers every pixel
    attributes.border_pixel      = 0;
    const auto     pixel_width   = static_cast<unsigned>(std::max(1, static_cast<int>(std::lround(width * scale))));
    const auto     pixel_height  = static_cast<unsigned>(std::max(1, static_cast<int>(std::lround(height * scale))));
    const ::Window window =
        XCreateWindow(display, parent_window, 0, 0, pixel_width, pixel_height, 0, CopyFromParent, InputOutput,
                      CopyFromParent, CWEventMask | CWBackPixmap | CWBorderPixel, &attributes);
    if (window == 0)
    {
        XCloseDisplay(display);
        return nullptr;
    }
    XMapWindow(display, window);
    XFlush(display);
    auto connection = std::make_unique<LinuxConnection>(display, std::move(loop));
    auto child = std::make_unique<LinuxChildWindow>(std::move(connection), parent_window, window,
                                                    parent_attributes.visual, parent_attributes.depth, root, scale);
    if (!child->Start())
    {
        return nullptr;
    }
    return child;
}
} // namespace reckless::window
