#pragma once

// The multimedia class scheduler's Pro Audio class for a thread, as the WASAPI stream thread takes it
// (WasapiDevice.cpp, Run), held for a consumer's own thread that joins the stream's standing
// (AudioSystem::JoinAudioThread, on WASAPI and ASIO). Where the scheduler refuses, the thread priority the stream
// thread falls back to: time-critical.
//
// Nothing here is realtime-safe, and nothing needs to be: each call is made once per thread, at its start or end. The
// caller links avrt.
#include <string>
#include <system_error>

#include <windows.h>

#include <avrt.h>

#include "RealtimeThread.h"

namespace reckless::audio::windows_realtime
{
/** The calling thread put in MMCSS's Pro Audio class (`AvSetMmThreadCharacteristicsW`); where that is refused,
    at THREAD_PRIORITY_TIME_CRITICAL, as the WASAPI stream thread falls back to. Undone on the same thread: the task
    reverted, or the earlier priority put back.
    @param membership  receives the undo
    @return            what was achieved or refused */
inline reckless::audio::RealtimeThreadResult JoinProAudio(reckless::audio::RealtimeThreadMembership &membership)
{
    reckless::audio::RealtimeThreadResult result;
    DWORD                                 task      = 0;
    HANDLE                                scheduler = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task);
    if (scheduler != nullptr)
    {
        membership.SetUndo([scheduler] { AvRevertMmThreadCharacteristics(scheduler); });
        result.is_joined = true;
        result.achieved  = "MMCSS Pro Audio class";
        return result;
    }
    const DWORD       why    = GetLastError();
    const std::string mmcss  = "MMCSS Pro Audio refused: " + std::system_category().message(static_cast<int>(why));
    const HANDLE      thread = GetCurrentThread();
    const int         before = GetThreadPriority(thread);
    if (before != THREAD_PRIORITY_ERROR_RETURN && SetThreadPriority(thread, THREAD_PRIORITY_TIME_CRITICAL) != 0)
    {
        membership.SetUndo([before] { SetThreadPriority(GetCurrentThread(), before); });
        result.is_joined = true;
        result.achieved  = "thread priority time-critical";
        result.refused   = mmcss;
        return result;
    }
    result.refused =
        mmcss + "; thread priority refused: " + std::system_category().message(static_cast<int>(GetLastError()));
    return result;
}
} // namespace reckless::audio::windows_realtime
