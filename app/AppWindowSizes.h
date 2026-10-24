#pragma once

#include <array>
#include <cstddef>

#include "AppPlatform.h"
#include "AppRoot.h"
#include "Geometry.h"

namespace reckless::app
{
/** The sizes a View menu offers, in percent of the layout's design size, smallest first. */
inline constexpr std::array<int, 5> kWindowSizes{50, 75, 100, 125, 150};

/** What a View menu shows of the window's size. */
struct WindowSizeMenu
{
    bool has_window = false; ///< a window to size: none in a suite or a render tool, where every size is dim
    int  in_force   = 0; ///< the size the window is drawn at, one of kWindowSizes; 0 where it was dragged to another
    std::array<bool, kWindowSizes.size()> fits{}; ///< whether each size's window stands wholly on the window's screen
    int larger  = 0; ///< the next size up that fits, from the size the window is now; 0 at the top
    int smaller = 0; ///< the next size down, likewise; 0 at the bottom

    /** Whether two states show the same menu.
        @param a  one state
        @param b  the other
        @return   whether every field is the same */
    friend bool operator==(const WindowSizeMenu &a, const WindowSizeMenu &b) = default;
};

/**
    A window's sizes from a View menu: the root held at one of kWindowSizes (AppRoot::SetFixedZoom) - the content laid
    out at its design size and drawn at exactly that scale - the window sized to it on its screen
    (AppWindow::SetContentSizeOnScreen), and the sizes its screen takes asked of the window (AppWindow::LargestContent)
    now and then, since it may be moved to another screen. A size the screen does not take is dim rather than shrunk,
    so a ticked size is always the size drawn; a window the person drags to another size has none in force. Where the
    size chosen is kept between launches is the app's: Restore opens at it, InForce says what to keep. Main thread.
*/
class WindowSizes
{
  public:
    /** How often the screen's room is asked again while the window is open, in seconds. */
    static constexpr double kRefreshSeconds = 0.5;

    /** The sizes of `root`'s window.
        @param root    the window's root; outlives this
        @param design  the layout's design size, which 100 % is, in design pixels */
    WindowSizes(AppRoot &root, gfx::PointI design);

    /** Before the window opens: the root held at `percent` and its bounds set to it, for the window to open round
        (RunApp holds a window to what its screen takes, which lets the size go where the screen is smaller).
        @param percent  one of kWindowSizes, as the app kept it; any other opens at 100 */
    void Restore(int percent);

    /** @param window  the window to size, once it is open; null as it closes */
    void SetWindow(AppWindow *window);

    /** A size chosen: the root held at it, the window sized to it and kept on its screen.
        @param percent  one of kWindowSizes
        @return         whether it was taken: not for a size not offered, with no window, or one the screen does not
                        take */
    bool Choose(int percent);

    /** One display frame: the screen's room asked again every kRefreshSeconds.
        @param seconds  since the last */
    void Frame(double seconds);

    /** @return  what the View menu shows now */
    WindowSizeMenu Menu() const;

    /** @return  the size the window is drawn at, one of kWindowSizes; 0 where it was dragged to another */
    int InForce() const;

    /** @param percent  a size
        @return         whether the View menu offers it */
    static bool IsOffered(int percent);

  private:
    /** @param percent  a size
        @return         the window's content at it: the design size times the percent, rounded, in design pixels */
    gfx::PointI SizeAt(int percent) const;

    /** @param percent  a size
        @return         whether its window stands wholly on the window's screen; true where that is not known */
    bool Fits(int percent) const;

    /** The screen's room asked of the window. */
    void RefreshRoom();

    AppRoot    &root_;              ///< the window's root
    gfx::PointI design_;            ///< the layout's design size
    AppWindow  *window_  = nullptr; ///< the window, while it is open
    gfx::PointI room_    = {0, 0};  ///< the largest content its screen takes; {0, 0} for not known
    double      elapsed_ = 0.0;     ///< seconds since the room was last asked
};
} // namespace reckless::app
