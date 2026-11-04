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

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <memory>
#include <string>
#include <thread>
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
