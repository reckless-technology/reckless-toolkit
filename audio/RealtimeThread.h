#pragma once

#include <functional>
#include <string>
#include <utility>

namespace reckless::audio
{
/**
    What an audio thread of a consumer's own was given when it asked to join the stream's callback thread's standing
    (AudioSystem::JoinAudioThread, RealtimeThreadScope): what took and what did not, in words a person reads in an app's
    audio settings. Never an abort and never silent: a platform that refuses - Linux with no `rtprio` limit, the
    commonest case - says why here, and the thread goes on running as it was.

    Not realtime-safe, and not meant to be: a thread asks once at its start and gives it back once at its end, so
    these strings may allocate.
*/
struct RealtimeThreadResult
{
    bool        is_joined{false}; ///< true when the thread has some realtime standing it did not have: see `achieved`
    std::string achieved;         ///< what the thread was given, e.g. "round-robin priority 70"; empty when nothing
    std::string refused;          ///< what was asked and not given, and why; empty when nothing was refused

    /** The result as one line for a settings card.
        @return  `achieved`, then `refused` where there is any, e.g. "realtime priority refused: ..." */
    std::string Describe() const
    {
        if (is_joined && refused.empty())
        {
            return achieved;
        }
        if (is_joined)
        {
            return achieved + " (" + refused + ")";
        }
        return refused.empty() ? std::string("no realtime standing given") : refused;
    }
};

/**
    What a joined thread keeps to give its standing back: set by the back end that joined it, run by `Leave()` on the
    thread that joined. Move-only, and thread-bound: the platforms' calls it undoes (a scheduling policy, an MMCSS
    task, a workgroup) belong to the calling thread. RealtimeThreadScope holds one.
*/
class RealtimeThreadMembership
{
  public:
    RealtimeThreadMembership() = default;

    RealtimeThreadMembership(const RealtimeThreadMembership &)            = delete;
    RealtimeThreadMembership &operator=(const RealtimeThreadMembership &) = delete;

    /** What it holds, taken from `other`, which is then empty.
        @param other  the membership to take */
    RealtimeThreadMembership(RealtimeThreadMembership &&other) noexcept : undo_(std::exchange(other.undo_, nullptr))
    {
    }

    /** What it holds given back, then `other`'s taken.
        @param other  the membership to take
        @return       this */
    RealtimeThreadMembership &operator=(RealtimeThreadMembership &&other) noexcept
    {
        if (this != &other)
        {
            Leave();
            undo_ = std::exchange(other.undo_, nullptr);
        }
        return *this;
    }

    /** Left, if it was joined. */
    ~RealtimeThreadMembership()
    {
        Leave();
    }

    /** What a back end that joined a thread hands over to be undone; replaces nothing it still holds.
        @param undo  gives the standing back, called once, on the thread that joined */
    void SetUndo(std::function<void()> undo)
    {
        Leave();
        undo_ = std::move(undo);
    }

    /** The standing given back, on the calling thread, which must be the one that joined; nothing when it holds none,
        so it may be called twice. */
    void Leave() noexcept
    {
        if (undo_)
        {
            const std::function<void()> undo = std::exchange(undo_, nullptr);
            undo();
        }
    }

    /** @return  true while it holds a standing to give back */
    bool IsJoined() const
    {
        return static_cast<bool>(undo_);
    }

  private:
    std::function<void()> undo_; ///< gives the standing back; empty when none is held
};
} // namespace reckless::audio
