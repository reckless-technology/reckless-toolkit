// The child window (window/Window.h) where a suite can make one: on Linux, under xvfb-run, the files a desktop
// drags over an app's top-level window taken by the child window that fills it (ChildWindow::TakeDropsFor), through
// XDND's proxy - a drag source asks the top-level window under the pointer whether it takes files, never the windows
// inside it - the top-level marked as taking files with the child as its proxy, the child naming itself its own as the
// protocol asks of a proxy, the source's messages addressed to the top-level handed to the child, and its answers
// naming the window the source addressed. The suite plays the drag source on a connection of its own: the enter, a
// position, the paths when the child asks the selection for them, and the drop. On macOS, a scroll the system sends
// reaching the views with whether the system inverted the device's direction (natural scrolling), so a knob turns the
// way the wheel does. On Windows and on Linux, Ctrl with a letter reaching a text view as the letter with the primary
// modifier, as Command with a letter does on a Mac, so its Ctrl+A selects and its Ctrl+C copies - but not with Alt held
// too, where Ctrl+Alt may be AltGr. Elsewhere, and with no display, each is skipped; a run that promises a window
// (TestHostWindow's WindowRequired) fails instead. And on every platform, the paths a drag source's text/uri-list names
// (UriList.h), whatever the text holds.
#include <gtest/gtest.h>

#include <string>
#include <utility>
#include <vector>

#include "UriList.h"

TEST(ChildWindow, AUriListsFilePathsArePercentDecodedAndAStrayPercentIsItself)
{
    using reckless::window::PathsOfUriList;
    EXPECT_EQ(PathsOfUriList("file:///home/a/Room%20One.wav\r\nfile://machine/home/a/b%2Fc.nam\r\n"),
              (std::vector<std::string>{"/home/a/Room One.wav", "/home/a/b/c.nam"}))
        << "each line's path, decoded, past an empty host or this machine's";
    EXPECT_EQ(PathsOfUriList("# a comment\nhttp://example/x.wav\nfile:///a%c3%A9.wav\n"),
              (std::vector<std::string>{"/a\xc3\xa9.wav"}))
        << "comments and other schemes skipped, either case of hexadecimal digit";
    EXPECT_EQ(PathsOfUriList("file:///mixes/100%_wet.wav\r\n"), (std::vector<std::string>{"/mixes/100%_wet.wav"}))
        << "a '%' a drag source never escaped, followed by no hexadecimal digits: itself";
    EXPECT_EQ(PathsOfUriList("file:///a/1z%1z.wav\nfile:///a/end%\nfile:///a/end%4\nfile:///a/%41"),
              (std::vector<std::string>{"/a/1z%1z.wav", "/a/end%", "/a/end%4", "/a/A"}))
        << "half an escape, and a '%' at the line's end or one digit from it, themselves; a whole one at the end read";
}

#if defined(__linux__)
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "TestHostWindow.h"
#include "TextView.h"
#include "View.h"
#include "Window.h"

// Xlib last: its macros (None, Bool, Status) would reach into the headers above.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/keysym.h>

namespace
{
/** The run loop a Linux host gives a plug-in's window, turned by the suite: the window's connection read and its timer
    ticked on each turn. */
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

    void Turn()
    {
        if (ready_)
        {
            ready_(); // whatever the connection holds; nothing when it holds nothing
        }
        if (tick_)
        {
            tick_();
        }
    }

  private:
    std::function<void()> ready_;
    std::function<void()> tick_;
};

/** The panel, as far as a drop asks of one: it takes capture files anywhere, and writes down what it hears. */
class DropTarget final : public reckless::view::View
{
  public:
    bool IsInterestedInFiles(const std::vector<std::string> &paths) const override
    {
        for (const std::string &path : paths)
        {
            if (path.ends_with(".nam"))
            {
                return true;
            }
        }
        return false;
    }

    void FileDragEnter(const std::vector<std::string> &paths, reckless::gfx::PointF) override
    {
        log.push_back("enter " + paths.front());
    }

    void FileDragExit() override
    {
        log.push_back("exit");
    }

    void FilesDropped(const std::vector<std::string> &paths, reckless::gfx::PointF) override
    {
        log.push_back("drop " + paths.front());
    }

    std::vector<std::string> log;
};

/** A window-valued property of `window`, or 0 for none. */
::Window WindowProperty(Display *display, ::Window window, Atom property)
{
    Atom           type   = 0;
    int            format = 0;
    unsigned long  count = 0, after = 0;
    unsigned char *data  = nullptr;
    ::Window       value = 0;
    if (XGetWindowProperty(display, window, property, 0, 1, False, XA_WINDOW, &type, &format, &count, &after, &data) ==
            Success &&
        data != nullptr && count == 1 && format == 32)
    {
        value = static_cast<::Window>(*reinterpret_cast<unsigned long *>(data));
    }
    if (data != nullptr)
    {
        XFree(data);
    }
    return value;
}

/** An atom-typed 32-bit property of `window` as a number - XdndAware's version - or -1 for none. */
long AtomProperty(Display *display, ::Window window, Atom property)
{
    Atom           type   = 0;
    int            format = 0;
    unsigned long  count = 0, after = 0;
    unsigned char *data  = nullptr;
    long           value = -1;
    if (XGetWindowProperty(display, window, property, 0, 1, False, XA_ATOM, &type, &format, &count, &after, &data) ==
            Success &&
        data != nullptr && count == 1 && format == 32)
    {
        value = *reinterpret_cast<long *>(data);
    }
    if (data != nullptr)
    {
        XFree(data);
    }
    return value;
}
} // namespace

TEST(ChildWindow, TakesTheDropsDraggedOverTheTopLevelItIsTheProxyOf)
{
    reckless::test::TestHostWindow top(240, 160);
    if (!top.Valid())
    {
        if (reckless::test::WindowRequired())
        {
            FAIL() << "the run promises a window and there is no display";
        }
        GTEST_SKIP() << "no display";
    }
    top.Show(true);
    DropTarget  root;
    auto        loop  = std::make_unique<TurnedLoop>();
    TurnedLoop &turns = *loop;
    auto        child = reckless::window::CreateChildWindow(top.Parent(), root, 240, 160, 1.0f, std::move(loop));
    ASSERT_NE(child, nullptr);
    child->TakeDropsFor(top.Parent());
    turns.Turn();

    Display *display = XOpenDisplay(nullptr);
    ASSERT_NE(display, nullptr);
    const auto toplevel   = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(top.Parent()));
    const Atom aware_atom = XInternAtom(display, "XdndAware", False);
    const Atom proxy_atom = XInternAtom(display, "XdndProxy", False);
    // The child's connection is not this one: what it asked of the server is waited for, a few milliseconds at a time.
    for (int i = 0; i < 200 && WindowProperty(display, toplevel, proxy_atom) == 0; ++i)
    {
        turns.Turn();
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    ::Window tree_root = 0, tree_parent = 0, *children = nullptr;
    unsigned count = 0;
    ASSERT_NE(XQueryTree(display, toplevel, &tree_root, &tree_parent, &children, &count), 0);
    ASSERT_EQ(count, 1u) << "the child window in the top-level";
    const ::Window proxy = children[0];
    XFree(children);

    EXPECT_EQ(AtomProperty(display, toplevel, aware_atom), 5) << "the top-level takes drops, by XDND's fifth version";
    EXPECT_EQ(WindowProperty(display, toplevel, proxy_atom), proxy) << "through the child";
    EXPECT_EQ(WindowProperty(display, proxy, proxy_atom), proxy) << "which names itself its own proxy";

    // The drag source: a window of its own, owning the drag's selection, sending as a desktop's file manager sends -
    // to the proxy, the top-level named in each message.
    const ::Window source    = XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, 1, 1, 0, 0, 0);
    const Atom     selection = XInternAtom(display, "XdndSelection", False);
    const Atom     uri_list  = XInternAtom(display, "text/uri-list", False);
    const Atom     copy      = XInternAtom(display, "XdndActionCopy", False);
    XSetSelectionOwner(display, selection, source, CurrentTime);
    const auto send = [&](const char *type, long l1, long l2, long l3, long l4) {
        XEvent message{};
        message.xclient.type         = ClientMessage;
        message.xclient.window       = toplevel;
        message.xclient.message_type = XInternAtom(display, type, False);
        message.xclient.format       = 32;
        message.xclient.data.l[0]    = static_cast<long>(source);
        message.xclient.data.l[1]    = l1;
        message.xclient.data.l[2]    = l2;
        message.xclient.data.l[3]    = l3;
        message.xclient.data.l[4]    = l4;
        XSendEvent(display, proxy, False, NoEventMask, &message);
        XFlush(display);
    };

    // What the source hears: the selection asked for, answered with the file's URI; the child's answers.
    const std::string                file = "/tmp/reckless-dropped.nam";
    std::vector<XClientMessageEvent> heard;
    bool                             asked = false;
    const auto                       serve = [&] {
        while (XPending(display) > 0)
        {
            XEvent event;
            XNextEvent(display, &event);
            if (event.type == SelectionRequest)
            {
                const XSelectionRequestEvent &request = event.xselectionrequest;
                const std::string             uris    = "file://" + file + "\r\n";
                XChangeProperty(display, request.requestor, request.property, request.target, 8, PropModeReplace,
                                reinterpret_cast<const unsigned char *>(uris.data()), static_cast<int>(uris.size()));
                XEvent notify{};
                notify.xselection.type      = SelectionNotify;
                notify.xselection.requestor = request.requestor;
                notify.xselection.selection = request.selection;
                notify.xselection.target    = request.target;
                notify.xselection.property  = request.property;
                notify.xselection.time      = request.time;
                XSendEvent(display, request.requestor, False, NoEventMask, &notify);
                XFlush(display);
                asked = true;
            }
            else if (event.type == ClientMessage)
            {
                heard.push_back(event.xclient);
            }
        }
    };
    const auto settle = [&] {
        for (int i = 0; i < 40; ++i)
        {
            turns.Turn();
            serve();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };

    int      x = 0, y = 0;
    ::Window under = 0;
    XTranslateCoordinates(display, proxy, DefaultRootWindow(display), 20, 30, &x, &y, &under);
    send("XdndEnter", 5L << 24, static_cast<long>(uri_list), 0, 0);
    send("XdndPosition", 0, (static_cast<long>(x) << 16) | static_cast<long>(y), CurrentTime, static_cast<long>(copy));
    settle();
    EXPECT_TRUE(asked) << "the child asked the selection for the paths";
    ASSERT_FALSE(heard.empty()) << "the child answered the position";
    EXPECT_EQ(heard.front().message_type, XInternAtom(display, "XdndStatus", False));
    EXPECT_EQ(static_cast<::Window>(heard.front().data.l[0]), toplevel) << "naming the window the source addressed";
    EXPECT_EQ(heard.front().data.l[1] & 1, 1) << "taking the drag";
    EXPECT_EQ(root.log, (std::vector<std::string>{"enter " + file})) << "the panel told the files came";

    heard.clear();
    send("XdndDrop", 0, CurrentTime, 0, 0);
    settle();
    EXPECT_EQ(root.log, (std::vector<std::string>{"enter " + file, "drop " + file}));
    ASSERT_FALSE(heard.empty()) << "the child answered the drop";
    EXPECT_EQ(heard.back().message_type, XInternAtom(display, "XdndFinished", False));
    EXPECT_EQ(static_cast<::Window>(heard.back().data.l[0]), toplevel);
    EXPECT_EQ(heard.back().data.l[1] & 1, 1) << "taken";

    child.reset(); // before the top-level it sits in
    XDestroyWindow(display, source);
    XCloseDisplay(display);
}

TEST(ChildWindow, CtrlWithALetterReachesTheViewsAsTheLetterWithThePrimaryModifier)
{
    reckless::test::TestHostWindow top(240, 160);
    if (!top.Valid())
    {
        if (reckless::test::WindowRequired())
        {
            FAIL() << "the run promises a window and there is no display";
        }
        GTEST_SKIP() << "no display";
    }
    top.Show(true);
    reckless::view::View     root;
    reckless::view::TextView text{reckless::view::TextView::Style{}};
    root.SetBounds({240, 160});
    text.SetBounds({0, 0, 240, 160});
    root.AddChild(text);
    auto        loop  = std::make_unique<TurnedLoop>();
    TurnedLoop &turns = *loop;
    auto        child = reckless::window::CreateChildWindow(top.Parent(), root, 240, 160, 1.0f, std::move(loop));
    ASSERT_NE(child, nullptr);
    turns.Turn();
    std::vector<std::string> copied;
    text.SetText("1. c4 e5");
    text.on_copy = [&](std::string copy) { copied.push_back(std::move(copy)); };
    root.host()->RequestKeyboardFocus(&text);

    Display *display = XOpenDisplay(nullptr);
    ASSERT_NE(display, nullptr);
    const auto toplevel  = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(top.Parent()));
    ::Window   tree_root = 0, tree_parent = 0, *children = nullptr;
    unsigned   count = 0;
    ASSERT_NE(XQueryTree(display, toplevel, &tree_root, &tree_parent, &children, &count), 0);
    ASSERT_EQ(count, 1u) << "the child window in the top-level";
    const ::Window window = children[0];
    XFree(children);
    // A key pressed in the child window, as the server delivers one, then the loop turned until it is heard.
    const auto press = [&](KeySym symbol, unsigned state) {
        XEvent key{};
        key.xkey.type        = KeyPress;
        key.xkey.display     = display;
        key.xkey.window      = window;
        key.xkey.root        = DefaultRootWindow(display);
        key.xkey.state       = state;
        key.xkey.keycode     = XKeysymToKeycode(display, symbol);
        key.xkey.same_screen = True;
        XSendEvent(display, window, False, KeyPressMask, &key);
        XFlush(display);
        for (int i = 0; i < 20; ++i)
        {
            turns.Turn();
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    };
    press(XK_a, ControlMask); // select all
    press(XK_c, ControlMask); // copy
    EXPECT_EQ(copied, (std::vector<std::string>{"1. c4 e5"})) << "Ctrl+A then Ctrl+C, as Command+A and +C on a Mac";
    press(XK_c, ControlMask | Mod1Mask);
    EXPECT_EQ(copied.size(), 1u) << "not with Alt held too";
    child.reset();
    XCloseDisplay(display);
}
#else
TEST(ChildWindow, TakesTheDropsDraggedOverTheTopLevelItIsTheProxyOf)
{
    GTEST_SKIP() << "XDND is X11's: a drag on macOS and Windows reaches the view or window under it";
}
#endif

#if defined(_WIN32)
#include <memory>
#include <string>
#include <vector>

#include "TextView.h"
#include "View.h"
#include "Window.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

TEST(ChildWindow, CtrlWithALetterReachesTheViewsAsTheLetterWithThePrimaryModifier)
{
    // A window to sit in, never shown: the keys are sent to the child, whose views hear them as its loop would.
    HWND top = CreateWindowExW(0, L"STATIC", L"", WS_OVERLAPPEDWINDOW, 0, 0, 240, 160, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    ASSERT_NE(top, nullptr);
    reckless::view::View     root;
    reckless::view::TextView text{reckless::view::TextView::Style{}};
    root.SetBounds({240, 160});
    text.SetBounds({0, 0, 240, 160});
    root.AddChild(text);
    auto child = reckless::window::CreateChildWindow(top, root, 240, 160, 1.0f);
    ASSERT_NE(child, nullptr);
    HWND window = GetWindow(top, GW_CHILD);
    ASSERT_NE(window, nullptr);
    std::vector<std::string> copied;
    text.SetText("1. c4 e5");
    text.on_copy = [&](std::string copy) { copied.push_back(std::move(copy)); };
    root.host()->RequestKeyboardFocus(&text);

    // Ctrl held in this thread's keyboard state, which the window reads: a sent message carries none of its own.
    const auto hold = [](BYTE vk, bool down) {
        BYTE state[256]{};
        GetKeyboardState(state);
        state[vk] = down ? 0x80 : 0;
        SetKeyboardState(state);
    };
    hold(VK_CONTROL, true);
    SendMessageW(window, WM_KEYDOWN, 'A', 0); // select all
    SendMessageW(window, WM_KEYDOWN, 'C', 0); // copy
    hold(VK_MENU, true);
    SendMessageW(window, WM_KEYDOWN, 'C', 0);
    hold(VK_MENU, false);
    hold(VK_CONTROL, false);
    EXPECT_EQ(copied, (std::vector<std::string>{"1. c4 e5"}))
        << "Ctrl+A then Ctrl+C, as Command+A and +C on a Mac; not with Alt held too";
    SendMessageW(window, WM_KEYDOWN, 'C', 0);
    EXPECT_EQ(copied.size(), 1u) << "a letter alone is typing, which its WM_CHAR brings";
    child.reset();
    DestroyWindow(top);
}
#endif

#if defined(__APPLE__)
#include "TestHostWindow.h"
#include "View.h"
#include "Window.h"

namespace
{
/** The panel, as far as a wheel asks of one: it takes every scroll, and writes down each one's distance and whether
    the system inverted the device's direction. */
class WheelTarget final : public reckless::view::View
{
  public:
    bool MouseWheel(const reckless::view::MouseEvent &e) override
    {
        heard.emplace_back(e.wheel_y, e.wheel_reversed);
        return true;
    }

    std::vector<std::pair<float, bool>> heard;
};
} // namespace

TEST(ChildWindow, AScrollSaysWhetherTheSystemInvertedTheDevicesDirection)
{
    // Natural scrolling, macOS's default, inverts the device's direction and says so: the views hear it (a knob then
    // follows the wheel rather than the content, as on Windows and Linux), and a scroll from a device it leaves alone
    // says it is not.
    reckless::test::TestHostWindow top(240, 160);
    ASSERT_TRUE(top.Valid());
    WheelTarget root;
    root.SetBounds({240, 160});
    auto child = reckless::window::CreateChildWindow(top.Parent(), root, 240, 160, 1.0f);
    ASSERT_NE(child, nullptr);
    ASSERT_TRUE(top.ScrollWheel(7.0f, true)) << "the system marks a scroll inverted";
    ASSERT_TRUE(top.ScrollWheel(-3.0f, false));
    EXPECT_EQ(root.heard, (std::vector<std::pair<float, bool>>{{7.0f, true}, {-3.0f, false}}))
        << "each scroll's pixels as sent, and whether it was inverted";
    child.reset(); // before the window it sits in
}
#endif
