// The app's platform of this build (app/AppPlatform.h: an NSApplication on macOS, a Win32 message loop on Windows, an
// X11 connection on a poll loop on Linux) as an app holds it: Skia started for the processor as it is made; calls other
// threads post run on the main thread while Run runs, each thread's in its order, posts made before Run waiting for it;
// once Run has returned nothing posted runs, not even when the loop runs again and finds the wakes still queued for it.
// A window that did not ask never goes full screen; one that did goes over the whole of its display and comes back to
// its own place and size - on Windows a borderless popup over the monitor and its style and placement put back, on
// Linux the window manager's full screen or, with none (xvfb-run), the window's own over the screen. On macOS the
// change is AppKit's animation into a space of its own, which this suite does not run: a machine's spaces are the
// person's. On Linux the platform needs a display, so the suite runs under xvfb-run with the window suites and skips
// with none - failing instead where the run promises a window (RECKLESS_REQUIRE_WINDOW). On macOS Run activates the
// app, taking the focus, as the window suites do.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include "AppPlatform.h"
#include "SkiaCanvas.h"
#include "TestHostWindow.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#if defined(__linux__)
#include <dlfcn.h>
// Xlib last: its macros (None, Bool, Status, True) would reach into the headers above.
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#if __has_include(<X11/extensions/Xrandr.h>)
#include <X11/extensions/Xrandr.h>
/// Whether this build can set RandR's monitors up for the screens' test: RandR's header was there to build against.
#define RECKLESS_TEST_MONITORS 1
#else
#define RECKLESS_TEST_MONITORS 0
#endif
#endif

namespace
{
/** The platform for a test, or null - the test skipped, or failed where a window is required - where this machine
    gives it no loop: Linux with no display.
    @return  the platform, or null */
std::unique_ptr<reckless::app::AppPlatform> PlatformOrNull()
{
#if defined(__linux__)
    const char *display = std::getenv("DISPLAY");
    if (display == nullptr || *display == '\0')
    {
        return nullptr;
    }
#endif
    return reckless::app::AppPlatform::Create("RecklessPlatformTest", {});
}

#if !defined(__APPLE__) // a Mac runs no change of full screen here
/** The platform's loop run until `done` holds - at once where it holds already - or five seconds have gone: what a
    change a window manager makes in its own time takes.
    @param platform  the platform
    @param done      what is waited for
    @return          whether it came to hold */
bool RunUntil(reckless::app::AppPlatform &platform, const std::function<bool()> &done)
{
    if (done())
    {
        return true;
    }
    const auto started   = std::chrono::steady_clock::now();
    auto       is_active = std::make_shared<bool>(true); // a timer lives as long as the platform: this one stops here
    platform.StartTimer(10, [&platform, &done, started, is_active] {
        if (*is_active && (done() || std::chrono::steady_clock::now() - started > std::chrono::seconds(5)))
        {
            *is_active = false;
            platform.Quit();
        }
    });
    platform.Run();
    *is_active = false;
    return done();
}

/** The platform's loop run for `seconds`, for a window to be shown and settled: a window manager places a window in
    its own time.
    @param platform  the platform
    @param seconds   how long */
void RunFor(reckless::app::AppPlatform &platform, double seconds)
{
    const auto started = std::chrono::steady_clock::now();
    RunUntil(platform, [started, seconds] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count() >= seconds;
    });
}
#endif

#define REQUIRE_PLATFORM(platform)                                              \
    if (!(platform))                                                            \
    {                                                                           \
        if (reckless::test::WindowRequired())                                   \
        {                                                                       \
            FAIL() << "a window was promised, and the platform has no display"; \
        }                                                                       \
        GTEST_SKIP() << "no display for the platform's loop";                   \
    }
} // namespace

TEST(AppPlatform, MakingThePlatformStartsSkiaForTheProcessor)
{
    // Every app makes its platform first, before it draws anything, so the platform is where Skia is started for the
    // processor (gfx::InitialiseSkia: SkGraphics::Init, its AVX2 and AVX-512 routines) - never left to each app.
    // Nothing in this suite starts Skia but the platform, and this test comes first, so Skia is not started before it.
    EXPECT_FALSE(reckless::gfx::IsSkiaInitialised()) << "nothing but the platform starts Skia in this suite";
    const std::unique_ptr<reckless::app::AppPlatform> platform = PlatformOrNull();
    REQUIRE_PLATFORM(platform);
    EXPECT_TRUE(reckless::gfx::IsSkiaInitialised());
}

TEST(AppPlatform, AWindowsTitleIsTheOneItWasOpenedWithUntilTheAppSetsAnother)
{
    const std::unique_ptr<reckless::app::AppPlatform> platform = PlatformOrNull();
    REQUIRE_PLATFORM(platform);
    reckless::app::WindowOptions options;
    options.title                                          = "Reckless Platform Test";
    options.width                                          = 320;
    options.height                                         = 200;
    const std::unique_ptr<reckless::app::AppWindow> window = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr) << "the work area " << platform->LargestContent(options).x << " x "
                               << platform->LargestContent(options).y << " (0 x 0 with no display)";
    EXPECT_EQ(window->Title(), "Reckless Platform Test");
    const std::string game = "White vs Black \xe2\x80\x93 G\xc3\xa4me.pgn - Reckless Platform Test"; // UTF-8, whole
    window->SetTitle(game);
    EXPECT_EQ(window->Title(), game);
    window->SetTitle("Reckless Platform Test");
    EXPECT_EQ(window->Title(), "Reckless Platform Test") << "and back";
}

TEST(AppPlatform, AnAppsOwnSizeIsToldAsAResizeAndHeldToWhatTheScreenTakes)
{
    const std::unique_ptr<reckless::app::AppPlatform> platform = PlatformOrNull();
    REQUIRE_PLATFORM(platform);
    std::vector<std::pair<int, int>> told;
    reckless::app::WindowOptions     options;
    options.title     = "Reckless Platform Test";
    options.width     = 320;
    options.height    = 200;
    options.constrain = [](reckless::gfx::PointF proposed) { return proposed; };
    options.resized   = [&told](int width, int height) { told.emplace_back(width, height); };
    const std::unique_ptr<reckless::app::AppWindow> window = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);
    const reckless::gfx::PointI largest = window->LargestContent();
    ASSERT_GT(largest.x, 320) << "a display wider than the window";
    ASSERT_GT(largest.y, 200);
    window->SetContentSizeOnScreen(360, 240);
    ASSERT_FALSE(told.empty()) << "the size taken told to resized, as a person's resize is";
    EXPECT_EQ(told.back(), (std::pair<int, int>{360, 240}));
    // The largest content the screen takes: the window put where all of it stands on the screen, its title bar on it.
    window->SetContentSizeOnScreen(largest.x, largest.y);
    EXPECT_EQ(told.back(), (std::pair<int, int>{largest.x, largest.y}));
    EXPECT_EQ(window->LargestContent(), largest) << "on the same screen";
}

TEST(AppPlatform, AKeptSizeThatFitsTheScreenOpensWholeToThePixel)
{
    // An app reopening at a size it kept holds the size to what the screen takes (LargestContent(options), as RunApp
    // does) and opens at that: a size that fits must come back whole at the display's scale, or the window opens a
    // pixel short and the app's View menu ticks no size. At 120 dpi 885 design pixels are 1106, which a truncation
    // took back to 884. Every scale is the display's own here, so this proves the most on a display at 125 % or 175 %.
    const std::unique_ptr<reckless::app::AppPlatform> platform = PlatformOrNull();
    REQUIRE_PLATFORM(platform);
    reckless::app::WindowOptions options;
    options.title = "Reckless Platform Test";
    for (int width = 300; width < 340; ++width)
    {
        options.width                       = width;
        options.height                      = width - 100;
        const reckless::gfx::PointI largest = platform->LargestContent(options);
        if (largest.x == 0 && largest.y == 0)
        {
            GTEST_SKIP() << "no work area known on this platform";
        }
        EXPECT_EQ(largest, (reckless::gfx::PointI{options.width, options.height}))
            << "a small window fits every screen";
    }
    // Sudoku's 75 %, opened as RunApp opens it, on a screen with room to spare for it: the room is what the screen
    // takes of a window far larger than any screen.
    options.width                    = 4000;
    options.height                   = 4000;
    const reckless::gfx::PointI room = platform->LargestContent(options);
    if (room.x <= 885 || room.y <= 615)
    {
        GTEST_SKIP() << "a screen too small for 885 x 615 design pixels: " << room.x << " x " << room.y;
    }
    options.width                       = 885;
    options.height                      = 615;
    const reckless::gfx::PointI largest = platform->LargestContent(options);
    EXPECT_EQ(largest, (reckless::gfx::PointI{885, 615})) << "the kept size fits, so it comes back whole";
    std::vector<std::pair<int, int>> told;
    options.resized = [&told](int width, int height) { told.emplace_back(width, height); };
    options.width   = std::min(options.width, largest.x);
    options.height  = std::min(options.height, largest.y);
    const std::unique_ptr<reckless::app::AppWindow> window = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);
#if defined(_WIN32)
    // The client area the window opened with, in the monitor's pixels: the kept size at the window's scale.
    RECT client{};
    GetClientRect(static_cast<HWND>(window->Handle()), &client);
    const float scale = window->Scale();
    EXPECT_EQ(client.right - client.left, static_cast<LONG>(std::lround(885.0f * scale))) << "at " << scale;
    EXPECT_EQ(client.bottom - client.top, static_cast<LONG>(std::lround(615.0f * scale))) << "at " << scale;
#endif
    for (const auto &size : told)
    {
        EXPECT_EQ(size, (std::pair<int, int>{885, 615})) << "any size told is the one kept";
    }
}

TEST(AppPlatform, PostsFromManyThreadsRunOnTheMainThreadInEachThreadsOrderAndNoneAfterRunReturns)
{
    const std::unique_ptr<reckless::app::AppPlatform> platform = PlatformOrNull();
    REQUIRE_PLATFORM(platform);
    ASSERT_NE(platform->Posts(), nullptr) << "every platform here gives a queue";

    constexpr int kThreads = 8;
    constexpr int kEach    = 400;

    const std::thread::id         main_thread = std::this_thread::get_id();
    std::vector<std::vector<int>> seen(kThreads); // the main thread's alone
    int                           total     = 0;
    int                           elsewhere = 0;
    bool                          timed_out = false;
    std::atomic<int>              after_quit{0};
    std::atomic<bool>             quitting{false};
    std::atomic<bool>             go{false};

    // Before Run: a post waits for it, and lets the threads loose from inside it.
    bool early = false;
    ASSERT_TRUE(platform->Post([&] {
        early = true;
        go    = true;
    }));
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t] {
            while (!go)
            {
                std::this_thread::yield();
            }
            for (int i = 0; i < kEach; ++i)
            {
                platform->Post([&, t, i] {
                    if (quitting)
                    {
                        ++after_quit; // a call that ran once the queue should have closed
                    }
                    elsewhere += std::this_thread::get_id() != main_thread ? 1 : 0;
                    seen[static_cast<std::size_t>(t)].push_back(i);
                    if (++total == kThreads * kEach)
                    {
                        platform->Quit();
                    }
                });
            }
            // Posting on past the quit: these race the loop's end, and never run once it has ended.
            for (int i = 0; i < kEach; ++i)
            {
                platform->Post([] {});
            }
        });
    }
    platform->StartTimer(30000, [&] {
        timed_out = true;
        platform->Quit();
    });
    EXPECT_FALSE(early) << "nothing runs before Run";
    platform->Run();
    quitting = true;
    for (std::thread &thread : threads)
    {
        thread.join();
    }
    EXPECT_FALSE(timed_out) << "every post ran within thirty seconds";
    EXPECT_TRUE(early);
    EXPECT_EQ(total, kThreads * kEach);
    EXPECT_EQ(elsewhere, 0) << "every call ran on the main thread";
    for (int t = 0; t < kThreads; ++t)
    {
        const std::vector<int> &got = seen[static_cast<std::size_t>(t)];
        ASSERT_EQ(got.size(), static_cast<std::size_t>(kEach)) << "thread " << t << "'s calls all ran, once";
        for (int i = 0; i < kEach; ++i)
        {
            ASSERT_EQ(got[static_cast<std::size_t>(i)], i) << "thread " << t << "'s calls in the order it posted them";
        }
    }

    // After Run: a post is refused, and running the loop again - whose wakes for the calls posted past the quit may
    // still be queued for it - runs none of them.
    int late = 0;
    EXPECT_FALSE(platform->Post([&] { ++late; })) << "Run has returned: the queue is closed";
    platform->StartTimer(200, [&] { platform->Quit(); });
    platform->Run();
    EXPECT_EQ(late, 0);
    EXPECT_EQ(after_quit.load(), 0) << "nothing posted ran after the loop had ended";
    EXPECT_EQ(total, kThreads * kEach) << "nothing more of the first run's ran in the second";
}

TEST(AppPlatform, AQueueAWorkerKeepsOutlivesThePlatformAndRefusesItsPosts)
{
    std::shared_ptr<reckless::core::PostQueue> kept;
    {
        const std::unique_ptr<reckless::app::AppPlatform> platform = PlatformOrNull();
        REQUIRE_PLATFORM(platform);
        kept = platform->Posts();
        ASSERT_NE(kept, nullptr);
        EXPECT_TRUE(kept->Post([] {})) << "queued before Run";
    }
    bool ran = false;
    EXPECT_FALSE(kept->Post([&] { ran = true; })) << "the platform has gone: the post is dropped";
    EXPECT_FALSE(ran);
}

TEST(AppPlatform, AWindowThatDidNotAskNeverGoesFullScreen)
{
    const std::unique_ptr<reckless::app::AppPlatform> platform = PlatformOrNull();
    REQUIRE_PLATFORM(platform);
    int                          changes = 0;
    reckless::app::WindowOptions options;
    options.title                                          = "Reckless Platform Test";
    options.width                                          = 320;
    options.height                                         = 200;
    options.constrain                                      = [](reckless::gfx::PointF proposed) { return proposed; };
    options.full_screen_changed                            = [&changes](bool) { ++changes; };
    const std::unique_ptr<reckless::app::AppWindow> window = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);
    EXPECT_FALSE(window->CanGoFullScreen());
    EXPECT_FALSE(window->EnterFullScreen());
    EXPECT_FALSE(window->IsFullScreen());
    EXPECT_EQ(changes, 0);

    options.may_go_full_screen                             = true;
    options.dialog                                         = true;
    options.owner                                          = window.get();
    const std::unique_ptr<reckless::app::AppWindow> dialog = platform->OpenWindow(options);
    ASSERT_NE(dialog, nullptr);
    EXPECT_FALSE(dialog->CanGoFullScreen()) << "a dialog never goes full screen, whatever it asks";
}

TEST(AppPlatform, AWindowGoesFullScreenOverItsDisplayAndComesBackToItsOwnPlaceAndSize)
{
#if defined(__APPLE__)
    GTEST_SKIP() << "a Mac's full screen is AppKit's animation into a space of its own, which is the person's";
#else
    const std::unique_ptr<reckless::app::AppPlatform> platform = PlatformOrNull();
    REQUIRE_PLATFORM(platform);
    std::vector<std::pair<int, int>> told;
    std::vector<bool>                changes;
    reckless::app::WindowOptions     options;
    options.title               = "Reckless Platform Test";
    options.width               = 320;
    options.height              = 200;
    options.may_go_full_screen  = true;
    options.constrain           = [](reckless::gfx::PointF proposed) { return proposed; };
    options.resized             = [&told](int width, int height) { told.emplace_back(width, height); };
    options.full_screen_changed = [&changes](bool is_full_screen) { changes.push_back(is_full_screen); };
    const std::unique_ptr<reckless::app::AppWindow> window = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);
    ASSERT_TRUE(window->CanGoFullScreen());
    RunFor(*platform, 0.3);
    told.clear(); // whatever a window manager did as it took the window on
    const std::pair<int, int>   place   = window->Position();
    const reckless::gfx::PointI largest = window->LargestContent();
#if defined(_WIN32)
    const auto hwnd = static_cast<HWND>(window->Handle());
    RECT       windowed{};
    GetWindowRect(hwnd, &windowed);
    const LONG_PTR style = GetWindowLongPtrW(hwnd, GWL_STYLE);
#endif

    ASSERT_TRUE(window->EnterFullScreen());
    EXPECT_TRUE(window->IsFullScreen());
    EXPECT_FALSE(window->EnterFullScreen()) << "full screen already: nothing done";
    ASSERT_TRUE(RunUntil(*platform, [&] { return !changes.empty(); })) << "the change told";
    EXPECT_EQ(changes, (std::vector<bool>{true}));
    ASSERT_TRUE(RunUntil(*platform, [&] { return !told.empty() && told.back().first > 320; }))
        << "told as a resize at the display's size";
    EXPECT_GT(told.back().second, 200);
    EXPECT_EQ(window->Position(), place) << "where it stood, to come back to";
    EXPECT_EQ(window->LargestContent(), largest) << "what the display takes of a window, as before";
#if defined(_WIN32)
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    ASSERT_TRUE(GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &monitor));
    RECT covered{};
    GetWindowRect(hwnd, &covered);
    EXPECT_TRUE(EqualRect(&covered, &monitor.rcMonitor) != FALSE) << "the whole monitor, the task bar's part too";
    RECT client{};
    GetClientRect(hwnd, &client);
    EXPECT_EQ(client.right, monitor.rcMonitor.right - monitor.rcMonitor.left) << "no frame left round the content";
    EXPECT_EQ(client.bottom, monitor.rcMonitor.bottom - monitor.rcMonitor.top);
    EXPECT_EQ(GetWindowLongPtrW(hwnd, GWL_STYLE) & WS_CAPTION, 0) << "no title bar";
    const float scale = window->Scale();
    EXPECT_EQ(told.back(),
              (std::pair<int, int>{static_cast<int>(std::lround(static_cast<float>(client.right) / scale)),
                                   static_cast<int>(std::lround(static_cast<float>(client.bottom) / scale))}))
        << "the monitor's pixels in design units at its scale";
    // Held full screen for a person's look, or a screenshot's, where RECKLESS_TEST_FULL_SCREEN_HOLD gives the seconds:
    // nothing CI sets.
    if (char hold[32] = {}; GetEnvironmentVariableA("RECKLESS_TEST_FULL_SCREEN_HOLD", hold, sizeof(hold)) > 0)
    {
        RunFor(*platform, std::atof(hold));
    }
#endif

    ASSERT_TRUE(window->LeaveFullScreen());
    EXPECT_FALSE(window->IsFullScreen());
    ASSERT_TRUE(RunUntil(*platform, [&] { return changes.size() == 2; }));
    EXPECT_EQ(changes, (std::vector<bool>{true, false}));
    ASSERT_TRUE(RunUntil(*platform, [&] { return told.back() == std::pair<int, int>{320, 200}; }))
        << "its own size again, told as a resize";
    RunUntil(*platform, [&] { return window->Position() == place; });
    EXPECT_EQ(window->Position(), place) << "and its own place";
#if defined(_WIN32)
    RECT back{};
    GetWindowRect(hwnd, &back);
    EXPECT_TRUE(EqualRect(&back, &windowed) != FALSE) << "the frame it had, to the pixel";
    EXPECT_EQ(GetWindowLongPtrW(hwnd, GWL_STYLE), style) << "and the style";
#endif
#endif
}

#if defined(__linux__)
TEST(AppPlatform, WithNoDisplayThePlatformSaysItCannotTellTheLargestContentRatherThanEndingTheProcess)
{
    // An app asks how large its window may be before it opens one (AppRun), and a run with no display - an app's
    // --smoke under ssh, DISPLAY unset - reached the screen's work area through a null connection and ended on
    // SIGSEGV (Sentinel's app_smoke, jonny-precision, 2026-11-09). With no display there is no screen to fit: {0, 0},
    // which AppPlatform's contract says is no limit known.
    const char *const was  = std::getenv("DISPLAY");
    const std::string kept = was != nullptr ? was : "";
    const bool        had  = was != nullptr;
    unsetenv("DISPLAY");
    {
        const std::unique_ptr<reckless::app::AppPlatform> platform =
            reckless::app::AppPlatform::Create("RecklessPlatformTest", {});
        ASSERT_NE(platform, nullptr);
        reckless::app::WindowOptions options;
        options.width  = 320;
        options.height = 200;
        EXPECT_EQ(platform->LargestContent(options), (reckless::gfx::PointI{0, 0}));
        EXPECT_TRUE(platform->ScreenAreas().empty()) << "no display, so no screens to name";
    }
    if (had)
    {
        setenv("DISPLAY", kept.c_str(), 1);
    }
}

namespace
{
/** Whether a window manager runs on `display`: one that says it is there (_NET_SUPPORTING_WM_CHECK on the root).
    @param display  a connection of the test's own
    @return         true on a person's desktop; false on xvfb-run's bare server */
bool HasWindowManager(Display *display)
{
    Atom           type   = 0;
    int            format = 0;
    unsigned long  items  = 0;
    unsigned long  after  = 0;
    unsigned char *data   = nullptr;
    const bool     has =
        XGetWindowProperty(display, DefaultRootWindow(display), XInternAtom(display, "_NET_SUPPORTING_WM_CHECK", False),
                           0, 1, False, XA_WINDOW, &type, &format, &items, &after, &data) == Success &&
        data != nullptr && items == 1;
    if (data != nullptr)
    {
        XFree(data);
    }
    return has;
}
} // namespace

TEST(AppPlatform, AWindowPutBackWhereItStandsStaysAndOnePutElsewhereGoesThere)
{
    // SetPosition is Position's exact inverse - on Linux the client area's top left - so a place read and put back is
    // where the window stood, under a window manager that draws a title bar over it as under none, and a place inside
    // the main display's area is where the window goes, once the window manager has placed it.
    const std::unique_ptr<reckless::app::AppPlatform> platform = PlatformOrNull();
    REQUIRE_PLATFORM(platform);
    reckless::app::WindowOptions options;
    options.title                                          = "Reckless Platform Test";
    options.width                                          = 320;
    options.height                                         = 200;
    const std::unique_ptr<reckless::app::AppWindow> window = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);
    RunFor(*platform, 0.5); // shown, and placed by any window manager
    const std::pair<int, int> stood = window->Position();
    ASSERT_TRUE(window->SetPosition(stood.first, stood.second));
    RunFor(*platform, 0.5);
    EXPECT_EQ(window->Position(), stood) << "a place read and put back is where it was";

    const std::vector<reckless::gfx::RectI> areas = platform->ScreenAreas();
    ASSERT_FALSE(areas.empty()) << "a display has at least one screen";
    const reckless::gfx::RectI main_area = areas.front();
    const std::pair<int, int>  target{main_area.x() + main_area.width() / 4, main_area.y() + main_area.height() / 4};
    ASSERT_NE(target, stood) << "a place to go that is not where the window stands";
    ASSERT_TRUE(window->SetPosition(target.first, target.second));
    EXPECT_TRUE(RunUntil(*platform, [&window, target] { return window->Position() == target; }))
        << "where it went: " << window->Position().first << ", " << window->Position().second;
}

TEST(AppPlatform, TheScreensAreRandrsMonitorsThePrimaryFirstEachHeldToWhatTheWindowManagerLeaves)
{
    const std::unique_ptr<reckless::app::AppPlatform> platform = PlatformOrNull();
    REQUIRE_PLATFORM(platform);
    Display *const display = XOpenDisplay(nullptr);
    ASSERT_NE(display, nullptr);
    const ::Window                          root = DefaultRootWindow(display);
    const reckless::gfx::RectI              whole{0, 0, DisplayWidth(display, DefaultScreen(display)),
                                                  DisplayHeight(display, DefaultScreen(display))};
    const std::vector<reckless::gfx::RectI> areas = platform->ScreenAreas();
    ASSERT_FALSE(areas.empty()) << "a server with RandR 1.5 names its monitors";
    if (HasWindowManager(display))
    {
        // A person's desktop, whose monitors and panels are the person's: only read. Each area is a part of the root.
        for (const reckless::gfx::RectI &area : areas)
        {
            EXPECT_FALSE(area.IsEmpty());
            EXPECT_TRUE(whole.Contains(area))
                << area.x() << ", " << area.y() << " " << area.width() << " x " << area.height();
        }
        XCloseDisplay(display);
        return;
    }
    // xvfb-run's bare server: one monitor over the whole screen, and no window manager to take a panel off it.
    EXPECT_EQ(areas, (std::vector<reckless::gfx::RectI>{whole}));
#if RECKLESS_TEST_MONITORS
    // Two monitors set up within it, the left the primary, as two panels side by side are, and a window manager's
    // word on what its panels leave said on the root as one would say it: the left display's own work area, as Mutter
    // and Muffin publish one display by display, and the desktop's one, 40 pixels off the bottom. A monitor set up
    // lasts as long as the connection that set it up, so all of it goes with this connection at the end.
    void *const library = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL);
    const auto  set_monitor =
        reinterpret_cast<decltype(&XRRSetMonitor)>(library != nullptr ? dlsym(library, "XRRSetMonitor") : nullptr);
    if (set_monitor == nullptr)
    {
        XCloseDisplay(display);
        GTEST_SKIP() << "no libXrandr to set monitors up with";
    }
    const int                  half = whole.width() / 2;
    const reckless::gfx::RectI left{0, 0, half, whole.height()};
    const reckless::gfx::RectI right{half, 0, whole.width() - half, whole.height()};
    for (const auto &[name, area, is_primary] :
         {std::tuple{"RecklessTestLeft", left, true}, std::tuple{"RecklessTestRight", right, false}})
    {
        XRRMonitorInfo monitor{};
        monitor.name    = XInternAtom(display, name, False);
        monitor.primary = is_primary ? True : False;
        monitor.x       = area.x();
        monitor.y       = area.y();
        monitor.width   = area.width();
        monitor.height  = area.height();
        monitor.mwidth  = area.width() / 4; // a millimetre a monitor must have, at about 100 dpi
        monitor.mheight = area.height() / 4;
        set_monitor(display, root, &monitor);
    }
    XSync(display, False);
    std::vector<reckless::gfx::RectI> named = platform->ScreenAreas();
    ASSERT_EQ(named.size(), 3u) << "the two set up and the screen's own";
    EXPECT_EQ(named.front(), left) << "the primary first";
    EXPECT_NE(std::find(named.begin(), named.end(), right), named.end());
    EXPECT_NE(std::find(named.begin(), named.end(), whole), named.end());

    const reckless::gfx::RectI left_work{0, 0, half, whole.height() - 100};
    const reckless::gfx::RectI desktop_work{0, 0, whole.width(), whole.height() - 40};
    const auto                 say = [display, root](const char *name, reckless::gfx::RectI area) {
        const long values[4] = {area.x(), area.y(), area.width(), area.height()};
        XChangeProperty(display, root, XInternAtom(display, name, False), XA_CARDINAL, 32, PropModeReplace,
                        reinterpret_cast<const unsigned char *>(values), 4);
    };
    say("_GTK_WORKAREAS_D0", left_work);
    say("_NET_WORKAREA", desktop_work);
    XSync(display, False);
    named = platform->ScreenAreas();
    XDeleteProperty(display, root, XInternAtom(display, "_GTK_WORKAREAS_D0", False));
    XDeleteProperty(display, root, XInternAtom(display, "_NET_WORKAREA", False));
    XCloseDisplay(display); // and the monitors with it
    ASSERT_EQ(named.size(), 3u);
    EXPECT_EQ(named.front(), left_work) << "the left display's own work area, which only it lies wholly inside";
    EXPECT_NE(std::find(named.begin(), named.end(), right.Intersection(desktop_work)), named.end())
        << "the right display has none of its own: the part of the desktop's work area on it";
    EXPECT_NE(std::find(named.begin(), named.end(), desktop_work), named.end())
        << "and the screen's own monitor the same, not the left display's area that lies inside it";
#else
    XCloseDisplay(display);
#endif
}
#endif
