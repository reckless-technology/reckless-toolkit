#pragma once
// The display's clock on X11 (ChildWindow::SetFramePacing, window/FramePacing.h, docs/window/frame-pacing.md): the
// vertical blanks of the monitor a window is on, as the X server's Present extension reports them, and the rate of
// that monitor's mode, as RandR names it.
//
// Present reports a blank without anything being presented: PresentNotifyMSC asks the server to say when a window's
// monitor reaches a media stream count - a count of its blanks - and PresentCompleteNotify says when it did, with the
// count and the moment (its UST, on the server's steady clock). Asked for the next count each time it answers, it is a
// report of every blank, read on the window's own connection to the display, which the host's loop already watches. A
// frame is then woken on a timerfd at the blank it is due at less the product's lead, which the same loop watches - so
// there is no thread, a plug-in's window inside a host's loop is paced as an app's is, and the frame callback runs on
// the window's thread as the timer's always has. Measured on jonny-precision (Intel UHD 630 driving the panel,
// Cinnamon's Muffin compositing every window, 2026-11-09): the stamps 16.6664 ms apart with a standard deviation of
// 0.006 ms while the screen is drawn, and each report arriving within 0.2 ms of the last one's period.
//
// The rate is the mode's and nothing else: an X server with no monitor behind it - Xvfb, or a window on no monitor at
// all - still answers PresentNotifyMSC, from a timer of its own about sixty times a second, but its mode names no
// timing, so there is no rate to divide and the window stays on the platform's timer and says so
// (FramePacingFallback::kRefreshRateUnknown). An X server without Present says kNoDisplayClock.
//
// Neither library is linked: libXpresent and libXrandr are opened by name the first time a window is asked for the
// display's clock or its rate, as Windows' compositor calls are looked up from dwmapi.dll - so no consumer's link line
// changes, a machine without them is the fallback, and a window no one asks opens neither. Their headers are needed
// at build time (libxpresent-dev, which brings libxrandr-dev); a build without them compiles the fallback alone.
//
// Included by WindowLinux.cpp alone, after Xlib.
#include <dlfcn.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "FramePacing.h"

#include <X11/Xlib.h>

#if __has_include(<X11/extensions/Xpresent.h>) && __has_include(<X11/extensions/Xrandr.h>)
#include <X11/extensions/Xpresent.h>
#include <X11/extensions/Xrandr.h>
/// Whether this build can read a display clock at all: the Present and RandR headers were there to build against.
#define RECKLESS_X11_DISPLAY_CLOCK 1
#else
#define RECKLESS_X11_DISPLAY_CLOCK 0
#endif

namespace reckless::window
{
namespace x11
{
/** The steady clock a window waits on, which the X server's blank stamps are on where it runs on the same machine
    (BlankOnTheClock): CLOCK_MONOTONIC, in seconds.
    @return  the clock now */
inline double SteadySeconds() noexcept
{
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1.0e-9;
}

#if RECKLESS_X11_DISPLAY_CLOCK
/** The calls of libXpresent and libXrandr this needs, looked up the first time either is needed and kept for the life
    of the process: a library opened by name is never closed, so no call looked up from it can outlive it. */
struct DisplayClockCalls
{
    decltype(&XPresentQueryExtension)       present_query_extension{nullptr};     ///< XPresentQueryExtension
    decltype(&XPresentSelectInput)          present_select_input{nullptr};        ///< XPresentSelectInput
    decltype(&XPresentFreeInput)            present_free_input{nullptr};          ///< XPresentFreeInput
    decltype(&XPresentNotifyMSC)            present_notify_msc{nullptr};          ///< XPresentNotifyMSC
    decltype(&XRRQueryExtension)            randr_query_extension{nullptr};       ///< XRRQueryExtension
    decltype(&XRRGetScreenResourcesCurrent) randr_screen_resources{nullptr};      ///< XRRGetScreenResourcesCurrent
    decltype(&XRRFreeScreenResources)       randr_free_screen_resources{nullptr}; ///< XRRFreeScreenResources
    decltype(&XRRGetCrtcInfo)               randr_crtc_info{nullptr};             ///< XRRGetCrtcInfo
    decltype(&XRRFreeCrtcInfo)              randr_free_crtc_info{nullptr};        ///< XRRFreeCrtcInfo
    decltype(&XRRGetOutputPrimary)          randr_output_primary{nullptr};        ///< XRRGetOutputPrimary
    decltype(&XRRGetOutputInfo)             randr_output_info{nullptr};           ///< XRRGetOutputInfo
    decltype(&XRRFreeOutputInfo)            randr_free_output_info{nullptr};      ///< XRRFreeOutputInfo

    /** @return  true where every Present call was found */
    bool HasPresent() const noexcept
    {
        return present_query_extension != nullptr && present_select_input != nullptr && present_free_input != nullptr &&
               present_notify_msc != nullptr;
    }

    /** @return  true where every RandR call was found */
    bool HasRandr() const noexcept
    {
        return randr_query_extension != nullptr && randr_screen_resources != nullptr &&
               randr_free_screen_resources != nullptr && randr_crtc_info != nullptr &&
               randr_free_crtc_info != nullptr && randr_output_primary != nullptr && randr_output_info != nullptr &&
               randr_free_output_info != nullptr;
    }
};

/** `name` looked up in `library`, as the call's own type.
    @param library  the library, opened; null for none, which finds nothing
    @param name     the call's name
    @return         the call, or null where the library has none of that name */
template <typename Call> Call LookedUp(void *library, const char *name) noexcept
{
    return library == nullptr ? nullptr : reinterpret_cast<Call>(dlsym(library, name));
}

/** The calls, looked up once for the process (DisplayClockCalls).
    @return  the calls; any of them null where its library or the call could not be found */
inline const DisplayClockCalls &TheDisplayClockCalls()
{
    static const DisplayClockCalls calls = [] {
        DisplayClockCalls found;
        void             *present = dlopen("libXpresent.so.1", RTLD_NOW | RTLD_LOCAL);
        void             *randr   = dlopen("libXrandr.so.2", RTLD_NOW | RTLD_LOCAL);
        found.present_query_extension =
            LookedUp<decltype(found.present_query_extension)>(present, "XPresentQueryExtension");
        found.present_select_input  = LookedUp<decltype(found.present_select_input)>(present, "XPresentSelectInput");
        found.present_free_input    = LookedUp<decltype(found.present_free_input)>(present, "XPresentFreeInput");
        found.present_notify_msc    = LookedUp<decltype(found.present_notify_msc)>(present, "XPresentNotifyMSC");
        found.randr_query_extension = LookedUp<decltype(found.randr_query_extension)>(randr, "XRRQueryExtension");
        found.randr_screen_resources =
            LookedUp<decltype(found.randr_screen_resources)>(randr, "XRRGetScreenResourcesCurrent");
        found.randr_free_screen_resources =
            LookedUp<decltype(found.randr_free_screen_resources)>(randr, "XRRFreeScreenResources");
        found.randr_crtc_info        = LookedUp<decltype(found.randr_crtc_info)>(randr, "XRRGetCrtcInfo");
        found.randr_free_crtc_info   = LookedUp<decltype(found.randr_free_crtc_info)>(randr, "XRRFreeCrtcInfo");
        found.randr_output_primary   = LookedUp<decltype(found.randr_output_primary)>(randr, "XRRGetOutputPrimary");
        found.randr_output_info      = LookedUp<decltype(found.randr_output_info)>(randr, "XRRGetOutputInfo");
        found.randr_free_output_info = LookedUp<decltype(found.randr_free_output_info)>(randr, "XRRFreeOutputInfo");
        return found;
    }();
    return calls;
}

/** The refresh rate of `mode` in `resources`, as its timing gives it (RefreshHertzOfModeTiming).
    @param resources  the screen's resources
    @param mode       the mode
    @return           the rate, in hertz; 0 where the mode is not listed or names no timing */
inline double RefreshHertzOfMode(const XRRScreenResources &resources, RRMode mode) noexcept
{
    for (int i = 0; i < resources.nmode; ++i)
    {
        const XRRModeInfo &info = resources.modes[i];
        if (info.id == mode)
        {
            return RefreshHertzOfModeTiming(static_cast<double>(info.dotClock), static_cast<int>(info.hTotal),
                                            static_cast<int>(info.vTotal), (info.modeFlags & RR_Interlace) != 0,
                                            (info.modeFlags & RR_DoubleScan) != 0);
        }
    }
    return 0.0;
}
#endif

/** The refresh rate of the mode of the monitor `window` is on (ChildWindow::DisplayModeHertz): the CRTC the most of the
    window lies on, or the primary output's where it lies on none. Reads RandR afresh each time, a few round trips to
    the server.
    @param display  the window's connection
    @param window   the window
    @return         the rate, in hertz; 0 where nothing can say - no RandR, no monitor, or a mode that names no timing,
                    as Xvfb's does */
inline double ModeHertzUnder(Display *display, ::Window window)
{
#if RECKLESS_X11_DISPLAY_CLOCK
    const DisplayClockCalls &calls      = TheDisplayClockCalls();
    int                      event_base = 0;
    int                      error_base = 0;
    if (!calls.HasRandr() || display == nullptr || window == 0 ||
        !calls.randr_query_extension(display, &event_base, &error_base))
    {
        return 0.0;
    }
    XWindowAttributes attributes{};
    ::Window          child = 0;
    int               left  = 0;
    int               top   = 0;
    if (XGetWindowAttributes(display, window, &attributes) == 0 ||
        !XTranslateCoordinates(display, window, attributes.root, 0, 0, &left, &top, &child))
    {
        return 0.0;
    }
    XRRScreenResources *resources = calls.randr_screen_resources(display, attributes.root);
    if (resources == nullptr)
    {
        return 0.0;
    }
    RRMode    mode         = 0;
    long long most_overlap = 0;
    for (int i = 0; i < resources->ncrtc; ++i)
    {
        XRRCrtcInfo *crtc = calls.randr_crtc_info(display, resources, resources->crtcs[i]);
        if (crtc == nullptr)
        {
            continue;
        }
        const long long across = std::min<long long>(left + attributes.width, crtc->x + static_cast<int>(crtc->width)) -
                                 std::max<long long>(left, crtc->x);
        const long long down = std::min<long long>(top + attributes.height, crtc->y + static_cast<int>(crtc->height)) -
                               std::max<long long>(top, crtc->y);
        const long long overlap = across > 0 && down > 0 ? across * down : 0;
        if (crtc->mode != 0 && overlap > most_overlap)
        {
            most_overlap = overlap;
            mode         = crtc->mode;
        }
        calls.randr_free_crtc_info(crtc);
    }
    if (mode == 0)
    {
        // On no monitor at all: the primary output's, which is where a window that comes back on screen comes.
        if (const RROutput primary = calls.randr_output_primary(display, attributes.root); primary != 0)
        {
            if (XRROutputInfo *output = calls.randr_output_info(display, resources, primary); output != nullptr)
            {
                if (output->crtc != 0)
                {
                    if (XRRCrtcInfo *crtc = calls.randr_crtc_info(display, resources, output->crtc); crtc != nullptr)
                    {
                        mode = crtc->mode;
                        calls.randr_free_crtc_info(crtc);
                    }
                }
                calls.randr_free_output_info(output);
            }
        }
    }
    const double hertz = mode != 0 ? RefreshHertzOfMode(*resources, mode) : 0.0;
    calls.randr_free_screen_resources(resources);
    return hertz;
#else
    static_cast<void>(display);
    static_cast<void>(window);
    return 0.0;
#endif
}

/**
    One window's frames on its monitor's vertical blanks (the file's comment says how). Made by the window's connection
    the first time a product asks for the display's clock, and kept: started on each request that reaches the clock,
    stopped on one that does not or when the display under the window stops being one to divide.

    The schedule is window::FrameClock's, kept on the blanks themselves and told the time as now plus the lead
    (LedWakeSeconds), as Windows' is: each due time is pulled onto the grid of the last blank Present reported and the
    mode's period (OnTheRefreshGrid), so a frame period that is a whole number of refreshes lands on one, and the timer
    is set for that blank less the lead. A blank reported late, or not at all for a while, leaves the grid where the
   last one put it, carried forwards by the period, and the frames go on.

    Present is asked for the next blank each time it reports one, so a window on the clock hears of every blank of its
    monitor: an event a refresh on the window's connection, whichever way the window draws - the GPU's own swaps through
    Present are answered on EGL's queue, not this one, and any report of them here is passed over.
*/
class X11DisplayClock
{
  public:
    /** A clock on `display`, reading whether the server has Present and making the timer the frames are woken on;
        nothing is asked of the server until Start.
        @param display  the window's connection, which outlives the clock */
    explicit X11DisplayClock(Display *display) : display_(display)
    {
#if RECKLESS_X11_DISPLAY_CLOCK
        const DisplayClockCalls &calls      = TheDisplayClockCalls();
        int                      error_base = 0;
        int                      event_base = 0;
        has_present_ = calls.HasPresent() &&
                       calls.present_query_extension(display_, &present_opcode_, &event_base, &error_base) == True;
#endif
        timer_ = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    }

    /** Stopped, its timer closed. */
    ~X11DisplayClock()
    {
        Stop();
        if (timer_ >= 0)
        {
            close(timer_);
        }
    }

    X11DisplayClock(const X11DisplayClock &)            = delete;
    X11DisplayClock &operator=(const X11DisplayClock &) = delete;

    /** The timer the frames are woken on, for the host's loop to watch; -1 where it could not be made.
        @return  the descriptor */
    int timer() const noexcept
    {
        return timer_;
    }

    /** What `window` can be paced on, as `request` asks, and the clock started where that is the display's: the
       window's monitor's mode read, and Present asked for the next blank. A request the display cannot meet leaves the
       clock stopped, and the answer says why.
        @param window   the window paced; outlives the clock's running
        @param request  the product's request, which asks for the display's clock
        @return         what the window is paced on */
    FramePacingState Start(::Window window, const FrameRequest &request)
    {
        Stop();
        window_  = window;
        request_ = request;
        DisplayClockReading reading;
        reading.has_clock     = has_present_ && timer_ >= 0;
        reading.display_hertz = reading.has_clock ? ModeHertzUnder(display_, window_) : 0.0;
        state_                = ChosenPacing(request_, reading);
        if (state_.pacing != FramePacing::kDisplayClock)
        {
            return state_;
        }
#if RECKLESS_X11_DISPLAY_CLOCK
        is_running_     = true;
        mode_read_      = SteadySeconds();
        period_seconds_ = 1.0 / state_.display_hertz;
        has_blank_      = false;
        heard_seconds_  = mode_read_;
        events_         = TheDisplayClockCalls().present_select_input(display_, window_, PresentCompleteNotifyMask);
        AskForTheNextBlank(0); // a count already past: answered at once, with the last blank
        clock_.Start(mode_read_ + Lead(), period_seconds_ * request_.refresh_divisor, request_.most_frames_behind);
        Arm();
#endif
        return state_;
    }

    /** Stopped: Present asked for nothing more, the timer disarmed; what it was pacing on is kept until the next Start,
        as what the window last answered. Nothing is done on a clock that is not running. */
    void Stop()
    {
        if (!is_running_)
        {
            return;
        }
        is_running_ = false;
        Disarm();
#if RECKLESS_X11_DISPLAY_CLOCK
        if (events_ != 0)
        {
            TheDisplayClockCalls().present_free_input(display_, window_, events_);
            events_ = 0;
        }
        XFlush(display_);
#endif
    }

    /** @return  the window paced: the one Present's selection is on while the clock runs; 0 before any Start */
    ::Window window() const noexcept
    {
        return window_;
    }

    /** @return  true while the window's frames are woken by this clock */
    bool is_running() const noexcept
    {
        return is_running_;
    }

    /** @return  what the window is paced on: the display's clock while running, and otherwise the fallback last decided
     */
    const FramePacingState &state() const noexcept
    {
        return state_;
    }

    /** `event` taken where it is a report of the window's monitor's blank: the grid moved onto it and the next blank
        asked for. Any other Present event on this connection - the GPU's swaps reported, or a blank of a clock since
        stopped - is taken and passed over, since none of them names a window a surface would know.
        @param event  an event off the connection
        @return       true where it was Present's, and nothing else is to read it */
    bool Handle(XEvent &event)
    {
#if RECKLESS_X11_DISPLAY_CLOCK
        if (!has_present_ || event.type != GenericEvent || event.xcookie.extension != present_opcode_)
        {
            return false;
        }
        if (XGetEventData(display_, &event.xcookie) == False)
        {
            return true;
        }
        if (event.xcookie.evtype == PresentCompleteNotify)
        {
            const auto *complete = static_cast<const XPresentCompleteNotifyEvent *>(event.xcookie.data);
            if (is_running_ && complete->kind == PresentCompleteKindNotifyMSC && complete->window == window_ &&
                complete->serial_number == serial_)
            {
                heard_seconds_      = SteadySeconds();
                blank_seconds_      = BlankOnTheClock(static_cast<double>(complete->ust) * 1.0e-6, heard_seconds_);
                const bool is_first = !has_blank_;
                has_blank_          = true;
                AskForTheNextBlank(complete->msc + 1);
                if (is_first)
                {
                    // The schedule started on the steady clock alone; the first blank heard of puts the frame already
                    // waited for on the grid, rather than leaving it for the frame after.
                    clock_.MoveNextFrameTo(
                        OnTheRefreshGrid(clock_.next_frame_seconds(), blank_seconds_, period_seconds_));
                    Arm();
                }
            }
        }
        XFreeEventData(display_, &event.xcookie);
        return true;
#else
        static_cast<void>(event);
        return false;
#endif
    }

    /** The timer's expiry read off it, on the host's loop calling back for it: true where a frame is due now, for the
        caller to run and then hand on to Advance. A wake of a clock that has since stopped, or of a timer re-armed
        for later, is no frame.
        @return  true where the frame is to run */
    bool TakeWaking() noexcept
    {
        std::uint64_t expiries = 0;
        const bool    has_read = read(timer_, &expiries, sizeof(expiries)) == static_cast<ssize_t>(sizeof(expiries));
        if (!is_running_ || !has_read)
        {
            return false;
        }
        woke_on_the_grid_ = SteadySeconds() + Lead();
        return true;
    }

    /** The frame just run handed on: the schedule advanced from the waking, pulled onto the grid, and the timer set for
        the next; once a second the mode under the window read again, which falls back where it no longer names a rate
        and starts the schedule again where it names another. A blank not heard of for kDisplayLinkSilentSeconds is
        asked for again, since a request that went unanswered would otherwise never be repeated. */
    void Advance()
    {
#if RECKLESS_X11_DISPLAY_CLOCK
        if (!is_running_)
        {
            return;
        }
        clock_.Advance(woke_on_the_grid_);
        const double now = SteadySeconds();
        if (now - mode_read_ >= kModeReadEverySeconds)
        {
            mode_read_ = now;
            DisplayClockReading reading;
            reading.has_clock           = true;
            reading.display_hertz       = ModeHertzUnder(display_, window_);
            const FramePacingState read = ChosenPacing(request_, reading);
            if (read.pacing != FramePacing::kDisplayClock)
            {
                Stop();
                state_ = read;
                return;
            }
            const double period = 1.0 / read.display_hertz;
            if (std::fabs(period - period_seconds_) > kRefreshPeriodsHoldStillWithin * period)
            {
                // Another rate under the window - a mode change, or a move to another monitor - and its own grid, so
                // nothing of the last one's is carried over.
                period_seconds_ = period;
                has_blank_      = false;
                clock_.Start(now + Lead(), period_seconds_ * request_.refresh_divisor, request_.most_frames_behind);
            }
            state_ = read;
        }
        if (IsDisplayLinkSilent(heard_seconds_, now))
        {
            heard_seconds_ = now;
            AskForTheNextBlank(0);
        }
        if (has_blank_)
        {
            clock_.MoveNextFrameTo(OnTheRefreshGrid(clock_.next_frame_seconds(), blank_seconds_, period_seconds_));
        }
        Arm();
#endif
    }

  private:
    /** How often the mode under a window on the clock is read again, in seconds: often enough that a window moved to a
        monitor of another rate is caught within a second, as a product reading FramePacingNow once a second expects,
        and seldom enough that its round trips are off almost every frame's path. */
    static constexpr double kModeReadEverySeconds = 1.0;

    /** @return  the lead in force, in seconds */
    double Lead() const noexcept
    {
        return static_cast<double>(state_.lead_microseconds) * 1.0e-6;
    }

    /** Present asked to report the window's monitor reaching `count`, and answered at once where it already has.
        @param count  the media stream count; 0 for whichever blank is next or just gone */
    void AskForTheNextBlank(std::uint64_t count)
    {
#if RECKLESS_X11_DISPLAY_CLOCK
        ++serial_;
        TheDisplayClockCalls().present_notify_msc(display_, window_, serial_, count, 0, 0);
        XFlush(display_);
#else
        static_cast<void>(count);
#endif
    }

    /** The timer set for the next frame's waking: its blank less the lead (LedWakeSeconds). A waking already past fires
        at once. */
    void Arm() noexcept
    {
        const double wake    = LedWakeSeconds(clock_.next_frame_seconds(), Lead());
        const double seconds = std::floor(wake);
        itimerspec   when{};
        when.it_value.tv_sec  = static_cast<time_t>(seconds);
        when.it_value.tv_nsec = static_cast<long>((wake - seconds) * 1.0e9);
        if (when.it_value.tv_sec <= 0 && when.it_value.tv_nsec <= 0)
        {
            when.it_value.tv_nsec = 1; // a zero would disarm it
        }
        timerfd_settime(timer_, TFD_TIMER_ABSTIME, &when, nullptr);
    }

    /** The timer disarmed, and any expiry it holds forgotten. */
    void Disarm() noexcept
    {
        const itimerspec never{};
        timerfd_settime(timer_, 0, &never, nullptr);
    }

    Display             *display_;               ///< the window's connection
    ::Window             window_{0};             ///< the window paced
    FrameRequest         request_{};             ///< the product's request
    FramePacingState     state_{};               ///< what the window is paced on, or last fell back to
    FrameClock           clock_;                 ///< the schedule, on the blanks themselves
    int                  timer_{-1};             ///< the timerfd each frame is woken on; -1 where it could not be made
    bool                 has_present_{false};    ///< the server has Present, and libXpresent was found
    bool                 is_running_{false};     ///< the frames are being woken
    bool                 has_blank_{false};      ///< a blank has been reported since the schedule started
    [[maybe_unused]] int present_opcode_{0};     ///< Present's major opcode, which its events carry
    [[maybe_unused]] XID events_{0};             ///< the Present event selection on the window; 0 for none
    std::uint32_t        serial_{0};             ///< the serial of the last NotifyMSC asked
    double               period_seconds_{0.0};   ///< the mode's refresh period, the grid's
    double               blank_seconds_{0.0};    ///< the last blank reported, on the steady clock
    double               heard_seconds_{0.0};    ///< when a blank was last heard of, or last asked for again
    double               mode_read_{0.0};        ///< when the mode was last read
    double               woke_on_the_grid_{0.0}; ///< the last waking plus the lead: the clock's own time for it
};
} // namespace x11
} // namespace reckless::window
