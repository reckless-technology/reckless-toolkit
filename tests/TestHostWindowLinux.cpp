// Test-only: TestHostWindow (TestHostWindow.h) on X11 - a window on the display the suite runs under, on a connection
// of its own, that an editor's child window goes in, mapped and unmapped as a host shows and hides its plugin window.
// With no display (DISPLAY unset, as in the Linux runner's plain suite run) there is no window, and a suite skips what
// needs one; under xvfb-run there is.
#include <X11/Xlib.h>

#include <cstdint>

#include "TestHostWindow.h"

namespace reckless::test
{
namespace
{
/** What `TestHostWindow::window_` points at: the connection and the window on it. */
struct HostWindow
{
    Display *display; ///< the connection of the window's own, closed with it
    ::Window window;  ///< the X window's id on that connection
};

/** An X error handler that swallows the error, for a call on a window that may already be gone.
    @return         zero, always */
int IgnoreErrors(Display *, XErrorEvent *)
{
    return 0;
}
} // namespace

TestHostWindow::TestHostWindow(int width, int height)
{
    Display *display = XOpenDisplay(nullptr);
    if (display == nullptr)
    {
        return;
    }
    const ::Window window =
        XCreateSimpleWindow(display, DefaultRootWindow(display), 0, 0, static_cast<unsigned>(width > 0 ? width : 1),
                            static_cast<unsigned>(height > 0 ? height : 1), 0, 0, 0);
    XSync(display, False);
    window_ = new HostWindow{display, window};
}

TestHostWindow::~TestHostWindow()
{
    if (window_ != nullptr)
    {
        auto *host = static_cast<HostWindow *>(window_);
        XDestroyWindow(host->display, host->window);
        XCloseDisplay(host->display);
        delete host;
        window_ = nullptr;
    }
}

bool TestHostWindow::Valid() const noexcept
{
    return window_ != nullptr;
}

void *TestHostWindow::Parent() const noexcept
{
    if (window_ == nullptr)
    {
        return nullptr;
    }
    return reinterpret_cast<void *>(static_cast<std::uintptr_t>(static_cast<const HostWindow *>(window_)->window));
}

void TestHostWindow::Show(bool on_screen)
{
    if (window_ == nullptr)
    {
        return;
    }
    auto *host = static_cast<HostWindow *>(window_);
    if (on_screen)
    {
        XMapWindow(host->display, host->window);
    }
    else
    {
        XUnmapWindow(host->display, host->window);
    }
    XSync(host->display, False); // the server has it before the editor's own connection asks
}

bool TestHostWindow::PopupShowing() const
{
    if (window_ == nullptr)
    {
        return false;
    }
    auto *host = static_cast<const HostWindow *>(window_);
    // The editor's connection destroys its windows on its own: one listed by the tree may be gone by the time it is
    // asked about, a BadWindow error, which Xlib's default handler answers by ending the process. Errors are taken as
    // nothing while the tree is read; the handler is the process's, and the suite runs on one thread.
    XSync(host->display, False);
    const XErrorHandler previous = XSetErrorHandler(IgnoreErrors);
    ::Window            root     = 0;
    ::Window            parent   = 0;
    ::Window           *children = nullptr;
    unsigned            count    = 0;
    bool                showing  = false;
    if (XQueryTree(host->display, DefaultRootWindow(host->display), &root, &parent, &children, &count) != 0)
    {
        // A popup's window sits at the top of the display's tree with nothing between it and the screen; with no
        // window manager under Xvfb, every other top-level window is a plain one.
        for (unsigned i = 0; i < count && !showing; ++i)
        {
            XWindowAttributes attributes{};
            showing = XGetWindowAttributes(host->display, children[i], &attributes) != 0 &&
                      attributes.override_redirect == True && attributes.map_state == IsViewable;
        }
        if (children != nullptr)
        {
            XFree(children);
        }
    }
    XSync(host->display, False); // every error of the questions asked lands under the handler
    XSetErrorHandler(previous);
    return showing;
}

bool TestHostWindow::ScrollWheel(float, bool)
{
    return false; // X11's wheel is a button, and no system inverts it
}
} // namespace reckless::test
