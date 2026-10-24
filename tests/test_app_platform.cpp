// The app's platform of this build (app/AppPlatform.h: an NSApplication on macOS, a Win32 message loop on Windows, an
// X11 connection on a poll loop on Linux) as an app holds it: calls other threads post run on the main thread while Run
// runs, each thread's in its order, posts made before Run waiting for it; once Run has returned nothing posted runs,
// not even when the loop runs again and finds the wakes still queued for it. On Linux the platform needs a display, so
// the suite runs under xvfb-run with the window suites and skips with none - failing instead where the run promises a
// window (RECKLESS_REQUIRE_WINDOW). On macOS Run activates the app, taking the focus, as the window suites do.
#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "AppPlatform.h"
#include "TestHostWindow.h"

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
