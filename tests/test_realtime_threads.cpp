// A consumer's own audio threads at the callback's standing (audio/RealtimeThread.h, RealtimeThreadScope.h): the
// refusal with no stream open, the scope's join and leave on a system that grants it, and the platform's own calls
// where this suite can make them - Linux's round-robin scheduling, refused on a machine whose `ulimit -r` is 0 and so
// on this one, and Windows' multimedia scheduler. A join on a real stream of each API is the device suite's
// (test_audio_devices.cpp).
#include <cstdint>
#include <cstdio>
#include <string>
#include <thread>

#include <gtest/gtest.h>

#include "AudioDevice.h"
#include "OfflineDevice.h"
#include "RealtimeThreadScope.h"

#if defined(__linux__)
#include "linux/RealtimeScheduling.h"
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#elif defined(_WIN32)
#include "win/MmcssThread.h"
#endif

namespace
{
using reckless::audio::AudioCallback;
using reckless::audio::OfflineAudioSystem;
using reckless::audio::RealtimeThreadMembership;
using reckless::audio::RealtimeThreadResult;
using reckless::audio::RealtimeThreadScope;
using reckless::audio::StreamRequest;

/// A callback that does nothing.
class SilentCallback final : public AudioCallback
{
  public:
    void Process(const float *const *, int, float *const *, int, int, std::int64_t, bool) noexcept override
    {
    }

    void StreamFailed() noexcept override
    {
    }
};

/// An open stream that grants the join, counting what it did.
class GrantingSystem final : public reckless::audio::AudioSystem
{
  public:
    std::string Name() const override
    {
        return "Granting";
    }

    std::vector<reckless::audio::DeviceInfo> Devices() override
    {
        return {};
    }

    bool Open(const StreamRequest &, AudioCallback &) override
    {
        return true;
    }

    bool Start() override
    {
        return true;
    }

    void Stop() override
    {
    }

    void Close() override
    {
    }

    bool IsOpen() const override
    {
        return true;
    }

    reckless::audio::StreamFormat Format() const override
    {
        return {};
    }

    std::string LastError() const override
    {
        return {};
    }

    RealtimeThreadResult JoinAudioThread(RealtimeThreadMembership &membership) const override
    {
        ++joins;
        membership.SetUndo([this] { ++leaves; });
        RealtimeThreadResult result;
        result.is_joined = true;
        result.achieved  = "a granted standing";
        return result;
    }

    mutable int joins{0};  ///< the joins made
    mutable int leaves{0}; ///< the leaves made
};
} // namespace

TEST(RealtimeThreads, AreRefusedWithAReasonWhenNoStreamIsOpen)
{
    const OfflineAudioSystem   system(2, 2);
    RealtimeThreadMembership   membership;
    const RealtimeThreadResult result = system.JoinAudioThread(membership);
    EXPECT_FALSE(result.is_joined);
    EXPECT_FALSE(membership.IsJoined());
    EXPECT_NE(result.refused.find("no stream is open"), std::string::npos) << result.refused;
    EXPECT_EQ(result.Describe(), result.refused);
}

TEST(RealtimeThreads, AreRefusedByAnApiThatGivesNothingAnOpenStreamToo)
{
    OfflineAudioSystem system(2, 2);
    SilentCallback     callback;
    StreamRequest      request;
    request.output_device = "Offline";
    request.output_count  = 2;
    ASSERT_TRUE(system.Open(request, callback)) << system.LastError();
    const RealtimeThreadScope scope(system);
    EXPECT_FALSE(scope.Result().is_joined);
    EXPECT_NE(scope.Result().refused.find("no realtime standing"), std::string::npos) << scope.Result().refused;
}

TEST(RealtimeThreadScope, JoinsOnConstructionAndLeavesOnceOnLeaveOrDestruction)
{
    GrantingSystem system;
    {
        RealtimeThreadScope scope(system);
        EXPECT_TRUE(scope.Result().is_joined);
        EXPECT_EQ(scope.Result().Describe(), "a granted standing");
        EXPECT_EQ(system.joins, 1);
        EXPECT_EQ(system.leaves, 0);
        scope.Leave();
        EXPECT_EQ(system.leaves, 1);
        scope.Leave(); // twice is harmless
        EXPECT_EQ(system.leaves, 1);
        EXPECT_TRUE(scope.Result().is_joined) << "the result stays as the join left it";
    }
    EXPECT_EQ(system.leaves, 1);
    {
        const RealtimeThreadScope scope(system);
    }
    EXPECT_EQ(system.joins, 2);
    EXPECT_EQ(system.leaves, 2) << "the destructor gives the standing back";
}

TEST(RealtimeThreadResult, DescribesWhatWasAchievedAndWhatWasRefused)
{
    RealtimeThreadResult result;
    EXPECT_EQ(result.Describe(), "no realtime standing given");
    result.is_joined = true;
    result.achieved  = "priority 70";
    EXPECT_EQ(result.Describe(), "priority 70");
    result.refused = "workgroup refused";
    EXPECT_EQ(result.Describe(), "priority 70 (workgroup refused)");
}

#if defined(__linux__)
namespace
{
/** What the calling thread's scheduling is now. */
void CurrentSchedule(int &policy, int &priority)
{
    sched_param param{};
    pthread_getschedparam(pthread_self(), &policy, &param);
    priority = param.sched_priority;
}
} // namespace

TEST(LinuxRealtimeScheduling, GivesTheAlsaPriorityOrRefusesWithTheSystemsReasonAndLeavesTheThreadAsItWas)
{
    std::thread([] {
        int policy_before   = 0;
        int priority_before = 0;
        CurrentSchedule(policy_before, priority_before);
        rlimit limit{};
        getrlimit(RLIMIT_RTPRIO, &limit);
        {
            RealtimeThreadMembership   membership;
            const RealtimeThreadResult result = reckless::audio::linux_realtime::JoinRoundRobin(membership);
            std::printf("round-robin join with RLIMIT_RTPRIO %llu: %s\n",
                        static_cast<unsigned long long>(limit.rlim_cur), result.Describe().c_str());
            if (result.is_joined)
            {
                int policy   = 0;
                int priority = 0;
                CurrentSchedule(policy, priority);
                EXPECT_EQ(policy, SCHED_RR);
                EXPECT_EQ(priority, reckless::audio::linux_realtime::StreamThreadPriority());
                EXPECT_TRUE(membership.IsJoined());
            }
            else
            {
                EXPECT_NE(result.refused.find("realtime priority refused: "), std::string::npos) << result.refused;
                EXPECT_FALSE(membership.IsJoined());
            }
        }
        int policy_after   = 0;
        int priority_after = 0;
        CurrentSchedule(policy_after, priority_after);
        EXPECT_EQ(policy_after, policy_before);
        EXPECT_EQ(priority_after, priority_before);
    }).join();
}

TEST(LinuxRealtimeScheduling, RefusesWithTheLimitNamedWhereNoRealtimePriorityIsAllowed)
{
    rlimit original{};
    ASSERT_EQ(getrlimit(RLIMIT_RTPRIO, &original), 0);
    rlimit none   = original;
    none.rlim_cur = 0;
    ASSERT_EQ(setrlimit(RLIMIT_RTPRIO, &none), 0);
    bool        was_granted = false;
    std::string refusal;
    std::thread([&] {
        RealtimeThreadMembership   membership;
        const RealtimeThreadResult result = reckless::audio::linux_realtime::JoinRoundRobin(membership);
        was_granted                       = result.is_joined;
        refusal                           = result.refused;
    }).join();
    setrlimit(RLIMIT_RTPRIO, &original);
    if (was_granted)
    {
        GTEST_SKIP() << "this process may set a realtime priority whatever the limit (root, or CAP_SYS_NICE)";
    }
    EXPECT_NE(refusal.find("realtime priority refused: "), std::string::npos) << refusal;
    EXPECT_NE(refusal.find("RLIMIT_RTPRIO"), std::string::npos) << refusal;
    std::printf("the refusal a person sees: %s\n", refusal.c_str());
}
#endif

#if defined(_WIN32)
TEST(WindowsRealtimeScheduling, JoinsTheProAudioClassOrFallsBackToThePriorityAndLeaves)
{
    std::thread([] {
        const int                  priority_before = GetThreadPriority(GetCurrentThread());
        RealtimeThreadMembership   membership;
        const RealtimeThreadResult result = reckless::audio::windows_realtime::JoinProAudio(membership);
        std::printf("MMCSS join: %s\n", result.Describe().c_str());
        EXPECT_TRUE(result.is_joined) << result.Describe();
        EXPECT_TRUE(membership.IsJoined());
        membership.Leave();
        EXPECT_EQ(GetThreadPriority(GetCurrentThread()), priority_before);
    }).join();
}
#endif
