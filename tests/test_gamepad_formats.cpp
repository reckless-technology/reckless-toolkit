// What each platform's controllers say, read as the back ends read it, with none of any platform's headers - so held
// on every platform: HID's collections, roles, logical ranges, signs and hat switches (input/HidUsages.h, the Windows
// and macOS back ends'), a Windows HID path's XInput mark and instance (input/win/HidPaths.h), an XInput pad's state
// (input/win/XInputPads.h), and evdev's capabilities, layout and events (input/linux/EvdevLayout.h). The kernel's own
// struct input_event is held by tests/test_input_devices.cpp on Linux, where its header is.
#include <gtest/gtest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "GamepadState.h"
#include "HidUsages.h"
#include "linux/EvdevLayout.h"
#include "win/HidPaths.h"
#include "win/XInputPads.h"

namespace
{
using reckless::input::GamepadAxisRest;
using reckless::input::GamepadAxisThresholds;
using reckless::input::GamepadControl;
using reckless::input::GamepadControlKind;
using reckless::input::GamepadDevice;
using reckless::input::GamepadEdge;
using reckless::input::GamepadHat;
namespace evdev  = reckless::input::evdev;
namespace hid    = reckless::input::hid;
namespace xinput = reckless::input::xinput;

/** Down at half the travel, let go under three tenths. */
constexpr GamepadAxisThresholds kThresholds{500, 300};

/** A hat pointing the given ways.
    @param up     up
    @param right  right
    @param down   down
    @param left   left
    @return       the hat */
constexpr GamepadHat Hat(bool up, bool right, bool down, bool left)
{
    return GamepadHat{up, right, down, left};
}

/** A game pad's capabilities as the kernel's xpad driver gives an Xbox pad's: the south, east, north and west buttons,
    the shoulders, select, start, mode and the sticks' clicks; X, Y, Z (the left trigger), RX, RY, RZ (the right
    trigger), and the cross as hat 0.
    @return  the capabilities */
evdev::EvdevCapabilities XpadCapabilities()
{
    evdev::EvdevCapabilities capabilities;
    for (const std::size_t code : {0x130, 0x131, 0x133, 0x134, 0x136, 0x137, 0x13a, 0x13b, 0x13c, 0x13d, 0x13e})
    {
        capabilities.keys.set(code);
    }
    for (const std::size_t code : {0x00, 0x01, 0x03, 0x04})
    {
        capabilities.absolutes.set(code);
        capabilities.absolute_info[code] = evdev::EvdevAbsoluteInfo{0, -32768, 32767};
    }
    for (const std::size_t code : {0x02, 0x05})
    {
        capabilities.absolutes.set(code);
        capabilities.absolute_info[code] = evdev::EvdevAbsoluteInfo{0, 0, 255};
    }
    for (const std::size_t code : {0x10, 0x11})
    {
        capabilities.absolutes.set(code);
        capabilities.absolute_info[code] = evdev::EvdevAbsoluteInfo{0, -1, 1};
    }
    capabilities.vendor_id     = 0x045E;
    capabilities.product_id    = 0x028E;
    capabilities.name          = "Microsoft X-Box 360 pad";
    capabilities.physical_path = "usb-0000:00:14.0-2/input0";
    return capabilities;
}

/** An event.
    @param type   its type
    @param code   its code
    @param value  its value
    @return       the event */
constexpr evdev::EvdevEvent Event(std::uint16_t type, std::uint16_t code, std::int32_t value)
{
    return evdev::EvdevEvent{type, code, value};
}
} // namespace

TEST(HidUsages, ControllersAreJoysticksGamePadsAndMultiAxisControllers)
{
    static_assert(hid::IsControllerCollection(0x01, 0x04));
    static_assert(hid::IsControllerCollection(0x01, 0x05));
    static_assert(hid::IsControllerCollection(0x01, 0x08));
    static_assert(!hid::IsControllerCollection(0x01, 0x02), "a mouse");
    static_assert(!hid::IsControllerCollection(0x01, 0x06), "a keyboard");
    static_assert(!hid::IsControllerCollection(0x0C, 0x05), "another page's usage 5");
}

TEST(HidUsages, AxesAreTheDesktopsAndTheSimulationControlsAndAHatIsAHat)
{
    for (std::uint16_t usage = 0x30; usage <= 0x38; ++usage)
    {
        EXPECT_EQ(hid::RoleOfValue(0x01, usage), hid::ValueRole::kAxis) << usage;
    }
    EXPECT_EQ(hid::RoleOfValue(0x01, 0x39), hid::ValueRole::kHat);
    EXPECT_EQ(hid::RoleOfValue(0x01, 0x2F), hid::ValueRole::kNone);
    EXPECT_EQ(hid::RoleOfValue(0x01, 0x3A), hid::ValueRole::kNone);
    for (const std::uint16_t usage : {0xBA, 0xBB, 0xC4, 0xC5, 0xC8})
    {
        EXPECT_EQ(hid::RoleOfValue(0x02, usage), hid::ValueRole::kAxis) << usage;
    }
    EXPECT_EQ(hid::RoleOfValue(0x02, 0xB0), hid::ValueRole::kNone);
    EXPECT_EQ(hid::RoleOfValue(0xFF00, 0x30), hid::ValueRole::kNone) << "a vendor's page";
}

TEST(HidUsages, AMaximumWrittenInTooFewBytesIsTheFieldsWholeRange)
{
    constexpr auto kRepaired = hid::RepairedLogicalRange(0, -1, 8);
    static_assert(kRepaired.minimum == 0 && kRepaired.maximum == 255);
    static_assert(kRepaired.rest == GamepadAxisRest::kFirstReading);
    static_assert(hid::RepairedLogicalRange(0, -1, 16).maximum == 65535);
    static_assert(hid::RepairedLogicalRange(0, 1023, 10).maximum == 1023, "a good range is kept");
    static_assert(hid::RepairedLogicalRange(-127, 127, 8).maximum == 127);
    static_assert(hid::RepairedLogicalRange(-5, -9, 8).maximum == -9, "a signed range is not guessed at");
    static_assert(hid::RepairedLogicalRange(0, -1, 0).maximum == -1, "nor one with no size");
    static_assert(hid::RepairedLogicalRange(0, -1, 32).maximum == -1);
}

TEST(HidUsages, AFieldIsSignedWhereItsMinimumIs)
{
    static_assert(hid::SignedValue(0xFF, 8, -127) == -1);
    static_assert(hid::SignedValue(0x81, 8, -127) == -127);
    static_assert(hid::SignedValue(0x7F, 8, -127) == 127);
    static_assert(hid::SignedValue(0x800, 12, -2048) == -2048);
    static_assert(hid::SignedValue(0xFF, 8, 0) == 255, "unsigned where the minimum is not negative");
    static_assert(hid::SignedValue(0x1FF, 8, -1) == -1, "bits past the field are not the field's");
    static_assert(hid::SignedValue(0xFFFFFFFFu, 32, -1) == -1, "a whole word is taken as it is");
}

TEST(HidUsages, AHatOfEightCountsClockwiseFromUpAndOutsideItsRangeIsCentred)
{
    static_assert(hid::HatOfSwitch(0, 0, 7) == Hat(true, false, false, false));
    static_assert(hid::HatOfSwitch(1, 0, 7) == Hat(true, true, false, false));
    static_assert(hid::HatOfSwitch(2, 0, 7) == Hat(false, true, false, false));
    static_assert(hid::HatOfSwitch(3, 0, 7) == Hat(false, true, true, false));
    static_assert(hid::HatOfSwitch(4, 0, 7) == Hat(false, false, true, false));
    static_assert(hid::HatOfSwitch(5, 0, 7) == Hat(false, false, true, true));
    static_assert(hid::HatOfSwitch(6, 0, 7) == Hat(false, false, false, true));
    static_assert(hid::HatOfSwitch(7, 0, 7) == Hat(true, false, false, true));
    static_assert(hid::HatOfSwitch(8, 0, 7) == GamepadHat{}, "the null state");
    static_assert(hid::HatOfSwitch(15, 0, 7) == GamepadHat{});
    static_assert(hid::HatOfSwitch(-1, 0, 7) == GamepadHat{});
    static_assert(hid::HatOfSwitch(1, 1, 8) == Hat(true, false, false, false), "counted from its minimum");
    static_assert(hid::HatOfSwitch(0, 1, 8) == GamepadHat{});
}

TEST(HidUsages, AHatOfFourCountsUpRightDownLeftAndAnyOtherIsCentred)
{
    static_assert(hid::HatOfSwitch(0, 0, 3) == Hat(true, false, false, false));
    static_assert(hid::HatOfSwitch(1, 0, 3) == Hat(false, true, false, false));
    static_assert(hid::HatOfSwitch(2, 0, 3) == Hat(false, false, true, false));
    static_assert(hid::HatOfSwitch(3, 0, 3) == Hat(false, false, false, true));
    static_assert(hid::HatOfSwitch(4, 0, 3) == GamepadHat{});
    static_assert(hid::HatOfSwitch(2, 0, 5) == GamepadHat{}, "six positions is neither");
}

TEST(HidPaths, AnXInputPathIsMarkedIgInEitherCase)
{
    using reckless::input::hidpath::IsXInputPath;
    constexpr std::wstring_view kXInput =
        L"\\\\?\\hid#vid_045e&pid_028e&ig_00#7&2f3a1b2&0&0000#{4d1e55b2-f16f-11cf-88cb-001111000030}";
    constexpr std::wstring_view kUpper = L"\\\\?\\HID#VID_045E&PID_02FF&IG_01#8&1&0&0000#{4d1e55b2}";
    constexpr std::wstring_view kPlain = L"\\\\?\\hid#vid_0079&pid_0006#7&1c4e3fa&0&0000#{4d1e55b2}";
    static_assert(IsXInputPath(kXInput));
    static_assert(IsXInputPath(kUpper));
    static_assert(!IsXInputPath(kPlain));
    static_assert(IsXInputPath(std::string_view("ig_")));
    static_assert(!IsXInputPath(std::string_view("ig")));
    static_assert(!IsXInputPath(std::string_view("big-box")), "ig without the underscore");
}

TEST(HidPaths, AnInstanceIsBetweenTheSecondAndThirdHash)
{
    using reckless::input::hidpath::InstanceOf;
    EXPECT_EQ(InstanceOf(std::wstring_view(L"\\\\?\\hid#vid_0079&pid_0006#7&1C4E3FA&0&0000#{4d1e55b2}")),
              "7&1c4e3fa&0&0000");
    EXPECT_EQ(InstanceOf(std::string_view("a#b#c")), "c") << "no third hash: to the end";
    EXPECT_EQ(InstanceOf(std::string_view("a#b")), "");
    EXPECT_EQ(InstanceOf(std::string_view("abc")), "");
    EXPECT_EQ(InstanceOf(std::wstring_view(L"a#b#\u00E9x#")), "?x") << "anything not ASCII is a question mark";
}

TEST(XInputPads, APadIsTenButtonsSixAxesAndAHat)
{
    const auto layout = xinput::PadLayout(2);
    EXPECT_EQ(layout.name, "XInput controller 3");
    EXPECT_EQ(layout.stable_id, "xinput/2");
    EXPECT_EQ(layout.button_count, 10);
    ASSERT_EQ(layout.axes.size(), 6u);
    EXPECT_EQ(layout.axes[0].rest, GamepadAxisRest::kCentre);
    EXPECT_EQ(layout.axes[2].rest, GamepadAxisRest::kMinimum) << "the left trigger rests at the bottom";
    EXPECT_EQ(layout.axes[5].rest, GamepadAxisRest::kMinimum);
    EXPECT_EQ(layout.hat_count, 1);
    static_assert(xinput::DownPositive(32767) == -32768);
    static_assert(xinput::DownPositive(-32768) == 32767);
    static_assert(xinput::DownPositive(0) == -1);
}

TEST(XInputPads, AStateIsToldButtonsInWindowsOrderAxesAndTheCross)
{
    GamepadDevice            device(1, xinput::PadLayout(0), kThresholds);
    std::vector<GamepadEdge> edges;
    xinput::ApplyPadState(xinput::PadState{}, device, edges);
    EXPECT_TRUE(edges.empty()) << "a pad at rest holds nothing, its triggers included";

    xinput::PadState state;
    state.buttons       = xinput::kA | xinput::kStart | xinput::kDpadUp | xinput::kDpadLeft;
    state.left_y        = 32767;
    state.right_x       = 20000;
    state.right_trigger = 200;
    xinput::ApplyPadState(state, device, edges);
    ASSERT_EQ(edges.size(), 7u);
    EXPECT_EQ(edges[0].control, (GamepadControl{GamepadControlKind::kButton, 0})) << "A is button 0";
    EXPECT_EQ(edges[1].control, (GamepadControl{GamepadControlKind::kButton, 7})) << "Start is button 7";
    EXPECT_EQ(edges[2].control, (GamepadControl{GamepadControlKind::kAxisNegative, 1})) << "the left stick up";
    EXPECT_EQ(edges[3].control, (GamepadControl{GamepadControlKind::kAxisPositive, 3})) << "the right stick right";
    EXPECT_EQ(edges[4].control, (GamepadControl{GamepadControlKind::kAxisPositive, 5})) << "the right trigger";
    EXPECT_EQ(edges[5].control, (GamepadControl{GamepadControlKind::kHatUp, 0}));
    EXPECT_EQ(edges[6].control, (GamepadControl{GamepadControlKind::kHatLeft, 0}));

    std::uint16_t every = 0;
    for (const std::uint16_t bit : xinput::kButtonOrder)
    {
        EXPECT_EQ(every & bit, 0) << "each button once";
        every = static_cast<std::uint16_t>(every | bit);
    }
    EXPECT_EQ(every & (xinput::kDpadUp | xinput::kDpadDown | xinput::kDpadLeft | xinput::kDpadRight), 0)
        << "the cross is the hat's, not buttons";
}

TEST(EvdevLayout, AControllerHasAControllersButtonsOrXAndYAndNothingOfAPointer)
{
    EXPECT_TRUE(evdev::IsController(XpadCapabilities()));

    evdev::EvdevCapabilities joystick;
    joystick.keys.set(evdev::kButtonJoystick);
    EXPECT_TRUE(evdev::IsController(joystick)) << "a trigger button alone";

    evdev::EvdevCapabilities extra;
    extra.keys.set(0x2C5);
    EXPECT_TRUE(evdev::IsController(extra)) << "a BTN_TRIGGER_HAPPY button";

    evdev::EvdevCapabilities stick;
    stick.absolutes.set(evdev::kAbsoluteX);
    stick.absolutes.set(evdev::kAbsoluteY);
    EXPECT_TRUE(evdev::IsController(stick)) << "X and Y and nothing else";

    evdev::EvdevCapabilities touchpad = stick;
    touchpad.keys.set(evdev::kButtonTouch);
    touchpad.keys.set(evdev::kButtonToolFinger);
    EXPECT_FALSE(evdev::IsController(touchpad));

    evdev::EvdevCapabilities tablet = stick;
    tablet.keys.set(evdev::kButtonDigitiser);
    tablet.keys.set(evdev::kButtonStylus);
    EXPECT_FALSE(evdev::IsController(tablet));

    evdev::EvdevCapabilities mouse = stick;
    mouse.keys.set(evdev::kButtonLeft);
    EXPECT_FALSE(evdev::IsController(mouse));

    evdev::EvdevCapabilities screen = stick;
    screen.properties.set(evdev::kPropertyDirect);
    EXPECT_FALSE(evdev::IsController(screen));

    evdev::EvdevCapabilities motion = XpadCapabilities();
    motion.properties.set(evdev::kPropertyAccelerometer);
    EXPECT_FALSE(evdev::IsController(motion)) << "a pad's motion sensor, though it has a pad's codes";

    evdev::EvdevCapabilities keyboard;
    keyboard.keys.set(30);
    keyboard.keys.set(0x160);
    EXPECT_FALSE(evdev::IsController(keyboard));
    EXPECT_FALSE(evdev::IsController(evdev::EvdevCapabilities{}));
}

TEST(EvdevLayout, NodesAreNumberedByTheirNames)
{
    static_assert(evdev::NodeNumber("event0") == 0);
    static_assert(evdev::NodeNumber("event12") == 12);
    static_assert(evdev::NodeNumber("event") == -1);
    static_assert(evdev::NodeNumber("mouse0") == -1);
    static_assert(evdev::NodeNumber("event1a") == -1);
    static_assert(evdev::NodeNumber("js0") == -1);
    static_assert(evdev::NodeNumber("event99999999999") == -1, "too long to be a node");
}

TEST(EvdevLayout, CodesBecomeIndicesInTheOrderLinuxNumbersThem)
{
    evdev::EvdevCapabilities capabilities = XpadCapabilities();
    capabilities.keys.set(evdev::kButtonMisc);         // BTN_0, after the joystick range
    capabilities.keys.set(evdev::kButtonTriggerHappy); // after the game pad's buttons
    capabilities.absolutes.set(0x2F);                  // ABS_MT_SLOT: not an axis
    capabilities.unique_id = "00:11:22:33:44:55";
    const evdev::EvdevLayout layout(capabilities);
    const auto               gamepad = layout.Layout(capabilities);
    EXPECT_EQ(gamepad.name, "Microsoft X-Box 360 pad");
    EXPECT_EQ(gamepad.stable_id, "045e:028e/00:11:22:33:44:55@usb-0000:00:14.0-2/input0");
    EXPECT_EQ(gamepad.button_count, 13);
    ASSERT_EQ(gamepad.axes.size(), 6u);
    EXPECT_EQ(gamepad.axes[2].minimum, 0) << "Z, the left trigger, third";
    EXPECT_EQ(gamepad.axes[2].maximum, 255);
    EXPECT_EQ(gamepad.hat_count, 1);

    reckless::input::GamepadDevice device(5, gamepad, kThresholds);
    std::vector<GamepadEdge>       edges;
    layout.Apply(Event(evdev::kKey, 0x130, 1), device, edges); // BTN_SOUTH
    layout.Apply(Event(evdev::kKey, 0x2C0, 1), device, edges); // BTN_TRIGGER_HAPPY1
    layout.Apply(Event(evdev::kKey, 0x100, 1), device, edges); // BTN_0
    ASSERT_EQ(edges.size(), 3u);
    EXPECT_EQ(edges[0].control, (GamepadControl{GamepadControlKind::kButton, 0}));
    EXPECT_EQ(edges[1].control, (GamepadControl{GamepadControlKind::kButton, 11}));
    EXPECT_EQ(edges[2].control, (GamepadControl{GamepadControlKind::kButton, 12})) << "BTN_MISC's come last";
}

TEST(EvdevLayout, EventsAreReadingsOfButtonsAxesAndHats)
{
    const evdev::EvdevCapabilities capabilities = XpadCapabilities();
    const evdev::EvdevLayout       layout(capabilities);
    reckless::input::GamepadDevice device(5, layout.Layout(capabilities), kThresholds);
    std::vector<GamepadEdge>       edges;
    layout.ApplySnapshot(evdev::EvdevLayout::ReadingsOf(capabilities), device, edges);
    EXPECT_TRUE(edges.empty()) << "a pad at rest, its triggers at the bottom";
    EXPECT_EQ(device.axes()[2].rest, GamepadAxisRest::kMinimum);
    EXPECT_EQ(device.axes()[0].rest, GamepadAxisRest::kCentre);

    layout.Apply(Event(evdev::kKey, 0x131, 1), device, edges); // BTN_EAST down
    layout.Apply(Event(evdev::kKey, 0x131, 2), device, edges); // the kernel's auto-repeat
    layout.Apply(Event(evdev::kKey, 0x131, 0), device, edges); // let go
    ASSERT_EQ(edges.size(), 2u) << "a repeat is not another down";
    EXPECT_EQ(edges[0].control, (GamepadControl{GamepadControlKind::kButton, 1}));
    EXPECT_FALSE(edges[1].is_down);
    edges.clear();

    layout.Apply(Event(evdev::kAbsolute, 0x00, -30000), device, edges); // X left
    layout.Apply(Event(evdev::kAbsolute, 0x05, 255), device, edges);    // RZ, the right trigger
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0].control, (GamepadControl{GamepadControlKind::kAxisNegative, 0}));
    EXPECT_EQ(edges[1].control, (GamepadControl{GamepadControlKind::kAxisPositive, 5}));
    edges.clear();

    layout.Apply(Event(evdev::kAbsolute, 0x10, 1), device, edges);  // HAT0X right
    layout.Apply(Event(evdev::kAbsolute, 0x11, -1), device, edges); // HAT0Y up
    layout.Apply(Event(evdev::kAbsolute, 0x10, 0), device, edges);  // HAT0X back
    ASSERT_EQ(edges.size(), 3u);
    EXPECT_EQ(edges[0].control, (GamepadControl{GamepadControlKind::kHatRight, 0}));
    EXPECT_EQ(edges[1].control, (GamepadControl{GamepadControlKind::kHatUp, 0})) << "the other code keeps right";
    EXPECT_EQ(edges[2].control, (GamepadControl{GamepadControlKind::kHatRight, 0}));
    EXPECT_FALSE(edges[2].is_down);
    EXPECT_EQ(device.Hat(0), Hat(true, false, false, false));
    edges.clear();

    layout.Apply(Event(evdev::kKey, 0x120, 1), device, edges);       // a code the pad does not have
    layout.Apply(Event(evdev::kAbsolute, 0x12, 1), device, edges);   // HAT1X, which it does not have
    layout.Apply(Event(evdev::kAbsolute, 0x07, 100), device, edges); // ABS_RUDDER, which it does not have
    layout.Apply(Event(evdev::kSynchronise, 0, 0), device, edges);
    layout.Apply(Event(0x04, 0x04, 9), device, edges);             // EV_MSC's scan code
    layout.Apply(Event(evdev::kKey, 0x400, 1), device, edges);     // past every key
    layout.Apply(Event(evdev::kAbsolute, 0x40, 1), device, edges); // past every axis
    EXPECT_TRUE(edges.empty()) << "nothing the pad does not have is a reading";
}

TEST(EvdevLayout, ASnapshotSaysWhatIsDownAndWhereEverythingIs)
{
    const evdev::EvdevCapabilities capabilities = XpadCapabilities();
    const evdev::EvdevLayout       layout(capabilities);
    reckless::input::GamepadDevice device(5, layout.Layout(capabilities), kThresholds);
    std::vector<GamepadEdge>       edges;
    layout.ApplySnapshot(evdev::EvdevLayout::ReadingsOf(capabilities), device, edges);
    evdev::EvdevSnapshot snapshot = evdev::EvdevLayout::ReadingsOf(capabilities);
    snapshot.keys.set(0x13B);               // BTN_START, button 7
    snapshot.absolute_values[0x01] = 32767; // Y down
    snapshot.absolute_values[0x11] = 1;     // HAT0Y down
    snapshot.absolute_values[0x10] = -1;    // HAT0X left
    layout.ApplySnapshot(snapshot, device, edges);
    ASSERT_EQ(edges.size(), 4u);
    EXPECT_EQ(edges[0].control, (GamepadControl{GamepadControlKind::kButton, 7}));
    EXPECT_EQ(edges[1].control, (GamepadControl{GamepadControlKind::kAxisPositive, 1}));
    EXPECT_EQ(edges[2].control, (GamepadControl{GamepadControlKind::kHatDown, 0}));
    EXPECT_EQ(edges[3].control, (GamepadControl{GamepadControlKind::kHatLeft, 0}));
    EXPECT_EQ(device.Hat(0), Hat(false, false, true, true));
}

TEST(EvdevLayout, AStickHeldAtAnEndAsThePadAppearsIsTakenToRestThere)
{
    // The limit docs/input/gamepads.md names: an axis's rest is judged from where it is as the device appears, so a
    // stick a person holds hard over at that moment is taken for a trigger resting at that end. It holds nothing down
    // - no control is left stuck - and has the one control moving away from that end gives.
    const evdev::EvdevCapabilities capabilities = XpadCapabilities();
    const evdev::EvdevLayout       layout(capabilities);
    reckless::input::GamepadDevice device(5, layout.Layout(capabilities), kThresholds);
    std::vector<GamepadEdge>       edges;
    evdev::EvdevSnapshot           snapshot = evdev::EvdevLayout::ReadingsOf(capabilities);
    snapshot.absolute_values[0x01]          = 32767;
    layout.ApplySnapshot(snapshot, device, edges);
    EXPECT_TRUE(edges.empty());
    EXPECT_EQ(device.axes()[1].rest, GamepadAxisRest::kMaximum);
    layout.Apply(Event(evdev::kAbsolute, 0x01, -32768), device, edges);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].control, (GamepadControl{GamepadControlKind::kAxisNegative, 1}));
}
