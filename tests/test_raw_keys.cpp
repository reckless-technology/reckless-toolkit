// The raw keys a window delivers (window/RawKey.h), on every platform: the keys a window has said are down - which
// answer whether a down is auto-repeat's, whether an up is owed and what is to be let go as the keyboard goes
// elsewhere - the scan code inside a Windows key message's flags, and X11's auto-repeat release. None of it needs a
// window, a display or a keyboard, so every assertion here is held on Linux, macOS and Windows alike, in the builds
// with no GUI too. What the windows themselves do with it is held by the child window's own suite
// (skia/tests/test_child_window.cpp), which needs a window on each platform.
#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

#include "RawKey.h"

namespace
{
using reckless::window::RawKeyEvent;
using reckless::window::RawKeysHeld;

/** A key down with `physical` as its physical code and `named` as the toolkit's name for it.
    @param physical  the physical key code
    @param named     the toolkit's name for the key
    @return          the down */
RawKeyEvent Down(std::uint32_t physical, reckless::view::Key named = reckless::view::Key::kNone)
{
    RawKeyEvent key;
    key.is_down           = true;
    key.named_key         = named;
    key.platform_key_code = physical;
    key.physical_key_code = physical;
    key.modifiers         = {true, false, false};
    return key;
}
} // namespace

TEST(RawKeyEventDefaults, AnEventSaysNothingUntilItIsFilled)
{
    const RawKeyEvent key;
    EXPECT_FALSE(key.is_down) << "no edge";
    EXPECT_FALSE(key.is_repeat);
    EXPECT_EQ(key.named_key, reckless::view::Key::kNone) << "no name until a platform gives one";
    EXPECT_EQ(key.platform_key_code, 0u);
    EXPECT_EQ(key.physical_key_code, 0u);
    EXPECT_FALSE(key.modifiers.shift);
    EXPECT_FALSE(key.modifiers.command);
    EXPECT_FALSE(key.modifiers.alt);
}

TEST(RawKeysHeld, ADownOfAKeyAlreadyHeldIsARepeatAndAnUpLetsItGo)
{
    RawKeysHeld held;
    EXPECT_EQ(held.count(), 0u);
    EXPECT_FALSE(held.NoteDown(Down(30))) << "the first down of a key is never a repeat";
    EXPECT_TRUE(held.IsHeld(30));
    EXPECT_EQ(held.count(), 1u);
    EXPECT_TRUE(held.NoteDown(Down(30))) << "a down of a key already held is auto-repeat's";
    EXPECT_TRUE(held.NoteDown(Down(30)));
    EXPECT_EQ(held.count(), 1u) << "however many repeats, the key is held once";
    EXPECT_TRUE(held.NoteUp(30)) << "the up is owed: the down was told";
    EXPECT_FALSE(held.IsHeld(30));
    EXPECT_EQ(held.count(), 0u);
    EXPECT_FALSE(held.NoteDown(Down(30))) << "the next down is a fresh press, not a repeat";
}

TEST(RawKeysHeld, AnUpForAKeyWhoseDownWasNeverToldIsNoOnes)
{
    RawKeysHeld held;
    EXPECT_FALSE(held.NoteUp(57)) << "the key went down before the window had the keyboard";
    EXPECT_FALSE(held.NoteDown(Down(57)));
    EXPECT_TRUE(held.NoteUp(57));
    EXPECT_FALSE(held.NoteUp(57)) << "and the same up twice is owed once";
}

TEST(RawKeysHeld, SeveralKeysAreHeldAtOnceAndEachIsItsOwn)
{
    RawKeysHeld held;
    // The left shift, the right shift and the space bar, as Windows' scan codes name them: the two shift keys differ
    // there, which is why the physical code and not the platform's own is a key's identity.
    EXPECT_FALSE(held.NoteDown(Down(0x2A)));
    EXPECT_FALSE(held.NoteDown(Down(0x36))) << "the other shift key is another key";
    EXPECT_FALSE(held.NoteDown(Down(0x39)));
    EXPECT_EQ(held.count(), 3u);
    EXPECT_TRUE(held.IsHeld(0x2A));
    EXPECT_TRUE(held.IsHeld(0x36));
    EXPECT_TRUE(held.IsHeld(0x39));
    EXPECT_FALSE(held.IsHeld(0x1E));
    EXPECT_TRUE(held.NoteUp(0x36));
    EXPECT_EQ(held.count(), 2u);
    EXPECT_FALSE(held.IsHeld(0x36));
    EXPECT_TRUE(held.IsHeld(0x2A)) << "letting one go leaves the rest held";
    EXPECT_TRUE(held.NoteDown(Down(0x2A))) << "and the one still held still repeats";
}

TEST(RawKeysHeld, EverythingHeldIsLetGoInTheOrderItWentDownWithTheCodesAndTheNameOfItsDown)
{
    RawKeysHeld held;
    held.NoteDown(Down(0x11, reckless::view::Key::kNone)); // W
    held.NoteDown(Down(0x48, reckless::view::Key::kUp));   // the up arrow
    held.NoteDown(Down(0x39));                             // the space bar
    held.NoteUp(0x48);
    const std::vector<RawKeyEvent> ups = held.TakeUps();
    ASSERT_EQ(ups.size(), 2u) << "what is still held, and only that";
    EXPECT_EQ(ups[0].physical_key_code, 0x11u) << "oldest first";
    EXPECT_EQ(ups[1].physical_key_code, 0x39u);
    for (const RawKeyEvent &up : ups)
    {
        EXPECT_FALSE(up.is_down) << "each is an up";
        EXPECT_FALSE(up.is_repeat);
        EXPECT_FALSE(up.modifiers.shift) << "no modifiers: nothing says what is held once the keyboard has gone";
    }
    EXPECT_EQ(held.count(), 0u) << "nothing held afterwards";
    EXPECT_TRUE(held.TakeUps().empty());

    RawKeysHeld named;
    named.NoteDown(Down(0x48, reckless::view::Key::kUp));
    const std::vector<RawKeyEvent> one = named.TakeUps();
    ASSERT_EQ(one.size(), 1u);
    EXPECT_EQ(one[0].named_key, reckless::view::Key::kUp) << "the name of the down it answers";
    EXPECT_EQ(one[0].platform_key_code, 0x48u);
}

TEST(WindowsPhysicalKey, TheScanCodeComesOutOfTheMessagesFlagsWithTheExtendedPrefix)
{
    using reckless::window::WindowsPhysicalKeyOf;
    // A Windows key message's lparam: the repeat count in bits 0 to 15, the scan code in 16 to 23, the extended bit
    // 24, the context, previous-state and transition bits above it. Only the scan code and the extended bit matter.
    EXPECT_EQ(WindowsPhysicalKeyOf(0x00390001u), 0x39u) << "the space bar, its repeat count passed over";
    EXPECT_EQ(WindowsPhysicalKeyOf(0x002A0001u), 0x2Au) << "the left shift";
    EXPECT_EQ(WindowsPhysicalKeyOf(0x00360001u), 0x36u) << "the right shift: another key, another code";
    EXPECT_EQ(WindowsPhysicalKeyOf(0x011D0001u), 0xE01Du) << "the right control, extended: 0xE0 before its code";
    EXPECT_EQ(WindowsPhysicalKeyOf(0x001D0001u), 0x1Du) << "and the left control, which is not";
    EXPECT_EQ(WindowsPhysicalKeyOf(0x01480001u), 0xE048u) << "the up arrow of the block between the letters and the "
                                                             "keypad";
    EXPECT_EQ(WindowsPhysicalKeyOf(0x00480001u), 0x48u) << "and the keypad's 8, which types the same arrow";
    EXPECT_EQ(WindowsPhysicalKeyOf(0xC0390001u), 0x39u) << "the transition and previous-state bits passed over";
    EXPECT_EQ(WindowsPhysicalKeyOf(0u), 0u) << "a message that carried no scan code names no key";
}

TEST(X11AutoRepeatRelease, AReleaseIsAutoRepeatsOnlyWhenTheSameKeysPressFollowsItAtTheSameMillisecond)
{
    using reckless::window::IsX11AutoRepeatRelease;
    using reckless::window::X11KeyStamp;
    const X11KeyStamp released{0x2000001u, 38u, 1234567u};
    EXPECT_TRUE(IsX11AutoRepeatRelease(released, true, {0x2000001u, 38u, 1234567u}))
        << "the same window, the same key, the same millisecond: auto-repeat's, where the server gives no detectable "
           "auto-repeat";
    EXPECT_FALSE(IsX11AutoRepeatRelease(released, false, {0x2000001u, 38u, 1234567u}))
        << "nothing waiting, or what waits is no key press: the person let the key go";
    EXPECT_FALSE(IsX11AutoRepeatRelease(released, true, {0x2000001u, 38u, 1234568u})) << "a millisecond later: a press";
    EXPECT_FALSE(IsX11AutoRepeatRelease(released, true, {0x2000001u, 39u, 1234567u})) << "another key";
    EXPECT_FALSE(IsX11AutoRepeatRelease(released, true, {0x2000002u, 38u, 1234567u})) << "another window";
}
