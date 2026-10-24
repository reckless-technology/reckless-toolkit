#pragma once

// Test-only: a window of the platform's own for an editor to be attached to, as a host attaches a plugin's view to a
// window of its own - made off the screen, put on it and taken off it - so a suite can hold what an editor does as its
// window comes and goes (the core told whether a panel is on a screen, core::PanelWatch; a menu's popup gone with the
// editor's window). macOS's on AppKit (TestHostWindowMac.mm) and Linux's on X11 (TestHostWindowLinux.cpp: a window on
// the display the suite runs under - Xvfb on the Linux runner, through xvfb-run - and none without one), in the suites'
// reckless_test_host_window. Elsewhere it holds nothing and Valid() is false, and a suite skips what needs a window
// there - on Windows too: the runners are services on a window station that shows nothing, where a window is made and
// takes its messages and timers but is never visible (IsWindowVisible false whatever its style says), so nothing a
// suite holds of a window's showing could hold. A run that gives the suites a window says so (WindowRequired), and a
// suite with none then fails rather than skips: a window step whose suites all skipped would pass with nothing held.
#include <cstdlib>
#include <string_view>

namespace reckless::test
{
/** Whether the run says the suites have a window (RECKLESS_REQUIRE_WINDOW set, and not to 0): the Mac job's suites on
    its window server, the Linux job's window step under xvfb-run. A suite with no window then fails.
    @return  true when a window is required */
inline bool WindowRequired()
{
    // MSVC deprecates getenv for _dupenv_s, whose copy a test has no use for.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    const char *required = std::getenv("RECKLESS_REQUIRE_WINDOW");
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
    return required != nullptr && *required != '\0' && std::string_view(required) != "0";
}

/** A window of the platform's own, made off the screen for an editor to be attached to. Valid() is false where the
    platform has none here. */
class TestHostWindow
{
  public:
    /** A borderless window `width` x `height`, off the screen.
        @param width   the content width, in pixels
        @param height  the content height, in pixels */
    TestHostWindow(int width, int height);
    /** The window taken off the screen and destroyed, where there was one. */
    ~TestHostWindow();

    TestHostWindow(const TestHostWindow &)            = delete;
    TestHostWindow &operator=(const TestHostWindow &) = delete;

    /** Whether there is a window: false on a platform without one here, and on Linux without a display.
        @return  true when the window exists */
    bool Valid() const noexcept;

    /** The platform's handle an editor attaches to: the window's content view (an NSView), or the X window's id.
        @return  the handle, owned by this window; nullptr when there is no window */
    void *Parent() const noexcept;

    /** The window put on the screen (ordered in, mapped), or taken off it (ordered out, unmapped).
        @param on_screen  true to put it on the screen, false to take it off */
    void Show(bool on_screen);

    /** Whether a popup of the editor's - a menu - is on the screen: a visible borderless panel at the menus' level on
        macOS, a mapped override-redirect window on the display on Linux.
        @return  true when a popup is showing */
    bool PopupShowing() const;

    /** A scroll of `dy` pixels (up positive, in the content's direction) at the middle of the window, sent to the view
        an editor attached there as the system sends one, the device's direction inverted when `inverted` (macOS's
        natural scrolling): true once sent. macOS's alone, where the system says the device was inverted; false
        elsewhere.
        @param dy        the scroll, in pixels, up positive
        @param inverted  true when the device's direction is inverted, as natural scrolling does
        @return          true once sent */
    bool ScrollWheel(float dy, bool inverted);

  private:
    void *window_{nullptr}; ///< the platform's window, retained
};

#if !defined(__APPLE__) && !defined(__linux__)
inline TestHostWindow::TestHostWindow(int, int)
{
}

inline TestHostWindow::~TestHostWindow() = default;

inline bool TestHostWindow::Valid() const noexcept
{
    return false;
}

inline void *TestHostWindow::Parent() const noexcept
{
    return nullptr;
}

inline void TestHostWindow::Show(bool)
{
}

inline bool TestHostWindow::PopupShowing() const
{
    return false;
}

inline bool TestHostWindow::ScrollWheel(float, bool)
{
    return false;
}
#endif
} // namespace reckless::test
