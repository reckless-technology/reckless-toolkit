#pragma once

// Linux's round-robin real-time scheduling for a thread, held in one place: the stream thread of the ALSA back end asks
// for it (AlsaDevice.cpp) and so does a consumer's own thread joining the stream's standing (AudioSystem::
// JoinAudioThread, on ALSA and, where libjack lacks the calls, on JACK). The priority is the back end's, once.
//
// Nothing here is realtime-safe, and nothing needs to be: each call is made once per thread, at its start or end.
#include <algorithm>
#include <cerrno>
#include <cstring>
#include <string>

#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>

#include "RealtimeThread.h"

namespace reckless::audio::linux_realtime
{
/// The round-robin real-time priority the ALSA stream's thread asks for, RtAudio's, held to what the system allows.
inline constexpr int kStreamThreadPriority = 70;

/** The priority a stream thread asks for: kStreamThreadPriority, or the system's highest where that is lower.
    @return  the priority */
inline int StreamThreadPriority()
{
    return std::min(kStreamThreadPriority, sched_get_priority_max(SCHED_RR));
}

/** The calling thread's round-robin real-time priority raised to `priority`, with what undoes it. Nothing happens when
    the system refuses, as it does a user with no `rtprio` limit (`ulimit -r` 0): the reason is the answer.
    @param priority    the SCHED_RR priority to ask for
    @param membership  receives the undo, which puts the thread's earlier policy and priority back, when it succeeds
    @param[out] error  what the system said when it did not, e.g. "Operation not permitted"; unchanged on success
    @return            true when the thread now runs at `priority` */
inline bool RaiseToRoundRobin(int priority, reckless::audio::RealtimeThreadMembership &membership, std::string &error)
{
    int         policy = SCHED_OTHER;
    sched_param before{};
    pthread_getschedparam(pthread_self(), &policy, &before);
    sched_param wanted{};
    wanted.sched_priority = priority;
    const int code        = pthread_setschedparam(pthread_self(), SCHED_RR, &wanted);
    if (code != 0)
    {
        error = std::strerror(code);
        rlimit limit{};
        if (code == EPERM && getrlimit(RLIMIT_RTPRIO, &limit) == 0 && limit.rlim_cur < static_cast<rlim_t>(priority))
        {
            error += ": the realtime priority limit (RLIMIT_RTPRIO, `ulimit -r`) is " +
                     (limit.rlim_cur == RLIM_INFINITY ? std::string("unlimited") : std::to_string(limit.rlim_cur)) +
                     ", below " + std::to_string(priority) +
                     "; raise it in /etc/security/limits.conf or run under rtkit";
        }
        return false;
    }
    membership.SetUndo([policy, before] {
        sched_param restored = before;
        pthread_setschedparam(pthread_self(), policy, &restored);
    });
    return true;
}

/** The calling thread given the policy and priority the ALSA back end gives its own stream thread, SCHED_RR at
    StreamThreadPriority(), as a result: refusal is "realtime priority refused: <why>".
    @param membership  receives the undo when the thread was raised
    @return            what was achieved or refused */
inline reckless::audio::RealtimeThreadResult JoinRoundRobin(reckless::audio::RealtimeThreadMembership &membership)
{
    reckless::audio::RealtimeThreadResult result;
    const int                             priority = StreamThreadPriority();
    std::string                           error;
    if (RaiseToRoundRobin(priority, membership, error))
    {
        result.is_joined = true;
        result.achieved  = "round-robin realtime priority " + std::to_string(priority);
    }
    else
    {
        result.refused = "realtime priority refused: " + error;
    }
    return result;
}
} // namespace reckless::audio::linux_realtime
