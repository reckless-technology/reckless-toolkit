#pragma once

#include <cstddef>
#include <deque>
#include <functional>
#include <mutex>
#include <utility>

namespace reckless::core
{
/**
    Calls any thread may hand to one thread - an app's UI thread - to run there soon: a worker's result, an engine's
    line of output. Post takes a call from any thread and queues it; the UI thread runs what is waiting (RunPending)
    when its loop is woken, in the order the calls were posted, so the calls one thread posts run in its order. The
    wake is the loop's own and is made once per batch: the post that finds the queue empty calls it - a message posted
    to a window, a block dispatched to the main queue, a byte written to a pipe the loop polls - and RunPending takes
    the whole batch at once, so a post during the batch finds the queue empty again and wakes the loop again.

    Closing the queue (Close: the loop ending, the platform going) drops every call still waiting, unrun, and every
    call posted after, which Post refuses: a call never runs once the queue is closed, not even the rest of the batch
    being run when a call in it closes the queue. A queue is held by a shared_ptr, so a worker that keeps one may post
    after the loop and the platform have gone, and its post is dropped.

    A call is destroyed outside the queue's lock, so a call whose captures post as they are destroyed - an object
    whose destructor posts its own last word - is safe however its call ends: run, dropped by Close, or refused by
    Post. A refused call is destroyed on the posting thread, a dropped or run one on the thread that closed or ran the
    queue. The wake runs under the lock, so it never runs once Close has returned: it must not block, nor post, nor
    call back into the queue.
*/
class PostQueue
{
  public:
    /** A call queued to run on the UI thread. */
    using Call = std::function<void()>;

    /** The wake: what tells the UI thread's loop that calls are waiting. The UI thread sets it before it hands the
        queue to any poster.
        @param wake  run, under the queue's lock, by the post that finds the queue empty; empty for none */
    void SetWake(std::function<void()> wake)
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        wake_ = std::move(wake);
    }

    /** `call` queued to run on the UI thread soon, after every call queued before it; any thread.
        @param call  what runs, once, on the UI thread; empty calls are dropped
        @return      true when queued; false when the queue is closed (or the call empty), and the call dropped -
                     destroyed on this thread, after the lock is let go */
    bool Post(Call call)
    {
        if (!call)
        {
            return false;
        }
        Call refused;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (!closed_)
            {
                const bool was_empty = calls_.empty();
                calls_.push_back(std::move(call));
                if (was_empty && wake_)
                {
                    wake_();
                }
                return true;
            }
            refused = std::move(call);
        }
        return false; // `refused` destroyed here, outside the lock
    }

    /** The UI thread: every call waiting when it is asked, run in order, unless the queue closes in one of them, which
        drops the rest. A call posted while the batch runs waits for the next batch, which its post woke the loop for.
        @return  how many calls ran */
    std::size_t RunPending()
    {
        std::deque<Call> batch;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            if (closed_)
            {
                return 0;
            }
            batch.swap(calls_);
        }
        std::size_t ran = 0;
        while (!batch.empty())
        {
            if (closed())
            {
                break; // closed by a call before: the rest are dropped, destroyed with `batch`
            }
            Call call = std::move(batch.front());
            batch.pop_front();
            call();
            ++ran;
        }
        return ran;
    }

    /** The queue closed for good: the calls waiting dropped, unrun, the wake let go, and every later post refused.
        Any thread; the dropped calls are destroyed on it, after the lock is let go. */
    void Close()
    {
        std::deque<Call>      dropped;
        std::function<void()> wake;
        {
            const std::lock_guard<std::mutex> lock(mutex_);
            closed_ = true;
            dropped.swap(calls_);
            wake.swap(wake_);
        }
    }

    /** Whether the queue is closed.
        @return  true once Close has run */
    bool closed() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return closed_;
    }

    /** Whether calls are waiting.
        @return  true when at least one call is queued and not yet taken by RunPending */
    bool HasPending() const
    {
        const std::lock_guard<std::mutex> lock(mutex_);
        return !calls_.empty();
    }

  private:
    mutable std::mutex    mutex_;         ///< guards everything below
    std::deque<Call>      calls_;         ///< the calls waiting, oldest first
    std::function<void()> wake_;          ///< tells the UI thread's loop that calls are waiting; may be empty
    bool                  closed_{false}; ///< true once Close has run
};
} // namespace reckless::core
