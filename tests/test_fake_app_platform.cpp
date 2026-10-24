// The suites' own app platform (tests/FakeAppPlatform.h) as an app's suite drives it: posts from other threads woken
// into its Run and run on the thread running it, in each thread's order, none once Run has returned nor once the
// platform has gone; its windows, timers, calls, menu and quit handler as a suite plays the person and the clock.
#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "FakeAppPlatform.h"

using reckless::test::FakeAppPlatform;
using reckless::test::FakeAppWindow;

TEST(FakeAppPlatform, PostsFromOtherThreadsRunInRunOnItsThreadAndNoneAfterItReturns)
{
    constexpr int   kThreads = 4;
    constexpr int   kEach    = 500;
    FakeAppPlatform platform;

    const std::thread::id         ui = std::this_thread::get_id();
    std::vector<std::vector<int>> seen(kThreads);
    int                           total     = 0;
    int                           elsewhere = 0;
    std::atomic<bool>             go{false};
    std::vector<std::thread>      threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t] {
            while (!go)
            {
                std::this_thread::yield();
            }
            for (int i = 0; i < kEach; ++i)
            {
                platform.Post([&, t, i] {
                    elsewhere += std::this_thread::get_id() != ui ? 1 : 0;
                    seen[static_cast<std::size_t>(t)].push_back(i);
                    if (++total == kThreads * kEach)
                    {
                        platform.Quit();
                    }
                });
            }
        });
    }
    // A post before Run waits for it.
    bool early = false;
    EXPECT_TRUE(platform.Post([&] {
        early = true;
        go    = true;
    }));
    EXPECT_FALSE(early) << "nothing runs before Run";
    EXPECT_EQ(platform.Run(), 0);
    for (std::thread &thread : threads)
    {
        thread.join();
    }
    EXPECT_TRUE(early);
    EXPECT_EQ(total, kThreads * kEach);
    EXPECT_EQ(elsewhere, 0);
    for (int t = 0; t < kThreads; ++t)
    {
        const std::vector<int> &got = seen[static_cast<std::size_t>(t)];
        ASSERT_EQ(got.size(), static_cast<std::size_t>(kEach));
        for (int i = 0; i < kEach; ++i)
        {
            ASSERT_EQ(got[static_cast<std::size_t>(i)], i);
        }
    }

    bool late = false;
    EXPECT_FALSE(platform.Post([&] { late = true; })) << "Run has returned: the queue is closed";
    EXPECT_EQ(platform.RunPosted(), 0u);
    EXPECT_FALSE(late);
}

TEST(FakeAppPlatform, AQueueAWorkerKeepsOutlivesThePlatformAndRefusesItsPosts)
{
    std::shared_ptr<reckless::core::PostQueue> kept;
    {
        FakeAppPlatform platform;
        kept = platform.Posts();
        ASSERT_NE(kept, nullptr);
        EXPECT_TRUE(kept->Post([] {}));
    }
    bool ran = false;
    EXPECT_FALSE(kept->Post([&] { ran = true; })) << "the platform has gone: dropped";
    EXPECT_FALSE(ran);
}

TEST(FakeAppPlatform, WindowsTimersCallsMenuAndQuitAreTheSuitesToPlay)
{
    FakeAppPlatform              platform;
    int                          closed  = 0;
    std::pair<int, int>          resized = {0, 0};
    reckless::app::WindowOptions options;
    options.title                                    = "Main";
    options.width                                    = 300;
    options.height                                   = 200;
    options.closing                                  = [&] { ++closed; };
    options.resized                                  = [&](int width, int height) { resized = {width, height}; };
    std::unique_ptr<reckless::app::AppWindow> window = platform.OpenWindow(options);
    ASSERT_NE(window, nullptr);
    ASSERT_EQ(platform.windows().size(), 1u);
    FakeAppWindow &fake = *platform.windows().front();
    EXPECT_EQ(fake.Handle(), window.get());
    EXPECT_EQ(fake.options().title, "Main");
    fake.Close();
    fake.Resize(320, 240);
    EXPECT_EQ(closed, 1);
    EXPECT_EQ(resized, (std::pair<int, int>{320, 240}));
    window->SetContentSize(400, 300);
    EXPECT_EQ(fake.options().width, 400);
    window->BringToFront();
    EXPECT_EQ(fake.brought_to_front(), 1);
    EXPECT_EQ(window->Position(), (std::pair<int, int>{0, 0}));
    EXPECT_EQ(window->Scale(), 1.0f);
    EXPECT_EQ(resized, (std::pair<int, int>{320, 240})) << "a size the shell follows is not told back";
    EXPECT_EQ(window->LargestContent(), (reckless::gfx::PointI{0, 0})) << "no work area given";
    window->SetContentSizeOnScreen(500, 350);
    EXPECT_EQ(resized, (std::pair<int, int>{500, 350})) << "an app's own size is told, as every platform tells it";
    EXPECT_EQ(window->Position(), (std::pair<int, int>{0, 0})) << "no work area: left where it stands";
    fake.work_area = reckless::gfx::RectI{100, 50, 600, 400};
    EXPECT_EQ(window->LargestContent(), (reckless::gfx::PointI{600, 400}));
    window->SetContentSizeOnScreen(550, 300);
    EXPECT_EQ(window->Position(), (std::pair<int, int>{100, 50})) << "moved onto the area";
    EXPECT_EQ(fake.on_screen_resizes(), 2);
    fake.work_area.reset();
    EXPECT_EQ(window->Title(), "Main");
    window->SetTitle("White vs Black - Main");
    EXPECT_EQ(fake.options().title, "White vs Black - Main") << "a title the app sets later";
    window.reset();
    EXPECT_TRUE(platform.windows().empty()) << "a window goes off the list as it goes";
    platform.refuse_windows = true;
    EXPECT_EQ(platform.OpenWindow(options), nullptr) << "no display: no window";

    int ticks = 0;
    platform.StartTimer(33, [&] { ++ticks; });
    platform.Tick();
    platform.Tick();
    EXPECT_EQ(ticks, 2);
    EXPECT_EQ(platform.timer_count(), 1u);

    std::vector<int> calls;
    platform.CallSoon([&] {
        calls.push_back(1);
        platform.CallSoon([&] { calls.push_back(2); });
    });
    EXPECT_EQ(platform.RunCalls(), 2);
    EXPECT_EQ(calls, (std::vector<int>{1, 2}));

    int chosen = 0;
    EXPECT_FALSE(platform.Choose(7)) << "no menu set";
    reckless::view::MenuItem settings;
    settings.text = "Settings...";
    settings.id   = 7;
    platform.SetMenu({settings}, [&](int id) { chosen = id; });
    ASSERT_EQ(platform.menu().size(), 1u);
    EXPECT_TRUE(platform.Choose(7));
    EXPECT_EQ(chosen, 7);

    int ended = 0;
    EXPECT_FALSE(platform.RequestQuit());
    platform.SetQuitHandler([&] { ++ended; });
    EXPECT_TRUE(platform.RequestQuit());
    EXPECT_EQ(ended, 1);
    EXPECT_TRUE(platform.SetClipboardText("e4"));
    EXPECT_EQ(platform.ClipboardText(), "e4") << "the fake's clipboard, never the system's";
    platform.clipboard = "another program's";
    EXPECT_EQ(platform.ClipboardText(), "another program's");
    EXPECT_EQ(platform.ChildLoop(), nullptr);
    EXPECT_EQ(platform.LargestContent(options), (reckless::gfx::PointI{0, 0}));
}

TEST(FakeAppPlatform, RunRunsTheCallsAfterTheCurrentEventAndQuitFromOneEndsIt)
{
    FakeAppPlatform platform;
    int             ran = 0;
    platform.CallSoon([&] {
        ++ran;
        platform.CallSoon([&] {
            ++ran;
            platform.Quit();
        });
    });
    EXPECT_EQ(platform.Run(), 0);
    EXPECT_EQ(ran, 2);
}
