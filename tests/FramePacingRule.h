#pragma once

// Test-only: what a child window's pacing state must say whichever branch its platform reached
// (window/FramePacing.h, ChildWindow::SetFramePacing), shared by the halves of the child window's own suite -
// skia/tests/test_child_window.cpp for Windows and Linux, skia/tests/test_child_window_mac.mm for macOS, which is
// compiled as Objective-C++ and so is a translation unit of its own.
//
// **No rate is ever asserted here**: a timing distribution is not a pass or a fail, and none of the three CI runners
// is a machine to measure one on - the Windows runners are services on a window station that shows nothing, where the
// compositor's timing is very probably not to be had, and the Linux job runs under Xvfb, which has no vertical blank.
// So this holds whichever branch the machine reached and prints the probe, so that one run of the suite says which
// that was. The arithmetic and the whole fallback decision are held with no display at all, on every platform, by
// tests/test_frame_pacing.cpp.

#include <cstdio>

#include "FramePacing.h"

#include <gtest/gtest.h>

namespace reckless::test
{
/** What a window's pacing state must say, whichever branch its platform reached, printed so that the run says which.
    @param state    what the window answered
    @param divisor  the refresh divisor that was asked for
    @param where    the platform and the moment, for the print */
inline void HoldsWhicheverPacingItReached(const window::FramePacingState &state, int divisor, const char *where)
{
    std::printf("[ frame pacing ] %s: %s - %s; display %.4f Hz, %.4f frames a second, divisor %d\n", where,
                state.pacing == window::FramePacing::kDisplayClock ? "the display's clock" : "the platform's timer",
                window::TextOf(state.why), state.display_hertz, state.frames_a_second, state.refresh_divisor);
    std::fflush(stdout);
    if (state.pacing == window::FramePacing::kDisplayClock)
    {
        EXPECT_EQ(state.why, window::FramePacingFallback::kNone);
        EXPECT_EQ(state.refresh_divisor, divisor);
        EXPECT_TRUE(window::IsBelievableRefreshRate(state.display_hertz))
            << "a window pacing on a display has read a rate it could believe";
        EXPECT_NEAR(state.frames_a_second, state.display_hertz / divisor, 1.0e-9)
            << "the rate is the display's over the divisor and nothing else";
    }
    else
    {
        EXPECT_NE(state.why, window::FramePacingFallback::kNone) << "a window on the platform's timer always says why";
        EXPECT_NE(state.why, window::FramePacingFallback::kNotAsked) << "and this one was asked";
        EXPECT_EQ(state.refresh_divisor, 0) << "the platform's timer divides nothing";
        EXPECT_EQ(state.frames_a_second, 0.0) << "and claims no rate: its own is not one the window knows";
    }
}
} // namespace reckless::test
