#pragma once

#include "AudioDevice.h"
#include "RealtimeThread.h"

namespace reckless::audio
{
/**
    A thread of a consumer's own at the standing of the audio callback's: joined by the thread itself as it starts
    (construct one on its stack first thing in the thread's function) and given back as it ends (the destructor, or
    Leave()). It is what a DAW's realtime worker threads, which render beside the device's callback inside its deadline,
    make at their start; the lower-urgency render-ahead threads do not.

    What the thread is given is the platform's and the API's: round-robin priority on ALSA, the multimedia scheduler's
    Pro Audio class on WASAPI and ASIO, the device's audio workgroup and a time-constraint policy on CoreAudio,
    JACK's own call on JACK - see AudioSystem::JoinAudioThread. Result() says which, or why not (with no stream open,
    or no `rtprio` limit on Linux), for the app's audio settings to show.

    Not realtime-safe, on purpose, and never used on the audio thread: it is made once per thread at its start and its
    end, so it may allocate. Thread-bound: construct, use and destroy on one thread. The system must outlive it and its
    stream stay open while it joins.
*/
class RealtimeThreadScope
{
  public:
    /** The calling thread joined to `system`'s stream.
        @param system  the audio system whose open stream's callback thread's standing is wanted */
    explicit RealtimeThreadScope(const AudioSystem &system) : result_(system.JoinAudioThread(membership_))
    {
    }

    RealtimeThreadScope(const RealtimeThreadScope &)            = delete;
    RealtimeThreadScope &operator=(const RealtimeThreadScope &) = delete;

    /** The standing given back, if it was joined. */
    ~RealtimeThreadScope() = default;

    /** What the join achieved and what it was refused.
        @return  the result, which stays as it was at the join after Leave() */
    const RealtimeThreadResult &Result() const
    {
        return result_;
    }

    /** The standing given back now, on the thread that joined; the destructor does nothing more. Twice is harmless. */
    void Leave() noexcept
    {
        membership_.Leave();
    }

  private:
    RealtimeThreadMembership membership_; ///< undoes the join; declared before the result it is filled by
    RealtimeThreadResult     result_;     ///< what the join said
};
} // namespace reckless::audio
