#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <limits>
#include <mutex>
#include <thread>
#include <utility>
#include <vector>

#include "AudioDevice.h"
#include "MidiOutputQueue.h"

namespace reckless::audio
{
/** How long a MIDI output's sender sleeps at most before it looks at its queues again: a Send cannot wake it (that
    would be a call to the system), so this is how late, at worst, a message sent for a time already past - or nearer
    than the back end's lead - leaves. One millisecond: a thousand wakes a second of a thread that reads a few atomics.
 */
inline constexpr std::int64_t kMidiSenderPollNanoseconds = 1'000'000;

/**
    One open MIDI output as its sender sees it: the queue Send fills (MidiOutputQueue), the schedule the sender moves it
    into (MidiOutputSchedule), and what the platform does with a message once it is due (Deliver, the back end's). A
    back end derives its output from this and keeps it at one address while it is open, since its queue is what
    MidiOutputPorts hands Send. Gather and DeliverDue are the sender's thread's.
*/
class MidiOutputStream
{
  public:
    MidiOutputStream() = default;

    /** Nothing: a back end's own destructor releases what it holds. */
    virtual ~MidiOutputStream() = default;

    MidiOutputStream(const MidiOutputStream &)            = delete;
    MidiOutputStream &operator=(const MidiOutputStream &) = delete;

    /** The queue Send fills, for MidiOutputPorts::Publish.
        @return  the queue */
    MidiOutputQueue &queue() noexcept
    {
        return queue_;
    }

    /** The sender's thread: every message queued moved into the schedule, as far as it has room - a full schedule
        leaves the rest in the queue, which then fills and refuses Sends rather than lose what it holds. */
    void Gather() noexcept
    {
        MidiOutputMessage message;
        while (!schedule_.full() && queue_.TryPop(message))
        {
            schedule_.Add(message);
        }
    }

    /** The sender's thread: every message whose time is at or before `horizon` handed to Deliver, earliest first,
        stopping at one Deliver cannot take now (it is offered again on the sender's next round).
        @param horizon  the latest time handed on, in nanoseconds on NowNanoseconds' clock: now plus the back end's
                        lead
        @return         the time of the earliest message still held, or the largest time there is for none */
    std::int64_t DeliverDue(std::int64_t horizon) noexcept
    {
        while (!schedule_.empty() && schedule_.EarliestTime() <= horizon)
        {
            if (!Deliver(schedule_.Earliest()))
            {
                break;
            }
            schedule_.RemoveEarliest();
        }
        return schedule_.EarliestTime();
    }

    /** The owner's thread, with the sender stopped: everything queued and scheduled dropped, as a close drops it. */
    void Drop() noexcept
    {
        MidiOutputMessage message;
        while (queue_.TryPop(message))
        {
        }
        schedule_.Clear();
    }

  protected:
    /** The sender's thread: `message`, due by the sender's horizon, handed to the platform - sent now, or scheduled
        with its time where the platform schedules. A message the platform will never take (the device has gone) is
        dropped and counts as taken.
        @param message  the message, with the time it is to leave
        @return         true when it was taken; false only when the platform cannot take it yet, to be offered again */
    virtual bool Deliver(const MidiOutputMessage &message) noexcept = 0;

  private:
    MidiOutputQueue    queue_;    ///< what Send fills
    MidiOutputSchedule schedule_; ///< what the sender holds, earliest first
};

/**
    A MIDI back end's sender: one ordinary thread - never a realtime one - that serves every open output of a system.
    Each round it moves what each output's queue holds into that output's schedule, hands each output's messages due
    within the back end's lead to its Deliver, and sleeps until the earliest message left is within the lead, or for
    kMidiSenderPollNanoseconds at most, since a Send cannot wake it. No Send ever waits for it: a sender that falls
    behind leaves its queues to fill, and Send to return false.

    The owner starts it with the outputs to serve and stops it before the set changes; once Stop returns, no Deliver
    runs. Sleeping is WaitUntil, a condition variable's timed wait here, which a back end whose platform waits more
    finely (Windows: a high-resolution waitable timer) overrides with Wake; a back end that overrides them stops the
    sender in its own destructor.
*/
class MidiOutputSender
{
  public:
    /** A sender not yet started.
        @param lead_nanoseconds  how far ahead of its time a message is handed to Deliver: 0 for a platform that sends
                                 when it is handed one, more for one that schedules by the message's time */
    explicit MidiOutputSender(std::int64_t lead_nanoseconds) noexcept : lead_(lead_nanoseconds)
    {
    }

    /** Stopped, if it runs. */
    virtual ~MidiOutputSender()
    {
        Stop();
    }

    MidiOutputSender(const MidiOutputSender &)            = delete;
    MidiOutputSender &operator=(const MidiOutputSender &) = delete;

    /** The owner's thread: the thread started on `streams`, if there are any and it is not running.
        @param streams  the outputs to serve; each must stay open, at its address, until Stop */
    void Start(std::vector<MidiOutputStream *> streams)
    {
        if (thread_.joinable() || streams.empty())
        {
            return;
        }
        streams_ = std::move(streams);
        stopping_.store(false, std::memory_order_seq_cst);
        thread_ = std::thread([this] { Run(); });
    }

    /** The owner's thread: the thread told to stop, woken and joined - once this returns, no Deliver runs, until the
        next Start. */
    void Stop() noexcept
    {
        if (!thread_.joinable())
        {
            return;
        }
        {
            const std::lock_guard lock(mutex_);
            stopping_.store(true, std::memory_order_seq_cst);
        }
        Wake();
        thread_.join();
        streams_.clear();
    }

    /** Whether the thread runs.
        @return  true between Start and Stop */
    bool running() const noexcept
    {
        return thread_.joinable();
    }

  protected:
    /** The sender's thread: asleep until `deadline` on NowNanoseconds' clock, or until Stop wakes it.
        @param deadline  when to wake, in nanoseconds on NowNanoseconds' clock */
    virtual void WaitUntil(std::int64_t deadline)
    {
        const auto until = std::chrono::steady_clock::time_point(
            std::chrono::duration_cast<std::chrono::steady_clock::duration>(std::chrono::nanoseconds(deadline)));
        std::unique_lock lock(mutex_);
        woken_.wait_until(lock, until, [this] { return stopping_.load(std::memory_order_seq_cst); });
    }

    /** The owner's thread, from Stop, after it has said stop: a WaitUntil under way ended. */
    virtual void Wake()
    {
        woken_.notify_all();
    }

    /** Whether Stop has said stop; for a WaitUntil of a back end's own.
        @return  true once Stop has begun */
    bool stopping() const noexcept
    {
        return stopping_.load(std::memory_order_seq_cst);
    }

  private:
    /** The thread: rounds of gathering, delivering what is due within the lead, and sleeping, until Stop. */
    void Run()
    {
        while (!stopping_.load(std::memory_order_seq_cst))
        {
            const std::int64_t now      = NowNanoseconds();
            std::int64_t       earliest = std::numeric_limits<std::int64_t>::max();
            for (MidiOutputStream *stream : streams_)
            {
                stream->Gather();
                stream->DeliverDue(now + lead_);
                stream->Gather(); // what a full schedule left in the queue, now there is room
                earliest = std::min(earliest, stream->DeliverDue(now + lead_));
            }
            std::int64_t deadline = now + kMidiSenderPollNanoseconds;
            if (earliest != std::numeric_limits<std::int64_t>::max() && earliest - lead_ > now)
            {
                deadline = std::min(deadline, earliest - lead_);
            }
            WaitUntil(deadline);
        }
    }

    std::int64_t                    lead_;            ///< how far ahead of its time a message is handed on
    std::vector<MidiOutputStream *> streams_;         ///< the outputs served; not owned
    std::atomic<bool>               stopping_{false}; ///< true tells the thread to end
    std::mutex                      mutex_;           ///< what the wait and the stop meet on
    std::condition_variable         woken_;           ///< what Stop wakes a wait through
    std::thread                     thread_;          ///< the sender, while it runs
};
} // namespace reckless::audio
