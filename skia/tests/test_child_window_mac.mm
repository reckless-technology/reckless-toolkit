// The child window's macOS half, which is Objective-C++ because it reaches AppKit's events and its run loop; the
// Windows and Linux halves are in test_child_window.cpp.
//
// The raw keys (window/RawKey.h, ChildWindow::SetRawKeyCallback): the key going down and the key let go as two edges,
// auto-repeat marked as such, the virtual key code as the physical key, and the modifier keys, which a Mac reports
// through `flagsChanged:` and no other way - with the views given exactly the key downs they were always given and
// nothing else. The events are made and sent in process; nothing is typed.
//
// And how the frame callback is paced (window/FramePacing.h, ChildWindow::SetFramePacing) on the one CI platform with
// a real window server, so the display link can genuinely be made and ticked: what is held of it, and why no rate is,
// is tests/FramePacingRule.h.
#import <AppKit/AppKit.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

#include "FramePacing.h"
#include "FramePacingRule.h"
#include "TestHostWindow.h"
#include "View.h"
#include "Window.h"

namespace
{
using reckless::test::HoldsWhicheverPacingItReached;
using reckless::window::FramePacing;
using reckless::window::FramePacingFallback;
using reckless::window::FramePacingState;

/** The panel, as far as the keys ask of one: it wants the keyboard, takes every key the views are given, and counts
    them. */
class KeyCounter final : public reckless::view::View
{
  public:
    bool WantsKeyboardFocus() const override
    {
        return true;
    }

    bool KeyPressed(const reckless::view::KeyEvent &) override
    {
        ++taken;
        return true;
    }

    int taken{0}; ///< how many keys the views were given
};

/** A key event as the keyboard sends one.
    @param type       the event's type: a key down, a key up or a change of the modifiers
    @param characters what the key types, unshifted; empty for a key that types nothing
    @param code       the key's virtual key code
    @param flags      the modifiers held once the event has happened
    @param is_repeat  whether the system marks the event as one of auto-repeat's
    @return           the event */
NSEvent *KeyEvent(NSEventType type, NSString *characters, unsigned short code, NSEventModifierFlags flags,
                  BOOL is_repeat)
{
    return [NSEvent keyEventWithType:type
                            location:NSZeroPoint
                       modifierFlags:flags
                           timestamp:0
                        windowNumber:0
                             context:nil
                          characters:characters
         charactersIgnoringModifiers:characters
                           isARepeat:is_repeat
                             keyCode:code];
}

constexpr unsigned short kRightArrow = 124; ///< the right arrow's virtual key code
constexpr unsigned short kSpaceBar   = 49;  ///< the space bar's virtual key code
constexpr unsigned short kLeftShift  = 56;  ///< the left shift's virtual key code
constexpr unsigned short kRightShift = 60;  ///< the right shift's virtual key code
} // namespace

TEST(ChildWindow, TheRawKeysGiveEveryEdgeAndLeaveTheViewsWhatTheyAlwaysHad)
{
    reckless::test::TestHostWindow top(240, 160);
    ASSERT_TRUE(top.Valid());
    top.Show(true);
    KeyCounter root;
    root.SetBounds({240, 160});
    auto child = reckless::window::CreateChildWindow(top.Parent(), root, 240, 160, 1.0f);
    ASSERT_NE(child, nullptr);
    NSView *view = [[(__bridge NSView *)top.Parent() subviews] firstObject];
    ASSERT_NE(view, nil) << "the surface's view inside the host's";
    root.host()->RequestKeyboardFocus(&root);

    // First with no one asking: the right arrow's down is the views', its up AppKit's own, as it always was.
    [view keyDown:KeyEvent(NSEventTypeKeyDown, @"", kRightArrow, 0, NO)];
    [view keyUp:KeyEvent(NSEventTypeKeyUp, @"", kRightArrow, 0, NO)];
    EXPECT_EQ(root.taken, 1) << "a key down to the views, and a key up to no one";
    const int before = root.taken;

    std::vector<reckless::window::RawKeyEvent> heard;
    child->SetRawKeyCallback([&](const reckless::window::RawKeyEvent &key) { heard.push_back(key); });

    // The right arrow held until it repeats and then let go: the first down is no repeat whatever the event's own
    // isARepeat says of it, since the window has not said the key is down.
    [view keyDown:KeyEvent(NSEventTypeKeyDown, @"", kRightArrow, 0, YES)];
    [view keyDown:KeyEvent(NSEventTypeKeyDown, @"", kRightArrow, 0, YES)];
    [view keyUp:KeyEvent(NSEventTypeKeyUp, @"", kRightArrow, 0, NO)];
    ASSERT_EQ(heard.size(), 3u);
    EXPECT_TRUE(heard[0].is_down);
    EXPECT_FALSE(heard[0].is_repeat) << "the first down the window delivers is never a repeat";
    EXPECT_EQ(heard[0].named_key, reckless::view::Key::kRight) << "the toolkit's own name, where it has one";
    EXPECT_EQ(heard[0].physical_key_code, static_cast<std::uint32_t>(kRightArrow))
        << "a Mac's key code, which is the physical key";
    EXPECT_EQ(heard[0].platform_key_code, heard[0].physical_key_code) << "and its own code besides";
    EXPECT_TRUE(heard[1].is_repeat) << "a down of a key the window has said is down";
    EXPECT_FALSE(heard[2].is_down);
    EXPECT_EQ(root.taken, before + 2) << "the views were given the two downs and nothing else: the key ups are not "
                                         "theirs, and the raw keys took none of the downs away";

    // The space bar, which the views' own enum does not name.
    heard.clear();
    [view keyDown:KeyEvent(NSEventTypeKeyDown, @" ", kSpaceBar, 0, NO)];
    ASSERT_EQ(heard.size(), 1u);
    EXPECT_EQ(heard[0].named_key, reckless::view::Key::kNone) << "the space bar has no name in the views' enum";
    EXPECT_EQ(heard[0].physical_key_code, static_cast<std::uint32_t>(kSpaceBar));

    // The two shift keys, which send no key down at all: `flagsChanged:` says which key changed, and its flag stays
    // set while the other is held, so the window answers from what it has said is down.
    heard.clear();
    [view flagsChanged:KeyEvent(NSEventTypeFlagsChanged, @"", kLeftShift, NSEventModifierFlagShift, NO)];
    [view flagsChanged:KeyEvent(NSEventTypeFlagsChanged, @"", kRightShift, NSEventModifierFlagShift, NO)];
    [view flagsChanged:KeyEvent(NSEventTypeFlagsChanged, @"", kLeftShift, NSEventModifierFlagShift, NO)];
    [view flagsChanged:KeyEvent(NSEventTypeFlagsChanged, @"", kRightShift, 0, NO)];
    ASSERT_EQ(heard.size(), 4u) << "a down and an up for each shift key";
    EXPECT_TRUE(heard[0].is_down);
    EXPECT_EQ(heard[0].physical_key_code, static_cast<std::uint32_t>(kLeftShift));
    EXPECT_TRUE(heard[1].is_down) << "the other shift key is another key, not a repeat";
    EXPECT_EQ(heard[1].physical_key_code, static_cast<std::uint32_t>(kRightShift));
    EXPECT_FALSE(heard[2].is_down) << "the left shift let go while the right one still holds the flag";
    EXPECT_EQ(heard[2].physical_key_code, static_cast<std::uint32_t>(kLeftShift));
    EXPECT_FALSE(heard[3].is_down) << "and then the right one, which clears it";

    // The keyboard gone elsewhere: the space bar is still held, and it is let go.
    heard.clear();
    root.host()->RequestKeyboardFocus(nullptr);
    ASSERT_EQ(heard.size(), 1u) << "an up for the key still held";
    EXPECT_FALSE(heard[0].is_down);
    EXPECT_EQ(heard[0].physical_key_code, static_cast<std::uint32_t>(kSpaceBar));

    // A callback taken back is told nothing more.
    heard.clear();
    child->SetRawKeyCallback(nullptr);
    [view keyDown:KeyEvent(NSEventTypeKeyDown, @"", kRightArrow, 0, NO)];
    [view keyUp:KeyEvent(NSEventTypeKeyUp, @"", kRightArrow, 0, NO)];
    EXPECT_TRUE(heard.empty());
    child.reset(); // before the window it sits in
}

TEST(ChildWindowFramePacing, IsOnThePlatformTimerUntilItIsAskedAndSaysWhichItGotAndWhy)
{
    reckless::test::TestHostWindow top(240, 160);
    ASSERT_TRUE(top.Valid());
    reckless::view::View root;
    root.SetBounds({240, 160});
    auto child = reckless::window::CreateChildWindow(top.Parent(), root, 240, 160, 1.0f);
    ASSERT_NE(child, nullptr);

    const FramePacingState unasked = child->FramePacingNow();
    EXPECT_EQ(unasked.pacing, FramePacing::kPlatformTimer) << "a window no one has asked is paced as it always was";
    EXPECT_EQ(unasked.why, FramePacingFallback::kNotAsked);

    // A lead asked for, which a Mac's display link does not yet keep: the state says 0 (docs/window/frame-pacing.md).
    const double mode_hertz = child->DisplayModeHertz();
    EXPECT_TRUE(mode_hertz == 0.0 || reckless::window::IsBelievableRefreshRate(mode_hertz)) << mode_hertz;
    EXPECT_EQ(child->FramePacingNow().why, FramePacingFallback::kNotAsked) << "reading the rate asks for nothing";

    const FramePacingState asked = child->SetFramePacing({FramePacing::kDisplayClock, 2, 1, 5000});
    HoldsWhicheverPacingItReached(asked, 2, "macOS, asked for a frame every second refresh, 5 ms ahead", 5000);

    const FramePacingState back = child->SetFramePacing({});
    EXPECT_EQ(back.pacing, FramePacing::kPlatformTimer) << "a default request puts it back on the platform's timer";
    EXPECT_EQ(back.why, FramePacingFallback::kNotAsked);
    child.reset(); // before the window it sits in
}

TEST(ChildWindowFramePacing, TicksOnTheDisplayLinkOfTheOneCiPlatformWithAWindowServer)
{
    // macOS is the one platform whose CI runner has a real window server, so a display link can genuinely be made and
    // ticked here. What is held is that the frames arrive and keep arriving inside a generous window - never a rate:
    // the runner is virtualised, and a rate measured on it would mean nothing. Both ways round, and the unasked
    // window first: one that was asked for nothing runs its callback from the NSTimer it always had, which is what
    // keeps a product that does not opt in unchanged.
    for (const bool does_ask : {false, true})
    {
        reckless::test::TestHostWindow top(240, 160);
        ASSERT_TRUE(top.Valid());
        top.Show(true);
        reckless::view::View root;
        root.SetBounds({240, 160});
        auto child = reckless::window::CreateChildWindow(top.Parent(), root, 240, 160, 1.0f);
        ASSERT_NE(child, nullptr);
        if (does_ask)
        {
            HoldsWhicheverPacingItReached(child->SetFramePacing({FramePacing::kDisplayClock, 1, 1, 0}), 1,
                                          "macOS, running frames");
        }
        else
        {
            ASSERT_EQ(child->FramePacingNow().why, FramePacingFallback::kNotAsked);
        }

        int frames = 0;
        child->SetFrameCallback([&] { ++frames; });
        const NSTimeInterval until = [NSDate timeIntervalSinceReferenceDate] + 3.0;
        while (frames < 20 && [NSDate timeIntervalSinceReferenceDate] < until)
        {
            [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
                                  beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.05]];
        }
        std::printf("[ frame pacing ] macOS: %d frames in at most three seconds, %s\n", frames,
                    does_ask ? "having asked for the display's clock" : "having asked for nothing");
        std::fflush(stdout);
        EXPECT_GE(frames, 20) << "the frames arrive and keep arriving, whether or not anything was asked";
        const int so_far = frames;
        [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.5]];
        EXPECT_GT(frames, so_far) << "and they are still arriving half a second later";
        child->SetFrameCallback(nullptr);
        child.reset(); // before the window it sits in
    }
}

TEST(ChildWindowFramePacing, AHiddenWindowThatDoesNotAskHasNoFramesUntilItShowsAndSaysWhyMeanwhile)
{
    // What a product that has not asked has always had, and may rely on: its frames stop with the link, and come back
    // on the clock as the window shows, its schedule its own.
    reckless::test::TestHostWindow top(240, 160);
    ASSERT_TRUE(top.Valid());
    top.Show(true);
    reckless::view::View root;
    root.SetBounds({240, 160});
    auto child = reckless::window::CreateChildWindow(top.Parent(), root, 240, 160, 1.0f);
    ASSERT_NE(child, nullptr);
    int frames = 0;
    child->SetFrameCallback([&] { ++frames; });
    const FramePacingState asked = child->SetFramePacing({FramePacing::kDisplayClock, 1, 1, 0});
    if (asked.pacing != FramePacing::kDisplayClock)
    {
        child->SetFrameCallback(nullptr);
        child.reset();
        GTEST_SKIP() << "no display link on this machine, so nothing to fall silent";
    }
    const auto run_for = [](NSTimeInterval seconds) {
        const NSTimeInterval until = [NSDate timeIntervalSinceReferenceDate] + seconds;
        while ([NSDate timeIntervalSinceReferenceDate] < until)
        {
            [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
                                  beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
    };
    run_for(0.5);
    EXPECT_GT(frames, 0);
    top.Show(false);
    run_for(reckless::window::kDisplayLinkSilentSeconds * 3.0);
    const int hidden_from = frames;
    run_for(0.5);
    std::printf(
        "[ frame pacing ] macOS, hidden, having not asked for frames off a screen: %d frames in half a second, %s\n",
        frames - hidden_from, reckless::window::TextOf(child->FramePacingNow().why));
    std::fflush(stdout);
    EXPECT_EQ(frames, hidden_from) << "no timer takes over for a product that did not ask for one";
    EXPECT_EQ(child->FramePacingNow().why, FramePacingFallback::kDisplayLinkSilent) << "and the window still says why";

    top.Show(true);
    run_for(0.5);
    EXPECT_GT(frames, hidden_from) << "shown again the frames come back";
    EXPECT_EQ(child->FramePacingNow().pacing, FramePacing::kDisplayClock) << "on the clock, with no one asking";
    child->SetFrameCallback(nullptr);
    child.reset(); // before the window it sits in
}

TEST(ChildWindowFramePacing, AHiddenWindowThatAsksStillHasFramesAndSaysWhyAndTheClockComesBackAsItShows)
{
    // macOS stops a display link made from a view whose window is hidden, and with nothing else to run it the frame
    // callback would stop too: no frame, so nothing to tell the panel it no longer shows and no way for a product to
    // stand its pacing down. A product that asks for frames off a screen is put on the timer the window has always
    // used, and is told why, with no one asking again.
    reckless::test::TestHostWindow top(240, 160);
    ASSERT_TRUE(top.Valid());
    top.Show(true);
    reckless::view::View root;
    root.SetBounds({240, 160});
    auto child = reckless::window::CreateChildWindow(top.Parent(), root, 240, 160, 1.0f);
    ASSERT_NE(child, nullptr);
    int frames = 0;
    child->SetFrameCallback([&] { ++frames; });
    const FramePacingState asked = child->SetFramePacing({FramePacing::kDisplayClock, 1, 1, 0, true});
    HoldsWhicheverPacingItReached(asked, 1, "macOS, a window about to be hidden");
    if (asked.pacing != FramePacing::kDisplayClock)
    {
        child->SetFrameCallback(nullptr);
        child.reset();
        GTEST_SKIP() << "no display link on this machine, so nothing to fall silent";
    }
    const auto run_for = [](NSTimeInterval seconds) {
        const NSTimeInterval until = [NSDate timeIntervalSinceReferenceDate] + seconds;
        while ([NSDate timeIntervalSinceReferenceDate] < until)
        {
            [[NSRunLoop mainRunLoop] runMode:NSDefaultRunLoopMode
                                  beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
    };
    run_for(0.5);
    EXPECT_GT(frames, 0) << "the link ticks while the window shows";
    EXPECT_EQ(child->FramePacingNow().pacing, FramePacing::kDisplayClock);

    top.Show(false);
    run_for(reckless::window::kDisplayLinkSilentSeconds * 3.0); // long enough for the link to be seen to have stopped
    const int              hidden_from = frames;
    const FramePacingState hidden      = child->FramePacingNow();
    run_for(0.5);
    std::printf("[ frame pacing ] macOS, hidden: %d frames in half a second, %s\n", frames - hidden_from,
                reckless::window::TextOf(hidden.why));
    std::fflush(stdout);
    EXPECT_FALSE(child->Showing());
    EXPECT_GT(frames, hidden_from) << "a hidden window still has frames, or nothing could tell it that it is hidden";
    EXPECT_EQ(hidden.pacing, FramePacing::kPlatformTimer) << "and does not claim the clock it is no longer on";
    EXPECT_EQ(hidden.why, FramePacingFallback::kDisplayLinkSilent);
    EXPECT_EQ(hidden.frames_a_second, 0.0);

    top.Show(true);
    run_for(0.5);
    const int shown_from = frames;
    run_for(1.0);
    const FramePacingState shown = child->FramePacingNow();
    EXPECT_LE(frames - shown_from, static_cast<int>(1.5 * shown.frames_a_second) + 5)
        << "and the timer that stood in for the link has stopped: this is no rate, only that frames do not come twice";
    EXPECT_EQ(shown.pacing, FramePacing::kDisplayClock) << "shown again, with no one asking, the clock comes back";
    EXPECT_EQ(shown.why, FramePacingFallback::kNone);
    EXPECT_GT(shown.frames_a_second, 0.0);
    child->SetFrameCallback(nullptr);
    child.reset(); // before the window it sits in
}
