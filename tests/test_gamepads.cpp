// The controller layer's promises (input/Gamepads.h, input/GamepadState.h), on every platform and with no controller:
// a control's name, the thresholds as they are used, the controls a device has said are down - which make every edge
// alternate and let a device that goes let go of everything - an axis's two halves through a threshold with
// hysteresis, an axis that rests at one end told apart from one that rests in the middle, a hat's four directions, and
// the roster's ids. What each platform's back end does with it is held by tests/test_gamepad_formats.cpp,
// tests/test_evdev_gamepads.cpp and, on the machine's own controllers, tests/test_input_devices.cpp.
#include <gtest/gtest.h>

#include <cstdint>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "GamepadState.h"
#include "Gamepads.h"

namespace
{
using reckless::input::GamepadAxisRange;
using reckless::input::GamepadAxisRest;
using reckless::input::GamepadAxisThresholds;
using reckless::input::GamepadControl;
using reckless::input::GamepadControlKind;
using reckless::input::GamepadControlsHeld;
using reckless::input::GamepadDevice;
using reckless::input::GamepadEdge;
using reckless::input::GamepadHat;
using reckless::input::GamepadLayout;
using reckless::input::GamepadRoster;

/** The thresholds most cases use: down at half the travel, let go under three tenths. */
constexpr GamepadAxisThresholds kThresholds{500, 300};

/** A control of a kind at an index.
    @param kind   the kind
    @param index  the index
    @return       the control */
constexpr GamepadControl Control(GamepadControlKind kind, std::uint16_t index)
{
    return GamepadControl{kind, index};
}

/** A layout of a device with `buttons` buttons, the axes given and `hats` hats.
    @param buttons  how many buttons
    @param axes     the axes' ranges
    @param hats     how many hats
    @return         the layout */
GamepadLayout Layout(int buttons, std::vector<GamepadAxisRange> axes = {}, int hats = 0)
{
    return GamepadLayout{"Test pad", "1234:abcd", buttons, std::move(axes), hats};
}

/** A stick's axis, 0 to 255, its rest judged from its first reading. */
constexpr GamepadAxisRange kByteAxis{0, 255, GamepadAxisRest::kFirstReading};

/** Whether every control's edges in `edges` alternate, beginning with a down, given nothing down before them.
    @param edges  the edges
    @return       true when they alternate */
bool Alternates(const std::vector<GamepadEdge> &edges)
{
    std::set<std::pair<std::uint32_t, std::pair<int, int>>> down;
    for (const GamepadEdge &edge : edges)
    {
        const auto key = std::pair(
            edge.device_id, std::pair(static_cast<int>(edge.control.kind), static_cast<int>(edge.control.index)));
        if (edge.is_down != (down.count(key) == 0))
        {
            return false;
        }
        if (edge.is_down)
        {
            down.insert(key);
        }
        else
        {
            down.erase(key);
        }
    }
    return true;
}
} // namespace

TEST(GamepadControlName, NamesCountFromOneAsADevicesOwnLabelsDo)
{
    using reckless::input::GamepadControlName;
    EXPECT_EQ(GamepadControlName(Control(GamepadControlKind::kButton, 3)), "Button 4");
    EXPECT_EQ(GamepadControlName(Control(GamepadControlKind::kAxisPositive, 1)), "Axis 2 +");
    EXPECT_EQ(GamepadControlName(Control(GamepadControlKind::kAxisNegative, 0)), "Axis 1 \xE2\x88\x92")
        << "a negative half's minus is U+2212, not a hyphen";
    EXPECT_EQ(GamepadControlName(Control(GamepadControlKind::kHatUp, 0)), "Hat 1 up");
    EXPECT_EQ(GamepadControlName(Control(GamepadControlKind::kHatDown, 1)), "Hat 2 down");
    EXPECT_EQ(GamepadControlName(Control(GamepadControlKind::kHatLeft, 0)), "Hat 1 left");
    EXPECT_EQ(GamepadControlName(Control(GamepadControlKind::kHatRight, 2)), "Hat 3 right");
    EXPECT_EQ(GamepadControlName(Control(GamepadControlKind::kButton, 65535)), "Button 65536");
    EXPECT_EQ(GamepadControlName(GamepadControl{}), "") << "no control has no name";
}

TEST(GamepadControl, AControlIsItsKindAndItsIndex)
{
    constexpr GamepadControl none;
    static_assert(none.kind == GamepadControlKind::kNone && none.index == 0);
    static_assert(Control(GamepadControlKind::kButton, 2) == Control(GamepadControlKind::kButton, 2));
    static_assert(!(Control(GamepadControlKind::kButton, 2) == Control(GamepadControlKind::kButton, 3)));
    static_assert(!(Control(GamepadControlKind::kAxisNegative, 2) == Control(GamepadControlKind::kAxisPositive, 2)));
    const GamepadEdge edge;
    EXPECT_EQ(edge.device_id, 0u);
    EXPECT_FALSE(edge.is_down);
    const reckless::input::GamepadInfo info;
    EXPECT_EQ(info.device_id, 0u) << "no device is 0";
    EXPECT_EQ(info.button_count + info.axis_count + info.hat_count, 0);
}

TEST(UsableThresholds, APressIsHeldToTheTravelAndAReleaseUnderThePress)
{
    using reckless::input::UsableThresholds;
    static_assert(UsableThresholds({500, 300}).press_thousandths == 500);
    static_assert(UsableThresholds({500, 300}).release_thousandths == 300);
    static_assert(UsableThresholds({1500, 300}).press_thousandths == 1000, "past the end is the end");
    static_assert(UsableThresholds({0, 0}).press_thousandths == 1, "no press is the least press");
    static_assert(UsableThresholds({0, 0}).release_thousandths == 1);
    static_assert(UsableThresholds({400, 600}).release_thousandths == 400, "a release past the press is the press");
    static_assert(UsableThresholds({-5, -5}).release_thousandths == 1);
}

TEST(GamepadControlsHeld, ADownIsNewsOnceAndAnUpOnlyAfterADown)
{
    GamepadControlsHeld  held;
    const GamepadControl button = Control(GamepadControlKind::kButton, 0);
    EXPECT_FALSE(held.NoteUp(button)) << "an up for a control never down is no one's";
    EXPECT_TRUE(held.NoteDown(button));
    EXPECT_FALSE(held.NoteDown(button)) << "a control already down is not down again";
    EXPECT_TRUE(held.IsHeld(button));
    EXPECT_EQ(held.count(), 1u);
    EXPECT_TRUE(held.NoteUp(button));
    EXPECT_FALSE(held.NoteUp(button)) << "one up for one down";
    EXPECT_EQ(held.count(), 0u);
}

TEST(GamepadControlsHeld, WhatIsStillDownIsLetGoOldestFirst)
{
    GamepadControlsHeld held;
    held.NoteDown(Control(GamepadControlKind::kHatUp, 0));
    held.NoteDown(Control(GamepadControlKind::kButton, 5));
    held.NoteDown(Control(GamepadControlKind::kAxisNegative, 1));
    held.NoteUp(Control(GamepadControlKind::kButton, 5));
    const std::vector<GamepadEdge> ups = held.TakeUps(7);
    ASSERT_EQ(ups.size(), 2u);
    EXPECT_EQ(ups[0].control, Control(GamepadControlKind::kHatUp, 0));
    EXPECT_EQ(ups[1].control, Control(GamepadControlKind::kAxisNegative, 1));
    for (const GamepadEdge &up : ups)
    {
        EXPECT_EQ(up.device_id, 7u);
        EXPECT_FALSE(up.is_down);
    }
    EXPECT_EQ(held.count(), 0u);
    EXPECT_TRUE(held.TakeUps(7).empty());
}

TEST(AxisDeflection, WholeNumbersFromTheMiddleOrFromAnEnd)
{
    using reckless::input::CentredDeflection;
    using reckless::input::OneSidedDeflection;
    constexpr GamepadAxisRange kSigned{-32768, 32767, GamepadAxisRest::kCentre};
    static_assert(CentredDeflection(kSigned, -32768) == -1000);
    static_assert(CentredDeflection(kSigned, 32767) == 1000);
    static_assert(CentredDeflection(kSigned, 0) == 0);
    static_assert(CentredDeflection(kByteAxis, 128) == 3, "a byte's middle is between 127 and 128");
    static_assert(CentredDeflection(kByteAxis, -40) == -1000, "a reading past the end is the end");
    static_assert(CentredDeflection(kByteAxis, 9999) == 1000);
    static_assert(CentredDeflection(GamepadAxisRange{5, 5, GamepadAxisRest::kCentre}, 5) == 0, "no range, no push");
    static_assert(OneSidedDeflection(kByteAxis, 0, false) == 0);
    static_assert(OneSidedDeflection(kByteAxis, 255, false) == 1000);
    static_assert(OneSidedDeflection(kByteAxis, 255, true) == 0);
    static_assert(OneSidedDeflection(kByteAxis, 0, true) == 1000);
    static_assert(OneSidedDeflection(kByteAxis, 300, false) == 1000);
    static_assert(OneSidedDeflection(GamepadAxisRange{9, 3, GamepadAxisRest::kMinimum}, 4, false) == 0);
    constexpr GamepadAxisRange kWide{-2147483647 - 1, 2147483647, GamepadAxisRest::kCentre};
    static_assert(CentredDeflection(kWide, 2147483647) == 1000, "the widest range does not overflow");
    static_assert(CentredDeflection(kWide, -2147483647 - 1) == -1000);
}

TEST(JudgeAxisRest, AnAxisRestsInTheMiddleUnlessItsControlWouldBeDownThere)
{
    using reckless::input::JudgeAxisRest;
    static_assert(JudgeAxisRest(kByteAxis, 128, kThresholds) == GamepadAxisRest::kCentre);
    static_assert(JudgeAxisRest(kByteAxis, 180, kThresholds) == GamepadAxisRest::kCentre, "under the press");
    static_assert(JudgeAxisRest(kByteAxis, 0, kThresholds) == GamepadAxisRest::kMinimum, "a trigger at rest");
    static_assert(JudgeAxisRest(kByteAxis, 255, kThresholds) == GamepadAxisRest::kMaximum, "an inverted trigger");
}

TEST(GamepadDevice, AButtonTellsItsChangesAndNothingElse)
{
    GamepadDevice            device(9, Layout(2), kThresholds);
    std::vector<GamepadEdge> edges;
    device.SetButton(1, false, edges);
    EXPECT_TRUE(edges.empty()) << "a button up that was never down tells nothing";
    device.SetButton(1, true, edges);
    device.SetButton(1, true, edges);
    ASSERT_EQ(edges.size(), 1u) << "a reading that changes nothing tells nothing";
    EXPECT_EQ(edges[0].device_id, 9u);
    EXPECT_EQ(edges[0].control, Control(GamepadControlKind::kButton, 1));
    EXPECT_TRUE(edges[0].is_down);
    device.SetButton(1, false, edges);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_FALSE(edges[1].is_down);
    device.SetButton(2, true, edges);
    device.SetButton(-1, true, edges);
    EXPECT_EQ(edges.size(), 2u) << "a button the device does not have is passed over";
}

TEST(GamepadDevice, AStickHasTwoHalvesEachWithHysteresis)
{
    GamepadDevice device(1, Layout(0, {GamepadAxisRange{-1000, 1000, GamepadAxisRest::kCentre}}), kThresholds);
    std::vector<GamepadEdge> edges;
    const GamepadControl     positive = Control(GamepadControlKind::kAxisPositive, 0);
    const GamepadControl     negative = Control(GamepadControlKind::kAxisNegative, 0);
    device.SetAxis(0, 499, edges);
    EXPECT_TRUE(edges.empty()) << "under the press";
    device.SetAxis(0, 500, edges);
    ASSERT_EQ(edges.size(), 1u) << "at the press";
    EXPECT_EQ(edges.back().control, positive);
    EXPECT_TRUE(edges.back().is_down);
    for (const int resting : {450, 320, 301, 300, 499, 300})
    {
        device.SetAxis(0, resting, edges);
    }
    EXPECT_EQ(edges.size(), 1u) << "a stick resting between the two thresholds does not chatter";
    device.SetAxis(0, 299, edges);
    ASSERT_EQ(edges.size(), 2u) << "under the release";
    EXPECT_EQ(edges.back().control, positive);
    EXPECT_FALSE(edges.back().is_down);
    device.SetAxis(0, 499, edges);
    EXPECT_EQ(edges.size(), 2u) << "let go, it needs the press again";
    device.SetAxis(0, -700, edges);
    ASSERT_EQ(edges.size(), 3u);
    EXPECT_EQ(edges.back().control, negative);
    EXPECT_TRUE(edges.back().is_down);
}

TEST(GamepadDevice, AnAxisSwungAcrossLetsOneHalfGoBeforeTheOtherGoesDown)
{
    GamepadDevice            device(1, Layout(0, {kByteAxis}), kThresholds);
    std::vector<GamepadEdge> edges;
    device.SetAxis(0, 128, edges);
    EXPECT_EQ(device.axes()[0].rest, GamepadAxisRest::kCentre) << "judged from the first reading";
    device.SetAxis(0, 0, edges);
    device.SetAxis(0, 255, edges);
    ASSERT_EQ(edges.size(), 3u);
    EXPECT_EQ(edges[0].control.kind, GamepadControlKind::kAxisNegative);
    EXPECT_TRUE(edges[0].is_down);
    EXPECT_EQ(edges[1].control.kind, GamepadControlKind::kAxisNegative);
    EXPECT_FALSE(edges[1].is_down) << "the up first";
    EXPECT_EQ(edges[2].control.kind, GamepadControlKind::kAxisPositive);
    EXPECT_TRUE(edges[2].is_down);
}

TEST(GamepadDevice, ATriggerAtRestHoldsNothingAndHasOneControl)
{
    GamepadDevice            device(1, Layout(0, {kByteAxis, kByteAxis}), kThresholds);
    std::vector<GamepadEdge> edges;
    device.SetAxis(0, 0, edges);
    EXPECT_EQ(device.axes()[0].rest, GamepadAxisRest::kMinimum);
    EXPECT_TRUE(edges.empty()) << "a trigger at rest is not its negative half held down";
    device.SetAxis(0, 127, edges);
    EXPECT_TRUE(edges.empty()) << "under half its travel from rest";
    device.SetAxis(0, 128, edges);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges.back().control, Control(GamepadControlKind::kAxisPositive, 0));
    device.SetAxis(0, 0, edges);
    ASSERT_EQ(edges.size(), 2u) << "back to rest lets it go and holds nothing else";
    EXPECT_FALSE(edges.back().is_down);

    device.SetAxis(1, 255, edges);
    EXPECT_EQ(device.axes()[1].rest, GamepadAxisRest::kMaximum);
    device.SetAxis(1, 0, edges);
    ASSERT_EQ(edges.size(), 3u) << "an inverted trigger's control is its negative half";
    EXPECT_EQ(edges.back().control, Control(GamepadControlKind::kAxisNegative, 1));
    EXPECT_TRUE(edges.back().is_down);
}

TEST(GamepadDevice, AnAxisWhoseRestIsGivenIsNotJudged)
{
    GamepadDevice            device(1, Layout(0, {GamepadAxisRange{0, 255, GamepadAxisRest::kCentre}}), kThresholds);
    std::vector<GamepadEdge> edges;
    device.SetAxis(0, 0, edges);
    EXPECT_EQ(device.axes()[0].rest, GamepadAxisRest::kCentre);
    ASSERT_EQ(edges.size(), 1u) << "a stick the back end said is centred, read at an end, is held";
    EXPECT_EQ(edges[0].control.kind, GamepadControlKind::kAxisNegative);
    device.SetAxis(1, 0, edges);
    device.SetAxis(-1, 0, edges);
    EXPECT_EQ(edges.size(), 1u) << "an axis the device does not have is passed over";
}

TEST(GamepadDevice, AnAxisWithNoRangeIsNeverDown)
{
    GamepadDevice device(1, Layout(0, {GamepadAxisRange{0, 0, GamepadAxisRest::kFirstReading}}), kThresholds);
    std::vector<GamepadEdge> edges;
    for (const int reading : {0, -100, 100, 0})
    {
        device.SetAxis(0, reading, edges);
    }
    EXPECT_TRUE(edges.empty());
}

TEST(GamepadDevice, AHatIsFourControlsAndADiagonalIsTwo)
{
    GamepadDevice            device(1, Layout(0, {}, 2), kThresholds);
    std::vector<GamepadEdge> edges;
    device.SetHat(1, GamepadHat{true, false, false, false}, edges);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges.back().control, Control(GamepadControlKind::kHatUp, 1));
    device.SetHat(1, GamepadHat{true, true, false, false}, edges);
    ASSERT_EQ(edges.size(), 2u) << "up and right: right goes down, up stays";
    EXPECT_EQ(edges.back().control, Control(GamepadControlKind::kHatRight, 1));
    EXPECT_EQ(device.Hat(1), (GamepadHat{true, true, false, false}));
    device.SetHat(1, GamepadHat{false, false, true, false}, edges);
    ASSERT_EQ(edges.size(), 5u) << "turned to down: two ups, then the down";
    EXPECT_FALSE(edges[2].is_down);
    EXPECT_FALSE(edges[3].is_down);
    EXPECT_EQ(edges[4].control, Control(GamepadControlKind::kHatDown, 1));
    EXPECT_TRUE(edges[4].is_down);
    device.SetHat(1, GamepadHat{false, false, false, true}, edges);
    EXPECT_EQ(edges.back().control, Control(GamepadControlKind::kHatLeft, 1));
    device.SetHat(2, GamepadHat{true, false, false, false}, edges);
    EXPECT_EQ(edges.size(), 7u) << "a hat the device does not have is passed over";
    EXPECT_EQ(device.Hat(5), GamepadHat{});
}

TEST(GamepadDevice, LettingGoOfEverythingLeavesNothingDown)
{
    GamepadDevice            device(4, Layout(3, {kByteAxis}, 1), kThresholds);
    std::vector<GamepadEdge> edges;
    device.SetButton(2, true, edges);
    device.SetAxis(0, 128, edges);
    device.SetAxis(0, 255, edges);
    device.SetHat(0, GamepadHat{false, false, true, true}, edges);
    EXPECT_EQ(device.HeldCount(), 4u);
    EXPECT_TRUE(device.IsHeld(Control(GamepadControlKind::kAxisPositive, 0)));
    edges.clear();
    device.ReleaseAll(edges);
    ASSERT_EQ(edges.size(), 4u);
    EXPECT_EQ(edges[0].control, Control(GamepadControlKind::kButton, 2)) << "oldest first";
    for (const GamepadEdge &edge : edges)
    {
        EXPECT_FALSE(edge.is_down);
        EXPECT_EQ(edge.device_id, 4u);
    }
    EXPECT_EQ(device.HeldCount(), 0u);
}

TEST(GamepadDevice, EdgesAlternateWhateverTheReadings)
{
    // A deterministic walk through every kind of reading - a linear congruential generator, so every platform reads
    // the same walk - and the edges it makes held to alternation throughout.
    GamepadDevice            device(3, Layout(4, {kByteAxis, kByteAxis}, 1), kThresholds);
    std::vector<GamepadEdge> edges;
    std::uint32_t            state = 12345u;
    const auto               next  = [&state](std::uint32_t below) {
        state = state * 1664525u + 1013904223u;
        return static_cast<int>((state >> 8) % below);
    };
    for (int step = 0; step < 20000; ++step)
    {
        switch (next(3))
        {
        case 0:
            device.SetButton(next(5), next(2) == 1, edges);
            break;
        case 1:
            device.SetAxis(next(2), next(300) - 20, edges);
            break;
        default:
            device.SetHat(0, GamepadHat{next(2) == 1, next(2) == 1, next(2) == 1, next(2) == 1}, edges);
            break;
        }
    }
    device.ReleaseAll(edges);
    EXPECT_GT(edges.size(), 1000u) << "the walk moved things";
    EXPECT_TRUE(Alternates(edges));
    EXPECT_EQ(device.HeldCount(), 0u);
}

TEST(GamepadDevice, ItsInfoIsWhatTheBackEndFound)
{
    const GamepadDevice device(11, Layout(-3, {kByteAxis, kByteAxis, kByteAxis}, 2), kThresholds);
    const auto          info = device.Info();
    EXPECT_EQ(info.device_id, 11u);
    EXPECT_EQ(info.name, "Test pad");
    EXPECT_EQ(info.stable_id, "1234:abcd");
    EXPECT_EQ(info.button_count, 0) << "a count under nothing is nothing";
    EXPECT_EQ(info.axis_count, 3);
    EXPECT_EQ(info.hat_count, 2);
}

TEST(GamepadStableId, TheModelThenWhatTellsTwoApart)
{
    using reckless::input::GamepadStableId;
    EXPECT_EQ(GamepadStableId(0x0079, 0x0006, ""), "0079:0006");
    EXPECT_EQ(GamepadStableId(0x045E, 0x028E, "usb-0000:00:14.0-2/input0"), "045e:028e/usb-0000:00:14.0-2/input0");
    EXPECT_EQ(GamepadStableId(0xFFFF, 0x0000, "x"), "ffff:0000/x");
}

TEST(GamepadRoster, IdsAreNeverGivenTwiceAndAGoneDeviceLetsGo)
{
    GamepadRoster roster(GamepadAxisThresholds{2000, 3000});
    EXPECT_EQ(roster.thresholds().press_thousandths, 1000) << "held as UsableThresholds holds them";
    EXPECT_EQ(roster.thresholds().release_thousandths, 1000);
    GamepadDevice      &first     = roster.Add(Layout(2));
    const std::uint32_t first_id  = first.device_id();
    GamepadDevice      &second    = roster.Add(Layout(1));
    const std::uint32_t second_id = second.device_id();
    EXPECT_NE(first_id, 0u);
    EXPECT_NE(first_id, second_id);
    EXPECT_EQ(roster.count(), 2u);
    EXPECT_EQ(roster.Find(second_id), &second);
    EXPECT_EQ(roster.Find(0), nullptr);

    std::vector<GamepadEdge> edges;
    first.SetButton(0, true, edges);
    first.SetButton(1, true, edges);
    edges.clear();
    EXPECT_TRUE(roster.Remove(first_id, edges));
    ASSERT_EQ(edges.size(), 2u) << "a device that goes lets go of everything";
    EXPECT_FALSE(edges[0].is_down);
    EXPECT_FALSE(roster.Remove(first_id, edges)) << "it is gone";
    EXPECT_EQ(roster.Find(first_id), nullptr);

    const std::uint32_t third_id = roster.Add(Layout(2)).device_id();
    EXPECT_NE(third_id, first_id) << "a device plugged in again is a new device";
    EXPECT_NE(third_id, second_id);
    GamepadRoster other(kThresholds);
    EXPECT_NE(other.Add(Layout(0)).device_id(), third_id) << "ids are the binary's, not one roster's";

    const std::vector<reckless::input::GamepadInfo> devices = roster.Devices();
    ASSERT_EQ(devices.size(), 2u);
    EXPECT_EQ(devices[0].device_id, second_id) << "in the order they appeared";
    EXPECT_EQ(devices[1].device_id, third_id);
}

TEST(NoGamepads, HasNothingToTell)
{
    reckless::input::NoGamepads none;
    std::vector<GamepadEdge>    edges{GamepadEdge{1, Control(GamepadControlKind::kButton, 0), true}};
    none.Poll(edges);
    EXPECT_EQ(edges.size(), 1u) << "what the caller had is kept";
    EXPECT_TRUE(none.Devices().empty());
}
