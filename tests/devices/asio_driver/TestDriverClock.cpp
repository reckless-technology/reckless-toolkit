// The ASIO test driver's clock: the 'hardware' of the SDK's sample driver (driver/asiosample, built as
// reckless_asio_test_driver for the device suite alone - never shipped), in place of the sample's own wintimer.cpp.
// The sample's thread sleeps a whole number of milliseconds a block - 5 for 256 frames, whatever the rate - and the
// scheduler's quantum on top, so its 'hardware' runs some way off the rate it reports, and a test that reads the rate
// by the clock could not judge it. This one keeps an absolute schedule: block n is due n block-lengths after the start,
// waited for on a high-resolution waitable timer, so the blocks come at the rate the stream opened at however late any
// one wait returns. It also lets the suite make the driver report what a real interface reports - an xrun (a resync
// request) and its own disappearance (a reset request, as a driver that loses its device or changes its buffer size
// sends) - through one exported function, reckless_asio_test_driver_request.
#include <windows.h>

#include "asiosmpl.h"

namespace
{
AsioSample    *g_driver    = nullptr; ///< the driver the clock drives, set by timerOn
ASIOCallbacks *g_callbacks = nullptr; ///< the host's callbacks, for the messages the suite asks the driver to send
double         g_block_seconds{0.0};  ///< one block's length, in seconds
HANDLE         g_thread  = nullptr;   ///< the clock's thread; nullptr when stopped
volatile LONG  g_done    = 0;         ///< 1 asks the clock's thread to finish
volatile LONG  g_request = 0;         ///< 1: an xrun (kAsioResyncRequest); 2: the device gone (kAsioResetRequest)

/** The clock's thread: one buffer switch per block on an absolute schedule, and the suite's requests sent first.
    @return        zero, once `g_done` is set */
DWORD WINAPI Clock(void *)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    const bool coarse = timer == nullptr; // a Windows before the high-resolution timer: the system's at 1 ms
    if (coarse)
    {
        timeBeginPeriod(1);
        timer = CreateWaitableTimerW(nullptr, TRUE, nullptr);
    }
    LARGE_INTEGER frequency, start, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    for (LONGLONG n = 1; InterlockedCompareExchange(&g_done, 0, 0) == 0; ++n)
    {
        const LONG request = InterlockedExchange(&g_request, 0);
        if (request == 1 && g_callbacks != nullptr)
        {
            g_callbacks->asioMessage(kAsioResyncRequest, 0, nullptr, nullptr);
        }
        else if (request == 2 && g_callbacks != nullptr)
        {
            g_callbacks->asioMessage(kAsioResetRequest, 0, nullptr, nullptr);
        }
        g_driver->bufferSwitch();

        const LONGLONG due = start.QuadPart + static_cast<LONGLONG>(static_cast<double>(n) * g_block_seconds *
                                                                    static_cast<double>(frequency.QuadPart));
        QueryPerformanceCounter(&now);
        if (due > now.QuadPart && timer != nullptr)
        {
            LARGE_INTEGER wait;
            wait.QuadPart = -static_cast<LONGLONG>(static_cast<double>(due - now.QuadPart) * 1e7 /
                                                   static_cast<double>(frequency.QuadPart));
            SetWaitableTimer(timer, &wait, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, INFINITE);
        }
    }
    if (timer != nullptr)
    {
        CloseHandle(timer);
    }
    if (coarse)
    {
        timeEndPeriod(1);
    }
    return 0;
}
} // namespace

/** The sample driver's system time, from the performance counter rather than the multimedia timer's milliseconds. */
void getNanoSeconds(ASIOTimeStamp *ts)
{
    LARGE_INTEGER frequency, now;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&now);
    const double nanoseconds = static_cast<double>(now.QuadPart) * 1e9 / static_cast<double>(frequency.QuadPart);
    const double two_to_32   = 4294967296.0;
    ts->hi                   = static_cast<unsigned long>(nanoseconds / two_to_32);
    ts->lo                   = static_cast<unsigned long>(nanoseconds - static_cast<double>(ts->hi) * two_to_32);
}

/// @cond
void AsioSample::timerOn()
{
    g_driver        = this;
    g_callbacks     = callbacks;
    g_block_seconds = static_cast<double>(blockFrames) / sampleRate;
    InterlockedExchange(&g_done, 0);
    InterlockedExchange(&g_request, 0);
    g_thread = CreateThread(nullptr, 0, &Clock, nullptr, 0, nullptr);
}

void AsioSample::timerOff()
{
    InterlockedExchange(&g_done, 1);
    if (g_thread != nullptr)
    {
        if (GetThreadId(g_thread) != GetCurrentThreadId())
        {
            WaitForSingleObject(g_thread, 2000);
        }
        CloseHandle(g_thread);
        g_thread = nullptr;
    }
}

/// @endcond

/** The suite's request of the running driver, made at its next block: 1 reports an xrun, 2 the device gone. */
extern "C" void reckless_asio_test_driver_request(int what)
{
    InterlockedExchange(&g_request, what);
}
