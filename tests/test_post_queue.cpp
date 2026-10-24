// The queue other threads post calls to the UI thread through (core/PostQueue.h): calls from many threads all run on
// the thread that runs the queue, each thread's in its order; one wake a batch; nothing runs once the queue is closed,
// not the rest of a batch a call closed it in; and a call whose captures post as they are destroyed is safe however
// it ends - run, dropped by Close, refused by Post.
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "PostQueue.h"

namespace
{
using reckless::core::PostQueue;

/** Something a call captures whose destructor posts to a queue - an object saying its last word as it goes. */
struct PostsAsItGoes
{
    PostQueue        &queue;     ///< where it posts
    std::atomic<int> &destroyed; ///< counted as it goes
    std::atomic<int> &ran;       ///< counted when its last word runs
    std::atomic<int> &refused;   ///< counted when its last word is refused

    ~PostsAsItGoes()
    {
        ++destroyed;
        if (!queue.Post([&ran = ran] { ++ran; }))
        {
            ++refused;
        }
    }
};

/** A UI thread's loop for the suite: woken by the queue, it runs the batches until told to stop. */
class Loop
{
  public:
    explicit Loop(PostQueue &queue) : queue_(queue)
    {
        queue_.SetWake([this] {
            const std::lock_guard<std::mutex> lock(mutex_);
            woken_ = true;
            wake_.notify_all();
        });
    }

    /** Batches run on this thread until `done` says so, or ten seconds go by.
        @param done  asked after each batch
        @return      true when `done` said so in time */
    template <typename Done> bool RunUntil(Done done)
    {
        const auto give_up = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!done())
        {
            {
                std::unique_lock<std::mutex> lock(mutex_);
                if (!wake_.wait_until(lock, give_up, [this] { return woken_; }))
                {
                    return done();
                }
                woken_ = false;
            }
            queue_.RunPending();
        }
        return true;
    }

  private:
    PostQueue              &queue_;
    std::mutex              mutex_;
    std::condition_variable wake_;
    bool                    woken_{false};
};
} // namespace

TEST(PostQueue, CallsFromManyThreadsAllRunOnTheRunningThreadEachThreadsInItsOrder)
{
    constexpr int kThreads = 8;
    constexpr int kEach    = 2000;
    PostQueue     queue;
    Loop          loop(queue);

    const std::thread::id         ui = std::this_thread::get_id();
    std::vector<std::vector<int>> seen(kThreads); // touched on the UI thread alone
    int                           total     = 0;
    int                           elsewhere = 0;
    std::atomic<int>              refused{0};
    std::vector<std::thread>      threads;
    for (int t = 0; t < kThreads; ++t)
    {
        threads.emplace_back([&, t] {
            for (int i = 0; i < kEach; ++i)
            {
                if (!queue.Post([&, t, i] {
                        if (std::this_thread::get_id() != ui)
                        {
                            ++elsewhere;
                        }
                        seen[static_cast<std::size_t>(t)].push_back(i);
                        ++total;
                    }))
                {
                    ++refused;
                }
            }
        });
    }
    EXPECT_TRUE(loop.RunUntil([&] { return total == kThreads * kEach; })) << "every post ran in ten seconds";
    for (std::thread &thread : threads)
    {
        thread.join();
    }
    EXPECT_EQ(refused.load(), 0) << "an open queue refuses nothing";
    EXPECT_EQ(elsewhere, 0) << "every call ran on the thread running the queue";
    for (int t = 0; t < kThreads; ++t)
    {
        const std::vector<int> &got = seen[static_cast<std::size_t>(t)];
        ASSERT_EQ(got.size(), static_cast<std::size_t>(kEach)) << "thread " << t << "'s calls all ran, once";
        for (int i = 0; i < kEach; ++i)
        {
            ASSERT_EQ(got[static_cast<std::size_t>(i)], i) << "thread " << t << "'s calls in the order it posted them";
        }
    }
}

TEST(PostQueue, ThePostThatFindsTheQueueEmptyWakesItAndABatchIsWhatWaitedWhenItRan)
{
    PostQueue queue;
    int       wakes = 0;
    queue.SetWake([&] { ++wakes; });
    std::vector<int> order;
    EXPECT_TRUE(queue.Post([&] { order.push_back(1); }));
    EXPECT_TRUE(queue.Post([&] { order.push_back(2); }));
    EXPECT_TRUE(queue.Post([&] {
        order.push_back(3);
        queue.Post([&] { order.push_back(4); }); // posted while the batch runs: the next batch's
    }));
    EXPECT_EQ(wakes, 1) << "one wake for the three: the first found the queue empty";
    EXPECT_TRUE(queue.HasPending());
    EXPECT_EQ(queue.RunPending(), 3u) << "the batch is what waited when it ran";
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3}));
    EXPECT_EQ(wakes, 2) << "the post during the batch found the queue empty, and woke the loop again";
    EXPECT_EQ(queue.RunPending(), 1u);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 4}));
    EXPECT_FALSE(queue.HasPending());
    EXPECT_EQ(queue.RunPending(), 0u) << "nothing waiting, nothing run";
    EXPECT_FALSE(queue.Post(nullptr)) << "an empty call is no call";
    EXPECT_EQ(wakes, 2);
}

TEST(PostQueue, CloseDropsWhatWaitsUnrunAndEveryLaterPostIsRefused)
{
    PostQueue queue;
    int       wakes = 0;
    queue.SetWake([&] { ++wakes; });
    int  ran   = 0;
    auto alive = std::make_shared<int>(0);
    for (int i = 0; i < 5; ++i)
    {
        queue.Post([&ran, alive] { ++ran; });
    }
    EXPECT_EQ(alive.use_count(), 6) << "each waiting call holds its captures";
    queue.Close();
    EXPECT_TRUE(queue.closed());
    EXPECT_EQ(alive.use_count(), 1) << "the waiting calls are destroyed as the queue closes";
    EXPECT_FALSE(queue.HasPending());
    EXPECT_EQ(queue.RunPending(), 0u);
    EXPECT_FALSE(queue.Post([&ran, alive] { ++ran; })) << "a post to a closed queue is refused";
    EXPECT_EQ(alive.use_count(), 1) << "and its call destroyed there and then, on the posting thread";
    EXPECT_EQ(ran, 0) << "nothing ran: not before the close, not after";
    EXPECT_EQ(wakes, 1) << "no wake once closed";
    queue.Close(); // again: nothing more to do
    EXPECT_TRUE(queue.closed());
}

TEST(PostQueue, ACallThatClosesTheQueueDropsTheRestOfItsBatch)
{
    PostQueue        queue;
    std::vector<int> order;
    queue.Post([&] { order.push_back(1); });
    queue.Post([&] {
        order.push_back(2);
        queue.Close(); // the app quitting from inside a posted call
    });
    queue.Post([&] { order.push_back(3); });
    EXPECT_EQ(queue.RunPending(), 2u) << "the two before and including the close ran";
    EXPECT_EQ(order, (std::vector<int>{1, 2})) << "the call after the close never ran";
}

TEST(PostQueue, ACallWhoseCapturesPostAsTheyGoIsSafeRunDroppedOrRefused)
{
    std::atomic<int> destroyed{0};
    std::atomic<int> ran{0};
    std::atomic<int> refused{0};
    PostQueue        queue;

    // Run: the call is destroyed after it runs, outside the lock; its last word waits for the next batch.
    {
        auto last = std::shared_ptr<PostsAsItGoes>(new PostsAsItGoes{queue, destroyed, ran, refused});
        queue.Post([last] {});
    }
    EXPECT_EQ(destroyed.load(), 0) << "the waiting call holds it";
    EXPECT_EQ(queue.RunPending(), 1u);
    EXPECT_EQ(destroyed.load(), 1) << "destroyed once run";
    EXPECT_TRUE(queue.HasPending()) << "its last word queued, with no deadlock on the queue's lock";
    EXPECT_EQ(queue.RunPending(), 1u);
    EXPECT_EQ(ran.load(), 1);

    // Dropped: destroyed as the queue closes, outside the lock; its last word refused.
    {
        auto last = std::shared_ptr<PostsAsItGoes>(new PostsAsItGoes{queue, destroyed, ran, refused});
        queue.Post([last] {});
    }
    queue.Close();
    EXPECT_EQ(destroyed.load(), 2);
    EXPECT_EQ(refused.load(), 1) << "its last word refused by the closed queue, with no deadlock";

    // Refused: a post to the closed queue whose call posts as it is destroyed.
    {
        auto last = std::shared_ptr<PostsAsItGoes>(new PostsAsItGoes{queue, destroyed, ran, refused});
        EXPECT_FALSE(queue.Post([last] {}));
        EXPECT_EQ(destroyed.load(), 2) << "still held here, by `last`";
    }
    EXPECT_EQ(destroyed.load(), 3);
    EXPECT_EQ(refused.load(), 2);
    EXPECT_EQ(ran.load(), 1) << "nothing of the closed queue's ran";
}

TEST(PostQueue, NothingRunsOnceACloseFromAnotherThreadHasReturned)
{
    // Posts racing a close from another thread: whatever got in before the close may have run; once Close has
    // returned, nothing more does, and every later post is refused. The poster posts a bounded number, so a runner
    // slower than it - an unoptimised build counting every line - meets batches that stay bounded too.
    constexpr int    kPosts = 20000;
    PostQueue        queue;
    std::atomic<int> posted{0};
    std::atomic<int> refused{0};
    std::atomic<int> ran{0};
    std::thread      poster([&] {
        for (int i = 0; i < kPosts; ++i)
        {
            if (queue.Post([&] { ++ran; }))
            {
                ++posted;
            }
            else
            {
                ++refused;
            }
        }
    });
    while (posted.load() < kPosts / 4)
    {
        queue.RunPending();
        std::this_thread::yield();
    }
    std::thread closer([&] { queue.Close(); });
    closer.join();
    const int at_close = ran.load();
    for (int i = 0; i < 100; ++i)
    {
        EXPECT_EQ(queue.RunPending(), 0u);
    }
    poster.join();
    EXPECT_EQ(ran.load(), at_close) << "nothing ran once the close had returned";
    EXPECT_EQ(posted.load() + refused.load(), kPosts);
    EXPECT_FALSE(queue.Post([&] { ++ran; }));
}
