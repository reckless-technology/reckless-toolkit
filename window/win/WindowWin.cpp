// The child window on Windows: a child HWND of the host's window, drawing a view tree through Skia into a raster the
// size of the window and putting it on screen with SetDIBitsToDevice in WM_PAINT, the mouse, the keys and dropped
// files handed to a MouseRouter, and a window timer for the frame callback - or, where a product has asked for it
// (ChildWindow::SetFramePacing), the display's own clock in a thread of the window's own, each frame posted here.
// A popup is the same surface in a no-activate tool window over the host's, watched by a thread mouse hook for
// presses outside it. The window class is the module's own, registered with the first window and unregistered with
// the last, so two plug-ins' copies of this code never share a window procedure. Screen points are the screen's
// pixels, opaque to the views.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

#include "SkiaCanvas.h"
#include "Window.h"

// windows.h last: it defines macros (DrawText, CreateWindow, min, max) that would reach into the headers above.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// The wide forms throughout: the calls here name them, and the cursor ids (IDC_ARROW) become the wide resource names
// LoadCursorW takes.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

// dwmapi.h after windows.h, which it needs for HWND and HRESULT: the compositor's timing types, whose functions are
// looked up rather than linked (DwmCalls below), so no consumer's link line changes for this.
#include <dwmapi.h>

#include "ClipboardWin.h"
#include "HandedKeys.h"

namespace reckless::window
{
namespace
{
/** The id of the window timer that drives the frame callback. */
constexpr UINT_PTR kFrameTimer = 1;

/** The frame callback's tick, in milliseconds: about 60 Hz. */
constexpr UINT kFrameMilliseconds = 16;

/** One click of a wheel (WHEEL_DELTA), in the views' pixels. */
constexpr float kNotchPixels = 20.0f;

/** The message posted to the window when a CallSoon's call is waiting. */
constexpr UINT kCallSoonMessage = WM_APP + 17;

/** The message posted to the window when a file dialog is waiting to open. */
constexpr UINT kChooseFileMessage = WM_APP + 18;

/** The message the display clock's thread posts to the window when a frame is due (ChildWindow::SetFramePacing).
    Never posted to a window on the platform's timer, which is every window until a product asks. */
constexpr UINT kDisplayFrameMessage = WM_APP + 19;

/** The message the display clock's thread posts to the window when it has given up on the display and the platform's
    own timer is to take over. */
constexpr UINT kDisplayClockGaveUpMessage = WM_APP + 20;

/** How long the display clock's thread waits for its timer before looking again: a net under a wait that should
    always end at the due time, so that a machine coming back from sleep with a timer that never signalled reads the
    compositor again rather than sitting there. It is not a frame period and nothing is paced by it. */
constexpr DWORD kClockWaitsAtMostMilliseconds = 1000;

/** The module this code is linked into: its window class is registered against it.
    @return the module's instance handle */
HINSTANCE ModuleInstance() noexcept
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleInstance), &module);
    return module;
}

/** The class name, unique to the module, so another plug-in's copy of this code registers a class of its own.
    @return the name, made on the first call and kept for the life of the process */
const wchar_t *ClassName() noexcept
{
    static wchar_t name[64]{};
    if (name[0] == L'\0')
    {
        std::swprintf(name, std::size(name), L"RecklessPanel%p", static_cast<void *>(ModuleInstance()));
    }
    return name;
}

/** The windows of the module alive: the class is registered with the first and unregistered with the last. */
std::atomic<int> window_count{0};

/** The modifier keys held, as the thread's input reads them.
    @return the Shift, Control and Alt states */
view::Modifiers CurrentModifiers() noexcept
{
    return {GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_MENU) < 0};
}

/** Text in the platform's wide form.
    @param utf8  the text, in UTF-8
    @return      the same text in UTF-16; empty for empty text */
std::wstring WideOf(const std::string &utf8)
{
    if (utf8.empty())
    {
        return {};
    }
    const int    length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

/** Text from the platform's wide form.
    @param wide  the text in UTF-16, null-terminated; may be null
    @return      the same text in UTF-8; empty for null or empty text */
std::string Utf8Of(const wchar_t *wide)
{
    if (wide == nullptr || *wide == L'\0')
    {
        return {};
    }
    const int   length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length > 0 ? length - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), length, nullptr, nullptr);
    return utf8;
}

/** The system cursor a view's cursor kind stands for.
    @param cursor  the cursor a view asked for
    @return        the system cursor's resource id; the arrow for kArrow, the hand for the grab kinds */
LPCWSTR CursorFor(view::Cursor cursor)
{
    switch (cursor)
    {
    case view::Cursor::kPointingHand:
    case view::Cursor::kGrab:
    case view::Cursor::kGrabbing:
        return IDC_HAND;
    case view::Cursor::kIBeam:
        return IDC_IBEAM;
    case view::Cursor::kUpDown:
        return IDC_SIZENS;
    case view::Cursor::kLeftRight:
        return IDC_SIZEWE;
    case view::Cursor::kCrosshair:
        return IDC_CROSS;
    case view::Cursor::kArrow:
        break;
    }
    return IDC_ARROW;
}

/** A named key from its virtual-key code; Key::kNone for one that types a character (WM_CHAR brings that).
    @param code  the virtual-key code, a key-down message's wparam
    @return      the key, or Key::kNone */
view::Key NamedKey(WPARAM code)
{
    switch (code)
    {
    case VK_ESCAPE:
        return view::Key::kEscape;
    case VK_RETURN:
        return view::Key::kReturn;
    case VK_TAB:
        return view::Key::kTab;
    case VK_BACK:
        return view::Key::kBackspace;
    case VK_DELETE:
        return view::Key::kDelete;
    case VK_LEFT:
        return view::Key::kLeft;
    case VK_RIGHT:
        return view::Key::kRight;
    case VK_UP:
        return view::Key::kUp;
    case VK_DOWN:
        return view::Key::kDown;
    case VK_HOME:
        return view::Key::kHome;
    case VK_END:
        return view::Key::kEnd;
    case VK_PRIOR:
        return view::Key::kPageUp;
    case VK_NEXT:
        return view::Key::kPageDown;
    default:
        return view::Key::kNone;
    }
}

/** A surface's life as a file dialog's caller reads it: the dialog runs a modal loop of its own, in which the host
    may close the editor - the surface, the panel and every view between them gone when it returns - so the caller
    holds this, which outlives the surface, and reads it before touching anything. The surface marks itself gone here,
    and closes the dialog up, as it goes. */
struct SurfaceLife
{
    bool         gone{false};        ///< true once the surface has let go.
    IFileDialog *dialog_up{nullptr}; ///< the file dialog while it shows, else null.
};

/** The shell's file dialog for `request`, modal over `owner`; the path chosen, or empty. `life.dialog_up` is the
    dialog while it shows.
    @param request  the title, the patterns, the folder and whether it saves
    @param owner    the window the dialog is modal over
    @param life     the surface's life, whose dialog_up is set for the dialog's loop
    @return         the path chosen, in UTF-8; empty for a cancel or a failure */
std::string RunFileDialog(const view::FileRequest &request, HWND owner, SurfaceLife &life)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string   result;
    IFileDialog  *dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(request.save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        if (!request.title.empty())
        {
            dialog->SetTitle(WideOf(request.title).c_str());
        }
        std::wstring spec;
        for (const auto &pattern : request.patterns)
        {
            spec += (spec.empty() ? L"" : L";") + WideOf(pattern);
        }
        if (!spec.empty())
        {
            const COMDLG_FILTERSPEC filter{L"Files", spec.c_str()};
            dialog->SetFileTypes(1, &filter);
        }
        if (request.save && !request.file_name.empty())
        {
            dialog->SetFileName(WideOf(request.file_name).c_str());
        }
        if (!request.folder.empty())
        {
            IShellItem *folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(WideOf(request.folder).c_str(), nullptr, IID_PPV_ARGS(&folder))))
            {
                dialog->SetFolder(folder);
                folder->Release();
            }
        }
        life.dialog_up      = dialog;
        const HRESULT shown = dialog->Show(owner);
        life.dialog_up      = nullptr;
        if (SUCCEEDED(shown))
        {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    result = Utf8Of(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
    return result;
}

// --------------------------------------------------------------------------------------------------------------
// The display's own clock, where a product has asked to be paced on it (ChildWindow::SetFramePacing).
// --------------------------------------------------------------------------------------------------------------

// The high resolution a waitable timer has carried since Windows 10 1803. Named here for a Windows SDK too old to
// name it, where CreateWaitableTimerExW simply refuses the flag and the window keeps the platform's timer.
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

/** The seconds in one tick of the performance counter, read once.
    @return  the seconds a tick is; 0 where the counter has no frequency */
double QpcTickSeconds() noexcept
{
    static const double seconds = [] {
        LARGE_INTEGER frequency{};
        QueryPerformanceFrequency(&frequency);
        return frequency.QuadPart > 0 ? 1.0 / static_cast<double>(frequency.QuadPart) : 0.0;
    }();
    return seconds;
}

/** The performance counter now, in seconds: the clock the compositor stamps its vertical blanks on, so the one a
    frame's due time is kept in.
    @return  the counter, in seconds */
double QpcSeconds() noexcept
{
    LARGE_INTEGER now{};
    QueryPerformanceCounter(&now);
    return static_cast<double>(now.QuadPart) * QpcTickSeconds();
}

/** Desktop Window Manager's composition timing, looked up rather than linked, as GetDpiForWindow is below: no
    consumer's link line changes for this, and a Windows whose dwmapi.dll has neither call is the fallback the pacing
    already has. The library is held for the life of the process, as every windowed process holds it anyway. */
struct DwmCalls
{
    /** DwmIsCompositionEnabled, as dwmapi.dll exports it.
        @param is_enabled  set to whether the compositor is composing
        @return            S_OK where it answered */
    using IsCompositionEnabledCall = HRESULT(WINAPI *)(BOOL *is_enabled);

    /** DwmGetCompositionTimingInfo, as dwmapi.dll exports it.
        @param window  the window; null for the desktop's timing, which is the only form this asks for
        @param timing  filled with the compositor's timing, its cbSize set by the caller
        @return        S_OK where it answered */
    using CompositionTimingInfoCall = HRESULT(WINAPI *)(HWND window, DWM_TIMING_INFO *timing);

    IsCompositionEnabledCall  is_composition_enabled{nullptr};  ///< null where dwmapi.dll has not got it.
    CompositionTimingInfoCall composition_timing_info{nullptr}; ///< null where dwmapi.dll has not got it.
};

/** The compositor's calls, looked up on the first ask.
    @return  the calls, whose members are null where dwmapi.dll has not got them */
const DwmCalls &Dwm() noexcept
{
    static const DwmCalls calls = [] {
        DwmCalls found;
        if (const HMODULE library = LoadLibraryW(L"dwmapi.dll"))
        {
            found.is_composition_enabled = reinterpret_cast<decltype(found.is_composition_enabled)>(
                reinterpret_cast<void *>(GetProcAddress(library, "DwmIsCompositionEnabled")));
            found.composition_timing_info = reinterpret_cast<decltype(found.composition_timing_info)>(
                reinterpret_cast<void *>(GetProcAddress(library, "DwmGetCompositionTimingInfo")));
        }
        return found;
    }();
    return calls;
}

/** What the compositor says about its own clock, in seconds on the performance counter. The desktop's, which is what
    DwmGetCompositionTimingInfo answers for: a window's own display is read from its mode instead
    (DisplayRefreshHertzOf), and the two are cross-checked. */
struct CompositionTiming
{
    bool   is_read{false};      ///< true where the compositor answered at all.
    double hertz{0.0};          ///< the refresh rate it reports; 0 where it reports none.
    double vblank_seconds{0.0}; ///< the last vertical blank it saw: the grid's anchor.
    double period_seconds{0.0}; ///< the refresh period: the grid's step.
};

/** The compositor's clock now.
    @return  what it said; `is_read` false where composition is off, the calls are missing, or the call failed */
CompositionTiming CompositionTimingNow() noexcept
{
    CompositionTiming read;
    const DwmCalls   &dwm          = Dwm();
    BOOL              is_composing = FALSE;
    if (dwm.is_composition_enabled == nullptr || dwm.composition_timing_info == nullptr ||
        FAILED(dwm.is_composition_enabled(&is_composing)) || is_composing == FALSE)
    {
        return read;
    }
    DWM_TIMING_INFO timing{};
    timing.cbSize = sizeof(timing);
    if (FAILED(dwm.composition_timing_info(nullptr, &timing)))
    {
        return read;
    }
    read.is_read        = true;
    read.vblank_seconds = static_cast<double>(timing.qpcVBlank) * QpcTickSeconds();
    read.period_seconds = static_cast<double>(timing.qpcRefreshPeriod) * QpcTickSeconds();
    if (timing.rateRefresh.uiDenominator != 0)
    {
        read.hertz =
            static_cast<double>(timing.rateRefresh.uiNumerator) / static_cast<double>(timing.rateRefresh.uiDenominator);
    }
    else if (read.period_seconds > 0.0)
    {
        read.hertz = 1.0 / read.period_seconds;
    }
    return read;
}

/** The refresh rate of the display `window` is on, from the mode that display is in. This is the one reading that is
    the window's own display rather than the whole desktop's, so a window on a second monitor of another rate is
    caught by cross-checking it against the compositor's and nowhere else.
    @param window  the window
    @return        the rate, in hertz; 0 where Windows named none, or named the hardware's default rather than a rate */
double DisplayRefreshHertzOf(HWND window) noexcept
{
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW monitor_info{};
    monitor_info.cbSize = sizeof(monitor_info);
    if (monitor == nullptr || GetMonitorInfoW(monitor, reinterpret_cast<LPMONITORINFO>(&monitor_info)) == FALSE)
    {
        return 0.0;
    }
    DEVMODEW mode{};
    mode.dmSize = sizeof(mode);
    if (EnumDisplaySettingsW(monitor_info.szDevice, ENUM_CURRENT_SETTINGS, &mode) == FALSE)
    {
        return 0.0;
    }
    // 0 and 1 are Windows' answers for "the hardware's default", which is no rate: IsBelievableRefreshRate refuses
    // both, and the compositor's reading is then the only one.
    return static_cast<double>(mode.dmDisplayFrequency);
}

/**
    The thread a window paced on the display's clock waits in. Windows has no present to put on the vertical blank
    here - the view tree is rastered on the CPU and blitted with SetDIBitsToDevice, and the compositor owns what
    reaches the panel - so this is the nearest honest thing to one: a waitable timer asked for the high resolution it
    has carried since Windows 10 1803, whose next due time is pulled onto the compositor's own vertical blank grid
    (DwmGetCompositionTimingInfo) every frame, so the frames land on whole refresh periods rather than near them.
    Measured on a 59.94 Hz display at 16.683 ms mean and 0.246 ms standard deviation, with 99 % of frames within a
    thirtieth of a whole refresh period, against the platform timer's 26.855 ms, 7.094 ms and 4.4 %
    (docs/window/frame-pacing.md).

    The waiting is in a thread of the window's own and each frame is posted to the window (kDisplayFrameMessage), so
    the callback runs on the window's thread as the timer's always has, and a plug-in's editor inside a host's loop is
    served as well as an app on its own loop. **At most one frame is in flight**: a window whose thread is busy - a
    modal loop, a dialog, a drag - is never handed a backlog to run in a burst.

    DwmFlush is not used, well though it measures: each call blocks the thread it is on for a whole refresh period,
    which would be right in a thread of one's own and is wrong anywhere near a window's, and its period is three times
    as loose as the timer's.
*/
class WinDisplayClock
{
  public:
    WinDisplayClock() = default;

    /** Stopped, its thread joined, before anything of the window it posts to can go. */
    ~WinDisplayClock()
    {
        Stop();
    }

    WinDisplayClock(const WinDisplayClock &)            = delete;
    WinDisplayClock &operator=(const WinDisplayClock &) = delete;

    /** Started on `window`: a frame on every `request.refresh_divisor`th refresh of the display the window is on,
        each posted to the window. False where the high-resolution waitable timer or the stop event could not be made
        - a Windows before 10 1803, or one out of handles - and the window then keeps the platform's timer.
        @param window   the window each frame is posted to; outlives the clock
        @param request  the divisor and the frames to make up, both the product's
        @return         true where the clock's thread is running */
    bool Start(HWND window, const FrameRequest &request)
    {
        Stop();
        window_  = window;
        request_ = request;
        gave_up_because_.store(static_cast<int>(FramePacingFallback::kNone), std::memory_order_relaxed);
        is_frame_in_flight_.store(false, std::memory_order_relaxed);
        display_hertz_.store(0.0, std::memory_order_relaxed);
        // Without the high resolution the timer is the system clock's 15.625 ms again, which is the whole fault being
        // fixed, so a Windows that refuses the flag is told apart from one that takes it rather than paced badly.
        timer_ = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                        TIMER_MODIFY_STATE | SYNCHRONIZE);
        stop_  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (timer_ == nullptr || stop_ == nullptr)
        {
            Stop();
            return false;
        }
        thread_ = std::thread([this] { Wait(); });
        return true;
    }

    /** Stopped: the thread woken and joined, its timer and event closed. Nothing is posted to the window afterwards,
        and calling it on a clock that is not running does nothing. */
    void Stop()
    {
        if (stop_ != nullptr)
        {
            SetEvent(stop_);
        }
        if (thread_.joinable())
        {
            thread_.join();
        }
        if (timer_ != nullptr)
        {
            CloseHandle(timer_);
            timer_ = nullptr;
        }
        if (stop_ != nullptr)
        {
            CloseHandle(stop_);
            stop_ = nullptr;
        }
    }

    /** The frame the window has just taken off its queue, so the next may be posted: what the window calls as it
        handles kDisplayFrameMessage. */
    void FrameTaken() noexcept
    {
        is_frame_in_flight_.store(false, std::memory_order_release);
    }

    /** Why the clock gave up on the display, where it has: what the window reads as it handles
        kDisplayClockGaveUpMessage.
        @return  the reason; FramePacingFallback::kNone while the clock is pacing */
    FramePacingFallback gave_up_because() const noexcept
    {
        return static_cast<FramePacingFallback>(gave_up_because_.load(std::memory_order_acquire));
    }

    /** The rate the clock is dividing now, in hertz, as its thread last read it - which is the display the window is
        on now, not the one it was on when the product asked.
        @return  the rate, in hertz; 0 before the first reading */
    double display_hertz_now() const noexcept
    {
        return display_hertz_.load(std::memory_order_acquire);
    }

  private:
    /** Why a wait ended. */
    enum class Woke
    {
        kDue,     ///< the due time came
        kStopped, ///< the clock was stopped, or the timer could not be set
    };

    /** Waits until `due_seconds` on the performance counter. A wait that ends early - the net timeout, a machine back
        from sleep - is simply waited again, so this returns at the due time or not at all.
        @param due_seconds  when the frame is due, in seconds on the performance counter
        @return             Woke::kDue at the due time, Woke::kStopped where the clock was stopped */
    Woke WaitUntil(double due_seconds)
    {
        for (;;)
        {
            const double seconds = due_seconds - QpcSeconds();
            if (seconds <= 0.0)
            {
                return Woke::kDue;
            }
            LARGE_INTEGER due{};
            // Negative is relative, in hundreds of nanoseconds: the only form that needs nothing of the system clock,
            // which a time change or a daylight saving moves under an absolute one.
            due.QuadPart = -static_cast<LONGLONG>(seconds * 10'000'000.0);
            if (due.QuadPart == 0)
            {
                due.QuadPart = -1; // less than a tick away: the shortest wait the timer takes
            }
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) == FALSE)
            {
                return Woke::kStopped;
            }
            const HANDLE waited_on[] = {stop_, timer_};
            const DWORD  woke        = WaitForMultipleObjects(2, waited_on, FALSE, kClockWaitsAtMostMilliseconds);
            if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED)
            {
                return Woke::kStopped;
            }
        }
    }

    /** `why` recorded and told to the window, which puts itself back on the platform's timer. The thread returns
        after this.
        @param why  why the display is no longer one to divide */
    void GiveUp(FramePacingFallback why)
    {
        gave_up_because_.store(static_cast<int>(why), std::memory_order_release);
        PostMessageW(window_, kDisplayClockGaveUpMessage, 0, 0);
    }

    /** The thread's whole body: the compositor read, the display under the window watched, the next frame put on the
        display's grid, waited for and posted. */
    void Wait()
    {
        FrameClock        clock;
        RefreshSteadiness steadiness;
        HMONITOR          display        = nullptr;
        double            display_hertz  = 0.0;
        double            period_divided = 0.0;
        std::size_t       unsteady       = 0;
        for (;;)
        {
            const CompositionTiming timing         = CompositionTimingNow();
            const HMONITOR          under          = MonitorFromWindow(window_, MONITOR_DEFAULTTONEAREST);
            const bool              is_display_new = under != display;
            // A period further from the one being divided than one period read twice could be: the mode changed, or
            // the window arrived on a display of another rate.
            const bool has_period_moved =
                timing.period_seconds > 0.0 && std::fabs(timing.period_seconds - period_divided) >
                                                   kRefreshPeriodsHoldStillWithin * timing.period_seconds;
            if (is_display_new)
            {
                // Another display under the window: another grid to land on, and nothing of the one being left is
                // judged together with the one being arrived at.
                display  = under;
                unsteady = 0;
                steadiness.Forget();
            }
            if (is_display_new || has_period_moved)
            {
                // The rate to divide is read again rather than carried over - a mode change on the one display is
                // caught here, and reading it only when something has moved keeps it off every frame's path.
                display_hertz = DisplayRefreshHertzOf(window_);
            }
            steadiness.Note(timing.period_seconds);
            unsteady = steadiness.does_hold_still() ? 0 : unsteady + 1;

            DisplayClockReading reading;
            reading.has_clock        = timing.is_read;
            reading.compositor_hertz = timing.hertz;
            reading.display_hertz    = display_hertz;
            // A period that will not hold still for as many readings as are judged together is a panel whose rate
            // varies; fewer than that is one mode change, whose jump is still inside the readings being judged.
            reading.does_rate_hold_still = unsteady < kRefreshPeriodsJudged;
            const FramePacingState state = ChosenPacing(request_, reading);
            if (state.pacing != FramePacing::kDisplayClock)
            {
                GiveUp(state.why);
                return;
            }
            display_hertz_.store(state.display_hertz, std::memory_order_release);

            if (has_period_moved)
            {
                // The first reading, or a period that has moved: the schedule starts again on what the display is
                // doing now rather than dividing what it was doing.
                period_divided = timing.period_seconds;
                clock.Start(QpcSeconds(), period_divided * request_.refresh_divisor, request_.most_frames_behind);
            }
            // The due time pulled onto the grid the compositor's last vertical blank and its period describe, which
            // is where a whole number of refresh periods lands by construction.
            clock.MoveNextFrameTo(
                OnTheRefreshGrid(clock.next_frame_seconds(), timing.vblank_seconds, timing.period_seconds));
            if (WaitUntil(clock.next_frame_seconds()) == Woke::kStopped)
            {
                return;
            }
            const double woke = QpcSeconds();
            // One frame in flight at a time: where the window has not taken the last one, this one is given up
            // rather than queued behind it, and the clock's own catching up decides what happens next.
            if (!is_frame_in_flight_.exchange(true, std::memory_order_acq_rel) &&
                PostMessageW(window_, kDisplayFrameMessage, 0, 0) == FALSE)
            {
                is_frame_in_flight_.store(false, std::memory_order_release);
            }
            clock.Advance(woke);
        }
    }

    HANDLE            timer_{nullptr};            ///< the high-resolution waitable timer each frame is waited on.
    HANDLE            stop_{nullptr};             ///< set to end the thread, waited on beside the timer.
    std::thread       thread_;                    ///< the thread that waits; not joinable while the clock is stopped.
    HWND              window_{nullptr};           ///< the window each frame is posted to.
    FrameRequest      request_{};                 ///< the divisor and the frames to make up, the product's.
    std::atomic<bool> is_frame_in_flight_{false}; ///< a frame posted and not yet taken off the window's queue.
    std::atomic<int>  gave_up_because_{
        static_cast<int>(FramePacingFallback::kNone)}; ///< FramePacingFallback, as an int for std::atomic.
    std::atomic<double> display_hertz_{0.0};           ///< the rate being divided, as the thread last read it.
};

/** The window procedure of the module's class: hands each message to the WinSurface the window holds in
    GWLP_USERDATA.
    @param hwnd     the window
    @param message  the message
    @param wparam   the message's first parameter
    @param lparam   the message's second parameter
    @return         the message's result */
LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
} // namespace

class WinPopup;

/** A view tree drawn in an HWND: the raster, the drawing, the mouse, keys and drops to a router, and the host the tree
    lives in. The child window and every popup are one. */
class WinSurface : public view::Host
{
  public:
    /** A surface for `root`, with no window until CreateSurface.
        @param root   the view tree to draw; outlives the surface
        @param scale  window pixels per design pixel */
    WinSurface(view::View &root, float scale) : root_(root), router_(root, this), scale_(scale)
    {
    }

    /** The surface released (Release): its popups closed, the tree's host dropped, the window gone. */
    ~WinSurface() override
    {
        Release();
    }

    /** Every popup opened over the surface closed, and theirs, each `closed_by_window` gathered into `heard` for
        whoever let the surface go to run once everything is closed.
        @param heard  the `closed_by_window` callbacks of the popups closed, appended to */
    void EndPopups(std::vector<std::function<void()>> &heard);

    /** `popup`, going on its own, off the list of those opened over the surface.
        @param popup  the popup being destroyed */
    void Forget(const WinPopup *popup)
    {
        popups_.erase(std::remove(popups_.begin(), popups_.end(), popup), popups_.end());
    }

    /** The window, with `style` and `ex_style`, at `x`, `y` in `parent`'s client (or the screen for a popup), the
        design size times the scale; false when Windows refuses it.
        @param parent    the parent or owner window
        @param style     the window style
        @param ex_style  the extended window style
        @param x         the window's left, in `parent`'s client pixels or screen pixels for a popup
        @param y         the window's top, in the same units
        @return          true when the window was made */
    bool CreateSurface(HWND parent, DWORD style, DWORD ex_style, int x, int y)
    {
        if (window_count++ == 0)
        {
            WNDCLASSEXW window_class{};
            window_class.cbSize        = sizeof(window_class);
            window_class.style         = CS_DBLCLKS; // a second press soon after the first arrives as a double click
            window_class.lpfnWndProc   = WindowProcedure;
            window_class.hInstance     = ModuleInstance();
            window_class.hCursor       = nullptr; // WM_SETCURSOR sets the views' shape
            window_class.lpszClassName = ClassName();
            RegisterClassExW(&window_class);
        }
        registered_ = true;
        ResizeRaster();
        hwnd_ = CreateWindowExW(ex_style, ClassName(), L"", style, x, y, pixel_width_, pixel_height_, parent, nullptr,
                                ModuleInstance(), this);
        if (hwnd_ == nullptr)
        {
            return false;
        }
        DragAcceptFiles(hwnd_, TRUE);
        root_.SetHost(this);
        return true;
    }

    /** The handle, from the window's first message on: messages arrive before CreateWindowExW returns it.
        @param hwnd  the window being created */
    void Adopt(HWND hwnd) noexcept
    {
        hwnd_ = hwnd;
    }

    // ---- view::Host -----------------------------------------------------------------------------------------------

    /** The pixels `area` covers marked for painting; nothing once the surface has let go.
        @param area  the area to repaint, in design pixels */
    void Invalidate(gfx::RectI area) override
    {
        if (hwnd_ == nullptr)
        {
            return; // let go: no window, and a null one would invalidate every window on the desktop
        }
        const gfx::RectI pixels = ToPixels(area);
        const RECT       r{pixels.x(), pixels.y(), pixels.right(), pixels.bottom()};
        InvalidateRect(hwnd_, &r, FALSE);
    }

    float TextWidth(const gfx::Font &font, std::string_view text) const override
    {
        return gfx::MeasureText(font, text);
    }

    /** The display's scale, which the window draws its root at: the zoom a JUCE editor's DPI transform gives it.
        @return window pixels per design pixel */
    float Zoom() const override
    {
        return scale_;
    }

    void CallSoon(std::function<void()> call) override
    {
        if (hwnd_ == nullptr)
        {
            return; // let go: nothing it defers runs
        }
        calls_.push_back(std::move(call));
        PostMessageW(hwnd_, kCallSoonMessage, 0, 0);
    }

    void SetCursor(view::Cursor cursor) override
    {
        cursor_ = cursor;
        ::SetCursor(LoadCursorW(nullptr, CursorFor(cursor)));
    }

    void RequestKeyboardFocus(view::View *view) override
    {
        router_.Focus(view);
    }

    void KeyboardFocusChanged(bool a_view_has_it) override
    {
        if (hwnd_ == nullptr)
        {
            return;
        }
        if (a_view_has_it)
        {
            previous_focus_ = GetFocus();
            focus_taken_    = true;
            SetFocus(hwnd_);
        }
        else if (focus_taken_)
        {
            focus_taken_ = false;
            if (GetFocus() == hwnd_ && previous_focus_ != nullptr && IsWindow(previous_focus_))
            {
                SetFocus(previous_focus_); // the keyboard back to whoever had it
            }
            previous_focus_ = nullptr;
        }
    }

    void ViewLeaving(const view::View &view) override
    {
        router_.Forget(view);
    }

    /** A popup of `content` in a no-activate tool window, kept whole on the monitor it opens on (Host::OpenPopup).
        @param content          the popup's root view; outlives the popup
        @param at               the popup's top left, in this surface's design pixels
        @param pressed_outside  runs with the screen point of every press outside the popup
        @param takes_mouse      false for a popup that only shows, which clicks fall through
        @return                 the popup; null once the surface has let go or Windows refuses the window */
    std::unique_ptr<view::Popup> OpenPopup(view::View &content, gfx::PointI at,
                                           std::function<void(gfx::PointI)> pressed_outside, bool takes_mouse) override;

    /** The clipboard's Unicode text, for a paste: opened against this window and closed again at once.
        @return  the text in UTF-8, or empty when it holds none or another program has it open */
    std::string ClipboardText() override
    {
        std::string text;
        if (hwnd_ == nullptr || !OpenClipboard(hwnd_))
        {
            return text;
        }
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT); data != nullptr)
        {
            if (const auto *wide = static_cast<const wchar_t *>(GlobalLock(data)); wide != nullptr)
            {
                text = Utf8Of(wide);
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        return text;
    }

    /** `text` on the clipboard, for a copy (win::WriteClipboardText): opened against this window, which owns it until
        another copy, and closed again at once; nothing once the surface is let go.
        @param text  the text, UTF-8
        @return      true when the clipboard took it */
    bool SetClipboardText(const std::string &text) override
    {
        return hwnd_ != nullptr && win::WriteClipboardText(hwnd_, text);
    }

    /** The shell's own dialog, modal over the host's window, opened from a message of its own once the event that
        asked for it is done: never inside that event, whose views' frames would lie below the dialog's loop on the
        stack and be returned into after the host had closed the editor in it. `picked` hears the answer while the
        window lives, and nothing once it has gone.
        @param request  the title, the patterns, the folder and whether it saves
        @param picked   told the path chosen, in UTF-8, or an empty string for a cancel; runs on the message thread */
    void ChooseFile(const view::FileRequest &request, std::function<void(const std::string &)> picked) override
    {
        if (hwnd_ == nullptr)
        {
            return; // let go: nothing hears an answer
        }
        dialogs_.push_back({request, std::move(picked)});
        PostMessageW(hwnd_, kChooseFileMessage, 0, 0);
    }

    // ---- The window's messages ------------------------------------------------------------------------------------

    /** The window's messages: painting, the cursor, the mouse, the wheel, the keys, dropped files, the frame timer, the
        queued calls and the file dialogs; the rest Windows'.
        @param message  the message
        @param wparam   the message's first parameter
        @param lparam   the message's second parameter
        @return         the message's result */
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC         dc = BeginPaint(hwnd_, &paint);
            Paint(dc, paint.rcPaint);
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1; // the raster covers every pixel
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT)
            {
                ::SetCursor(LoadCursorW(nullptr, CursorFor(cursor_)));
                return TRUE;
            }
            break;
        case WM_MOUSEACTIVATE:
            if (no_activate_)
            {
                return MA_NOACTIVATE; // a popup never takes the host's window's place
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            Press(lparam, message == WM_LBUTTONDBLCLK ? 2 : 1, false);
            return 0;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
            Press(lparam, 1, true);
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            if (pressed_)
            {
                pressed_ = false;
                ReleaseCapture();
                router_.Up(PointOf(lparam), CurrentModifiers());
            }
            return 0;
        case WM_MOUSEMOVE:
            if (!tracking_)
            {
                TRACKMOUSEEVENT track{static_cast<DWORD>(sizeof(TRACKMOUSEEVENT)), TME_LEAVE, hwnd_, 0};
                tracking_ = TrackMouseEvent(&track) != FALSE;
            }
            if (pressed_)
            {
                router_.Drag(PointOf(lparam), CurrentModifiers());
            }
            else
            {
                router_.Move(PointOf(lparam), CurrentModifiers());
            }
            return 0;
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (!pressed_)
            {
                router_.Exit();
            }
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            // The wheel's position comes in screen coordinates.
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &point);
            const float       notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            const gfx::PointF at{static_cast<float>(point.x) / scale_, static_cast<float>(point.y) / scale_};
            if (message == WM_MOUSEWHEEL)
            {
                router_.Wheel(at, 0.0f, notches * kNotchPixels, CurrentModifiers());
            }
            else
            {
                router_.Wheel(at, -notches * kNotchPixels, 0.0f, CurrentModifiers()); // a tilt right is positive
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (pressed_ && reinterpret_cast<HWND>(lparam) != hwnd_)
            {
                pressed_ = false; // taken by someone else mid-drag: the press ends where it was
                router_.Exit();
            }
            return 0;
        case WM_GETDLGCODE:
            if (router_.focused() != nullptr)
            {
                return DLGC_WANTALLKEYS | DLGC_WANTCHARS; // a dialog-like host must not take the arrows or Tab
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            TellRawKey(true, wparam, lparam); // nothing where no owner asked; the views are given what they always were
            const view::Key       key       = NamedKey(wparam);
            const view::Modifiers modifiers = CurrentModifiers();
            if (key != view::Key::kNone && router_.Key({key, 0, modifiers}))
            {
                return 0;
            }
            // Ctrl with a letter as the letter with the primary modifier, as a Mac's Command-letter reaches the views:
            // its WM_CHAR is a control character, which no view reads. Not with Alt, where Ctrl+Alt may be AltGr
            // typing a character of its own, which WM_CHAR brings.
            if (key == view::Key::kNone && modifiers.command && !modifiers.alt && wparam >= 'A' && wparam <= 'Z' &&
                router_.Key({view::Key::kCharacter, static_cast<char32_t>(U'a' + (wparam - 'A')), modifiers}))
            {
                return 0;
            }
            if (key_target_ != nullptr && SendMessageW(key_target_, win::kHandedKeyDown, wparam, lparam) != 0)
            {
                return 0; // no view's: the window it sits in used it
            }
            break;
        }
        case WM_CHAR:
            if (wparam >= 32 && router_.Key({view::Key::kCharacter, static_cast<char32_t>(wparam), CurrentModifiers()}))
            {
                return 0;
            }
            if (wparam >= 32 && key_target_ != nullptr &&
                SendMessageW(key_target_, win::kHandedChar, wparam, lparam) != 0)
            {
                return 0;
            }
            break;
        case WM_KEYUP:
        case WM_SYSKEYUP:
            // Windows' own, as it always was: nothing but an owner that asked for raw keys hears a key let go, and
            // where none did this message is Windows' to answer, which DefWindowProcW below does.
            TellRawKey(false, wparam, lparam);
            break;
        case WM_KILLFOCUS:
            LetGoOfHeldRawKeys(); // the keyboard went elsewhere: nothing it said was down stays down
            if (focus_taken_)
            {
                focus_taken_ = false;
                router_.Focus(nullptr); // the keyboard went elsewhere
            }
            return 0;
        case WM_DROPFILES: {
            HDROP                    drop  = reinterpret_cast<HDROP>(wparam);
            const UINT               count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::string> paths;
            for (UINT i = 0; i < count; ++i)
            {
                wchar_t path[MAX_PATH * 4]{};
                DragQueryFileW(drop, i, path, static_cast<UINT>(std::size(path)));
                paths.push_back(Utf8Of(path));
            }
            POINT point{};
            DragQueryPoint(drop, &point);
            DragFinish(drop);
            router_.DropFiles({static_cast<float>(point.x) / scale_, static_cast<float>(point.y) / scale_}, paths);
            return 0;
        }
        case WM_TIMER:
            if (wparam == kFrameTimer && frame_)
            {
                frame_();
            }
            return 0;
        case kDisplayFrameMessage:
        case kDisplayClockGaveUpMessage:
            // The display clock's thread (ChildWindow::SetFramePacing), whose messages reach a window that asked to
            // be paced on it and no other: `paced_message_` is empty until a product asks, and nothing posts these.
            if (paced_message_)
            {
                // Copied: answering the clock's giving up replaces the callback, and a call may not destroy itself.
                const std::function<void(UINT)> paced = paced_message_;
                paced(message);
            }
            return 0;
        case kCallSoonMessage: {
            // One posted message per call; each runs the oldest waiting.
            if (!calls_.empty())
            {
                auto call = std::move(calls_.front());
                calls_.pop_front();
                call();
            }
            return 0;
        }
        case kChooseFileMessage: {
            if (!dialogs_.empty())
            {
                PendingDialog pending = std::move(dialogs_.front());
                dialogs_.pop_front();
                const std::shared_ptr<SurfaceLife> life = life_;
                const std::string path = RunFileDialog(pending.request, GetAncestor(hwnd_, GA_ROOT), *life);
                if (!life->gone) // else the surface went in the dialog's loop: nothing of it may be touched again
                {
                    pending.picked(path);
                }
            }
            return 0;
        }
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    /** The window.
        @return the handle; null before it is made and after the surface lets go */
    HWND hwnd() const noexcept
    {
        return hwnd_;
    }

  protected:
    /** Every raw key edge told to `raw_key` (ChildWindow::SetRawKeyCallback): nothing while it is empty, which it is
        until an owner asks. The keys held are let go first, so a callback replaced mid-press leaves the one being
        replaced owed nothing and the new one holding nothing.
        @param raw_key  told of each edge; empty for none */
    void TakeRawKeys(RawKeyCallback raw_key)
    {
        LetGoOfHeldRawKeys();
        raw_key_ = std::move(raw_key);
    }

    /** One edge of one key told to whoever asked for the raw keys, and nothing where no one has: the virtual-key code
        as the message carries it, the scan code out of its flags (WindowsPhysicalKeyOf), the toolkit's name for the
        key where it has one, and the modifiers the thread's input holds. A down of a key already held is auto-repeat's
        (`is_repeat`), and an up is told only for a key whose down was.
        @param is_down  true for WM_KEYDOWN or WM_SYSKEYDOWN, false for WM_KEYUP or WM_SYSKEYUP
        @param wparam   the message's virtual-key code
        @param lparam   the message's flags: the scan code, the extended bit and the rest */
    void TellRawKey(bool is_down, WPARAM wparam, LPARAM lparam)
    {
        if (!raw_key_)
        {
            return;
        }
        RawKeyEvent key;
        key.is_down           = is_down;
        key.named_key         = NamedKey(wparam);
        key.platform_key_code = static_cast<std::uint32_t>(wparam);
        key.physical_key_code = WindowsPhysicalKeyOf(static_cast<std::uint32_t>(lparam));
        key.modifiers         = CurrentModifiers();
        if (is_down)
        {
            key.is_repeat = raw_keys_held_.NoteDown(key);
            raw_key_(key);
        }
        else if (raw_keys_held_.NoteUp(key.physical_key_code))
        {
            raw_key_(key);
        }
    }

    /** An up told for every key the raw key callback was told is down, and nothing held afterwards: what the window
        does as it loses the keyboard (WM_KILLFOCUS), so no key it said was down stays down. */
    void LetGoOfHeldRawKeys()
    {
        for (const RawKeyEvent &up : raw_keys_held_.TakeUps())
        {
            if (raw_key_)
            {
                raw_key_(up);
            }
        }
    }

    /** The surface let go, once - what its destruction does, and what a popup's closing with the window it opened
        over does early. A file dialog's caller, below this on the stack when the host closes the editor inside the
        dialog's loop, answers nothing from here on, and the dialog goes; the popups opened over the surface close, and
        theirs; the tree loses its host and the window goes. Then each closed popup's `closed_by_window` runs - a menu
        ends dismissed, its owner free to drop it there - when nothing of the surface is left for it to reach. */
    void Release()
    {
        std::vector<std::function<void()>> heard;
        ReleaseInto(heard);
        for (const auto &closed : heard)
        {
            closed();
        }
    }

    /** Release's letting go, the closed popups' `closed_by_window` gathered into `heard` for the caller to run.
        @param heard  the `closed_by_window` callbacks of the popups closed, appended to */
    void ReleaseInto(std::vector<std::function<void()>> &heard)
    {
        if (released_)
        {
            return;
        }
        released_   = true;
        life_->gone = true;
        if (life_->dialog_up != nullptr)
        {
            life_->dialog_up->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }
        EndPopups(heard);
        root_.SetHost(nullptr);
        if (hwnd_ != nullptr)
        {
            if (focus_taken_ && GetFocus() == hwnd_ && previous_focus_ != nullptr && IsWindow(previous_focus_))
            {
                SetFocus(previous_focus_);
            }
            KillTimer(hwnd_, kFrameTimer);
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (registered_ && --window_count == 0)
        {
            UnregisterClassW(ClassName(), ModuleInstance());
        }
        registered_ = false;
    }

    /** The raster made again at the root's size times the scale, cleared, with the Skia surface over it. */
    void ResizeRaster()
    {
        pixel_width_  = std::max(1, static_cast<int>(std::lround(root_.width() * scale_)));
        pixel_height_ = std::max(1, static_cast<int>(std::lround(root_.height() * scale_)));
        pixels_.assign(static_cast<std::size_t>(pixel_width_) * static_cast<std::size_t>(pixel_height_), 0u);
        surface_ = SkSurfaces::WrapPixels(
            SkImageInfo::Make(pixel_width_, pixel_height_, kBGRA_8888_SkColorType, kPremul_SkAlphaType), pixels_.data(),
            static_cast<std::size_t>(pixel_width_) * 4u);
    }

    /** The design pixels `area` covers, in the window's pixels, rounded out.
        @param area  an area, in design pixels
        @return      the smallest whole-pixel rectangle that holds it, in window pixels */
    gfx::RectI ToPixels(gfx::RectI area) const noexcept
    {
        return gfx::RectF{area.x() * scale_, area.y() * scale_, area.width() * scale_, area.height() * scale_}
            .SmallestIntegerContainer();
    }

    view::View                &root_;            ///< the tree drawn.
    view::MouseRouter          router_;          ///< hands the mouse, keys and drops to the tree.
    float                      scale_;           ///< window pixels per design pixel.
    HWND                       hwnd_{nullptr};   ///< the window; null before it is made and after the surface lets go.
    int                        pixel_width_{1};  ///< the raster's width, in pixels.
    int                        pixel_height_{1}; ///< the raster's height, in pixels.
    std::vector<std::uint32_t> pixels_;          ///< the raster, B G R A with premultiplied alpha, top-down.
    sk_sp<SkSurface>           surface_;         ///< Skia's surface over the raster.
    std::function<void()>      frame_;           ///< the frame callback, run on the frame timer; may be empty.
    /** What the window does with a message its display clock's thread posted - a frame due, or the clock giving up
        (ChildWindow::SetFramePacing). **Empty until a product asks to be paced on the display's clock**, and nothing
        posts those messages until then: a popup's and an unasked child window's is empty for the window's whole
        life, and the frame callback runs from WM_TIMER alone, exactly as it always did. */
    std::function<void(UINT message)> paced_message_;
    bool                              no_activate_{false}; ///< a click does not activate the window: a popup's.
    /** The window a key no view took is handed on to (ChildWindow::HandOnKeysTo); null to hand on none. */
    HWND key_target_{nullptr};
    /** Told of every raw key edge (ChildWindow::SetRawKeyCallback); empty until an owner asks, and then nothing of the
        raw keys is read or delivered. */
    RawKeyCallback raw_key_;
    /** The keys the raw key callback has been told are down, which answer whether a down is auto-repeat's, whether an
        up is owed, and what is to be let go as the window loses the keyboard. */
    RawKeysHeld raw_keys_held_;

  private:
    /** A press to the router, the mouse captured until its release.
        @param lparam  the message's client position
        @param clicks  1 for a press, 2 for a double click
        @param right   true for the right button */
    void Press(LPARAM lparam, int clicks, bool right)
    {
        pressed_ = true;
        SetCapture(hwnd_);
        router_.Down(PointOf(lparam), clicks, right, CurrentModifiers());
    }

    /** A mouse message's position as the views see it.
        @param lparam  the message's client position, in window pixels
        @return        the position, in design pixels */
    gfx::PointF PointOf(LPARAM lparam) const noexcept
    {
        return {static_cast<float>(GET_X_LPARAM(lparam)) / scale_, static_cast<float>(GET_Y_LPARAM(lparam)) / scale_};
    }

    /** The tree drawn over `area` (the window's pixels), and the raster put on screen, which the paint's own clip
        holds to the part that changed.
        @param dc    the device context WM_PAINT gave
        @param area  the part to draw again, in window pixels */
    void Paint(HDC dc, const RECT &area)
    {
        if (!surface_)
        {
            return;
        }
        const gfx::RectI dirty = gfx::RectI::LeftTopRightBottom(area.left, area.top, area.right, area.bottom)
                                     .Intersection({pixel_width_, pixel_height_});
        if (!dirty.IsEmpty())
        {
            // Only the dirty part is drawn again: the rest of the raster still holds the last frame.
            const gfx::RectI design =
                gfx::RectF{dirty.x() / scale_, dirty.y() / scale_, dirty.width() / scale_, dirty.height() / scale_}
                    .SmallestIntegerContainer();
            SkCanvas *sk = surface_->getCanvas();
            sk->save();
            sk->resetMatrix();
            sk->scale(scale_, scale_);
            {
                gfx::SkiaCanvas canvas(*sk, 1.0f);
                canvas.ClipToRect(design);
                view::PaintTree(root_, canvas);
            }
            sk->restore();
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth       = pixel_width_;
        info.bmiHeader.biHeight      = -pixel_height_; // top-down, as the raster lies
        info.bmiHeader.biPlanes      = 1;
        info.bmiHeader.biBitCount    = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(pixel_width_), static_cast<DWORD>(pixel_height_), 0, 0, 0,
                          static_cast<UINT>(pixel_height_), pixels_.data(), &info, DIB_RGB_COLORS);
    }

    /** A file dialog asked for and waiting for its message to open. */
    struct PendingDialog
    {
        view::FileRequest                        request; ///< the dialog's title, patterns and folder.
        std::function<void(const std::string &)> picked;  ///< told the path chosen.
    };

    std::deque<std::function<void()>> calls_;   ///< the calls CallSoon queued, one run per kCallSoonMessage.
    std::deque<PendingDialog>         dialogs_; ///< the file dialogs asked for, one opened per kChooseFileMessage.
    std::shared_ptr<SurfaceLife>      life_{std::make_shared<SurfaceLife>()}; ///< what a file dialog's caller reads.
    std::vector<WinPopup *>           popups_;                  ///< the popups opened over it, which go with it
    bool                              released_{false};         ///< true once the surface has let go.
    bool                              registered_{false};       ///< true while this window counts in window_count.
    bool                              pressed_{false};          ///< a press is down, the mouse captured.
    bool                              tracking_{false};         ///< a WM_MOUSELEAVE is asked for.
    bool                              focus_taken_{false};      ///< the keyboard focus was taken for a view.
    HWND                              previous_focus_{nullptr}; ///< the window that had the focus before it was taken.
    view::Cursor                      cursor_{view::Cursor::kArrow}; ///< the cursor the views last asked for.
};

namespace
{
/** The open popups, for the thread's mouse hook, which tells each of a press outside it. */
std::vector<WinPopup *> open_popups;

/** The thread's mouse hook, installed with the first open popup and removed with the last; null when none is open. */
HHOOK mouse_hook = nullptr;

/** The mouse hook's procedure: each button press, told to every open popup as a screen point.
    @param code    HC_ACTION for a message to handle
    @param wparam  the mouse message
    @param lparam  a MOUSEHOOKSTRUCT with the press's screen point
    @return        the next hook's result */
LRESULT CALLBACK PopupMouseHook(int code, WPARAM wparam, LPARAM lparam);
} // namespace

/** A popup: the surface in a no-activate tool window over everything, and the thread's mouse hook telling its opener
    of every press outside it. It goes with the surface it opened over (CloseInto) if that goes first. */
class WinPopup final : public view::Popup, public WinSurface
{
  public:
    /** A popup over `opener`, not yet shown.
        @param opener   the surface it opens over, which lets it go if it goes first; outlives the popup or lets it go
        @param content  the popup's root view; outlives the popup
        @param scale    window pixels per design pixel
        @param outside  runs with the screen point of every press outside the popup */
    WinPopup(WinSurface &opener, view::View &content, float scale, std::function<void(gfx::PointI)> outside)
        : WinSurface(content, scale), opener_(&opener), outside_(std::move(outside))
    {
        no_activate_ = true;
    }

    /** The popup taken off its opener's list and off the mouse hook's. */
    ~WinPopup() override
    {
        if (opener_ != nullptr)
        {
            opener_->Forget(this);
        }
        Unhook();
    }

    /** Closed with the surface it opened over: off the mouse hook's list, then the surface let go - its own popups
        with it - and its `closed_by_window` gathered into `heard` after theirs, the handle left inert for its owner to
        drop, its destruction then reaching nothing.
        @param heard  the `closed_by_window` callbacks, this popup's after those of the popups opened over it */
    void CloseInto(std::vector<std::function<void()>> &heard)
    {
        if (closed_)
        {
            return;
        }
        closed_ = true;
        opener_ = nullptr;
        Unhook();
        ReleaseInto(heard);
        if (closed_by_window)
        {
            heard.push_back(closed_by_window);
        }
    }

    /** Make the window, show it without activating it and put it on the mouse hook's list.
        @param owner        the top-level window it is owned by
        @param screen       its top left, in screen pixels
        @param takes_mouse  false for a popup clicks fall through
        @return             true when the window was made */
    bool Open(HWND owner, POINT screen, bool takes_mouse)
    {
        DWORD ex_style = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
        if (!takes_mouse)
        {
            ex_style |= WS_EX_TRANSPARENT | WS_EX_LAYERED; // clicks fall through it
        }
        if (!CreateSurface(owner, WS_POPUP, ex_style, screen.x, screen.y))
        {
            return false;
        }
        if (!takes_mouse)
        {
            SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        }
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        open_popups.push_back(this);
        if (mouse_hook == nullptr)
        {
            mouse_hook = SetWindowsHookExW(WH_MOUSE, PopupMouseHook, nullptr, GetCurrentThreadId());
        }
        return true;
    }

    bool Contains(gfx::PointI screen) const override
    {
        if (hwnd_ == nullptr)
        {
            return false; // closed with the surface it opened over
        }
        RECT r{};
        GetWindowRect(hwnd_, &r);
        return screen.x >= r.left && screen.x < r.right && screen.y >= r.top && screen.y < r.bottom;
    }

    /** A press anywhere on the thread: the popup's opener hears of it when it lands outside the popup.
        @param screen  the press, in screen pixels */
    void PressedAt(gfx::PointI screen)
    {
        if (!Contains(screen) && outside_)
        {
            outside_(screen);
        }
    }

  private:
    /** Off the thread's mouse hook's list, the hook going with the last popup on it. */
    void Unhook()
    {
        const auto listed = std::find(open_popups.begin(), open_popups.end(), this);
        if (listed != open_popups.end())
        {
            open_popups.erase(listed);
        }
        if (open_popups.empty() && mouse_hook != nullptr)
        {
            UnhookWindowsHookEx(mouse_hook);
            mouse_hook = nullptr;
        }
    }

    WinSurface                      *opener_;        ///< the surface it opened over; null once closed with it.
    std::function<void(gfx::PointI)> outside_;       ///< told the screen point of each press outside the popup.
    bool                             closed_{false}; ///< true once CloseInto has run.
};

void WinSurface::EndPopups(std::vector<std::function<void()>> &heard)
{
    for (WinPopup *popup : std::exchange(popups_, {}))
    {
        popup->CloseInto(heard);
    }
}

namespace
{
LRESULT CALLBACK PopupMouseHook(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION && (wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN || wparam == WM_MBUTTONDOWN ||
                              wparam == WM_NCLBUTTONDOWN || wparam == WM_NCRBUTTONDOWN))
    {
        const auto                   *mouse = reinterpret_cast<const MOUSEHOOKSTRUCT *>(lparam);
        const gfx::PointI             at{mouse->pt.x, mouse->pt.y};
        const std::vector<WinPopup *> now = open_popups; // a popup may close as it hears
        for (WinPopup *popup : now)
        {
            if (std::find(open_popups.begin(), open_popups.end(), popup) != open_popups.end())
            {
                popup->PressedAt(at);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
} // namespace

std::unique_ptr<view::Popup> WinSurface::OpenPopup(view::View &content, gfx::PointI at,
                                                   std::function<void(gfx::PointI)> pressed_outside, bool takes_mouse)
{
    if (released_ || hwnd_ == nullptr)
    {
        return nullptr;
    }
    POINT screen{static_cast<LONG>(std::lround(at.x * scale_)), static_cast<LONG>(std::lround(at.y * scale_))};
    ClientToScreen(hwnd_, &screen);
    // Kept whole on the monitor it opens on: moved in from an edge it would cross.
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST), &monitor))
    {
        const LONG width  = std::lround(content.width() * scale_);
        const LONG height = std::lround(content.height() * scale_);
        screen.x          = std::max(monitor.rcWork.left, std::min(screen.x, monitor.rcWork.right - width));
        screen.y          = std::max(monitor.rcWork.top, std::min(screen.y, monitor.rcWork.bottom - height));
    }
    auto popup = std::make_unique<WinPopup>(*this, content, scale_, std::move(pressed_outside));
    if (!popup->Open(GetAncestor(hwnd_, GA_ROOT), screen, takes_mouse))
    {
        return nullptr;
    }
    popups_.push_back(popup.get());
    return popup;
}

/** The child window inside the host's editor: the surface as a child HWND, with the frame timer. */
class WinChildWindow final : public ChildWindow, public WinSurface
{
  public:
    /** A child window for `root`, not yet made.
        @param root   the view tree to draw; outlives the window
        @param scale  window pixels per design pixel */
    WinChildWindow(view::View &root, float scale) : WinSurface(root, scale)
    {
    }

    /** The display clock stopped and its thread joined before the window itself goes, so nothing of a window being
        destroyed is posted to. A window on the platform's timer has no clock to stop. */
    ~WinChildWindow() override
    {
        clock_.Stop();
    }

    /** Make the window as a visible child of the host's.
        @param parent  the host's window
        @return        true when the window was made */
    bool Create(HWND parent)
    {
        return CreateSurface(parent, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0);
    }

    void SetScale(float scale) override
    {
        scale_ = scale;
        ResizeRaster();
        SetWindowPos(hwnd_, nullptr, 0, 0, pixel_width_, pixel_height_, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SetFrameCallback(std::function<void()> frame) override
    {
        frame_ = std::move(frame);
        ArrangePacing();
    }

    /** The frame callback paced as `request` asks: the display's own clock in a thread of this window's own where
        this Windows has a compositor whose timing can be read and a rate the compositor and the window's display
        agree on, and the window timer it has always used where it has not.
        @param request  what the product asks for; FrameRequest{} for the platform's timer
        @return         what the window is pacing on, and why where it fell back */
    FramePacingState SetFramePacing(const FrameRequest &request) override
    {
        request_ = request;
        pacing_  = DecidedPacing();
        if (pacing_.pacing == FramePacing::kDisplayClock && !paced_message_)
        {
            // Installed once a product has asked and never taken back: a frame still on the queue as the clock stops
            // is a frame callback the platform timer would have run anyway.
            paced_message_ = [this](UINT message) { HandlePacedMessage(message); };
        }
        ArrangePacing();
        return FramePacingNow();
    }

    /** What the window is pacing on now, with the rate of the display it is on now: the clock's thread re-reads that
        as the window moves between monitors, so the rate here is not always the one SetFramePacing answered with.
        @return  the pacing in force */
    FramePacingState FramePacingNow() const override
    {
        FramePacingState state = pacing_;
        if (state.pacing == FramePacing::kDisplayClock)
        {
            if (const double hertz = clock_.display_hertz_now(); hertz > 0.0)
            {
                state.display_hertz   = hertz;
                state.frames_a_second = hertz / state.refresh_divisor;
            }
        }
        return state;
    }

    /** Visible with every window above it, and its top-level window not minimised.
        @return true when the window shows */
    bool Showing() const override
    {
        return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE && IsIconic(GetAncestor(hwnd_, GA_ROOT)) == FALSE;
    }

    /** Keys no view takes handed on to `top_level`, the window this one sits in, as win::kHandedKeyDown and
        win::kHandedChar: Windows gives a key to the window with the focus alone, and this one holds the focus while a
        view has the keyboard.
        @param top_level  the HWND of the window this one sits in; null to hand on none */
    void HandOnKeysTo(void *top_level) override
    {
        key_target_ = static_cast<HWND>(top_level);
    }

    /** Every raw key edge told to `raw_key`: the key downs the views are given too (WM_KEYDOWN, WM_SYSKEYDOWN) and the
        key ups Windows alone answered before (WM_KEYUP, WM_SYSKEYUP), with everything let go at WM_KILLFOCUS.
        @param raw_key  told of each edge, on the window's thread; empty to ask for no more */
    void SetRawKeyCallback(RawKeyCallback raw_key) override
    {
        TakeRawKeys(std::move(raw_key));
    }

  private:
    /** The pacing the request and this window's display decide between, with nothing started: what SetFramePacing
        answers whether or not there is a frame callback yet. The display clock's own thread judges whether the
        refresh period holds still, which nothing can know before a frame has been waited for, so that is left
        believed here.
        @return  the state, whose `why` says which step refused */
    FramePacingState DecidedPacing() const
    {
        const CompositionTiming timing = CompositionTimingNow();
        DisplayClockReading     reading;
        reading.has_clock        = timing.is_read;
        reading.compositor_hertz = timing.hertz;
        reading.display_hertz    = DisplayRefreshHertzOf(hwnd_);
        return ChosenPacing(request_, reading);
    }

    /** The frame callback's source arranged for the pacing in force: the display clock's thread where that is what
        was decided and the clock can be made, and the platform's window timer otherwise - which is what a window
        with no request of its own always reaches, as KillTimer and SetTimer alone. Nothing runs while there is no
        callback. */
    void ArrangePacing()
    {
        KillTimer(hwnd_, kFrameTimer);
        clock_.Stop();
        if (!frame_)
        {
            return;
        }
        if (pacing_.pacing == FramePacing::kDisplayClock)
        {
            if (clock_.Start(hwnd_, request_))
            {
                return;
            }
            // No high-resolution waitable timer to be had: the platform's own timer, and the reason said rather than
            // pacing on the system clock's 15.625 ms under another name.
            pacing_ = {FramePacing::kPlatformTimer, 0, pacing_.display_hertz, 0.0,
                       FramePacingFallback::kNoDisplayClock};
        }
        SetTimer(hwnd_, kFrameTimer, kFrameMilliseconds, nullptr);
    }

    /** A message the display clock's thread posted, on this window's thread: a frame due - the clock told it has been
        taken, and the callback run as the timer's is - or the clock giving up, which puts the window back on the
        platform's timer with the reason kept for the product to read.
        @param message  kDisplayFrameMessage or kDisplayClockGaveUpMessage */
    void HandlePacedMessage(UINT message)
    {
        if (message == kDisplayFrameMessage)
        {
            clock_.FrameTaken();
            if (frame_)
            {
                frame_();
            }
            return;
        }
        pacing_ = {FramePacing::kPlatformTimer, 0, pacing_.display_hertz, 0.0, clock_.gave_up_because()};
        ArrangePacing();
    }

    FrameRequest     request_{}; ///< what the product asked for; FrameRequest{} until it asks, which is the default.
    FramePacingState pacing_{};  ///< what the window is pacing on, and why where it fell back.
    WinDisplayClock  clock_;     ///< the display clock's thread; stopped, and holding nothing, until a product asks.
};

namespace
{
LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        auto *created = static_cast<WinSurface *>(reinterpret_cast<const CREATESTRUCTW *>(lparam)->lpCreateParams);
        created->Adopt(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    auto *surface = reinterpret_cast<WinSurface *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return surface != nullptr ? surface->Handle(message, wparam, lparam)
                              : DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace

void RevealInFileManager(const std::string &path)
{
    // As JUCE's File::revealToUser: the item selected in the file manager's window of its folder, through the shell's
    // item list, with COM up for the call as the shell asks.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(WideOf(path).c_str()))
    {
        SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ILFree(item);
    }
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
}

void OpenInBrowser(const std::string &url)
{
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)
    {
        return;
    }
    // The shell hands the URL to the default browser, with COM up for the call as the shell asks.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ShellExecuteW(nullptr, L"open", WideOf(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
}

float DisplayScaleOf(void *parent)
{
    // GetDpiForWindow is Windows 10's (1607 on), so it is looked up rather than linked.
    using GetDpiForWindowFunction = UINT(WINAPI *)(HWND);
    static const auto get_dpi     = reinterpret_cast<GetDpiForWindowFunction>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    HWND window = static_cast<HWND>(parent);
    if (get_dpi == nullptr || window == nullptr)
    {
        return 1.0f;
    }
    const UINT dpi = get_dpi(window);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

std::unique_ptr<ChildWindow> CreateChildWindow(void *parent, view::View &root, int width, int height, float scale,
                                               std::unique_ptr<RunLoop>)
{
    HWND parent_window = static_cast<HWND>(parent);
    if (parent_window == nullptr || !IsWindow(parent_window))
    {
        return nullptr;
    }
    root.SetBounds({width, height});
    auto child = std::make_unique<WinChildWindow>(root, scale);
    if (!child->Create(parent_window))
    {
        return nullptr;
    }
    return child;
}
} // namespace reckless::window
