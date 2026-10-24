#pragma once

#include <functional>
#include <memory>
#include <string>

#include "Geometry.h"
#include "View.h"

namespace reckless::window
{
/**
    The event loop a child window runs on where it has none of its own. A Linux plug-in's window may not run a loop
    in the host's process: the host watches the window's connection to the display and calls back when it has events,
    and runs its timers (a VST3 host through its Linux::IRunLoop). On macOS and Windows the window's own toolkit
    delivers both, and no RunLoop is needed. Every callback runs on the host's UI thread.
*/
class RunLoop
{
  public:
    virtual ~RunLoop() = default;

    /** Calls `ready` whenever `fd` has something to read, until the loop is destroyed; false when the host refuses.
        @param fd     the descriptor to watch, owned by the caller
        @param ready  the callback, run on the host's UI thread
        @return       true when the host took the watch */
    virtual bool WatchFd(int fd, std::function<void()> ready) = 0;

    /** Calls `tick` every `milliseconds`, until the loop is destroyed; false when the host refuses.
        @param milliseconds  the period, in milliseconds
        @param tick          the callback, run on the host's UI thread
        @return              true when the host started the timer */
    virtual bool StartTimer(int milliseconds, std::function<void()> tick) = 0;

    /** `text` (UTF-8) held on the clipboard for the windows on this loop, by a loop that keeps it for them: an app's,
        whose connection to the display outlives every window it shows, so a copy made in a dialog stays on the
        clipboard after the dialog has gone. X11 keeps no clipboard of its own - the copying window's connection holds
        the text and answers each paste - so a loop that keeps none (a plug-in's host's, the default) leaves the copy
        to the window, which holds it while it lives.
        @param text  the text, UTF-8
        @return      true when the loop took it; false where the window is to hold it itself */
    virtual bool SetClipboardText(const std::string &text)
    {
        (void)text;
        return false;
    }
};

/**
    A panel shown in a window of the platform's own, written against no toolkit: a child window inside the window a
    host hands a plug-in's editor (an NSView on macOS, an HWND on Windows, an X11 window on Linux), the size of the
    panel's design times the scale it is shown at, drawing the root view's tree through Skia at the display's own
    pixel density whenever part of it was invalidated, and handing it the mouse through a view::MouseRouter. The
    window is the root's view::Host while it lives, and what it does for the tree comes only while it lives: a call a
    view deferred (Host::CallSoon) or a file dialog's answer (Host::ChooseFile) that would come after the window has
    gone never runs, and a dialog still up goes with the window. So does every popup opened over it, and theirs
    (Host::OpenPopup): each closes there and then, its handle left inert for its owner to drop whenever it likes - its
    destruction reaching nothing of the window - and its `closed_by_window` runs once the window is let go, on which
    a menu ends dismissed. So the views and the panel those callbacks hold, and the menus their popups show, may go
    with the window or after it, in any order. A frame callback, if set, runs at the display's rate on the window's
    thread, for whatever moves on its own (a lamp, a meter).

    Sizes are the platform's: points on macOS, where the backing scale sets the pixels, and physical pixels on
    Windows and Linux, where the host tells the editor the display's scale and the window is the design size times
    it.
*/
class ChildWindow
{
  public:
    virtual ~ChildWindow() = default;

    /** The root drawn at `scale` window units per design pixel, the window resized to fit the root's bounds: what a
        window does when its root (a shell) takes a new size, at the same scale.
        @param scale  window units per design pixel: 1 is the design size, 0.5 to 2 for a shell's range */
    virtual void SetScale(float scale) = 0;

    /** Runs `frame` once a display frame until replaced or the window goes.
        @param frame  the callback, run on the window's thread; empty for none */
    virtual void SetFrameCallback(std::function<void()> frame) = 0;

    /** Whether the panel is on a screen a person can see: in a window that is shown and not minimised. What a panel
        tells its core once a frame (core::PanelWatch), so what runs only for someone looking stops when nobody is.
        @return  true when the window shows; the default, for a window with no way to know, is true */
    virtual bool Showing() const
    {
        return true;
    }

    /** Files a desktop drags over `top_level` - the window this one sits in, which its owner hands this one the drops
        of: a Standalone's own window, never a host's - taken here as though dragged over this window. On Linux a drag
        source asks the top-level window under the pointer whether it takes files and never the windows inside it
        (XDND), so the top-level is marked as taking them with this window as its proxy; on macOS and Windows a drag
        reaches the view or the window under it, and nothing is done.
        @param top_level  the platform's native handle of the top-level window: an X11 window id on Linux, unused
                          elsewhere; owned by the caller */
    virtual void TakeDropsFor(void *top_level)
    {
        (void)top_level;
    }

    /** Keys no view takes handed on to `top_level`, the window this one sits in - a Standalone's own window, whose
        shortcuts they may be (AppPlatform::SetMenuBar). On Windows a key goes to the window with the focus alone, and
        this one holds the focus while a view has the keyboard, so each key down and character no view takes is sent
        to `top_level` (window/win/HandedKeys.h); on macOS a key no view takes goes up the responder chain to the
        window, and on Linux this window sends it to its parent, whatever is asked, and nothing is done. Nothing is
        handed on until this is called.
        @param top_level  the platform's native handle of the window this one sits in: an HWND on Windows, unused
                          elsewhere; owned by the caller; null to hand on none */
    virtual void HandOnKeysTo(void *top_level)
    {
        (void)top_level;
    }
};

/** The scale of the display the window `parent` is on, where the platform sizes a plug-in's window in pixels: on
    Windows its DPI over 96 (1 where the host is not DPI aware, and Windows scales its whole window itself), on Linux
    the X resources' Xft.dpi over 96 (1 where none is set); 1 on macOS, which sizes windows in points.
    @param parent  the platform's native handle of the parent window: an NSView, an HWND or an X11 window id; owned by
                   the caller
    @return        the display's scale, 1 at 96 dpi and on macOS */
float DisplayScaleOf(void *parent);

/** A folder, `path` (UTF-8), shown in the platform's file manager as JUCE's File::revealToUser shows one: selected in a
    window of the folder it is in on macOS and on Windows, and opened itself with xdg-open on Linux.
    @param path  the folder's path, in UTF-8 */
void RevealInFileManager(const std::string &path);

/** A web page, `url` (UTF-8, http or https), opened in the person's default browser: through NSWorkspace on macOS, the
    shell's ShellExecute on Windows and xdg-open on Linux, in a process of its own that the caller never waits for. A
   URL of any other scheme is refused, so nothing a page hands over opens a file or a program.
    @param url  the page's URL */
void OpenInBrowser(const std::string &url);

/** A child window of `parent` (the platform's native handle: an NSView, an HWND, an X11 window id) showing `root`,
    `width` x `height` design pixels at `scale`; null when the platform has none or the parent is unusable. `root`
    outlives the window. `loop` is the host's event loop on Linux, where it is required, and unused elsewhere; the
    window owns it and destroys it first, so no callback reaches a window that has gone.
    @param parent  the platform's native handle of the parent window: an NSView, an HWND or an X11 window id; owned by
                   the caller, who keeps it alive for the window's life
    @param root    the view tree the window shows; must outlive the window
    @param width   the content's width, in design pixels
    @param height  the content's height, in design pixels
    @param scale   the scale from design pixels to the window's pixels
    @param loop    the host's event loop, required on Linux and unused elsewhere; the window takes ownership
    @return        the window, or nullptr when the platform has none or the parent is unusable */
std::unique_ptr<ChildWindow> CreateChildWindow(void *parent, view::View &root, int width, int height, float scale,
                                               std::unique_ptr<RunLoop> loop = nullptr);
} // namespace reckless::window
