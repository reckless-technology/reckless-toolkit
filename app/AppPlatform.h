#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "Menu.h"
#include "MenuBar.h"
#include "MenuBarView.h"
#include "PostQueue.h"
#include "Window.h"

namespace reckless::app
{
/**
    The platform under the Standalone, written against no toolkit: what the app needs beyond the child window a panel
    is drawn in (window/Window.h's ChildWindow, which every window here holds) - windows of its own at the top level, a
    timer, the event loop and its end, the call after the current event, and the platform's own menu where it has
    one. One implementation a platform, behind this seam as window/'s are behind its: an NSApplication on
    macOS (mac/AppMac.mm), a Win32 message loop on Windows (win/AppWin.cpp) and an X11 connection on a poll loop on
    Linux (linux/AppLinux.cpp). Everything is the main thread's, but for the queue any thread posts calls to the main
    thread through (AppPlatform::Posts, AppPlatform::Post).
*/

/** A window at the top level: its title bar the platform's, its content one child window the size of the design
    times the window's scale. */
class AppWindow
{
  public:
    virtual ~AppWindow() = default;

    /** The native handle a child window is made in (window::CreateChildWindow): the content NSView on macOS, the HWND
        on Windows, the X11 window on Linux.
        @return  the handle, owned by the window and valid while it lives */
    virtual void *Handle() const = 0;

    /** The window's units per design pixel: 1 on macOS, whose windows are sized in points; the scale of the display
        the window is on on Windows, whose windows are sized in pixels and which tells a window that moves to a display
        of another scale (WindowOptions::rescaled); the desktop's on Linux (window::DisplayScaleOf).
        @return  the window's units per design pixel */
    virtual float Scale() const = 0;

    /** The window's top left on the desktop, y down from the top of the main display, in the platform's units: where
        it stands, or where it stood before it was minimised.
        @return  the x and the y of the top left, in the platform's units */
    virtual std::pair<int, int> Position() const = 0;

    /** Brought in front of the app's other windows and given the keyboard, restored first where it was minimised,
        shaded or on another desktop. */
    virtual void BringToFront() = 0;

    /** The content made `width` x `height` design pixels at the window's scale, the window's top left where it stands:
        a size the shell asked for - a panel that changed its own size, a size or a mode from the View submenu -
        followed as a JUCE Standalone's window follows its editor. Not held to the constraint again.
        @param width   the content's width, in design pixels
        @param height  the content's height, in design pixels */
    virtual void SetContentSize(int width, int height) = 0;

    /** The content made `width` x `height` design pixels at the window's scale, as SetContentSize makes it, its top
        left where it stands unless the window would then cross an edge of the work area it is most on now - what a
        menu bar, a dock or a task bar leave of its display - when it is moved in from that edge until the whole window
        stands on the area (WindowPlace.h's MovedOnto; a window larger than the area keeps its top left on it). A
        window maximised is restored first. The size taken is told to the window's `resized` (WindowOptions::resized),
        as a size the person dragged to is, on every platform, so whatever follows the person's resizes follows this
        too: an app's own choice of size, from a menu of sizes. Not held to the constraint. The default is
        SetContentSize, for a platform with no work area to keep to.
        @param width   the content's width, in design pixels
        @param height  the content's height, in design pixels */
    virtual void SetContentSizeOnScreen(int width, int height)
    {
        SetContentSize(width, height);
    }

    /** The largest content, in design units at the window's scale, the window may have and still stand wholly on the
        work area it is most on now, its title bar and borders included: what an app offering sizes of its own holds
        them to. {0, 0} where the platform cannot say (the default).
        @return  the content's largest width and height, in design units; {0, 0} for no limit known */
    virtual gfx::PointI LargestContent() const
    {
        return {0, 0};
    }

    /** The title bar's text made `title` - the window's name following what it shows, a document's or a game's -
        where the platform shows it: the window's title on macOS, its caption and the task bar's button on Windows,
        _NET_WM_NAME and WM_NAME on Linux. A window that is never given one keeps the title it was opened with.
        @param title  the text, UTF-8 */
    virtual void SetTitle(const std::string &title)
    {
        static_cast<void>(title); // a platform with no title bar
    }

    /** The title bar's text as the platform holds it, read back from it.
        @return  the text, UTF-8; empty where the platform has no title bar */
    virtual std::string Title() const
    {
        return {};
    }
};

/** What a window is opened with: its title, its content's design size, whether it is a dialog, and the calls the
    platform makes back as the person moves, resizes or closes it. */
struct WindowOptions
{
    std::string title;          ///< the title bar's text
    int         width{0};       ///< the content's design width, in design pixels
    int         height{0};      ///< the content's design height, in design pixels
    bool        dialog{false};  ///< a dialog: no minimise button, Escape closes it, it stays over its owner
    AppWindow  *owner{nullptr}; ///< the window a dialog belongs to
    std::optional<std::pair<int, int>> position; ///< where it stood last (AppWindow::Position); else centred
    std::function<void()>              closing;  ///< the person closed it: its close button, or Escape on a dialog
    /** The window moved to a display of another scale and was sized again for it, its content the design size times
        `scale` now (Windows' per-monitor scaling): its child window is to draw at that scale (SetScale). */
    std::function<void(float scale)> rescaled;

    /** A window the person may resize, by its frame or its corner, as a JUCE Standalone's window is resized round its
        editor's constrainer: the content size it may take for one proposed, in design units; null for a window of
        one size (a dialog). Asked as the window is dragged, where the platform asks (macOS, Windows) - on Windows for a
        size given it any other way too, a snap to a side of the screen - and for the least and the most the window may
        be (Linux, whose window manager takes limits and an aspect, not a function). */
    std::function<gfx::PointF(gfx::PointF proposed)> constrain;

    /** The aspect a resize holds the content to, the fixed height (`fixed_height`, a bar the content carries over what
        scales) left out: the width and the height it is the ratio of - the panel's design size, exactly - or {0, 0}
        for none. Read whenever the window is resized or its content's size set, for a window manager that takes an
        aspect as two whole numbers (Linux). */
    std::function<gfx::PointI()> aspect;
    int fixed_height{0}; ///< the height of the bar the content carries over what scales, in design pixels

    /** The person resized the window: its content is `width` x `height` design pixels now. */
    std::function<void(int width, int height)> resized;

    /** The window carries the app's menu bar (SetMenuBar) where the platform puts one on a window: on Windows the
        window's own bar, across its top above the content, which keeps its design size; nothing on macOS, whose bar
        is the screen's, nor on Linux, where the app places a drawn bar in its content (MakeMenuBarView). */
    bool menu_bar{false};
};

/** The platform under the Standalone as the app holds it: windows, a timer, a deferred call, the menu, the event loop
    and its end. The app makes one with Create() and everything on it is the main thread's but Post, and the queue
    Posts gives, which any thread may use. */
class AppPlatform
{
  public:
    /** The platform set up for the app named `name` - its menu, its icon in the Dock or the task bar - with
        `icon_png` its window icon where the platform takes one from the window (Linux).
        @param name      the app's name, as the menu and the task bar show it
        @param icon_png  the icon's PNG bytes, which must outlive the call; empty for none
        @return          the platform, owned by the caller */
    static std::unique_ptr<AppPlatform> Create(const std::string &name, std::span<const std::uint8_t> icon_png);

    virtual ~AppPlatform() = default;

    /** A window of `options`, shown; null where the platform will not make one (no display).
        @param options  what the window is opened with; its callbacks are kept for the window's life
        @return         the window, owned by the caller, or null for none */
    virtual std::unique_ptr<AppWindow> OpenWindow(const WindowOptions &options) = 0;

    /** The largest content, in design units, a window opened with `options` may have and still stand wholly on the
        work area it would open on - where it stood (`options.position`), else the main display's - its title bar and
        borders included. An app sizing its window from settings saved on a larger display holds the size to it before
        opening, so the window never reopens past the screen. {0, 0} where the platform cannot say.
        @param options  what the window would be opened with: its size, its place, whether it is a dialog, whether it
                        is resizable
        @return         the content's largest width and height, in design units; {0, 0} for no limit known */
    virtual gfx::PointI LargestContent(const WindowOptions &options) const
    {
        static_cast<void>(options);
        return {0, 0};
    }

    /** The run loop a child window in one of these windows runs on: the app's loop on Linux, where the child window
        has none of its own; null elsewhere.
        @return  the loop, owned by the caller, or null where the child window brings its own */
    virtual std::unique_ptr<window::RunLoop> ChildLoop() = 0;

    /** `tick` every `milliseconds`, until the platform goes.
        @param milliseconds  the period, in milliseconds
        @param tick          the call made on the main thread each period */
    virtual void StartTimer(int milliseconds, std::function<void()> tick) = 0;

    /** `call` after the event being handled, on the main thread.
        @param call  the call to make once, after the current event */
    virtual void CallSoon(std::function<void()> call) = 0;

    /** The queue any thread hands calls to the main thread through (Post): a worker's result, an engine's output. Its
        calls run on the main thread while Run runs - soon after each post, in the order posted, so the calls one
        thread posts run in that thread's order - and also while the platform's own modal loops run (a file dialog, a
        menu tracked, a window dragged). Calls posted before Run wait for it. When Run returns the queue closes for
        good: the calls still waiting are dropped unrun, and so is every later post, Run entered again included - so a
        worker the app joins after Run returns may post to its last moment and nothing of it runs on objects the app
        is tearing down. A worker may keep the queue itself, which outlives the platform: a post after the platform
        has gone is dropped too (core::PostQueue says how a dropped call is destroyed, and why a post from a
        destructor is safe). The platforms here each give one: a message posted to the platform's message-only window
        on Windows, a block dispatched to the main queue on macOS, a byte written to a pipe the loop polls on Linux.
        @return  the queue, shared with whoever keeps it; null for a platform with no cross-thread post (the default,
                 for a platform of a suite's own that gives none) */
    virtual std::shared_ptr<core::PostQueue> Posts()
    {
        return nullptr;
    }

    /** `call` on the main thread soon, from any thread: Posts()'s Post.
        @param call  what runs, once, on the main thread
        @return      true when queued; false when dropped - the platform has no queue, or the queue has closed (Run
                     returned) */
    bool Post(std::function<void()> call)
    {
        const std::shared_ptr<core::PostQueue> queue = Posts();
        return queue != nullptr && queue->Post(std::move(call));
    }

    /** `text` on the system's clipboard, for a copy the app makes of its own accord - a menu's Copy, with no view to
        ask (a view asks its window, view::Host::SetClipboardText). Held while the app runs: on Windows the platform's
        message-only window owns it, on macOS it is the general pasteboard's, and on Linux, where X keeps no clipboard
        of its own, the platform's connection to the display holds it and answers every paste from the loop until
        another copy takes it, and the child windows on the platform's loop copy through it too
        (window::RunLoop::SetClipboardText), so a copy made in a dialog outlives the dialog.
        @param text  the text, UTF-8
        @return      true when the clipboard took it; false where the platform has none (the default) or the system
                     refused */
    virtual bool SetClipboardText(const std::string &text)
    {
        (void)text;
        return false;
    }

    /** The system clipboard's text, for a paste the app makes of its own accord: this app's own copy where it holds
        the clipboard, else the clipboard's - on Linux asked of its owner, waited for at most half a second.
        @return  the text, UTF-8; empty when the clipboard holds none, or the platform has none (the default) */
    virtual std::string ClipboardText()
    {
        return {};
    }

    /** The app's menu bar, set or set anew (a tick, an item enabled), shown natively where the platform has a bar -
        the macOS menu bar, its menus after the app menu with Quit; the bar of each window opened with
        WindowOptions::menu_bar on Windows - and on Linux, which has none, by the drawn bars MakeMenuBarView makes. Each
        entry's shortcut chooses it from any of the app's windows while no menu is open, once the views have had the
        key: a key the view with the keyboard takes (a text view's Ctrl+C) is the view's, and one it does not is the
        bar's. Choosing an entry, by its click or its shortcut, runs its action and then tells `chosen` its id. On
        macOS the shortcuts show as the items' key equivalents, the Command key standing for the primary modifier; on
        Windows and Linux as "Ctrl+N" beside the item. Setting the bar replaces what SetMenu set, and SetMenu what this
        set. The default does nothing, for a platform with no bar.
        @param bar     the menus, left to right
        @param chosen  told, on the main thread, the id of each entry chosen that has one; may be empty */
    virtual void SetMenuBar(const MenuBar &bar, std::function<void(int id)> chosen)
    {
        (void)bar;
        (void)chosen;
    }

    /** Whether the platform shows SetMenuBar's bar itself: macOS and Windows. Where it does not (Linux, the default),
        the app places MakeMenuBarView's bar at the top of its window.
        @return  true when the platform's own bar shows the menus */
    virtual bool HasNativeMenuBar() const
    {
        return false;
    }

    /** A drawn menu bar showing SetMenuBar's menus, for the app to place across the top of its window
        (MenuBarView::PreferredHeight high), its choices and the shortcuts' one and the same: where the platform has no
        bar of its own (Linux). Null where it has (macOS, Windows), and for the default.
        @param style  the bar's look
        @return       the bar, owned by the caller, or null where the platform shows the menus itself */
    virtual std::unique_ptr<MenuBarView> MakeMenuBarView(const MenuBarStyle &style)
    {
        (void)style;
        return nullptr;
    }

    /** The app's commands in the platform's own menu, where it has one - the macOS menu bar, beside the app menu with
        Quit - each reporting its id to `chosen`; nothing elsewhere, where the window's bar holds them.
        @param items   the commands, in order
        @param chosen  called on the main thread with the id of the item the person chose */
    virtual void SetMenu(const std::vector<view::MenuItem> &items, std::function<void(int)> chosen) = 0;

    /** The event loop, until Quit.
        @return  the process's exit code */
    virtual int Run() = 0;

    /** The loop ends after the event being handled, Run returning. */
    virtual void Quit() = 0;

    /** What runs when the person asks the app to end from outside its windows (the app menu's Quit, the Dock), or the
        session ends (a log off, a shut down, a termination signal): the app's own ending, which writes what it keeps
        before it returns - the process may end then - and calls Quit.
        @param handler  the app's ending, called on the main thread */
    virtual void SetQuitHandler(std::function<void()> handler) = 0;
};
} // namespace reckless::app
