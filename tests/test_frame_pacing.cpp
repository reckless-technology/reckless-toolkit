// How a window paces its frame callback (window/FramePacing.h), on every platform: the refresh rate chosen from what
// a platform could read and the whole of the fallback decision, the subdivision arithmetic, the grid a due time is
// pulled onto, whether a refresh period holds still enough to be divided, the schedule with its catching up, and the
// lead that wakes a frame before the vertical blank it is due at.
// None of it needs a display, a compositor or a thread, so every assertion here is held on Linux, macOS and Windows
// alike, in the builds with no GUI too - and the two shapes a platform drives it in are simulated here against a
// made-up display, which is where the property that matters is actually held: that every frame is shown for exactly
// the divisor's refresh periods. What the windows themselves do with it is held by the child window's own suite
// (skia/tests/test_child_window.cpp), which needs a real display clock and only has one on some platforms.
#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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
using reckless::window::kLeadLeavesAtLeastMicroseconds;
using reckless::window::kRefreshPeriodsJudged;
using reckless::window::LeadInForceMicroseconds;
using reckless::window::LedWakeSeconds;
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
    @param lead_microseconds  how long before the blank each frame is woken
    @return                   the request */
FrameRequest Asking(int divisor, int most_frames_behind = 1, std::int32_t lead_microseconds = 0)
{
    return {FramePacing::kDisplayClock, divisor, most_frames_behind, lead_microseconds};
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
    EXPECT_EQ(request.lead_microseconds, 0) << "and wakes at the blank, as a window always has";
}

TEST(FramePacingStateDefaults, ADefaultStateIsAWindowNoOneHasAsked)
{
    const FramePacingState state;
    EXPECT_EQ(state.pacing, FramePacing::kPlatformTimer);
    EXPECT_EQ(state.refresh_divisor, 0) << "the platform timer divides nothing";
    EXPECT_EQ(state.display_hertz, 0.0) << "and reads no display";
    EXPECT_EQ(state.frames_a_second, 0.0) << "the platform timer's rate is not a number the window knows";
    EXPECT_EQ(state.why, FramePacingFallback::kNotAsked);
    EXPECT_EQ(state.lead_microseconds, 0) << "and leads nothing";
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
                                         FramePacingFallback::kRefreshRateUnsteady,
                                         FramePacingFallback::kDisplayLinkSilent};
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
    for (const FrameRequest &request : {Asking(0), Asking(-1), FrameRequest{FramePacing::kDisplayClock, 1, -1, 0}})
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

// ------------------------------------------------------------------------------------------------------------------
// Leading the vertical blank: the frame woken before the blank it is due at, so it is finished as it is composed.
// ------------------------------------------------------------------------------------------------------------------

TEST(Lead, TheLeadAskedForIsKeptWhereTheFramePeriodLeavesRoomForIt)
{
    const double sixty = 1.0 / 60.0; // 16666.67 microseconds
    EXPECT_EQ(LeadInForceMicroseconds(5000, sixty), 5000);
    EXPECT_EQ(LeadInForceMicroseconds(1, sixty), 1);
    EXPECT_EQ(LeadInForceMicroseconds(15666, sixty), 15666) << "the frame period less a millisecond is the longest";
    // A lead longer than one refresh period is a lead like any other where the divisor makes the frame longer.
    EXPECT_EQ(LeadInForceMicroseconds(10000, 4.0 / 240.0), 10000) << "two and a half refreshes ahead, at 240 Hz / 4";
}

TEST(Lead, ALeadThePeriodCannotHoldIsHeldToTheLongestItCan)
{
    const double sixty = 1.0 / 60.0;
    EXPECT_EQ(LeadInForceMicroseconds(15667, sixty), 15666);
    EXPECT_EQ(LeadInForceMicroseconds(20000, sixty), 15666) << "a lead longer than the frame period itself";
    EXPECT_EQ(LeadInForceMicroseconds(2'000'000'000, sixty), 15666);
    EXPECT_EQ(LeadInForceMicroseconds(5000, 1.0 / 240.0), 3166) << "at 240 Hz a 5 ms lead is more than one frame";
    EXPECT_EQ(16666 - LeadInForceMicroseconds(99999, sixty), kLeadLeavesAtLeastMicroseconds)
        << "a millisecond of the frame is always left";
}

TEST(Lead, NoLeadIsKeptForALeadUnderZeroOrAPeriodWithNoRoom)
{
    EXPECT_EQ(LeadInForceMicroseconds(0, 1.0 / 60.0), 0);
    EXPECT_EQ(LeadInForceMicroseconds(-5000, 1.0 / 60.0), 0) << "a lead under 0 is none, not a lag";
    EXPECT_EQ(LeadInForceMicroseconds(500, 1.0 / 1000.0), 0) << "a 1 ms frame has no room left in it to lead";
    EXPECT_EQ(LeadInForceMicroseconds(500, 0.0), 0) << "and a period that is not one leads nothing";
    EXPECT_EQ(LeadInForceMicroseconds(500, -1.0), 0);
}

TEST(Lead, TheStateSaysTheLeadInForceAndOnlyOnTheDisplaysClock)
{
    EXPECT_EQ(ChosenPacing(Asking(1, 1, 5000), GoodReading(60.0, 60.0)).lead_microseconds, 5000);
    EXPECT_EQ(ChosenPacing(Asking(1, 1, 20000), GoodReading(60.0, 60.0)).lead_microseconds, 15666)
        << "held to the frame period, and the held lead is what is reported";
    EXPECT_EQ(ChosenPacing(Asking(4, 1, 10000), GoodReading(240.0, 240.0)).lead_microseconds, 10000);
    EXPECT_EQ(ChosenPacing(Asking(1, 1, -1), GoodReading(60.0, 60.0)).why, FramePacingFallback::kNone)
        << "a lead under 0 is held to none rather than refusing the request";

    FrameRequest timer           = Asking(1, 1, 5000);
    timer.pacing                 = FramePacing::kPlatformTimer;
    DisplayClockReading varying  = GoodReading(60.0, 60.0);
    varying.does_rate_hold_still = false;
    EXPECT_EQ(ChosenPacing(timer, GoodReading(60.0, 60.0)).lead_microseconds, 0) << "the platform timer leads nothing";
    EXPECT_EQ(ChosenPacing(Asking(1, 1, 5000), DisplayClockReading{}).lead_microseconds, 0);
    EXPECT_EQ(ChosenPacing(Asking(1, 1, 5000), varying).lead_microseconds, 0);
    EXPECT_EQ(ChosenPacing(Asking(0, 1, 5000), GoodReading(60.0, 60.0)).lead_microseconds, 0);
}

TEST(Lead, AFrameIsWokenTheLeadBeforeTheBlankItIsDueAt)
{
    static_assert(LedWakeSeconds(10.0, 0.0) == 10.0, "no lead wakes at the blank");
    EXPECT_DOUBLE_EQ(LedWakeSeconds(10.0, 0.005), 9.995);
}

namespace
{
/** One frame of a led loop: the blank it was due at and when it was woken. */
struct LedFrame
{
    double due_seconds;  ///< the vertical blank the frame was due at
    double woke_seconds; ///< when the wait for it ended
};

/** The loop window/win/WindowWin.cpp's display clock runs, against a made-up display: the compositor's latest blank
    read, the due time pulled onto its grid, the wait for the due time less the lead - which ends at once for a time
    already past - and the clock told the time as the waking plus the lead.
    @param period        the refresh period
    @param first_blank   the display's first blank, which the loop starts just after
    @param divisor       the refresh periods each frame is shown for
    @param lead_seconds  the lead in force
    @param late_by       how late each wait ends after its wake time, frame by frame; its size is the frames run
    @return              every frame's due blank and waking */
std::vector<LedFrame> RunALedLoop(double period, double first_blank, int divisor, double lead_seconds,
                                  const std::vector<double> &late_by)
{
    double     now = first_blank + 0.37 * period; // started part way through a refresh, as a product's window is
    FrameClock clock;
    clock.Start(now + lead_seconds, period * divisor, 1);
    std::vector<LedFrame> frames;
    for (const double late : late_by)
    {
        const double anchor = BlankAt(first_blank, period, static_cast<int>(std::floor((now - first_blank) / period)));
        clock.MoveNextFrameTo(OnTheRefreshGrid(clock.next_frame_seconds(), anchor, period));
        const double wake = LedWakeSeconds(clock.next_frame_seconds(), lead_seconds);
        now               = std::max(wake, now) + late;
        frames.push_back({clock.next_frame_seconds(), now});
        clock.Advance(now + lead_seconds);
    }
    return frames;
}
} // namespace

TEST(Lead, WaitedWithALeadEveryFrameIsStillTheDivisorsRefreshPeriodsAndWokenTheLeadAhead)
{
    struct Case
    {
        double hertz;
        int    divisor;
        double lead_seconds;
    };

    // A lead of nothing, a few milliseconds, more than half a refresh - where pulling a led due time onto the grid
    // would land it on the blank before - the longest the period allows, and leads longer than a refresh period
    // where the divisor leaves room for one.
    const Case every[] = {{60.0, 1, 0.0},    {60.0, 1, 0.005},  {60.0, 1, 0.012},  {60.0, 1, 0.015666},
                          {240.0, 4, 0.010}, {59.94, 2, 0.020}, {144.0, 1, 0.0045}};
    for (const Case &one : every)
    {
        const double        period = 1.0 / one.hertz;
        std::vector<double> late_by(240);
        double              jitter = 0.0;
        for (double &late : late_by)
        {
            jitter = std::fmod(jitter + 0.000173, 0.0003); // a real wait ends a little after its time, never exactly
            late   = jitter;
        }
        const double                first_blank = 1234.5;
        const std::vector<LedFrame> frames = RunALedLoop(period, first_blank, one.divisor, one.lead_seconds, late_by);
        ASSERT_EQ(frames.size(), late_by.size());
        const double started = first_blank + 0.37 * period;
        EXPECT_GT(frames.front().woke_seconds - late_by.front(), started)
            << one.hertz << " Hz, lead " << one.lead_seconds << ": the first frame is never woken for in the past";
        for (std::size_t i = 0; i < frames.size(); ++i)
        {
            const double on_grid = (frames[i].due_seconds - first_blank) / period;
            EXPECT_NEAR(on_grid, std::round(on_grid), 1.0e-6) << "frame " << i << " is due on a blank";
            EXPECT_NEAR(frames[i].due_seconds - (frames[i].woke_seconds - late_by[i]), one.lead_seconds, 1.0e-9)
                << "frame " << i << " is woken the lead before its blank, at " << one.hertz << " Hz";
            if (i > 0)
            {
                EXPECT_NEAR((frames[i].due_seconds - frames[i - 1].due_seconds) / period, one.divisor, 1.0e-6)
                    << "frame " << i << " at " << one.hertz << " Hz, lead " << one.lead_seconds;
            }
        }
    }
}

TEST(Lead, ALeadMovesOnlyTheWakingNotTheScheduleNorItsCatchingUp)
{
    // The same lateness, measured from each frame's waking, with and without a lead - a stall of three and a half
    // frames among it, which the clock gives up on - must leave the two loops' frames exactly the same blanks apart.
    // Which blank is the first may differ: a lead longer than half a refresh starts the schedule one blank later, so
    // that its first waking is a frame period ahead rather than already past.
    const double        period = 1.0 / 60.0;
    std::vector<double> late_by(120, 0.0002);
    late_by[40]                       = 3.5 * period;
    late_by[41]                       = 1.2 * period;
    const std::vector<LedFrame> unled = RunALedLoop(period, 50.0, 1, 0.0, late_by);
    const std::vector<LedFrame> led   = RunALedLoop(period, 50.0, 1, 0.011, late_by);
    ASSERT_EQ(unled.size(), led.size());
    long given_up = 0;
    for (std::size_t i = 0; i < unled.size(); ++i)
    {
        EXPECT_NEAR(led[i].due_seconds - led.front().due_seconds, unled[i].due_seconds - unled.front().due_seconds,
                    1.0e-9)
            << "frame " << i;
        if (i > 0)
        {
            given_up += std::lround((unled[i].due_seconds - unled[i - 1].due_seconds) / period) - 1;
        }
    }
    EXPECT_GT(given_up, 0) << "the stall was long enough to be given up on, so the catching up was exercised";
}

TEST(DisplayLinkSilence, ALinkThatHasStoppedIsToldFromOneThatIsTicking)
{
    using reckless::window::IsDisplayLinkSilent;
    using reckless::window::kDisplayLinkSilentSeconds;
    EXPECT_FALSE(IsDisplayLinkSilent(10.0, 10.0));
    EXPECT_FALSE(IsDisplayLinkSilent(10.0, 10.0 + 1.0 / 20.0)) << "the slowest refresh believed, a whole tick on";
    EXPECT_FALSE(IsDisplayLinkSilent(10.0, 10.0 + kDisplayLinkSilentSeconds * 0.99));
    EXPECT_TRUE(IsDisplayLinkSilent(10.0, 10.0 + kDisplayLinkSilentSeconds));
    EXPECT_TRUE(IsDisplayLinkSilent(10.0, 1000.0));
    EXPECT_GE(kDisplayLinkSilentSeconds, 4.0 / 20.0)
        << "more than a few ticks of the slowest refresh a display can have";
}

TEST(FrameStandsBack, AFrameInFlightIsTakenForLostOnlyAfterTheBound)
{
    using reckless::window::IsFrameInFlightLost;
    using reckless::window::kFrameInFlightIsLostAfterSeconds;
    EXPECT_FALSE(IsFrameInFlightLost(0.0));
    EXPECT_FALSE(IsFrameInFlightLost(0.25)) << "a frame that has only stood back for as long as one can";
    EXPECT_TRUE(IsFrameInFlightLost(kFrameInFlightIsLostAfterSeconds));
    EXPECT_TRUE(IsFrameInFlightLost(60.0));
}

TEST(FrameStandsBack, AsksAgainSoonUnlessItIsAskingFasterThanTheThreadCanGetOn)
{
    using reckless::window::FrameAsksAgainAfter;
    using reckless::window::kFrameAsksAgainAfterAtLeastSeconds;
    using reckless::window::kFrameAsksAgainAfterAtMostSeconds;
    EXPECT_EQ(FrameAsksAgainAfter(0.0, 0.0), kFrameAsksAgainAfterAtLeastSeconds)
        << "a first stand back waits the least";
    EXPECT_EQ(FrameAsksAgainAfter(0.0, 0.0035), kFrameAsksAgainAfterAtLeastSeconds);
    EXPECT_EQ(FrameAsksAgainAfter(kFrameAsksAgainAfterAtLeastSeconds, 0.0035), kFrameAsksAgainAfterAtLeastSeconds)
        << "answered only after a paint: the thread is getting on, and the wait stays short";
    // Answered at once, over and over: a thread answering questions and reaching no paint, however many windows ask.
    double waited = 0.0;
    double most   = 0.0;
    for (int asked = 0; asked < 40; ++asked)
    {
        waited = FrameAsksAgainAfter(waited, 0.00002);
        most   = waited > most ? waited : most;
    }
    EXPECT_EQ(most, kFrameAsksAgainAfterAtMostSeconds) << "the wait grows to its bound";
    EXPECT_EQ(waited, kFrameAsksAgainAfterAtMostSeconds)
        << "and holds there while the thread is still answering at once";
    EXPECT_EQ(FrameAsksAgainAfter(waited, 0.0035 + 2.0 * waited), kFrameAsksAgainAfterAtLeastSeconds)
        << "and the first answer that took long enough to be a paint brings it back down";
}

TEST(CompositorReadings, AReadingThatWandersIsSetAsideForTheModeOfThePanelUnderIt)
{
    // Readings of the kind taken on a 240 Hz panel whose kernel vertical blank held at 240 a second, one window at 30
    // frames a second.
    using reckless::window::CompositorReadings;
    const double       measured[] = {240.0, 233.1, 221.4, 208.7, 197.3, 216.2, 238.9, 244.8, 212.5, 199.6,
                                     205.1, 228.4, 240.0, 251.3, 240.0, 226.3, 181.0, 203.4, 219.9, 239.1};
    CompositorReadings readings;
    int                set_aside = 0;
    for (double hertz : measured)
    {
        set_aside += readings.Note(hertz, 240.0) ? 1 : 0;
        EXPECT_FALSE(readings.does_disagree_for_good()) << "they do not hold still among themselves";
        EXPECT_FALSE(readings.does_vary()) << "and are not whole multiples apart";
    }
    EXPECT_GT(set_aside, 0) << "the ones further than a twentieth off were set aside";
    EXPECT_FALSE(readings.Note(240.0, 240.0)) << "and one that agrees is taken as it is";
}

TEST(CompositorReadings, ASteadyReadingIsWorthMoreThanTheModeAndAWanderingOneIsNot)
{
    using reckless::window::CompositorReadings;
    using reckless::window::kRefreshPeriodsJudged;
    CompositorReadings steady;
    EXPECT_FALSE(steady.does_read_steadily(59.0)) << "nothing judged yet";
    for (std::size_t reading = 0; reading < kRefreshPeriodsJudged; ++reading)
    {
        steady.Note(59.9413 + 0.002 * static_cast<double>(reading % 3), 59.0);
    }
    EXPECT_TRUE(steady.does_read_steadily(59.0)) << "59.94 Hz read over again, on a mode Windows gives as 59";
    CompositorReadings wandering;
    const double       measured[] = {240.0, 233.1, 241.4, 228.7, 240.3, 236.2, 238.9, 244.8};
    for (double hertz : measured)
    {
        wandering.Note(hertz, 240.0);
    }
    EXPECT_FALSE(wandering.does_read_steadily(240.0)) << "these all agree with the mode, and are no rate to divide";
    CompositorReadings stuck;
    for (std::size_t reading = 0; reading < kRefreshPeriodsJudged; ++reading)
    {
        stuck.Note(233.0, 240.0);
    }
    EXPECT_FALSE(stuck.does_read_steadily(240.0)) << "nor is a wandering reading that has stuck for a moment";
}

TEST(ModeHertz, TheModeIsTheRateAndTheCompositorOnlyWhereTheModeNamesNone)
{
    using reckless::window::ModeHertzOf;
    DisplayClockReading reading;
    reading.display_hertz    = 240.0;
    reading.compositor_hertz = 181.0; // measured on a 240 Hz panel with one window drawing thirty pictures a second
    EXPECT_EQ(ModeHertzOf(reading), 240.0) << "the mode holds still where the compositor's reading wanders";
    reading.display_hertz    = 1.0; // Windows' answer for the hardware's default, which is no rate
    reading.compositor_hertz = 59.94;
    EXPECT_EQ(ModeHertzOf(reading), 59.94);
    reading.display_hertz    = 0.0; // a built-in panel's mode on a Mac can name no rate
    reading.compositor_hertz = 120.0;
    EXPECT_EQ(ModeHertzOf(reading), 120.0);
    reading.compositor_hertz = 5000.0;
    EXPECT_EQ(ModeHertzOf(reading), 0.0) << "nothing a display could be, so nothing to work a divisor out from";
    EXPECT_EQ(ModeHertzOf(DisplayClockReading{}), 0.0);
}

TEST(ModeHertz, ARateOfSixtyOrBelowComesOfEveryPanelsModeHoweverItIsNamed)
{
    // What docs/window/frame-pacing.md, "Choosing a divisor", tells a product to write, on the numbers a mode is named
    // in: whole hertz on Windows, where a fractional panel's may be rounded either way, and as the mode says on a Mac.
    const auto divisor_for = [](double mode_hertz) {
        return static_cast<int>(std::ceil(static_cast<double>(std::lround(mode_hertz)) / 60.0));
    };

    const struct
    {
        double hertz;
        int    divisor;
    } panels[] = {{30.0, 1},  {50.0, 1},  {59.0, 1},  {59.94, 1},  {60.0, 1},   {60.02, 1}, {75.0, 2},
                  {90.0, 2},  {100.0, 2}, {119.0, 2}, {119.88, 2}, {120.0, 2},  {143.0, 3}, {143.98, 3},
                  {144.0, 3}, {165.0, 3}, {180.0, 3}, {239.0, 4},  {239.76, 4}, {240.0, 4}, {360.0, 6}};

    for (const auto &panel : panels)
    {
        EXPECT_EQ(divisor_for(panel.hertz), panel.divisor) << panel.hertz << " Hz";
        const double frames_a_second = panel.hertz / panel.divisor;
        EXPECT_GE(frames_a_second, 30.0) << panel.hertz << " Hz";
        EXPECT_LE(frames_a_second, 60.02) << panel.hertz << " Hz";
    }
}

TEST(CompositorReadings, OneReadingIsAnsweredWithOnlyWhereItIsTheModeReadMoreFinely)
{
    using reckless::window::RateOfOneReading;
    EXPECT_EQ(RateOfOneReading(59.94, 60.0), 59.94) << "a mode Windows rounded up";
    EXPECT_EQ(RateOfOneReading(59.94, 59.0), 59.94) << "and one it cut down";
    EXPECT_EQ(RateOfOneReading(143.98, 144.0), 143.98);
    EXPECT_EQ(RateOfOneReading(240.2576, 240.0), 240.2576);
    // Within the twentieth the two are taken to agree, and past what a divisor can be worked out from: 252 / 60 is over
    // 4, and a product rounding up would be at 48 frames a second on a panel that gives it 60.
    EXPECT_EQ(RateOfOneReading(252.0, 240.0), 240.0);
    EXPECT_EQ(RateOfOneReading(181.0, 240.0), 240.0);
    EXPECT_EQ(RateOfOneReading(0.0, 240.0), 240.0) << "a compositor that said nothing";
    EXPECT_EQ(RateOfOneReading(59.94, 0.0), 59.94) << "a mode that names no rate leaves the reading";
    EXPECT_EQ(RateOfOneReading(59.94, 1.0), 59.94) << "and so does the hardware's default";
}

TEST(CompositorReadings, AWindowOnAnotherMonitorDisagreesForGood)
{
    // A window dragged to a 60 Hz monitor of a desktop the compositor reads at 240: the mode moves, the reading does
    // not.
    using reckless::window::CompositorReadings;
    using reckless::window::kCompositorDisagreesForGoodAfterReadings;
    CompositorReadings readings;
    for (int i = 0; i < 100; ++i)
    {
        readings.Note(240.0, 240.0);
    }
    readings.Forget();
    for (std::size_t reading = 0; reading + 1 < kCompositorDisagreesForGoodAfterReadings; ++reading)
    {
        EXPECT_TRUE(readings.Note(240.0 + static_cast<double>(reading % 2), 60.0))
            << "set aside at first: reading " << reading;
        EXPECT_FALSE(readings.does_disagree_for_good());
    }
    EXPECT_FALSE(readings.Note(240.0, 60.0)) << "the reading that fills the count is believed";
    EXPECT_TRUE(readings.does_disagree_for_good());
    EXPECT_FALSE(readings.does_vary()) << "this is another monitor, not a varying panel";
    EXPECT_FALSE(readings.Note(60.0, 60.0)) << "back on a display it agrees with";
    EXPECT_FALSE(readings.does_disagree_for_good());
}

TEST(CompositorReadings, AWanderingReadingThatSticksForAMomentIsNotAnotherMonitor)
{
    // A run of readings far from the mode holding still among themselves, as was seen once on the unchanged 240 Hz
    // panel.
    using reckless::window::CompositorReadings;
    using reckless::window::kRefreshPeriodsJudged;
    CompositorReadings readings;
    for (std::size_t reading = 0; reading < kRefreshPeriodsJudged + 2; ++reading)
    {
        EXPECT_TRUE(readings.Note(218.0, 240.0)) << "set aside: reading " << reading;
    }
    EXPECT_FALSE(readings.does_disagree_for_good());
}

TEST(CompositorReadings, APanelWhoseRateReallyVariesIsTold)
{
    // A variable-refresh panel following what is drawn on it: no real panel of this kind has been measured here, so
    // this is the shape the page describes, whole multiples apart.
    using reckless::window::CompositorReadings;
    using reckless::window::kRefreshPeriodsJudged;
    CompositorReadings readings;
    const double       varying[] = {240.0, 120.0, 80.0, 48.0, 96.0, 200.0, 144.0, 60.0};
    for (double hertz : varying)
    {
        readings.Note(hertz, 240.0);
    }
    EXPECT_TRUE(readings.does_vary());
    EXPECT_FALSE(readings.does_disagree_for_good()) << "it does not hold still at another rate";

    CompositorReadings too_few;
    for (std::size_t reading = 0; reading + 1 < kRefreshPeriodsJudged; ++reading)
    {
        too_few.Note(reading % 2 == 0 ? 240.0 : 60.0, 240.0);
    }
    EXPECT_FALSE(too_few.does_vary()) << "nothing is judged until there are enough to judge";

    CompositorReadings no_mode;
    EXPECT_FALSE(no_mode.Note(60.0, 0.0)) << "with no mode to set a reading aside for, a reading is all there is";
    EXPECT_FALSE(no_mode.Note(0.0, 240.0)) << "and a compositor that says nothing has said nothing to set aside";
}

TEST(FrameStandsBack, OnlyForWorkAheadAndNotForEver)
{
    using reckless::window::DoesFrameStandBack;
    using reckless::window::kFrameStandsBackAtMostSeconds;
    EXPECT_FALSE(DoesFrameStandBack(false, 0.0)) << "a thread with nothing waiting runs the frame at once";
    EXPECT_FALSE(DoesFrameStandBack(false, 10.0));
    EXPECT_TRUE(DoesFrameStandBack(true, 0.0));
    EXPECT_TRUE(DoesFrameStandBack(true, kFrameStandsBackAtMostSeconds * 0.99));
    EXPECT_FALSE(DoesFrameStandBack(true, kFrameStandsBackAtMostSeconds))
        << "a queue whose paint or timer never comes costs a frame this long, not the window's frames for good";
}

namespace
{
/** What a thread's queue does with paced frames, in a model with no window: a posted message is taken before any paint,
    an invalid window nearest the top of the z-order is painted first, and a timer waits for both. Each window asks to
    run a frame at every tick; `does_stand_back` says whether the frame is held to DoesFrameStandBack.

    The model is what docs/window/frame-pacing.md measures on a real thread, and the queue's three rules are the ones
    `tools/frame-pacing-harness.cpp`'s paint-rank run shows there. */
struct QueueRun
{
    std::vector<int> paints;         ///< each window's paints, the top of the z-order first.
    int              host_timers{0}; ///< how many of the host-like window's timers ran.
};

QueueRun RunAQueue(int windows, double paint_seconds, double frame_seconds, double run_seconds, bool does_stand_back)
{
    QueueRun run;
    run.paints.assign(static_cast<std::size_t>(windows), 0);
    std::vector<char> is_invalid(static_cast<std::size_t>(windows), 0);
    std::vector<char> is_frame_waiting(static_cast<std::size_t>(windows), 0);
    const double      host_timer_seconds = 0.020;
    double            next_host_timer    = host_timer_seconds;
    bool              is_host_timer_due  = false;
    double            next_tick          = 0.0;
    double            now                = 0.0;
    while (now < run_seconds)
    {
        while (now >= next_tick)
        {
            std::fill(is_frame_waiting.begin(), is_frame_waiting.end(), char{1});
            next_tick += frame_seconds;
        }
        while (now >= next_host_timer)
        {
            is_host_timer_due = true;
            next_host_timer += host_timer_seconds;
        }
        // The posted frames first: each read the queue before any of this refresh has run, so they run or stand back
        // together.
        const bool is_paint_waiting = std::any_of(is_invalid.begin(), is_invalid.end(), [](char v) { return v != 0; });
        const bool does_frame_wait =
            does_stand_back && reckless::window::DoesFrameStandBack(is_paint_waiting || is_host_timer_due, 0.0);
        if (!does_frame_wait &&
            std::any_of(is_frame_waiting.begin(), is_frame_waiting.end(), [](char v) { return v != 0; }))
        {
            for (std::size_t i = 0; i < is_invalid.size(); ++i)
            {
                if (is_frame_waiting[i] != 0)
                {
                    is_invalid[i]       = 1;
                    is_frame_waiting[i] = 0;
                }
            }
            continue;
        }
        // Then the paint nearest the top of the z-order.
        const auto paint = std::find(is_invalid.begin(), is_invalid.end(), char{1});
        if (paint != is_invalid.end())
        {
            *paint = 0;
            ++run.paints[static_cast<std::size_t>(paint - is_invalid.begin())];
            now += paint_seconds;
            continue;
        }
        // Then a timer, and otherwise the thread sleeps until something is due.
        if (is_host_timer_due)
        {
            is_host_timer_due = false;
            ++run.host_timers;
            continue;
        }
        now = std::min(next_tick, next_host_timer);
    }
    return run;
}
} // namespace

TEST(FrameStandsBack, ASaturatedThreadStarvesNoWindowAndNoHostWhenItsFramesWaitTheirTurn)
{
    const double   frame_seconds = 1.0 / 60.0;
    const double   paint_seconds = 0.0036; // about what a panel cost on the display this page measures
    const QueueRun free_running  = RunAQueue(8, paint_seconds, frame_seconds, 10.0, false);
    EXPECT_EQ(free_running.paints.back(), 0) << "frames ahead of the paints: the bottom of the z-order gets none";
    EXPECT_EQ(free_running.host_timers, 0) << "and neither does a host's timer on the thread";

    const QueueRun waiting    = RunAQueue(8, paint_seconds, frame_seconds, 10.0, true);
    const auto [fewest, most] = std::minmax_element(waiting.paints.begin(), waiting.paints.end());
    EXPECT_GT(*fewest, 0) << "every window is painted";
    EXPECT_LE(*most - *fewest, 2) << "and they take it in turn";
    EXPECT_GT(waiting.host_timers, 0) << "and the host's timer runs";
    int total_free    = 0;
    int total_waiting = 0;
    for (int paints : free_running.paints)
    {
        total_free += paints;
    }
    for (int paints : waiting.paints)
    {
        total_waiting += paints;
    }
    EXPECT_GE(total_waiting * 100, total_free * 90) << "at about what the thread can paint either way";
}

TEST(FrameStandsBack, AThreadWithRoomRunsEveryFrameOnItsOwnTick)
{
    const double   frame_seconds = 1.0 / 60.0;
    const QueueRun run           = RunAQueue(4, 0.0036, frame_seconds, 10.0, true);
    for (int paints : run.paints)
    {
        EXPECT_NEAR(paints, 600, 2) << "four panels fit a refresh, and every one has every frame";
    }
    EXPECT_GT(run.host_timers, 0);
}
