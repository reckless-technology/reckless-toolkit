#pragma once

// How a child window paces its frame callback (ChildWindow::SetFramePacing): the platform's own window timer, which
// is what every window here has run on since the toolkit began, or the display's own clock - the frame delivered on
// every Nth refresh of the display in front of the person, so that each frame drawn is shown for exactly N refresh
// periods. A product asks for N, never for a frequency: a frequency has no relationship to the panel, and a frame
// period that is not a whole number of refresh periods is held for six composition frames and then seven, which is
// what the eye reads as stutter (docs/window/frame-pacing.md has the measurements).
//
// Everything a window decides about pacing is here, and none of it needs a display, a compositor or a thread: the
// rate to divide and whether it can be believed, whether the display holds still enough to have a period at all, the
// subdivision arithmetic, the grid a due time is pulled onto, and how far behind a clock may fall before it gives up
// and starts again. So the suites hold all of it on every platform, as window/RawKey.h holds the raw keys' policy and
// audio/win/WasapiFormats.h holds Windows Audio's arithmetic. What is left to each platform is reading its own
// display's clock and waiting on it.

#include <cmath>
#include <cstddef>

namespace reckless::window
{
/** What a window paces its frame callback on. */
enum class FramePacing
{
    kPlatformTimer, ///< the platform's own window timer, which every window here has been paced by from the start
    kDisplayClock,  ///< the display's own clock, a frame every Nth refresh; kPlatformTimer wherever there is none
};

/** Why a window is on the platform's timer. Everything but kNone is a window that was asked for the display's clock
    and could not give it, and every one of them is reported rather than passed over: a product that asked for
    uniform motion and did not get it is owed the reason. */
enum class FramePacingFallback
{
    kNone,                     ///< nothing fell back: the window is on the display's clock
    kNotAsked,                 ///< no one asked for the display's clock - a window's state until a product does
    kRequestOutOfRange,        ///< the divisor was under 1, or the frames to make up were under 0
    kNoDisplayClock,           ///< the platform has no clock to pace on: no compositor timing, no display link
    kRefreshRateUnknown,       ///< neither the compositor nor the display said what the refresh rate is
    kRefreshRateNotBelievable, ///< the rate read is not one a display can have, so nothing is divided by it
    kRefreshRateDisagrees,     ///< the compositor and the window's own display named different rates
    kRefreshRateUnsteady,      ///< the refresh period will not hold still: a variable-refresh panel, mid-change
};

/** `why` in words, for a product to report or log.
    @param why  the reason
    @return     the words, a phrase in lower case, never null and never empty */
inline const char *TextOf(FramePacingFallback why) noexcept
{
    switch (why)
    {
    case FramePacingFallback::kNone:
        return "paced on the display's clock";
    case FramePacingFallback::kNotAsked:
        return "the display's clock was not asked for";
    case FramePacingFallback::kRequestOutOfRange:
        return "the pacing asked for is out of range";
    case FramePacingFallback::kNoDisplayClock:
        return "this platform has no display clock to pace on";
    case FramePacingFallback::kRefreshRateUnknown:
        return "nothing said what the display's refresh rate is";
    case FramePacingFallback::kRefreshRateNotBelievable:
        return "the refresh rate read is not one a display can have";
    case FramePacingFallback::kRefreshRateDisagrees:
        return "the compositor and the window's display name different refresh rates";
    case FramePacingFallback::kRefreshRateUnsteady:
        return "the display's refresh period will not hold still";
    }
    return "unknown";
}

/**
    What a product asks of its window's frame callback. Every number here is the product's: the toolkit carries no
    rate, no divisor and no patience of its own beyond the platform timer it has always run on, and a default-made
    FrameRequest asks for exactly what a window did before any of this existed.

    `refresh_divisor` is the whole number of the display's refresh periods each drawn frame is to be shown for, and it
    is the request - not a frequency. On a 60 Hz panel a divisor of 1 gives 60 frames a second and 2 gives 30; on a
    240 Hz panel 4 gives 60 and 8 gives 30. A product that wants half rate for something cheap asks for twice its
    divisor and its motion stays uniform at half the cost, which a halved frequency cannot promise on a panel whose
    rate it does not know.
*/
struct FrameRequest
{
    /** What to pace on. The default is the platform's own timer: what every window did before this existed. */
    FramePacing pacing{FramePacing::kPlatformTimer};

    /** N: the display's refresh periods each drawn frame is shown for, 1 or more. Read only where `pacing` asks for
        the display's clock. */
    int refresh_divisor{1};

    /** How many whole frames of lateness the clock will try to make up before it gives up and starts again from the
        display's next grid instant, 0 or more: 0 for a clock that re-phases at the first sign of lateness rather than
        ever running two frames together, and more for a product that would rather catch up. Read only where `pacing`
        asks for the display's clock. */
    int most_frames_behind{1};
};

/** What a window ended up pacing on, for a product to report, log or adapt to (ChildWindow::SetFramePacing,
    ChildWindow::FramePacingNow). */
struct FramePacingState
{
    /** What the frame callback is paced on: kPlatformTimer wherever the display's clock could not be had, and `why`
        then says which of the reasons it was. */
    FramePacing pacing{FramePacing::kPlatformTimer};

    /** The divisor in force: the refresh periods each frame is shown for. 0 on the platform timer, which divides
        nothing. */
    int refresh_divisor{0};

    /** The display's refresh rate, in hertz, as the window read it. 0 where nothing said, and on the platform timer,
        which reads none. */
    double display_hertz{0.0};

    /** The frames a second the callback is being run at: `display_hertz` over `refresh_divisor`. **0 on the platform
        timer**, whose rate is not a number the window knows - it is the platform's, it is not the period asked for,
        and on Windows it is not even steady (docs/window/frame-pacing.md). */
    double frames_a_second{0.0};

    /** Why the window is on the platform's timer; kNone where it is on the display's clock. */
    FramePacingFallback why{FramePacingFallback::kNotAsked};
};

/** The lowest refresh rate, in hertz, a reading is believed at. Below this nothing a display does is being described,
    so the reading is refused rather than divided by - it is a bound on what a panel can be, not a rate anything here
    wants. */
inline constexpr double kLowestBelievableRefreshHertz = 20.0;

/** The highest refresh rate, in hertz, a reading is believed at; the companion of kLowestBelievableRefreshHertz. */
inline constexpr double kHighestBelievableRefreshHertz = 1000.0;

/** How far apart, as a fraction, two readings of one display's refresh rate may be and still be the same display's:
    the compositor's own against the mode the window's display is in. A tenth of a hertz apart at 240 is the same
    panel read twice; a quarter of the rate apart is two different panels, or a reading of something else. */
inline constexpr double kRefreshRatesAgreeWithin = 0.05;

/** How far apart, as a fraction, the refresh periods of one run of readings may be and still be called a period: a
    panel whose rate is fixed reads a few hundredths of a percent apart, and one that is varying reads whole
    multiples apart. Beyond this there is no denominator to divide, and a window says so instead of inventing one. */
inline constexpr double kRefreshPeriodsHoldStillWithin = 0.05;

/** How many of a display's refresh periods are judged together for whether the rate holds still
    (RefreshSteadiness). */
inline constexpr std::size_t kRefreshPeriodsJudged = 8;

/** Whether `hertz` is a rate a display can have, so that a frame period may be derived from it: a reading outside
    this is refused rather than divided by, because the rate a platform reports is not always the rate a panel runs at
    - Windows' WMI reported 59 Hz on a machine whose compositor and display mode both said 240.
    @param hertz  the rate read, in hertz
    @return       true where the reading is believable */
constexpr bool IsBelievableRefreshRate(double hertz) noexcept
{
    return hertz >= kLowestBelievableRefreshHertz && hertz <= kHighestBelievableRefreshHertz;
}

/** What a platform can say about the display clock a window would be paced on, with none of that platform's own
    types: what ChosenPacing decides from. */
struct DisplayClockReading
{
    /** Whether the platform has a clock to pace on at all: a compositor whose timing it can read, a display link it
        can attach. False where it has none, and the window stays on the platform's timer. */
    bool has_clock{false};

    /** The refresh rate the compositor itself reports, in hertz; 0 where it reports none. Preferred over
        `display_hertz`, being the rate the frames are actually composited at. */
    double compositor_hertz{0.0};

    /** The refresh rate the mode of the display this window is on reports, in hertz; 0 where it reports none. The
        cross-check on `compositor_hertz`, and the reading used where the compositor gives none. */
    double display_hertz{0.0};

    /** Whether the refresh period held still over the readings judged (RefreshSteadiness). True where the platform
        has not watched it yet, which is the honest answer before a frame has been waited for. */
    bool does_rate_hold_still{true};
};

/** The refresh rate to divide, from the two readings a platform can take, and the reason where there is none: the
    compositor's own where it is believable, the window's display's where the compositor gave none, and none at all
    where neither can be believed or the two disagree. The compositor is preferred because it is the clock the frames
    are composited on, and the display's mode is the cross-check - a window on a 60 Hz second monitor of a desktop the
    compositor reports at 240 is the case the disagreement catches, and falling back there is right: dividing the
    wrong panel's rate would be worse than not pacing at all.
    @param reading  what the platform said
    @return         the rate in hertz and FramePacingFallback::kNone, or 0 and the reason */
struct RefreshRateChoice
{
    double              hertz{0.0};                                    ///< the rate to divide; 0 where there is none
    FramePacingFallback why{FramePacingFallback::kRefreshRateUnknown}; ///< kNone where `hertz` is a rate
};

/** RefreshRateChoice from a platform's reading; see RefreshRateChoice.
    @param reading  what the platform said
    @return         the rate to divide, or the reason there is none */
inline RefreshRateChoice ChosenRefreshRate(const DisplayClockReading &reading) noexcept
{
    const bool is_compositor_believable = IsBelievableRefreshRate(reading.compositor_hertz);
    const bool is_display_believable    = IsBelievableRefreshRate(reading.display_hertz);
    if (!is_compositor_believable && !is_display_believable)
    {
        // Nothing said anything, or nothing said anything a display could be: tell those two apart, since a reading
        // of 59 Hz where the panel runs at 240 is a different fault from a platform that answers nothing at all.
        const bool said_nothing = reading.compositor_hertz <= 0.0 && reading.display_hertz <= 0.0;
        return {0.0, said_nothing ? FramePacingFallback::kRefreshRateUnknown
                                  : FramePacingFallback::kRefreshRateNotBelievable};
    }
    if (is_compositor_believable && is_display_believable)
    {
        const double apart = std::fabs(reading.compositor_hertz - reading.display_hertz) / reading.display_hertz;
        if (apart > kRefreshRatesAgreeWithin)
        {
            return {0.0, FramePacingFallback::kRefreshRateDisagrees};
        }
        return {reading.compositor_hertz, FramePacingFallback::kNone};
    }
    return {is_compositor_believable ? reading.compositor_hertz : reading.display_hertz, FramePacingFallback::kNone};
}

/** The pacing a window ends up on: the whole of the fallback decision, in the order it is made, from the product's
    request and what the platform could say. The platform does the reading and the waiting; this says whether there is
    anything to wait on.
    @param request  what the product asked for
    @param reading  what the platform said about its display clock
    @return         the state the window reports, whose `why` says which step refused */
inline FramePacingState ChosenPacing(const FrameRequest &request, const DisplayClockReading &reading) noexcept
{
    if (request.pacing == FramePacing::kPlatformTimer)
    {
        return {FramePacing::kPlatformTimer, 0, 0.0, 0.0, FramePacingFallback::kNotAsked};
    }
    if (request.refresh_divisor < 1 || request.most_frames_behind < 0)
    {
        return {FramePacing::kPlatformTimer, 0, 0.0, 0.0, FramePacingFallback::kRequestOutOfRange};
    }
    if (!reading.has_clock)
    {
        return {FramePacing::kPlatformTimer, 0, 0.0, 0.0, FramePacingFallback::kNoDisplayClock};
    }
    const RefreshRateChoice rate = ChosenRefreshRate(reading);
    if (rate.why != FramePacingFallback::kNone)
    {
        return {FramePacing::kPlatformTimer, 0, 0.0, 0.0, rate.why};
    }
    if (!reading.does_rate_hold_still)
    {
        return {FramePacing::kPlatformTimer, 0, rate.hertz, 0.0, FramePacingFallback::kRefreshRateUnsteady};
    }
    return {FramePacing::kDisplayClock, request.refresh_divisor, rate.hertz, rate.hertz / request.refresh_divisor,
            FramePacingFallback::kNone};
}

/** `instant_seconds` moved to the nearest instant of the grid that `anchor_seconds` and `period_seconds` describe -
    the display's own vblanks, carried forwards and backwards from the last one the platform reported. The move is
    never more than half a period, so a due time derived from the same period as the grid is pulled onto it rather
    than past it, and a grid whose period wanders a little is followed rather than drifted away from. Nothing is
    moved for a period that is not positive.
    @param instant_seconds  the instant to move, on the same clock as `anchor_seconds`
    @param anchor_seconds   one instant of the grid: the last vblank the platform reported
    @param period_seconds   the grid's period: the display's refresh period
    @return                 the nearest grid instant, or `instant_seconds` where `period_seconds` is not positive */
inline double OnTheRefreshGrid(double instant_seconds, double anchor_seconds, double period_seconds) noexcept
{
    if (!(period_seconds > 0.0))
    {
        return instant_seconds;
    }
    const double periods = std::round((instant_seconds - anchor_seconds) / period_seconds);
    return anchor_seconds + periods * period_seconds;
}

/** Whether the display tick at `tick_seconds` is the one a frame due at `due_seconds` belongs on: the tick nearest
    the due time, so a frame whose due time falls between two refreshes is shown on the nearer of them rather than
    always on the later. A tick within half a refresh period before the due time, or at it or past it, is the one.
    What a platform whose display clock calls back once a refresh asks, rather than waiting: a display link on macOS,
    where the subdivision is a count of refreshes and nothing has to be timed.
    @param tick_seconds    when the display ticked
    @param due_seconds     when the frame is due, on the same clock
    @param period_seconds  the display's refresh period
    @return                true where this tick is the one the frame belongs on */
constexpr bool IsFrameDueOnTick(double tick_seconds, double due_seconds, double period_seconds) noexcept
{
    return tick_seconds >= due_seconds - 0.5 * period_seconds;
}

/**
    The refresh periods a platform has lately read, and whether they are the same period read over again. A panel
    whose rate is fixed reads a few hundredths of a percent apart; a variable-refresh panel that is actually varying
    reads whole multiples apart, and then there is no denominator for a subdivision to divide - so a window that finds
    this says so and goes back to the platform's timer rather than pacing to a number that is not real.

    The last kRefreshPeriodsJudged readings are kept, and until there are that many the answer is that the period does
    hold still: a window has to start somewhere, and one reading cannot disagree with itself.
*/
class RefreshSteadiness
{
  public:
    /** `period_seconds` noted as the display's latest refresh period, the oldest of the readings kept dropped where
        there are already kRefreshPeriodsJudged. A reading that is not positive is passed over, since it says nothing.
        @param period_seconds  the refresh period the platform just read, in seconds */
    void Note(double period_seconds) noexcept
    {
        if (!(period_seconds > 0.0))
        {
            return;
        }
        periods_[next_] = period_seconds;
        next_           = (next_ + 1) % kRefreshPeriodsJudged;
        if (count_ < kRefreshPeriodsJudged)
        {
            ++count_;
        }
    }

    /** Whether the readings are one period read over again: the widest and the narrowest of them within
        kRefreshPeriodsHoldStillWithin of the narrowest. True while fewer than kRefreshPeriodsJudged have been noted.
        @return  true where there is a period to divide */
    bool does_hold_still() const noexcept
    {
        if (count_ < kRefreshPeriodsJudged)
        {
            return true;
        }
        double narrowest = periods_[0];
        double widest    = periods_[0];
        for (std::size_t i = 1; i < count_; ++i)
        {
            narrowest = periods_[i] < narrowest ? periods_[i] : narrowest;
            widest    = periods_[i] > widest ? periods_[i] : widest;
        }
        return widest - narrowest <= kRefreshPeriodsHoldStillWithin * narrowest;
    }

    /** How many readings are in hand, at most kRefreshPeriodsJudged.
        @return  the count */
    std::size_t count() const noexcept
    {
        return count_;
    }

    /** Every reading forgotten: what a window does when its display changes, so the panel it is leaving cannot be
        judged together with the one it is arriving at. */
    void Forget() noexcept
    {
        count_ = 0;
        next_  = 0;
    }

  private:
    double      periods_[kRefreshPeriodsJudged]{}; ///< the readings kept, oldest overwritten first
    std::size_t next_{0};                          ///< where the next reading goes
    std::size_t count_{0};                         ///< how many of `periods_` hold a reading
};

/**
    When a window's next frame is due, on whatever clock the platform measures in seconds. The schedule is kept
    absolutely - each frame is due one frame period after the last was due, never one period after it was delivered -
    so a frame that runs late does not push every frame after it late too, which is what makes the motion uniform.

    A frame that runs very late is a different matter: making up a second of them would run the product's world
    forward in a burst. So the clock makes up at most the product's `most_frames_behind` whole frames and then gives
    up on the rest and starts again from where it is, and says how many it gave up. That is what WM_TIMER does by
    accident on Windows - it coalesces, so at most one tick waits and a stall loses ticks rather than bursting - and
    doing it on purpose is better than relying on a platform's queue.

    The clock knows nothing of displays or vblanks: a platform that has a grid pulls each due time onto it
    (OnTheRefreshGrid) and tells the clock where it went (MoveNextFrameTo), so the schedule and the grid stay one
    schedule.
*/
class FrameClock
{
  public:
    /** Started at `now_seconds`, with the first frame due one frame period later.
        @param now_seconds         the clock now, in seconds
        @param frame_seconds       the frame period: the refresh period times the divisor
        @param most_frames_behind  how many whole frames of lateness are made up before the schedule starts again */
    void Start(double now_seconds, double frame_seconds, int most_frames_behind) noexcept
    {
        frame_seconds_      = frame_seconds > 0.0 ? frame_seconds : 0.0;
        most_frames_behind_ = most_frames_behind > 0 ? most_frames_behind : 0;
        next_frame_seconds_ = now_seconds + frame_seconds_;
        frames_given_up_    = 0;
    }

    /** When the next frame is due.
        @return  the due time, in seconds on the clock Start was given */
    double next_frame_seconds() const noexcept
    {
        return next_frame_seconds_;
    }

    /** The frame period in force.
        @return  the period, in seconds */
    double frame_seconds() const noexcept
    {
        return frame_seconds_;
    }

    /** The next frame moved to `seconds`: what a platform does once it has pulled the due time onto its display's own
        grid (OnTheRefreshGrid), so that the schedule the clock keeps is the grid's schedule.
        @param seconds  where the next frame is due instead */
    void MoveNextFrameTo(double seconds) noexcept
    {
        next_frame_seconds_ = seconds;
    }

    /** The frame due at `next_frame_seconds` dealt with at `now_seconds` - delivered, or given up on - and the clock
        advanced: one frame period on from where it was, unless that is further behind `now_seconds` than the product
        allows to be made up, in which case the schedule starts again from `now_seconds` and the frames between are
        given up.
        @param now_seconds  the clock now, in seconds
        @return             how many whole frames were given up: 0 for a frame that was on time, or late by no more
                            than the product allows to be made up */
    int Advance(double now_seconds) noexcept
    {
        if (!(frame_seconds_ > 0.0))
        {
            next_frame_seconds_ = now_seconds;
            return 0;
        }
        double       next     = next_frame_seconds_ + frame_seconds_;
        const double late     = now_seconds - next;
        int          given_up = 0;
        if (late > most_frames_behind_ * frame_seconds_)
        {
            given_up = static_cast<int>(late / frame_seconds_);
            next     = now_seconds + frame_seconds_;
            frames_given_up_ += given_up;
        }
        next_frame_seconds_ = next;
        return given_up;
    }

    /** How many whole frames the clock has given up on since it started.
        @return  the count */
    long long frames_given_up() const noexcept
    {
        return frames_given_up_;
    }

  private:
    double    frame_seconds_{0.0};      ///< the frame period, in seconds; 0 before Start
    int       most_frames_behind_{0};   ///< whole frames of lateness made up before the schedule starts again
    double    next_frame_seconds_{0.0}; ///< when the next frame is due
    long long frames_given_up_{0};      ///< frames given up on since Start
};
} // namespace reckless::window
