// How a window paces its frame callback (window/FramePacing.h), on every platform: the refresh rate chosen from what
// a platform could read and the whole of the fallback decision, the subdivision arithmetic, the grid a due time is
// pulled onto, whether a refresh period holds still enough to be divided, and the schedule with its catching up.
// None of it needs a display, a compositor or a thread, so every assertion here is held on Linux, macOS and Windows
// alike, in the builds with no GUI too - and the two shapes a platform drives it in are simulated here against a
// made-up display, which is where the property that matters is actually held: that every frame is shown for exactly
// the divisor's refresh periods. What the windows themselves do with it is held by the child window's own suite
// (skia/tests/test_child_window.cpp), which needs a real display clock and only has one on some platforms.
#include <gtest/gtest.h>

#include <cmath>
#include <cstddef>
#include <iterator>
#include <string>
#include <vector>

#include "FramePacing.h"

namespace
{
using reckless::window::ChosenPacing;
using reckless::window::ChosenRefreshRate;
using reckless::window::DisplayClockReading;
using reckless::window::FrameClock;
using reckless::window::FramePacing;
using reckless::window::FramePacingFallback;
using reckless::window::FramePacingState;
using reckless::window::FrameRequest;
using reckless::window::IsBelievableRefreshRate;
using reckless::window::IsFrameDueOnTick;
using reckless::window::kRefreshPeriodsJudged;
using reckless::window::OnTheRefreshGrid;
using reckless::window::RefreshSteadiness;
using reckless::window::TextOf;

/** A reading a platform might take of a display that is there and well behaved.
    @param compositor_hertz  what the compositor reports
    @param display_hertz     what the window's own display's mode reports
    @return                  the reading, with a clock and a period that holds still */
DisplayClockReading GoodReading(double compositor_hertz, double display_hertz)
{
    DisplayClockReading reading;
    reading.has_clock            = true;
    reading.compositor_hertz     = compositor_hertz;
    reading.display_hertz        = display_hertz;
    reading.does_rate_hold_still = true;
    return reading;
}

/** A request for the display's clock.
    @param divisor            the refresh periods each frame is shown for
    @param most_frames_behind the whole frames of lateness made up
    @return                   the request */
FrameRequest Asking(int divisor, int most_frames_behind = 1)
{
    return {FramePacing::kDisplayClock, divisor, most_frames_behind};
}
} // namespace

// ------------------------------------------------------------------------------------------------------------------
// What a product asks for, and what it is told.
// ------------------------------------------------------------------------------------------------------------------

TEST(FrameRequestDefaults, ADefaultRequestAsksForWhatAWindowAlwaysDid)
{
    const FrameRequest request;
    EXPECT_EQ(request.pacing, FramePacing::kPlatformTimer) << "the platform's own timer, unchanged";
    EXPECT_EQ(request.refresh_divisor, 1);
    EXPECT_EQ(request.most_frames_behind, 1);
}

TEST(FramePacingStateDefaults, ADefaultStateIsAWindowNoOneHasAsked)
{
    const FramePacingState state;
    EXPECT_EQ(state.pacing, FramePacing::kPlatformTimer);
    EXPECT_EQ(state.refresh_divisor, 0) << "the platform timer divides nothing";
    EXPECT_EQ(state.display_hertz, 0.0) << "and reads no display";
    EXPECT_EQ(state.frames_a_second, 0.0) << "the platform timer's rate is not a number the window knows";
    EXPECT_EQ(state.why, FramePacingFallback::kNotAsked);
}

TEST(FramePacingFallbackText, EveryReasonHasWordsOfItsOwn)
{
    const FramePacingFallback every[] = {FramePacingFallback::kNone,
                                         FramePacingFallback::kNotAsked,
                                         FramePacingFallback::kRequestOutOfRange,
                                         FramePacingFallback::kNoDisplayClock,
                                         FramePacingFallback::kRefreshRateUnknown,
                                         FramePacingFallback::kRefreshRateNotBelievable,
                                         FramePacingFallback::kRefreshRateDisagrees,
                                         FramePacingFallback::kRefreshRateUnsteady};
    std::vector<std::string>  seen;
    for (FramePacingFallback why : every)
    {
        const char *words = TextOf(why);
        ASSERT_NE(words, nullptr);
        EXPECT_FALSE(std::string(words).empty()) << "a reason a product is to report is never blank";
        for (const std::string &already : seen)
        {
            EXPECT_NE(already, words) << "two reasons must not read the same: " << words;
        }
        seen.emplace_back(words);
    }
}

// ------------------------------------------------------------------------------------------------------------------
// The rate to divide: believing a reading, and cross-checking two.
// ------------------------------------------------------------------------------------------------------------------

TEST(BelievableRefreshRate, OnlyARateADisplayCouldHaveIsDividedBy)
{
    EXPECT_TRUE(IsBelievableRefreshRate(59.94)) << "a television rate";
    EXPECT_TRUE(IsBelievableRefreshRate(60.0));
    EXPECT_TRUE(IsBelievableRefreshRate(240.2576)) << "the rate measured on the machine this was written on";
    EXPECT_TRUE(IsBelievableRefreshRate(20.0)) << "the lowest believed, inclusive";
    EXPECT_TRUE(IsBelievableRefreshRate(1000.0)) << "the highest believed, inclusive";
    EXPECT_FALSE(IsBelievableRefreshRate(0.0)) << "nothing said";
    EXPECT_FALSE(IsBelievableRefreshRate(1.0)) << "Windows' answer for the hardware's default, which is no rate";
    EXPECT_FALSE(IsBelievableRefreshRate(19.9));
    EXPECT_FALSE(IsBelievableRefreshRate(1000.1));
    EXPECT_FALSE(IsBelievableRefreshRate(-60.0));
}

TEST(ChosenRate, TheCompositorIsPreferredAndTheDisplayModeCrossChecksIt)
{
    // The machine this was measured on: the compositor and the display mode agree to a tenth of a hertz in 240.
    const auto agreeing = ChosenRefreshRate(GoodReading(240.2576, 240.0));
    EXPECT_EQ(agreeing.why, FramePacingFallback::kNone);
    EXPECT_DOUBLE_EQ(agreeing.hertz, 240.2576) << "the compositor's, being the clock the frames are composited on";

    const auto compositor_only = ChosenRefreshRate(GoodReading(240.0, 0.0));
    EXPECT_EQ(compositor_only.why, FramePacingFallback::kNone);
    EXPECT_DOUBLE_EQ(compositor_only.hertz, 240.0) << "the mode said nothing; the compositor's stands alone";

    const auto mode_only = ChosenRefreshRate(GoodReading(0.0, 60.0));
    EXPECT_EQ(mode_only.why, FramePacingFallback::kNone);
    EXPECT_DOUBLE_EQ(mode_only.hertz, 60.0) << "the compositor said nothing; the mode's is used";
}

TEST(ChosenRate, NothingIsDividedWhereTheReadingsAreNoDisplaysOrAreTwoDifferentDisplays)
{
    EXPECT_EQ(ChosenRefreshRate(GoodReading(0.0, 0.0)).why, FramePacingFallback::kRefreshRateUnknown)
        << "neither said anything at all";
    EXPECT_EQ(ChosenRefreshRate(GoodReading(3.0, 0.0)).why, FramePacingFallback::kRefreshRateNotBelievable)
        << "something was said, and it is not a rate a display has";
    EXPECT_EQ(ChosenRefreshRate(GoodReading(0.0, 4000.0)).why, FramePacingFallback::kRefreshRateNotBelievable);

    // A window on a 60 Hz second monitor of a desktop the compositor reports at 240: the one case the cross-check is
    // for, and the one where dividing either reading would be worse than not pacing.
    const auto disagreeing = ChosenRefreshRate(GoodReading(240.2576, 60.0));
    EXPECT_EQ(disagreeing.why, FramePacingFallback::kRefreshRateDisagrees);
    EXPECT_EQ(disagreeing.hertz, 0.0) << "nothing is handed back to divide";

    // 59 Hz against 240 is what WMI reported on the machine this was measured on. It is believable on its own, and
    // the disagreement is what catches it.
    EXPECT_EQ(ChosenRefreshRate(GoodReading(240.0, 59.0)).why, FramePacingFallback::kRefreshRateDisagrees);
}

// ------------------------------------------------------------------------------------------------------------------
// The whole fallback decision, every branch.
// ------------------------------------------------------------------------------------------------------------------

TEST(ChosenPacingFallback, AWindowNoOneHasAskedIsOnThePlatformTimerWhateverItsDisplayCouldDo)
{
    const FramePacingState state = ChosenPacing(FrameRequest{}, GoodReading(240.0, 240.0));
    EXPECT_EQ(state.pacing, FramePacing::kPlatformTimer);
    EXPECT_EQ(state.why, FramePacingFallback::kNotAsked);
    EXPECT_EQ(state.display_hertz, 0.0) << "a window that was not asked reads nothing of its display";
    EXPECT_EQ(state.frames_a_second, 0.0);
}

TEST(ChosenPacingFallback, ARequestOutOfRangeIsRefusedBeforeAnythingIsRead)
{
    for (const FrameRequest &request : {Asking(0), Asking(-1), FrameRequest{FramePacing::kDisplayClock, 1, -1}})
    {
        const FramePacingState state = ChosenPacing(request, GoodReading(240.0, 240.0));
        EXPECT_EQ(state.pacing, FramePacing::kPlatformTimer);
        EXPECT_EQ(state.why, FramePacingFallback::kRequestOutOfRange);
    }
}

TEST(ChosenPacingFallback, EachThingAPlatformCannotGiveIsItsOwnReason)
{
    EXPECT_EQ(ChosenPacing(Asking(1), DisplayClockReading{}).why, FramePacingFallback::kNoDisplayClock)
        << "Linux: there is no vertical blank to be had with a copy for a present";

    EXPECT_EQ(ChosenPacing(Asking(1), GoodReading(0.0, 0.0)).why, FramePacingFallback::kRefreshRateUnknown);
    EXPECT_EQ(ChosenPacing(Asking(1), GoodReading(5.0, 0.0)).why, FramePacingFallback::kRefreshRateNotBelievable);
    EXPECT_EQ(ChosenPacing(Asking(1), GoodReading(240.0, 60.0)).why, FramePacingFallback::kRefreshRateDisagrees);

    DisplayClockReading varying     = GoodReading(240.0, 240.0);
    varying.does_rate_hold_still    = false;
    const FramePacingState unsteady = ChosenPacing(Asking(4), varying);
    EXPECT_EQ(unsteady.pacing, FramePacing::kPlatformTimer) << "a panel whose rate varies has no denominator";
    EXPECT_EQ(unsteady.why, FramePacingFallback::kRefreshRateUnsteady);
    EXPECT_DOUBLE_EQ(unsteady.display_hertz, 240.0) << "the rate it did read is still reported";
    EXPECT_EQ(unsteady.frames_a_second, 0.0) << "but no rate is claimed for the frames";
}

// ------------------------------------------------------------------------------------------------------------------
// The subdivision: a divisor, not a frequency.
// ------------------------------------------------------------------------------------------------------------------

TEST(Subdivision, TheRateIsTheDisplaysOverTheDivisorOnWhateverPanelIsThere)
{
    struct Case
    {
        double display_hertz;
        int    divisor;
        double frames_a_second;
    };

    // The same request means a different rate on each panel, and the same uniformity on all of them: that is the
    // whole point of asking for a divisor. A product that wanted "60 Hz" could not express the first two rows at
    // once, and would judder on both.
    const Case every[] = {
        {60.0, 1, 60.0},  {240.0, 4, 60.0},  {120.0, 2, 60.0},       {144.0, 2, 72.0},
        {240.0, 8, 30.0}, {59.94, 1, 59.94}, {240.2576, 4, 60.0644},
    };
    for (const Case &one : every)
    {
        const FramePacingState state =
            ChosenPacing(Asking(one.divisor), GoodReading(one.display_hertz, one.display_hertz));
        EXPECT_EQ(state.pacing, FramePacing::kDisplayClock) << one.display_hertz << " Hz over " << one.divisor;
        EXPECT_EQ(state.why, FramePacingFallback::kNone);
        EXPECT_EQ(state.refresh_divisor, one.divisor);
        EXPECT_DOUBLE_EQ(state.display_hertz, one.display_hertz);
        EXPECT_NEAR(state.frames_a_second, one.frames_a_second, 1.0e-9);
    }
}

TEST(Subdivision, HalfTheRateIsTwiceTheDivisorAndStaysUniform)
{
    const FramePacingState full = ChosenPacing(Asking(4), GoodReading(240.0, 240.0));
    const FramePacingState half = ChosenPacing(Asking(8), GoodReading(240.0, 240.0));
    EXPECT_DOUBLE_EQ(half.frames_a_second, full.frames_a_second / 2.0);
    EXPECT_EQ(half.refresh_divisor, full.refresh_divisor * 2) << "a whole number of refreshes either way";
}

// ------------------------------------------------------------------------------------------------------------------
// The display's grid, and which tick a frame belongs on.
// ------------------------------------------------------------------------------------------------------------------

TEST(RefreshGrid, AnInstantIsPulledOntoTheNearestVerticalBlankAndNoFurther)
{
    const double anchor = 100.0;
    const double period = 1.0 / 240.0;
    EXPECT_DOUBLE_EQ(OnTheRefreshGrid(anchor, anchor, period), anchor) << "the anchor is already on the grid";
    EXPECT_NEAR(OnTheRefreshGrid(anchor + 4.0 * period + 0.0001, anchor, period), anchor + 4.0 * period, 1.0e-12)
        << "just past the fourth blank: back onto it";
    EXPECT_NEAR(OnTheRefreshGrid(anchor + 4.0 * period - 0.0001, anchor, period), anchor + 4.0 * period, 1.0e-12)
        << "just before it: forward onto it, the nearest and not the next";
    EXPECT_NEAR(OnTheRefreshGrid(anchor - 3.0 * period + 0.0001, anchor, period), anchor - 3.0 * period, 1.0e-12)
        << "the grid runs backwards from the anchor too";
    const double moved = OnTheRefreshGrid(anchor + 10.5 * period, anchor, period);
    EXPECT_LE(std::fabs(moved - (anchor + 10.5 * period)), 0.5 * period + 1.0e-12) << "never more than half a period";
}

TEST(RefreshGrid, NothingIsMovedByAPeriodThatIsNotAPeriod)
{
    EXPECT_DOUBLE_EQ(OnTheRefreshGrid(7.5, 1.0, 0.0), 7.5);
    EXPECT_DOUBLE_EQ(OnTheRefreshGrid(7.5, 1.0, -1.0), 7.5);
}

TEST(RefreshGrid, TheTickAFrameBelongsOnIsTheNearestToItsDueTime)
{
    const double period = 1.0 / 240.0;
    const double due    = 50.0;
    EXPECT_TRUE(IsFrameDueOnTick(due, due, period)) << "the tick at the due time";
    EXPECT_TRUE(IsFrameDueOnTick(due + period, due, period)) << "and any after it";
    EXPECT_TRUE(IsFrameDueOnTick(due - 0.49 * period, due, period)) << "the tick just inside half a period before it";
    EXPECT_FALSE(IsFrameDueOnTick(due - 0.51 * period, due, period)) << "the one just outside is the frame before's";
    EXPECT_FALSE(IsFrameDueOnTick(due - period, due, period));
}

// ------------------------------------------------------------------------------------------------------------------
// Whether a display has a period at all.
// ------------------------------------------------------------------------------------------------------------------

TEST(RefreshSteadinessTest, AWindowStartsBelievingAndKeepsBelievingAFixedPanel)
{
    RefreshSteadiness steadiness;
    EXPECT_TRUE(steadiness.does_hold_still()) << "nothing read yet: one reading cannot disagree with itself";
    EXPECT_EQ(steadiness.count(), 0u);
    for (std::size_t i = 0; i < kRefreshPeriodsJudged - 1; ++i)
    {
        steadiness.Note(1.0 / 240.0);
        EXPECT_TRUE(steadiness.does_hold_still()) << "fewer readings than are judged together";
    }
    // The spread the compositor showed across runs on the machine this was measured on: 240.0038, 240.1593, 240.2576
    // hertz, which is a hundredth of a percent and is one panel read three times.
    const double wandering[] = {240.0038, 240.1593, 240.2576, 240.0038, 240.1593, 240.2576, 240.0038, 240.1593};
    for (double hertz : wandering)
    {
        steadiness.Note(1.0 / hertz);
    }
    EXPECT_EQ(steadiness.count(), kRefreshPeriodsJudged);
    EXPECT_TRUE(steadiness.does_hold_still()) << "a hundredth of a percent apart is one period read over again";
}

TEST(RefreshSteadinessTest, APanelWhoseRateActuallyVariesHasNoPeriodToDivide)
{
    RefreshSteadiness steadiness;
    constexpr double  varying[] = {240.0, 48.0, 120.0, 240.0, 72.0, 240.0, 96.0, 60.0};
    static_assert(std::size(varying) == kRefreshPeriodsJudged, "as many readings as are judged together");
    for (double hertz : varying)
    {
        steadiness.Note(1.0 / hertz);
    }
    EXPECT_FALSE(steadiness.does_hold_still()) << "whole multiples apart: a variable-refresh panel in play";

    steadiness.Forget();
    EXPECT_TRUE(steadiness.does_hold_still()) << "forgotten with the display it described";
    EXPECT_EQ(steadiness.count(), 0u);
}

TEST(RefreshSteadinessTest, OneModeChangeWashesThroughTheReadingsJudged)
{
    RefreshSteadiness steadiness;
    for (std::size_t i = 0; i < kRefreshPeriodsJudged; ++i)
    {
        steadiness.Note(1.0 / 240.0);
    }
    ASSERT_TRUE(steadiness.does_hold_still());
    steadiness.Note(1.0 / 60.0); // the mode changed
    EXPECT_FALSE(steadiness.does_hold_still()) << "the jump is inside the readings being judged";
    for (std::size_t i = 1; i < kRefreshPeriodsJudged; ++i)
    {
        steadiness.Note(1.0 / 60.0);
    }
    EXPECT_TRUE(steadiness.does_hold_still()) << "and once it has washed through, the new period is a period";
}

TEST(RefreshSteadinessTest, AReadingThatSaysNothingIsPassedOver)
{
    RefreshSteadiness steadiness;
    for (std::size_t i = 0; i < kRefreshPeriodsJudged * 2; ++i)
    {
        steadiness.Note(0.0);
        steadiness.Note(-1.0);
    }
    EXPECT_EQ(steadiness.count(), 0u) << "neither is a period";
    EXPECT_TRUE(steadiness.does_hold_still());
}

// ------------------------------------------------------------------------------------------------------------------
// The schedule, and how far behind it will fall.
// ------------------------------------------------------------------------------------------------------------------

TEST(FrameClockTest, TheScheduleIsAbsoluteSoOneLateFrameDoesNotMakeTheNextLate)
{
    FrameClock   clock;
    const double period = 1.0 / 60.0;
    clock.Start(10.0, period, 1);
    EXPECT_DOUBLE_EQ(clock.frame_seconds(), period);
    EXPECT_DOUBLE_EQ(clock.next_frame_seconds(), 10.0 + period);

    // A frame delivered three milliseconds late: the next is still due one period after the one before it was, not
    // one period after this one ran.
    EXPECT_EQ(clock.Advance(10.0 + period + 0.003), 0) << "nothing given up: it only ran late";
    EXPECT_DOUBLE_EQ(clock.next_frame_seconds(), 10.0 + 2.0 * period);
    EXPECT_EQ(clock.Advance(10.0 + 2.0 * period), 0);
    EXPECT_DOUBLE_EQ(clock.next_frame_seconds(), 10.0 + 3.0 * period);
}

TEST(FrameClockTest, LatenessIsMadeUpAsFarAsTheProductAllowsAndThenGivenUp)
{
    // How far behind the clock is, is how many whole frame periods past the next due time it already is when a frame
    // is dealt with: that is how many frames would run one after another with no wait between them.
    const double period = 1.0 / 60.0;

    FrameClock patient;
    patient.Start(0.0, period, 2);
    // Dealt with at three and a half periods, where the next two due times (two and three periods) are both past:
    // two frames to make up, which is what this product allows, so the absolute schedule is kept.
    EXPECT_EQ(patient.Advance(3.5 * period), 0) << "nothing given up: two frames are to be made up";
    EXPECT_DOUBLE_EQ(patient.next_frame_seconds(), 2.0 * period);
    EXPECT_EQ(patient.frames_given_up(), 0);

    FrameClock impatient;
    impatient.Start(0.0, period, 0);
    // A product that will make up nothing keeps the absolute schedule while the next frame is not yet due...
    EXPECT_EQ(impatient.Advance(1.5 * period), 0) << "half a period late, and the next frame still ahead";
    EXPECT_DOUBLE_EQ(impatient.next_frame_seconds(), 2.0 * period) << "the absolute schedule, kept";
    // ...and the first frame that leaves the next one already due starts the schedule again rather than running two
    // frames together.
    EXPECT_EQ(impatient.Advance(3.5 * period), 0) << "half a period past the next due time: no whole frame lost";
    EXPECT_DOUBLE_EQ(impatient.next_frame_seconds(), 4.5 * period) << "the schedule starts again from here";
    EXPECT_EQ(impatient.frames_given_up(), 0) << "the phase was given up, not a frame";
}

TEST(FrameClockTest, ALongStallIsGivenUpOnRatherThanRunInABurst)
{
    const double period = 1.0 / 60.0;
    FrameClock   clock;
    clock.Start(0.0, period, 1);
    // A second of nothing - a modal dialog, a window dragged - and then a frame.
    const double woke     = 1.0;
    const int    given_up = clock.Advance(woke);
    EXPECT_GT(given_up, 50) << "about a second of frames at sixty a second";
    EXPECT_EQ(clock.frames_given_up(), given_up);
    EXPECT_DOUBLE_EQ(clock.next_frame_seconds(), woke + period) << "and the schedule starts again from the waking";
}

TEST(FrameClockTest, AClockWithNoPeriodDividesNothingAndDoesNotDivideByZero)
{
    FrameClock clock;
    clock.Start(5.0, 0.0, 1);
    EXPECT_DOUBLE_EQ(clock.frame_seconds(), 0.0);
    EXPECT_EQ(clock.Advance(6.0), 0);
    EXPECT_DOUBLE_EQ(clock.next_frame_seconds(), 6.0);
}

TEST(FrameClockTest, APlatformMayMoveTheNextFrameOntoItsOwnGrid)
{
    FrameClock   clock;
    const double period = 1.0 / 240.0;
    clock.Start(0.0, 4.0 * period, 1);
    clock.MoveNextFrameTo(0.5);
    EXPECT_DOUBLE_EQ(clock.next_frame_seconds(), 0.5);
    EXPECT_EQ(clock.Advance(0.5), 0);
    EXPECT_DOUBLE_EQ(clock.next_frame_seconds(), 0.5 + 4.0 * period) << "and the schedule carries on from there";
}

// ------------------------------------------------------------------------------------------------------------------
// The property that matters, simulated in both shapes a platform drives it in.
// ------------------------------------------------------------------------------------------------------------------

namespace
{
/** A display: a vertical blank every `period` seconds from `first_blank`.
    @param first_blank  when the first blank was
    @param period       the refresh period
    @param index        which blank
    @return             when that blank is */
double BlankAt(double first_blank, double period, int index)
{
    return first_blank + period * index;
}
} // namespace

TEST(Uniformity, WaitedOnAWindowsShapedLoopEveryFrameIsExactlyTheDivisorsRefreshPeriods)
{
    // The shape window/win/WindowWin.cpp drives: wait for the due time, read the compositor's latest blank, pull the
    // next due time onto that grid. The waking is deliberately scattered inside the period, as a real wait is.
    const double period      = 1.0 / 240.2576;
    const double first_blank = 1234.5;
    const int    divisor     = 4;
    FrameClock   clock;
    clock.Start(first_blank, period * divisor, 1);
    clock.MoveNextFrameTo(OnTheRefreshGrid(clock.next_frame_seconds(), first_blank, period));

    std::vector<double> delivered;
    double              jitter = 0.0;
    for (int frame = 0; frame < 300; ++frame)
    {
        const double due = clock.next_frame_seconds();
        // The wait wakes a little after the due time, by a different amount each frame.
        jitter           = std::fmod(jitter + 0.000173, 0.0008);
        const double now = due + jitter;
        delivered.push_back(due);
        // The compositor's latest blank, as the thread reads it anew each frame.
        const int    blank  = static_cast<int>(std::floor((now - first_blank) / period));
        const double anchor = BlankAt(first_blank, period, blank);
        clock.Advance(now);
        clock.MoveNextFrameTo(OnTheRefreshGrid(clock.next_frame_seconds(), anchor, period));
    }

    ASSERT_EQ(delivered.size(), 300u);
    for (std::size_t i = 1; i < delivered.size(); ++i)
    {
        const double periods = (delivered[i] - delivered[i - 1]) / period;
        EXPECT_NEAR(periods, divisor, 1.0e-6)
            << "frame " << i << " was " << periods << " refresh periods, not " << divisor;
    }
    // And the whole run has not drifted off the panel: the last frame is a whole number of periods from the first.
    const double whole = (delivered.back() - delivered.front()) / period;
    EXPECT_NEAR(whole, std::round(whole), 1.0e-6);
    EXPECT_NEAR(whole, static_cast<double>(divisor * 299), 1.0e-6);
}

TEST(Uniformity, CountedOnADisplayLinksTicksEveryNthTickIsAFrameAndNoOther)
{
    // The shape window/mac/WindowMac.mm drives: the platform calls once a refresh and the window asks, of each tick,
    // whether the next frame belongs on it.
    const double      period      = 1.0 / 120.0;
    const double      first_blank = 77.25;
    const int         divisor     = 2;
    FrameClock        clock;
    RefreshSteadiness steadiness;
    bool              is_started = false;

    std::vector<int> frames_on_tick;
    for (int tick = 0; tick < 400; ++tick)
    {
        const double at = BlankAt(first_blank, period, tick);
        steadiness.Note(period);
        ASSERT_TRUE(steadiness.does_hold_still());
        if (!is_started)
        {
            is_started = true;
            clock.Start(at, period * divisor, 1);
            continue; // the first tick starts the schedule and is not a frame of it
        }
        if (!IsFrameDueOnTick(at, clock.next_frame_seconds(), period))
        {
            continue;
        }
        clock.Advance(at);
        clock.MoveNextFrameTo(OnTheRefreshGrid(clock.next_frame_seconds(), at, period));
        frames_on_tick.push_back(tick);
    }

    ASSERT_FALSE(frames_on_tick.empty());
    EXPECT_EQ(frames_on_tick.front(), divisor) << "the first frame is the divisor's refreshes after the start";
    for (std::size_t i = 1; i < frames_on_tick.size(); ++i)
    {
        EXPECT_EQ(frames_on_tick[i] - frames_on_tick[i - 1], divisor)
            << "frame " << i << " landed " << frames_on_tick[i] - frames_on_tick[i - 1] << " refreshes on";
    }
    EXPECT_EQ(frames_on_tick.size(), (400u - 1u) / static_cast<std::size_t>(divisor))
        << "every refresh is accounted for: no tick was both a frame and not one";
}

TEST(Uniformity, CountingCallbacksWouldDriftOffThePanelWhereTheTimestampsDoNot)
{
    // A display link's callbacks are coalesced where the run loop is busy, so a window that delivered a frame every
    // Nth callback would fall behind the panel by every callback it never got. The schedule is kept in the ticks' own
    // timestamps instead, so it stays on the panel's refreshes and loses at most the one frame whose tick was missed.
    const double period      = 1.0 / 240.0;
    const double first_blank = 3.5;
    const int    divisor     = 4;
    const int    ticks       = 400;
    /** Whether the display link's callback for `tick` arrived: one in seven did not. */
    const auto does_arrive = [](int tick) { return tick % 7 != 3; };

    std::vector<int> by_timestamp;
    FrameClock       clock;
    bool             is_started = false;
    for (int tick = 0; tick < ticks; ++tick)
    {
        if (!does_arrive(tick))
        {
            continue;
        }
        const double at = BlankAt(first_blank, period, tick);
        if (!is_started)
        {
            is_started = true;
            clock.Start(at, period * divisor, 1);
            continue;
        }
        if (!IsFrameDueOnTick(at, clock.next_frame_seconds(), period))
        {
            continue;
        }
        clock.Advance(at);
        clock.MoveNextFrameTo(OnTheRefreshGrid(clock.next_frame_seconds(), at, period));
        by_timestamp.push_back(tick);
    }

    std::vector<int> by_counting;
    int              since_frame = 0;
    for (int tick = 1; tick < ticks; ++tick)
    {
        if (does_arrive(tick) && ++since_frame == divisor)
        {
            since_frame = 0;
            by_counting.push_back(tick);
        }
    }

    ASSERT_FALSE(by_timestamp.empty());
    ASSERT_FALSE(by_counting.empty());
    // Every frame the timestamps delivered is on, or one refresh past, the refresh the schedule asked for - the one
    // that is past it is a tick that never arrived - and the run as a whole has not drifted.
    for (std::size_t i = 0; i < by_timestamp.size(); ++i)
    {
        const int asked_for = divisor * static_cast<int>(i + 1);
        EXPECT_GE(by_timestamp[i], asked_for) << "frame " << i << " cannot be early";
        EXPECT_LE(by_timestamp[i], asked_for + 1) << "frame " << i << " is at most the one missed tick late";
    }
    // Counting callbacks drifts by one refresh for every callback that did not arrive, which over this run is many.
    const int counted_drift = by_counting.back() - divisor * static_cast<int>(by_counting.size());
    EXPECT_GT(counted_drift, 20) << "a count of callbacks is " << counted_drift << " refreshes behind the panel";
}
