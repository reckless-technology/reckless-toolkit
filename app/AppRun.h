#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "AppPlatform.h"
#include "Geometry.h"
#include "View.h"
#include "Window.h"

namespace reckless::app
{
/** What the command line asks of an app's window, of what every app on the toolkit takes. */
struct AppOptions
{
    bool                     is_smoke{false}; ///< --smoke: a launch's check - the window shown for a few frames, closed
    int                      smoke_frames{30}; ///< --smoke [frames]: the frames the window must draw before it quits
    std::vector<std::string> unknown;          ///< the arguments not understood, in order, for the product's own
};

/** The options of a command line: `--smoke`, with an optional count of frames after it, and every other argument
    kept in `unknown` in order - the product reads its own flags from there and leaves what it does not know, which
    RunApp reports.
    @param arguments  the arguments after the program's name
    @return           the options */
AppOptions ParseAppOptions(const std::vector<std::string> &arguments);

/** Makes the child window a top-level window's views are drawn in: window::CreateChildWindow, or a suite's own where
    there is no display.
    @param parent  the top-level window's native handle
    @param root    the views to draw; outlive the child window
    @param width   the content's width, in design pixels
    @param height  the content's height, in design pixels
    @param scale   the window's units per design pixel
    @param loop    the run loop the child window runs on, or null for its own
    @return        the child window, or null where none can be made */
using ChildMaker = std::function<std::unique_ptr<window::ChildWindow>(
    void *parent, view::View &root, int width, int height, float scale, std::unique_ptr<window::RunLoop> loop)>;

/** What RunApp says of the app and its window: its name and version, and the hooks it calls. */
struct AppRunHooks
{
    /** Called once a display frame with the seconds since the last (view::FrameSteps); may be empty. */
    std::function<void(double seconds)> on_frame;
    /** Called as the loop ends with where the window stood, before it closes; may be empty. */
    std::function<void(std::optional<std::pair<int, int>> position)> on_end;
    /** Called once the window is open, with it, before the loop runs - the place to hand a product the window it set
        a menu or a dialog's owner by; may be empty. */
    std::function<void(AppWindow &window)> on_open;
    /** The window carries the app's menu bar (AppPlatform::SetMenuBar): opened with WindowOptions::menu_bar, so it
        shows the bar where the platform puts one on a window (Windows), and its child window handing on the keys its
        views do not take (ChildWindow::HandOnKeysTo), so the bar's shortcuts work whichever has the keyboard. */
    bool has_menu_bar{false};
};

/** An app on `platform`: its window, titled "<name>  v<version>", opened round `root` at the root's size (held to what
    fits the display it opens on, never under `minimum`) where it stood last (`position`) or centred, resizable down to
    `minimum`, its views drawn by `make_child`, the event loop run until the window is closed or the app is quit, the
    hooks called. Each argument the options did not understand is reported and ignored. A smoke run prints the name and
    the version, ends once the window has drawn its frames, and fails (3) where it drew none in twenty seconds.
    @param platform    the platform's windows and loop
    @param root        the window's views, sized to the window's content
    @param minimum     the smallest content the window may be resized to, in design pixels
    @param options     the command line's options
    @param name        the app's name, as the title bar, a smoke run and the messages give it
    @param version     the app's version, "MAJOR.MINOR.BUILD"
    @param make_child  makes the child window the views are drawn in
    @param hooks       what the app is told: each frame, the window opened, the loop's end
    @param position    where the window stood last, if known; else it is centred
    @return            the exit code: the loop's, 1 where no window could be shown, 3 for a smoke run that drew too
                       few frames */
int RunApp(AppPlatform &platform, view::View &root, gfx::PointI minimum, const AppOptions &options,
           const std::string &name, const std::string &version, const ChildMaker &make_child,
           const AppRunHooks &hooks = {}, std::optional<std::pair<int, int>> position = std::nullopt);
} // namespace reckless::app
