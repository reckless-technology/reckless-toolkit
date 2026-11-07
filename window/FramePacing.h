#pragma once

// How a child window paces its frame callback (ChildWindow::SetFramePacing): the platform's own window timer, which
// is what every window here has run on since the toolkit began, or the display's own clock - the frame delivered on
// every Nth refresh of the display in front of the person, so that each frame drawn is shown for exactly N refresh
// periods. A product asks for N, never for a frequency: a frequency has no relationship to the panel, and a frame
// period that is not a whole number of refresh periods is held for one count of refreshes and then another - one and
// two on a 60 Hz panel, six and seven on a 240 Hz one - and that alternation is what the eye reads as stutter
// (docs/window/frame-pacing.md has the measurements).
//
// Everything a window decides about pacing is here, and none of it needs a display, a compositor or a thread: the
// rate to divide and whether it can be believed, whether the display holds still enough to have a period at all, the
// subdivision arithmetic, the grid a due time is pulled onto, and how far behind a clock may fall before it gives up
// and starts again. So the suites hold all of it on every platform, as window/RawKey.h holds the raw keys' policy and
// audio/win/WasapiFormats.h holds Windows Audio's arithmetic. What is left to each platform is reading its own
// display's clock and waiting on it.

#include <cmath>
#include <cstddef>
#include <cstdint>

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
    kDisplayLinkSilent,        ///< the display link has stopped ticking: the window is on no screen, for now
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
    case FramePacingFallback::kDisplayLinkSilent:
        return "the display link has stopped ticking, so the window is on no screen";
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

    /** How long before the vertical blank a frame is shown at the frame callback is to run, in microseconds: the
        product's estimate of what a frame costs it, with a margin, so the frame is finished just as the compositor
        takes it rather than a whole refresh early - which is a refresh of input latency (docs/window/frame-pacing.md,
        "Leading the vertical blank"). 0, the default, runs the callback at the blank, as a window always has. Held
        between 0 and the frame period less a millisecond (LeadInForceMicroseconds), and read only where `pacing` asks
        for the display's clock and the platform can lead it - Windows; FramePacingState::lead_microseconds says what
        is in force. A frame that misses the blank it led is shown a whole refresh later, so the margin is the
        product's to choose with care. */
    std::int32_t lead_microseconds{0};

    /** Whether the frame callback goes on running while a window is on no screen, where its display's clock stops for
        that - a Mac's display link, which macOS stops for a window that is hidden. True, a window whose clock has
       fallen silent runs on the platform's timer until the clock ticks again, so a product goes on being told, and can
       tell its panel the window no longer shows or stand its pacing down. False, the default, a window is left with no
        frames until it shows again. Read only where `pacing` asks for the display's clock, and it changes nothing on
        Windows, whose clock does not stop. */
    bool keeps_frames_off_screen{false};
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

    /** The display's refresh rate, in hertz, as the window read it: the rate being divided, which is the one to report.
        It can move by a fraction of a hertz from one reading to the next, so a divisor is worked out from
        ChildWindow::DisplayModeHertz instead. 0 where nothing said, and on the platform timer, which reads none. */
    double display_hertz{0.0};

    /** The frames a second the callback is being run at: `display_hertz` over `refresh_divisor`. **0 on the platform
        timer**, whose rate is not a number the window knows - it is the platform's, it is not the period asked for,
        and on Windows it is not even steady (docs/window/frame-pacing.md). */
    double frames_a_second{0.0};

    /** Why the window is on the platform's timer; kNone where it is on the display's clock. */
    FramePacingFallback why{FramePacingFallback::kNotAsked};

    /** The lead in force, in microseconds: how long before each vertical blank the frame callback runs, as
        LeadInForceMicroseconds held the one asked for. 0 on the platform timer, which knows no blank, and on a
        platform whose display clock cannot lead - macOS and Linux (docs/window/frame-pacing.md says why). */
    std::int32_t lead_microseconds{0};
};

/** The least of a frame period a lead leaves between the callback's start and the vertical blank it leads, in
    microseconds: a lead may take the frame period less this and no more, so a lead never asks for a frame to be
    started at or before the blank of the frame before it. A bound on what a lead can mean, not a tuning number. */
inline constexpr std::int32_t kLeadLeavesAtLeastMicroseconds = 1000;

/** The lead a window keeps of the one a product asked for: none for a lead under 0, and at most the frame period less
    kLeadLeavesAtLeastMicroseconds, in whole microseconds - so a lead longer than the frame period, which would ask for
    a frame to start before the one ahead of it is shown, is the longest lead the period allows rather than refused.
    @param asked_microseconds  the lead the product asked for (FrameRequest::lead_microseconds)
    @param frame_seconds       the frame period: the refresh period times the divisor
    @return                    the lead in force, in microseconds, from 0 */
constexpr std::int32_t LeadInForceMicroseconds(std::int32_t asked_microseconds, double frame_seconds) noexcept
{
    if (asked_microseconds <= 0 || !(frame_seconds > 0.0))
    {
        return 0;
    }
    // Whole microseconds, rounded down. A believed rate's period is a few tens of milliseconds at most; one of more
    // than half an hour is no display's, and is held there so the cast cannot overflow.
    const double       frame_microseconds = frame_seconds < 2000.0 ? frame_seconds * 1.0e6 : 2.0e9;
    const std::int32_t longest = static_cast<std::int32_t>(frame_microseconds) - kLeadLeavesAtLeastMicroseconds;
    if (longest <= 0)
    {
        return 0;
    }
    return asked_microseconds < longest ? asked_microseconds : longest;
}

/** When a frame due at the vertical blank `due_seconds` is woken for: `lead_seconds` before it. A platform that waits
    keeps its FrameClock on the blanks themselves and tells it the time as `now + lead` (FrameClock::Start and
    FrameClock::Advance), so the clock's schedule, its catching up and its giving up are those of a clock with no lead
    at all and only the waking moves - and because the clock starts a whole frame period ahead of `now + lead`, the
    first frame woken for is never already past: at least half a refresh period ahead, whatever the lead.
    @param due_seconds   the vertical blank the frame is due at, on the grid (OnTheRefreshGrid)
    @param lead_seconds  the lead in force, in seconds
    @return              the instant to wake at, on the same clock */
constexpr double LedWakeSeconds(double due_seconds, double lead_seconds) noexcept
{
    return due_seconds - lead_seconds;
}

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

/** The refresh rate a window would divide, or the reason there is none to divide (ChosenRefreshRate). */
struct RefreshRateChoice
{
    double              hertz{0.0};                                    ///< the rate to divide; 0 where there is none
    FramePacingFallback why{FramePacingFallback::kRefreshRateUnknown}; ///< kNone where `hertz` is a rate
};

/** How close a compositor's reading must be to the mode of the window's display to be that mode read more finely, in
    hertz: Windows gives a mode in whole hertz, so a 59.94 Hz panel's mode may say 59 or 60. A bound on what a mode's
    number can mean, not a rate. */
inline constexpr double kModeIsNamedWithinHertz = 1.0;

/** The rate of the panel, for a product that works its divisor out from it (ChildWindow::DisplayModeHertz): the mode of
    the window's display where that is believable, since it holds still where a compositor's reading wanders with what
   is being composed, and only where the mode names no rate the compositor's.
    @param reading  what the platform said
    @return         the rate, in hertz; 0 where neither can be believed */
constexpr double ModeHertzOf(const DisplayClockReading &reading) noexcept
{
    if (IsBelievableRefreshRate(reading.display_hertz))
    {
        return reading.display_hertz;
    }
    return IsBelievableRefreshRate(reading.compositor_hertz) ? reading.compositor_hertz : 0.0;
}

/** The rate a window answers with where it has one reading of the compositor to go on, as it is asked for the clock:
   the reading where that is the mode of the window's display read more finely, and otherwise the mode. One reading can
   be anywhere a compositor's go (CompositorReadings), and the clock this starts stays on the mode until its readings
    settle, so the answer does not say a rate that clock is not dividing.
    @param compositor_hertz  the rate the compositor reports now, in hertz
    @param display_hertz     the rate of the mode of the window's display, in hertz
    @return                  the rate to answer with, in hertz: `compositor_hertz` where the mode names no rate */
inline double RateOfOneReading(double compositor_hertz, double display_hertz) noexcept
{
    if (!IsBelievableRefreshRate(display_hertz) ||
        std::fabs(compositor_hertz - display_hertz) <= kModeIsNamedWithinHertz)
    {
        return compositor_hertz;
    }
    return display_hertz;
}

/** The refresh rate to divide, from the two readings a platform can take, and the reason where there is none: the
    compositor's own where it is believable, the window's display's where the compositor gave none, and none at all
    where neither can be believed or the two disagree. The compositor is preferred because it is the clock the frames
    are composited on, and the display's mode is the cross-check - a window on a 60 Hz second monitor of a desktop the
    compositor reports at 240 is the case the disagreement catches, and falling back there is right: dividing the
    wrong panel's rate would be worse than not pacing at all.
    @param reading  what the platform said
    @return         the rate in hertz and FramePacingFallback::kNone, or 0 and the reason there is none */
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
    anything to wait on, and the lead it would keep (LeadInForceMicroseconds) - which a platform whose clock cannot
    lead sets back to 0.
    @param request  what the product asked for
    @param reading  what the platform said about its display clock
    @return         the state the window reports, whose `why` says which step refused */
inline FramePacingState ChosenPacing(const FrameRequest &request, const DisplayClockReading &reading) noexcept
{
    if (request.pacing == FramePacing::kPlatformTimer)
    {
        return {FramePacing::kPlatformTimer, 0, 0.0, 0.0, FramePacingFallback::kNotAsked, 0};
    }
    if (request.refresh_divisor < 1 || request.most_frames_behind < 0)
    {
        return {FramePacing::kPlatformTimer, 0, 0.0, 0.0, FramePacingFallback::kRequestOutOfRange, 0};
    }
    if (!reading.has_clock)
    {
        return {FramePacing::kPlatformTimer, 0, 0.0, 0.0, FramePacingFallback::kNoDisplayClock, 0};
    }
    const RefreshRateChoice rate = ChosenRefreshRate(reading);
    if (rate.why != FramePacingFallback::kNone)
    {
        return {FramePacing::kPlatformTimer, 0, 0.0, 0.0, rate.why, 0};
    }
    if (!reading.does_rate_hold_still)
    {
        return {FramePacing::kPlatformTimer, 0, rate.hertz, 0.0, FramePacingFallback::kRefreshRateUnsteady, 0};
    }
    return {FramePacing::kDisplayClock,
            request.refresh_divisor,
            rate.hertz,
            rate.hertz / request.refresh_divisor,
            FramePacingFallback::kNone,
            LeadInForceMicroseconds(request.lead_microseconds, request.refresh_divisor / rate.hertz)};
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

/** How long a display link may say nothing before its window takes it to have stopped, in seconds. A display's slowest
    believable refresh ticks five times over in this, so a link that is ticking at all is never mistaken; what it comes
    to is how long a window just hidden goes with no frame at all. */
inline constexpr double kDisplayLinkSilentSeconds = 0.25;

/** Whether a display link has stopped ticking. macOS stops a link made from a view whose window is hidden or on no
    screen, and a window paced on nothing but that link would then get no frame at all: nothing to run its owner's
    frame callback, so nothing to tell the panel it no longer shows, or to stand the pacing down
    (docs/window/frame-pacing.md, "A window that does not show").
    @param last_tick_seconds  when the link last ticked, or when it started where it has not yet
    @param now_seconds        the clock now, on the same clock
    @return                   true where the link has said nothing for kDisplayLinkSilentSeconds */
constexpr bool IsDisplayLinkSilent(double last_tick_seconds, double now_seconds) noexcept
{
    return now_seconds - last_tick_seconds >= kDisplayLinkSilentSeconds;
}

/** The longest a frame stands back for other work on its window's thread (DoesFrameStandBack), in seconds. A bound on
    what a thread can be doing, not a rate: long enough that a thread saturated with panels goes round them all well
    inside it, so a healthy thread never reaches it, and short enough that a queue whose paint or timer never comes - a
    host pumping only some messages, a window whose paint never validates it - costs a frame this long rather than the
    window's frames for good. */
inline constexpr double kFrameStandsBackAtMostSeconds = 0.25;

/** Whether a frame the display's clock has handed to a window waits for the work ahead of it on the window's thread,
    rather than running now. A posted frame message outranks every WM_PAINT and every WM_TIMER on its thread, so a frame
    that simply ran would invalidate its window again ahead of paints still owed, and on a thread with more panels than
    a refresh has room for the windows nearest the top of the z-order would take every paint there is. So a frame goes
    behind them, as the platform's own timer always has by being the last message a thread takes
    (docs/window/frame-pacing.md, "Several windows on one thread").
    @param is_work_ahead       whether a paint or an expired timer is waiting on the thread, of any window, as the
                               window read it before any frame of this refresh had run
    @param seconds_stood_back  how long this frame has already stood back
    @return                    true where the frame is to wait and ask again */
constexpr bool DoesFrameStandBack(bool is_work_ahead, double seconds_stood_back) noexcept
{
    return is_work_ahead && seconds_stood_back < kFrameStandsBackAtMostSeconds;
}

/** How long a frame may be in flight unanswered before the clock asks for it again, in seconds. A frame is a posted
    message, and a queue can lose one that WM_TIMER, which is generated as it is taken, could not: a host's pump that
    takes a message and throws it away, a hook that rewrites it. With a frame in flight no other is posted, so without
    this one lost message would leave the window with no frames at all, and nothing to run a product's own check on
    them. A bound on what a thread can be doing, not a rate: long enough that a thread that is merely busy is asked only
    a few times, and it answers just the last. */
inline constexpr double kFrameInFlightIsLostAfterSeconds = 1.0;

/** Whether a frame in flight has gone unanswered for as long as one that was lost.
    @param seconds_since_asked  how long since the frame was last posted, in seconds
    @return                     true where the clock is to ask for it again */
constexpr bool IsFrameInFlightLost(double seconds_since_asked) noexcept
{
    return seconds_since_asked >= kFrameInFlightIsLostAfterSeconds;
}

/** The shortest a frame that stood back waits before asking again, in seconds: short, because a thread that has just
    finished its last paint stays idle until some frame asks. Measured on a Tape panel's paint at this and at a tenth of
    a millisecond's multiples (docs/window/frame-pacing.md, "Several windows on one thread"). */
inline constexpr double kFrameAsksAgainAfterAtLeastSeconds = 0.0001;

/** The longest a frame that stood back waits before asking again, in seconds. */
inline constexpr double kFrameAsksAgainAfterAtMostSeconds = 0.002;

/** How long a frame that has just stood back waits before asking again. Its asking again is a posted message, which
    outranks the very paint it stood back for, so a thread with many windows all asking could go on answering them and
    never reach the paint: with no wait at all, six panels went from 47 pictures a second each to 9. A thread that is
    getting on with its work answers a frame only between paints; one that answers within a moment of being asked is not
    getting past its questions, and is left alone for twice as long. Each window finds this out for itself, so it holds
    however many windows are on the thread, and in however many modules.
    @param waited_seconds       how long the frame waited before it last asked; 0 where it has not stood back before
    @param answered_in_seconds  how long the window took to answer it, from the asking to its standing back
    @return                     how long to wait before asking again, in seconds */
constexpr double FrameAsksAgainAfter(double waited_seconds, double answered_in_seconds) noexcept
{
    const double doubled = 2.0 * waited_seconds;
    if (waited_seconds <= 0.0 || answered_in_seconds >= doubled)
    {
        return kFrameAsksAgainAfterAtLeastSeconds;
    }
    return doubled < kFrameAsksAgainAfterAtMostSeconds ? doubled : kFrameAsksAgainAfterAtMostSeconds;
}

/** How many times its narrowest the widest of the readings judged together may be, before a compositor's readings are
    taken for a panel whose rate is really varying rather than for a reading that wanders with the panel where it is. A
    bound on what a reading of an unchanged panel does: its readings stayed within 1.42 times their narrowest on a panel
    whose scan-out was measured holding still, where a panel that really varies reads whole multiples apart. */
inline constexpr double kCompositorReadingsVaryAbove = 1.5;

/** How many readings in a row must disagree with the mode before they can be another monitor's. A bound on what a
   reading of an unchanged panel does: the longest such run measured was 8, and a run of 8 that also held still among
   itself was seen, which left the clock for two seconds on a panel that had not changed. */
inline constexpr std::size_t kCompositorDisagreesForGoodAfterReadings = 24;

/** How far apart the readings judged together may be and still be one rate read over again, as a share of the
   narrowest: the same panel's reading went from 240.0038 to 240.2576 Hz, a tenth of a percent, while it held still. */
inline constexpr double kCompositorReadsSteadilyWithin = 0.01;

/**
    What a compositor's readings of its own rate mean, where the mode of the window's display gives a rate to compare
   them with. They are not a measure of the panel on their own: on a 240 Hz panel whose kernel vertical blank and scan
   line held at 240 a second, with one window drawing thirty pictures a second, the compositor read 181 to 257 Hz, and
   at sixty a second 212 to 242 (docs/window/frame-pacing.md). So one reading that disagrees with the mode is no reason
   to leave the display's clock, and no period to put a frame on: it is set aside for the mode. Two things are still
   believed: readings that have gone on disagreeing for kCompositorDisagreesForGoodAfterReadings and hold still among
   themselves, which is a window on a monitor the compositor is not timed by; and readings further apart than
   kCompositorReadingsVaryAbove, which is a panel whose rate really does vary.
*/
class CompositorReadings
{
  public:
    /** A reading noted.
        @param compositor_hertz  the rate the compositor reports now, in hertz
        @param display_hertz     the rate of the mode of the window's display, in hertz
        @return                  true where the reading disagrees with a mode that can be believed and is to be set
       aside for it */
    bool Note(double compositor_hertz, double display_hertz) noexcept
    {
        const bool can_compare = IsBelievableRefreshRate(display_hertz) && IsBelievableRefreshRate(compositor_hertz);
        const bool does_disagree =
            can_compare && std::fabs(compositor_hertz - display_hertz) / display_hertz > kRefreshRatesAgreeWithin;
        disagreeing_ = does_disagree ? disagreeing_ + 1 : 0;
        if (can_compare)
        {
            readings_[next_] = compositor_hertz;
            next_            = (next_ + 1) % kRefreshPeriodsJudged;
            count_           = count_ < kRefreshPeriodsJudged ? count_ + 1 : count_;
        }
        return does_disagree && !does_disagree_for_good();
    }

    /** Whether the readings have disagreed with the mode for as long as a wandering reading cannot, and held still
       among themselves at the end of it: another monitor's rate, and to be believed.
        @return  true where they have */
    bool does_disagree_for_good() const noexcept
    {
        return disagreeing_ >= kCompositorDisagreesForGoodAfterReadings &&
               widest() - narrowest() <= kRefreshPeriodsHoldStillWithin * narrowest();
    }

    /** Whether the readings judged together are further apart than a reading of an unchanged panel goes: a panel whose
       rate really does vary.
        @return  true where they are; false until there are enough to judge */
    bool does_vary() const noexcept
    {
        return count_ == kRefreshPeriodsJudged && widest() > kCompositorReadingsVaryAbove * narrowest();
    }

    /** Whether the readings judged together are one rate read over again, and the mode's to within the whole hertz
       Windows gives a mode in, so worth more than the mode: a 59.94 Hz panel's reading is the rate to divide, and a
       wandering one, even one that has stuck for a moment, is not.
        @param display_hertz  the rate of the mode of the window's display, in hertz
        @return               true where they are; false until there are enough to judge */
    bool does_read_steadily(double display_hertz) const noexcept
    {
        return count_ == kRefreshPeriodsJudged &&
               widest() - narrowest() <= kCompositorReadsSteadilyWithin * narrowest() &&
               widest() <= display_hertz + kModeIsNamedWithinHertz &&
               narrowest() >= display_hertz - kModeIsNamedWithinHertz;
    }

    /** Every reading forgotten: what a window does when its display changes. */
    void Forget() noexcept
    {
        disagreeing_ = 0;
        next_        = 0;
        count_       = 0;
    }

  private:
    /** The widest of the readings kept.
        @return  the rate, in hertz */
    double widest() const noexcept
    {
        double most = 0.0;
        for (std::size_t i = 0; i < count_; ++i)
        {
            most = readings_[i] > most ? readings_[i] : most;
        }
        return most;
    }

    /** The narrowest of the readings kept.
        @return  the rate, in hertz */
    double narrowest() const noexcept
    {
        double least = count_ > 0 ? readings_[0] : 0.0;
        for (std::size_t i = 1; i < count_; ++i)
        {
            least = readings_[i] < least ? readings_[i] : least;
        }
        return least;
    }

    double      readings_[kRefreshPeriodsJudged]{}; ///< the readings kept, oldest overwritten first
    std::size_t next_{0};                           ///< where the next reading goes
    std::size_t count_{0};                          ///< how many of `readings_` hold a reading
    std::size_t disagreeing_{0};                    ///< how many readings in a row have disagreed with the mode
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
