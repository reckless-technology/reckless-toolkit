#pragma once

// The controllers a program reads beside the keyboard - gamepads, joysticks, and the arcade encoders that present
// themselves to the machine as a USB joystick - as the toolkit gives them to a program: every control of a device a
// digital one, and every change of one an edge. A button is one control; an axis is two, its negative and its positive
// half, each down past a threshold and up again under a lower one; a hat is four. That is the whole of what a device
// says here, whatever the platform said: how far a stick is pushed is turned into those edges on the way in, and what
// a control means - which action a program binds it to, and where it keeps that binding - is the program's business,
// not the toolkit's (docs/input/gamepads.md).
//
// Everything here needs no platform's headers. MakeGamepads, the one function a program calls to reach the platform,
// is compiled into reckless_input_devices with the back ends this build carries (input/Gamepads.cpp); the rest is
// header only, in reckless_common_input. What turns a platform's readings into edges, and holds the promises above,
// is input/GamepadState.h, which no platform's headers reach either.

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace reckless::input
{
/** What a control of a device is: a button, one half of an axis, or one direction of a hat. */
enum class GamepadControlKind : std::uint8_t
{
    kNone,         ///< no control: what a control that has not been set is
    kButton,       ///< a button, down while it is pressed
    kAxisNegative, ///< an axis's negative half: down while it is pushed far enough below its centre (left, or up)
    kAxisPositive, ///< an axis's positive half: down while it is pushed far enough above its centre (right, or down)
    kHatUp,        ///< a hat pushed up, or up and to one side
    kHatDown,      ///< a hat pushed down, or down and to one side
    kHatLeft,      ///< a hat pushed left, or left and up or down
    kHatRight,     ///< a hat pushed right, or right and up or down
};

/**
    One digital control of a device - "Button 4", "Axis 1 −", "Hat 1 up" - by its kind and its index among the
    device's controls of that kind. The index counts from 0 and is the device's own: button 0 is the first button the
    platform lists for the device, axis 1 its second axis, hat 0 its first hat. Two controls are the same control when
    they are equal, and a control is only a control of one device: a program that keeps one keeps the device's
    GamepadInfo::stable_id beside it.
*/
struct GamepadControl
{
    GamepadControlKind kind{GamepadControlKind::kNone}; ///< what the control is; kNone for no control
    std::uint16_t      index{0}; ///< the button's, axis's or hat's index among the device's own, from 0

    /** Whether two controls are the same control.
        @param other  the other control
        @return       true when both kind and index are equal */
    constexpr bool operator==(const GamepadControl &other) const = default;
};

/** A control's name for a person, counting from 1 as a device's own labels do: "Button 4" for button index 3,
    "Axis 2 +" for axis index 1's positive half (the minus of a negative half is U+2212, "Axis 2 −"), "Hat 1 left"
    for hat index 0's left. English, as every name the toolkit gives a person is; a program that shows another
    language makes its own from the kind and the index.
    @param control  the control
    @return         its name, UTF-8; empty for a control of kind kNone */
inline std::string GamepadControlName(GamepadControl control)
{
    const std::string number = std::to_string(static_cast<unsigned>(control.index) + 1u);
    switch (control.kind)
    {
    case GamepadControlKind::kButton:
        return "Button " + number;
    case GamepadControlKind::kAxisNegative:
        return "Axis " + number + " −";
    case GamepadControlKind::kAxisPositive:
        return "Axis " + number + " +";
    case GamepadControlKind::kHatUp:
        return "Hat " + number + " up";
    case GamepadControlKind::kHatDown:
        return "Hat " + number + " down";
    case GamepadControlKind::kHatLeft:
        return "Hat " + number + " left";
    case GamepadControlKind::kHatRight:
        return "Hat " + number + " right";
    case GamepadControlKind::kNone:
        break;
    }
    return {};
}

/**
    One edge of one control: the control going down or being let go, and the device it belongs to. The edges of one
    control of one device alternate - every down is followed by exactly one up, and no up arrives for a control whose
    down did not - and a device that goes away lets go of everything it held, so a set of held controls built from
    these edges cannot go wrong.
*/
struct GamepadEdge
{
    std::uint32_t  device_id{0};   ///< the device, as GamepadInfo::device_id names it
    GamepadControl control{};      ///< the control
    bool           is_down{false}; ///< true for the control going down, false for it being let go
};

/** A device a Gamepads reads: what a program shows a person to choose from, and what it keeps to find the device
    again. */
struct GamepadInfo
{
    /** The device for as long as it is plugged in: unique for the life of the process (within one binary) and never
        given to another device, so a device unplugged and plugged in again is a new device with a new id. Never 0. */
    std::uint32_t device_id{0};

    std::string name; ///< what the device calls itself, UTF-8: "Generic USB Joystick", "Xbox Wireless Controller"

    /** What a settings file can keep to find this device again on a later launch: the vendor and product ids as four
        lower-case hexadecimal digits each ("0079:0006"), then a slash and what the platform gives to tell two devices
        of that model apart - their serial number or the port they are plugged into - where it gives anything. The
        part before the slash names the model, so a program that does not find the whole id can look for a device of
        the same model. The platform's own words, so the same device has a different id on each platform. A Windows
        XInput controller, of which XInput says neither vendor nor product, is "xinput/" and its slot, 0 to 3. */
    std::string stable_id;

    int button_count{0}; ///< how many buttons it has: its buttons are indices 0 to button_count - 1
    int axis_count{0};   ///< how many axes it has, each two controls
    int hat_count{0};    ///< how many hats it has, each four controls
};

/**
    How far an axis is pushed before its control goes down, and how far back it must come before the control is let
    go, in thousandths of the axis's travel from its rest to its end - half its whole range for a stick that rests in
    the middle. A control goes down where the axis is pushed `press_thousandths` or further, and is let go where it
    comes back under `release_thousandths`; the gap between the two is what keeps a stick resting near the threshold
    from chattering. These are a program's feel, so the program gives them; the toolkit has no default.

    A press beyond 1000 is taken as 1000 and one under 1 as 1; a release is held between 1 and the press, so a
    release above the press is the press itself, with no gap.
*/
struct GamepadAxisThresholds
{
    std::int32_t press_thousandths;   ///< how far the axis is pushed for its control to go down
    std::int32_t release_thousandths; ///< how far pushed under which a control that is down is let go
};

/** The thresholds as they are used: the press held between 1 and 1000, the release between 1 and the press.
    @param thresholds  the thresholds a program gave
    @return            the thresholds used */
constexpr GamepadAxisThresholds UsableThresholds(GamepadAxisThresholds thresholds) noexcept
{
    const std::int32_t press   = thresholds.press_thousandths < 1      ? 1
                                 : thresholds.press_thousandths > 1000 ? 1000
                                                                       : thresholds.press_thousandths;
    const std::int32_t release = thresholds.release_thousandths < 1       ? 1
                                 : thresholds.release_thousandths > press ? press
                                                                          : thresholds.release_thousandths;
    return GamepadAxisThresholds{press, release};
}

/**
    The controllers this machine has, read on one thread - the program's main thread, the one its window's frame
    callback runs on - and never another. A program makes one with MakeGamepads, calls Poll once a frame, and is told
    every edge since the last call; devices plugged in and pulled out are found by Poll too, so a program that polls
    sees a device come and go with nothing else to do.
*/
class Gamepads
{
  public:
    /** Lets go of every device: closes what it opened. No edge is told after this. */
    virtual ~Gamepads() = default;

    /** Every edge since the last call, appended to `edges` in the order they happened on each device, and every
        device plugged in or pulled out since then found: a new device's controls that are already down are told as
        downs, and a device gone lets go of every control it held, oldest first. Called once a frame; never blocks.
        @param edges  where the edges are appended; what it held is kept */
    virtual void Poll(std::vector<GamepadEdge> &edges) = 0;

    /** The devices as the last Poll left them - or, before the first, as they were found when this was made - in the
        order they were found.
        @return  the devices; empty when there are none */
    virtual std::vector<GamepadInfo> Devices() const = 0;
};

/** A Gamepads with no devices, ever: what MakeGamepads gives where the build carries no back end, and what a program's
    suite can give where it wants none. */
class NoGamepads final : public Gamepads
{
  public:
    /** Nothing: there is nothing to tell.
        @param edges  left as it was */
    void Poll(std::vector<GamepadEdge> &edges) override
    {
        static_cast<void>(edges);
    }

    /** No devices.
        @return  an empty list */
    std::vector<GamepadInfo> Devices() const override
    {
        return {};
    }
};

/** The platform's controllers: evdev on Linux, XInput and HID on Windows, the IOKit HID manager on macOS
    (docs/input/gamepads.md). Made on, and used only from, the thread that will poll it; on macOS that is the main
    thread, whose run loop delivers the devices and their values between polls.
    @param axis_thresholds  where an axis's controls go down and are let go; UsableThresholds says how it is held
    @return                 the controllers; never null - a NoGamepads where the build carries no back end */
std::unique_ptr<Gamepads> MakeGamepads(const GamepadAxisThresholds &axis_thresholds);
} // namespace reckless::input
