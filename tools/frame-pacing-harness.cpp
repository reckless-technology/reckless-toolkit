// A hand-run harness for the numbers in docs/window/frame-pacing.md: it measures the mechanisms a child window
// paces its frame callback on, so the figures there can be taken again on any machine when the mechanism changes.
// Nothing asserts anything here - a timing distribution is not a pass or a fail - and nothing of Skia, the views or
// the window is needed, so it builds in seconds.
//
// It is built with the suites (reckless_frame_pacing_harness), and on a machine with no configured build tree one
// command is enough:
//
//   Windows   cl /nologo /std:c++20 /O2 /EHsc /W4 /I window tools/frame-pacing-harness.cpp user32.lib dwmapi.lib
//   Linux     g++ -std=c++20 -O2 -Wall -Wextra -I window tools/frame-pacing-harness.cpp -o frame-pacing-harness
//   macOS     clang++ -std=c++20 -O2 -Wall -Wextra -I window tools/frame-pacing-harness.cpp -o frame-pacing-harness
//
// What it runs, named as arguments or all of them with none:
//
//   clock                     the system clock's increment and the timer resolution in force, and what the
//                             compositor says about its own: the grid everything else lands on
//   platform-timer            the platform's own frame timer, exactly as the window arranges it: the baseline
//   platform-timer-sweep      the mean interval against the period asked for, which is what explains the baseline
//   platform-timer-schedule   whether the platform timer's schedule is absolute or re-armed at each expiry
//   display-clock             the display's own clock, as window/win/WindowWin.cpp waits on it (Windows only)
//
// The display clock's measurement is Windows' alone. macOS's is a display link, whose ticks come from the window
// server and are held by the child window's own suite; Linux has no display clock at all, and the platform-timer
// run there measures the app loop's own deadline, which is what the toolkit paces on.
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <vector>

#include "FramePacing.h"

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

// dwmapi.h and timeapi.h after windows.h, which they need for HWND and the basic types.
#include <dwmapi.h>
#include <timeapi.h>
#else
#include <chrono>
#include <thread>
#endif

namespace
{
using reckless::window::FrameClock;
using reckless::window::OnTheRefreshGrid;

/** The frames each run measures. */
constexpr int kFramesMeasured = 2000;

/** The period, in milliseconds, the toolkit's platform timer asks for: window/win/WindowWin.cpp's and
    window/linux/WindowLinux.cpp's kFrameMilliseconds. Measured, not changed. */
constexpr int kFrameMilliseconds = 16;

/** The clock now, in seconds: the performance counter on Windows, the steady clock elsewhere.
    @return  the clock, in seconds */
double NowSeconds() noexcept
{
#if defined(_WIN32)
    static const double tick_seconds = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart > 0 ? 1.0 / static_cast<double>(frequency.QuadPart) : 0.0;
    }();
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * tick_seconds;
#else
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
}

/** What a run of intervals came to. */
struct Figures
{
    double mean{};    ///< the mean interval, in milliseconds
    double sd{};      ///< its standard deviation, in milliseconds
    double lowest{};  ///< the shortest interval
    double median{};  ///< the middle interval
    double highest{}; ///< the longest interval
    double hertz{};   ///< the frames a second the mean comes to
};

/** The figures of `milliseconds`.
    @param milliseconds  the intervals measured, which are sorted in place
    @return              the figures; all zero for an empty run */
Figures FiguresOf(std::vector<double> &milliseconds)
{
    Figures figures{};
    if (milliseconds.empty())
    {
        return figures;
    }
    double sum = 0.0;
    for (double one : milliseconds)
    {
        sum += one;
    }
    figures.mean  = sum / static_cast<double>(milliseconds.size());
    double square = 0.0;
    for (double one : milliseconds)
    {
        square += (one - figures.mean) * (one - figures.mean);
    }
    figures.sd    = std::sqrt(square / static_cast<double>(milliseconds.size()));
    figures.hertz = figures.mean > 0.0 ? 1000.0 / figures.mean : 0.0;
    std::sort(milliseconds.begin(), milliseconds.end());
    figures.lowest  = milliseconds.front();
    figures.median  = milliseconds[milliseconds.size() / 2];
    figures.highest = milliseconds.back();
    return figures;
}

/** The share of `milliseconds` that are a whole number of refresh periods, within `within` of one - the figure that
    says whether each frame drawn is shown for a whole number of refreshes, which is what the eye reads as smooth.

    The tolerance matters, and two of them are reported. A tenth of a period is the one the audit this was built on
    used, and on a 240 Hz panel it is safe: the system clock's 15.625 ms scan is 3.75 refresh periods there, nowhere
    near whole. On a 60 Hz panel the two are only 6 % apart, so a tenth of a period cannot tell one scan from one
    refresh and flatters the platform timer; a thirtieth can.
    @param milliseconds    the intervals measured
    @param period_seconds  the display's refresh period; 0 for a display whose period is not known
    @param within          how far from whole, in refresh periods, still counts as whole
    @return                the share, 0 to 1; 0 where the period is not known */
double WholeRefreshPeriodShare(const std::vector<double> &milliseconds, double period_seconds, double within)
{
    if (!(period_seconds > 0.0) || milliseconds.empty())
    {
        return 0.0;
    }
    const double period_milliseconds = period_seconds * 1000.0;
    int          whole               = 0;
    for (double one : milliseconds)
    {
        const double periods = one / period_milliseconds;
        whole += std::fabs(periods - std::round(periods)) <= within ? 1 : 0;
    }
    return static_cast<double>(whole) / static_cast<double>(milliseconds.size());
}

/** `milliseconds` reported: a line of figures, the share of whole refresh periods, and a histogram in one-millisecond
    bins of every bin that holds anything.
    @param what            what was measured
    @param milliseconds    the intervals measured, which are sorted in place
    @param period_seconds  the display's refresh period; 0 where it is not known */
void Report(const char *what, std::vector<double> &milliseconds, double period_seconds)
{
    const Figures figures = FiguresOf(milliseconds);
    std::printf("\n%s - %zu intervals\n", what, milliseconds.size());
    std::printf("  mean %8.3f ms -> %8.3f Hz   sd %6.3f   lowest %6.2f   median %6.2f   highest %6.2f\n", figures.mean,
                figures.hertz, figures.sd, figures.lowest, figures.median, figures.highest);
    if (period_seconds > 0.0)
    {
        std::printf("  a whole number of refresh periods: %5.1f %% of frames within a tenth of one, %5.1f %% within "
                    "a thirtieth   (period %.4f ms)\n",
                    100.0 * WholeRefreshPeriodShare(milliseconds, period_seconds, 1.0 / 10.0),
                    100.0 * WholeRefreshPeriodShare(milliseconds, period_seconds, 1.0 / 30.0), period_seconds * 1000.0);
    }
    if (milliseconds.empty())
    {
        return;
    }
    std::vector<int> bins(static_cast<std::size_t>(milliseconds.back()) + 2, 0);
    for (double one : milliseconds)
    {
        ++bins[static_cast<std::size_t>(std::max(0.0, one))];
    }
    std::printf("  one-millisecond bins:\n");
    for (std::size_t bin = 0; bin < bins.size(); ++bin)
    {
        if (bins[bin] > 0)
        {
            std::printf("    %3zu-%3zu ms  %6d  %5.2f %%\n", bin, bin + 1, bins[bin],
                        100.0 * bins[bin] / static_cast<double>(milliseconds.size()));
        }
    }
}

#if defined(_WIN32)
/** The compositor's clock, in seconds on the performance counter: what window/win/WindowWin.cpp reads each frame. */
struct Composition
{
    bool   is_read{false};      ///< true where the compositor answered
    double hertz{0.0};          ///< the rate it reports
    double vblank_seconds{0.0}; ///< the last vertical blank it saw
    double period_seconds{0.0}; ///< the refresh period
};

/** The compositor's clock now.
    @return  what it said; `is_read` false where composition is off or the call failed */
Composition CompositionNow() noexcept
{
    Composition read;
    BOOL        is_composing = FALSE;
    if (FAILED(DwmIsCompositionEnabled(&is_composing)) || is_composing == FALSE)
    {
        return read;
    }
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    if (FAILED(DwmGetCompositionTimingInfo(nullptr, &timing)))
    {
        return read;
    }
    static const double tick_seconds = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart > 0 ? 1.0 / static_cast<double>(frequency.QuadPart) : 0.0;
    }();
    read.is_read        = true;
    read.vblank_seconds = static_cast<double>(timing.qpcVBlank) * tick_seconds;
    read.period_seconds = static_cast<double>(timing.qpcRefreshPeriod) * tick_seconds;
    if (timing.rateRefresh.uiDenominator != 0)
    {
        read.hertz =
            static_cast<double>(timing.rateRefresh.uiNumerator) / static_cast<double>(timing.rateRefresh.uiDenominator);
    }
    return read;
}

/** A window of this harness's own for the platform timer to be set on: never shown, as a runner's window station
    would never show it anyway.
    @return  the window, or null where Windows refused it */
HWND MakeWindow()
{
    WNDCLASSW window_class{};
    window_class.lpfnWndProc   = DefWindowProcW;
    window_class.hInstance     = GetModuleHandleW(nullptr);
    window_class.lpszClassName = L"RecklessFramePacingHarness";
    RegisterClassW(&window_class);
    return CreateWindowExW(0, window_class.lpszClassName, L"frame pacing", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240, nullptr,
                           nullptr, window_class.hInstance, nullptr);
}

/** The intervals between `frames` WM_TIMERs of a timer asked for `elapse` milliseconds, each handler burning
    `burn_milliseconds` of processor before the next message is taken - which tells an absolute schedule from one
    re-armed at each expiry, since only the first becomes handler-bound.
    @param window             the window the timer is set on
    @param elapse             the period asked for, in milliseconds
    @param frames             how many messages to take
    @param burn_milliseconds  how long each handler burns before the next message is taken
    @return                   the intervals, in milliseconds */
std::vector<double> PlatformTimerIntervals(HWND window, UINT elapse, int frames, double burn_milliseconds)
{
    std::vector<double> intervals;
    if (SetTimer(window, 1, elapse, nullptr) == 0)
    {
        return intervals;
    }
    double last  = 0.0;
    int    taken = 0;
    MSG    message{};
    while (taken < frames && GetMessageW(&message, nullptr, 0, 0) > 0)
    {
        if (message.message != WM_TIMER)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
            continue;
        }
        const double now = NowSeconds();
        if (taken > 0)
        {
            intervals.push_back((now - last) * 1000.0);
        }
        last = now;
        ++taken;
        for (const double until = now + burn_milliseconds / 1000.0; NowSeconds() < until;)
        {
            // burned, so the next message cannot be taken before this
        }
    }
    KillTimer(window, 1);
    while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
    {
        // anything of this run's left in the queue
    }
    return intervals;
}

/** The intervals of the display clock the window waits on (window/win/WindowWin.cpp): a high-resolution waitable
    timer whose next due time is pulled onto the compositor's vertical blank grid every frame.
    @param divisor  the refresh periods each frame is shown for
    @param frames   how many frames to wait for
    @param period   the refresh period measured, written here for the report
    @return         the intervals, in milliseconds; empty where there is no compositor to read */
std::vector<double> DisplayClockIntervals(int divisor, int frames, double &period)
{
    std::vector<double> intervals;
    period                  = 0.0;
    const Composition first = CompositionNow();
    if (!first.is_read || !(first.period_seconds > 0.0))
    {
        return intervals;
    }
    HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, 0x00000002 /* HIGH_RESOLUTION */, TIMER_MODIFY_STATE | SYNCHRONIZE);
    if (timer == nullptr)
    {
        std::printf("  no high-resolution waitable timer on this Windows: nothing to measure\n");
        return intervals;
    }
    period = first.period_seconds;
    FrameClock clock;
    clock.Start(NowSeconds(), first.period_seconds * divisor, 1);
    double last = 0.0;
    for (int frame = 0; frame < frames; ++frame)
    {
        const Composition now_timing = CompositionNow();
        if (now_timing.is_read && now_timing.period_seconds > 0.0)
        {
            period = now_timing.period_seconds;
            clock.MoveNextFrameTo(
                OnTheRefreshGrid(clock.next_frame_seconds(), now_timing.vblank_seconds, now_timing.period_seconds));
        }
        const double seconds = clock.next_frame_seconds() - NowSeconds();
        if (seconds > 0.0)
        {
            LARGE_INTEGER due{};
            due.QuadPart = -static_cast<LONGLONG>(seconds * 10'000'000.0);
            if (due.QuadPart == 0)
            {
                due.QuadPart = -1;
            }
            SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
            WaitForSingleObject(timer, 1000);
        }
        const double woke = NowSeconds();
        if (frame > 0)
        {
            intervals.push_back((woke - last) * 1000.0);
        }
        last = woke;
        clock.Advance(woke);
    }
    CloseHandle(timer);
    return intervals;
}
#else
/** The intervals of the app loop's own deadline, which is what the toolkit paces on where there is no display clock:
    a deadline kept in the steady clock and advanced by the period, never by the waking - the shape
    app/linux/AppLinux.cpp's poll loop has.
    @param milliseconds  the period asked for
    @param frames        how many frames to wait for
    @return              the intervals, in milliseconds */
std::vector<double> PlatformTimerIntervals(int milliseconds, int frames)
{
    std::vector<double> intervals;
    const double        period = milliseconds / 1000.0;
    double              due    = NowSeconds() + period;
    double              last   = 0.0;
    for (int frame = 0; frame < frames; ++frame)
    {
        const double seconds = due - NowSeconds();
        if (seconds > 0.0)
        {
            std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
        }
        const double woke = NowSeconds();
        if (frame > 0)
        {
            intervals.push_back((woke - last) * 1000.0);
        }
        last = woke;
        due  = std::max(due + period, woke);
    }
    return intervals;
}
#endif

/** The system clock and the compositor reported: the grid every other run here lands on. */
void ReportClock()
{
    std::printf("\nthe clocks\n");
#if defined(_WIN32)
    DWORD increment = 0;
    DWORD period    = 0;
    BOOL  disabled  = FALSE;
    GetSystemTimeAdjustment(&increment, &period, &disabled);
    std::printf("  the system clock's increment in force   %.4f ms\n", increment / 10000.0);
    std::printf("  USER_TIMER_MINIMUM %u ms, which is the floor a window timer is clamped to\n", USER_TIMER_MINIMUM);
    const Composition composition = CompositionNow();
    if (composition.is_read)
    {
        std::printf("  the compositor: %.4f Hz, period %.4f ms, last vertical blank %.6f s\n", composition.hertz,
                    composition.period_seconds * 1000.0, composition.vblank_seconds);
        std::printf("  believable: %s\n", reckless::window::IsBelievableRefreshRate(composition.hertz) ? "yes" : "no");
    }
    else
    {
        std::printf("  the compositor: nothing to read - composition is off, or this window station has no "
                    "desktop\n");
    }
    // Every display on the desktop and the mode it is in: the compositor's reading above is the desktop's, so a
    // desktop whose displays run at different rates is one where the window's own display is the cross-check
    // (window/FramePacing.h, ChosenRefreshRate).
    std::printf("  the displays attached to the desktop:\n");
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    for (DWORD index = 0; EnumDisplayDevicesW(nullptr, index, &device, 0) != FALSE; ++index)
    {
        device.cb = sizeof(device);
        if ((device.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) == 0)
        {
            continue;
        }
        DEVMODEW mode{};
        mode.dmSize = sizeof(mode);
        if (EnumDisplaySettingsW(device.DeviceName, ENUM_CURRENT_SETTINGS, &mode) != FALSE)
        {
            std::printf("    %ls  %lu x %lu at %lu Hz%s\n", device.DeviceName,
                        static_cast<unsigned long>(mode.dmPelsWidth), static_cast<unsigned long>(mode.dmPelsHeight),
                        static_cast<unsigned long>(mode.dmDisplayFrequency),
                        (device.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) != 0 ? "  (primary)" : "");
        }
    }
#else
    std::printf("  the steady clock, which is what the app loop keeps its deadline in\n");
    std::printf("  there is no display clock on this platform (window/FramePacing.h says why)\n");
#endif
}

/** The platform's own frame timer measured as the window arranges it: the baseline every other figure is against. */
void ReportPlatformTimer()
{
#if defined(_WIN32)
    HWND window = MakeWindow();
    if (window == nullptr)
    {
        std::printf("\nno window: nothing to set a timer on\n");
        return;
    }
    const Composition   composition = CompositionNow();
    std::vector<double> intervals   = PlatformTimerIntervals(window, kFrameMilliseconds, kFramesMeasured, 0.0);
    char                what[128];
    std::snprintf(what, sizeof(what), "the platform timer, SetTimer(%d) - today's pacing", kFrameMilliseconds);
    Report(what, intervals, composition.is_read ? composition.period_seconds : 0.0);
    DestroyWindow(window);
#else
    std::vector<double> intervals = PlatformTimerIntervals(kFrameMilliseconds, kFramesMeasured);
    char                what[128];
    std::snprintf(what, sizeof(what), "the app loop's deadline, %d ms - today's pacing", kFrameMilliseconds);
    Report(what, intervals, 0.0);
#endif
}

/** The platform timer's mean interval against the period asked for: the staircase that explains the baseline. */
void ReportPlatformTimerSweep()
{
#if defined(_WIN32)
    HWND window = MakeWindow();
    if (window == nullptr)
    {
        return;
    }
    DWORD increment  = 0;
    DWORD resolution = 0;
    BOOL  disabled   = FALSE;
    GetSystemTimeAdjustment(&increment, &resolution, &disabled);
    const double grid = increment / 10000.0;
    std::printf("\nthe platform timer's mean interval against the period asked for, %d frames each\n", 400);
    std::printf("  the clock's increment here is %.4f ms, so the scans are on that grid\n", grid);
    std::printf("  asked   mean       -> Hz      sd      scans of the grid\n");
    for (UINT elapse : {1u, 10u, 14u, 15u, 16u, 17u, 20u, 30u, 31u, 32u, 33u, 46u, 47u, 48u})
    {
        std::vector<double> intervals = PlatformTimerIntervals(window, elapse, 400, 0.0);
        const Figures       figures   = FiguresOf(intervals);
        std::printf("  %5u  %8.3f  %8.3f  %6.3f   %6.3f\n", elapse, figures.mean, figures.hertz, figures.sd,
                    grid > 0.0 ? figures.mean / grid : 0.0);
    }
    DestroyWindow(window);
#else
    std::printf("\nthe platform timer's sweep is Windows' alone: the deadline elsewhere is kept in a steady clock "
                "and has no grid to land on\n");
#endif
}

/** Whether the platform timer's schedule is absolute or re-armed at each expiry: a handler that burns longer than the
    period makes an absolute schedule handler-bound, and leaves a re-armed one on its grid. */
void ReportPlatformTimerSchedule()
{
#if defined(_WIN32)
    HWND window = MakeWindow();
    if (window == nullptr)
    {
        return;
    }
    DWORD increment  = 0;
    DWORD resolution = 0;
    BOOL  disabled   = FALSE;
    GetSystemTimeAdjustment(&increment, &resolution, &disabled);
    const double grid = increment / 10000.0;
    std::printf("\nthe platform timer's schedule: SetTimer(%d) with a handler that burns first, 400 frames each\n",
                kFrameMilliseconds);
    std::printf("  an absolute schedule (due += period) becomes handler-bound once the burn passes the period;\n");
    std::printf("  a schedule re-armed at each expiry stays on the %.4f ms grid until the burn passes that\n", grid);
    std::printf("  burn   mean       sd      scans of the grid\n");
    for (double burn : {0.0, 8.0, 20.0, 25.0, 40.0})
    {
        std::vector<double> intervals = PlatformTimerIntervals(window, kFrameMilliseconds, 400, burn);
        const Figures       figures   = FiguresOf(intervals);
        std::printf("  %4.0f  %8.3f  %6.3f   %6.3f\n", burn, figures.mean, figures.sd,
                    grid > 0.0 ? figures.mean / grid : 0.0);
    }
    DestroyWindow(window);
#else
    std::printf("\nthe platform timer's schedule is Windows' question: elsewhere the deadline is the toolkit's own "
                "and is absolute by construction\n");
#endif
}

/** The display's own clock measured as the window waits on it, at a few divisors. */
void ReportDisplayClock()
{
#if defined(_WIN32)
    for (int divisor : {1, 2, 4})
    {
        double              period    = 0.0;
        std::vector<double> intervals = DisplayClockIntervals(divisor, kFramesMeasured / divisor, period);
        if (intervals.empty())
        {
            std::printf("\nthe display clock, a frame every %d refreshes: nothing to pace on here\n", divisor);
            continue;
        }
        char what[160];
        std::snprintf(what, sizeof(what),
                      "the display clock, a frame every %d refreshes - the vblank-snapped waitable timer", divisor);
        Report(what, intervals, period);
    }
#else
    std::printf("\nthe display clock is not measured here: macOS's is a display link, held by the child window's own "
                "suite, and Linux has none\n");
#endif
}
} // namespace

/** The runs named on the command line, or all of them where none is named.
    @param argc  the argument count
    @param argv  the arguments
    @return      0 always: nothing here can fail a run */
int main(int argc, char **argv)
{
    std::vector<std::string> wanted(argv + 1, argv + argc);
    const auto               asked = [&](const char *name) {
        return wanted.empty() || std::find(wanted.begin(), wanted.end(), name) != wanted.end();
    };
    std::printf("reckless-toolkit frame pacing harness - docs/window/frame-pacing.md holds what these mean\n");
    if (asked("clock"))
    {
        ReportClock();
    }
    if (asked("platform-timer"))
    {
        ReportPlatformTimer();
    }
    if (asked("platform-timer-sweep"))
    {
        ReportPlatformTimerSweep();
    }
    if (asked("platform-timer-schedule"))
    {
        ReportPlatformTimerSchedule();
    }
    if (asked("display-clock"))
    {
        ReportDisplayClock();
    }
    std::printf("\n");
    return 0;
}
