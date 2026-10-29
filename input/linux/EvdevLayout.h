#pragma once

// The Linux kernel's event interface (evdev) read as controllers, with none of the kernel's headers: which of a
// device's codes it has makes it a controller or not, and gives each button, axis and hat its index, and each event
// read from it is a reading of one of them. The numbers are the kernel's own, from linux/input-event-codes.h, which is
// part of the kernel's interface to programs and so never changes them; input/linux/EvdevSystemPort.cpp, the one
// translation unit that includes the kernel's headers, holds every one of them to the kernel's with a static_assert.
// Everything else here is held by the suites on every platform.

#include <array>
#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "GamepadState.h"

namespace reckless::input::evdev
{
inline constexpr std::uint16_t kSynchronise        = 0x00;  ///< EV_SYN: the end of a frame, or a frame dropped
inline constexpr std::uint16_t kKey                = 0x01;  ///< EV_KEY: a key or button
inline constexpr std::uint16_t kAbsolute           = 0x03;  ///< EV_ABS: an absolute axis
inline constexpr std::uint16_t kSynchroniseReport  = 0;     ///< SYN_REPORT: the end of a frame
inline constexpr std::uint16_t kSynchroniseDropped = 3;     ///< SYN_DROPPED: the kernel's buffer overran
inline constexpr std::size_t   kKeyCount           = 0x300; ///< KEY_CNT: every key and button code is under it
inline constexpr std::size_t   kAbsoluteCount      = 0x40;  ///< ABS_CNT: every absolute axis code is under it
inline constexpr std::size_t   kPropertyCount      = 0x20;  ///< INPUT_PROP_CNT: every property is under it
inline constexpr std::uint16_t kButtonMisc         = 0x100; ///< BTN_MISC: the first of the buttons
inline constexpr std::uint16_t kButtonLeft         = 0x110; ///< BTN_LEFT: a mouse's left button
inline constexpr std::uint16_t kButtonJoystick     = 0x120; ///< BTN_JOYSTICK (BTN_TRIGGER): a joystick's first button
inline constexpr std::uint16_t kButtonGamepad      = 0x130; ///< BTN_GAMEPAD (BTN_SOUTH): a game pad's first button
inline constexpr std::uint16_t kButtonDigitiser    = 0x140; ///< BTN_DIGI (BTN_TOOL_PEN): the first of a tablet's codes
inline constexpr std::uint16_t kButtonToolFinger   = 0x145; ///< BTN_TOOL_FINGER: a touchpad's finger
inline constexpr std::uint16_t kButtonTouch        = 0x14A; ///< BTN_TOUCH: a touch surface touched
inline constexpr std::uint16_t kButtonStylus       = 0x14B; ///< BTN_STYLUS: a pen's button
inline constexpr std::uint16_t kButtonTriggerHappy = 0x2C0; ///< BTN_TRIGGER_HAPPY1: the first of forty more buttons
inline constexpr std::uint16_t kButtonTriggerHappyEnd = 0x2E8; ///< one past BTN_TRIGGER_HAPPY40
inline constexpr std::uint16_t kAbsoluteX             = 0x00;  ///< ABS_X
inline constexpr std::uint16_t kAbsoluteY             = 0x01;  ///< ABS_Y
inline constexpr std::uint16_t kAbsoluteHat0X         = 0x10;  ///< ABS_HAT0X: the first hat's left and right
inline constexpr std::uint16_t kAbsoluteHat3Y   = 0x17; ///< ABS_HAT3Y: the fourth hat's up and down, the last hat code
inline constexpr std::uint16_t kAbsoluteMisc    = 0x28; ///< ABS_MISC: the last code taken as an axis
inline constexpr std::size_t   kPropertyPointer = 0x00; ///< INPUT_PROP_POINTER: moves a pointer
inline constexpr std::size_t   kPropertyDirect  = 0x01; ///< INPUT_PROP_DIRECT: a touch screen or a tablet's surface
inline constexpr std::size_t   kPropertyAccelerometer = 0x06; ///< INPUT_PROP_ACCELEROMETER: a motion sensor's axes
inline constexpr int           kHatCount              = 4;    ///< how many hats the kernel has codes for

/** One event as the kernel gives it (struct input_event, without its time stamp). */
struct EvdevEvent
{
    std::uint16_t type{0};  ///< EV_SYN, EV_KEY, EV_ABS or another
    std::uint16_t code{0};  ///< which key, button or axis
    std::int32_t  value{0}; ///< a key's 0 (up), 1 (down) or 2 (auto-repeat); an axis's reading
};

/** An absolute axis's range and reading as the kernel gives them (struct input_absinfo, in part). */
struct EvdevAbsoluteInfo
{
    std::int32_t value{0};   ///< the reading
    std::int32_t minimum{0}; ///< the lowest reading
    std::int32_t maximum{0}; ///< the highest reading
};

/** What a device node says of itself as it is opened: its codes, its properties and its names. */
struct EvdevCapabilities
{
    std::bitset<kKeyCount>                        keys;            ///< its key and button codes (EVIOCGBIT EV_KEY)
    std::bitset<kAbsoluteCount>                   absolutes;       ///< its absolute axis codes (EVIOCGBIT EV_ABS)
    std::bitset<kPropertyCount>                   properties;      ///< its properties (EVIOCGPROP)
    std::array<EvdevAbsoluteInfo, kAbsoluteCount> absolute_info{}; ///< each axis's range and reading (EVIOCGABS)
    std::uint16_t                                 vendor_id{0};    ///< its USB vendor id (EVIOCGID)
    std::uint16_t                                 product_id{0};   ///< its USB product id (EVIOCGID)
    std::string                                   name;            ///< what it calls itself (EVIOCGNAME)
    std::string unique_id;     ///< its unique id where it has one: a serial number, a Bluetooth address (EVIOCGUNIQ)
    std::string physical_path; ///< where it is plugged in: "usb-0000:00:14.0-2/input0" (EVIOCGPHYS)
};

/** What a device holds now, read again after the kernel dropped events (a SYN_DROPPED). */
struct EvdevSnapshot
{
    std::bitset<kKeyCount>                   keys;              ///< the keys and buttons down (EVIOCGKEY)
    std::array<std::int32_t, kAbsoluteCount> absolute_values{}; ///< every axis's reading (EVIOCGABS)
};

/** Whether a key code is one of a joystick's or a game pad's buttons: BTN_JOYSTICK to the last of BTN_GAMEPAD's, or
    one of the forty BTN_TRIGGER_HAPPY buttons.
    @param code  the key code
    @return      true for a controller's button */
constexpr bool IsControllerButton(std::size_t code) noexcept
{
    return (code >= kButtonJoystick && code < kButtonDigitiser) ||
           (code >= kButtonTriggerHappy && code < kButtonTriggerHappyEnd);
}

/** Whether a device node is a controller, as udev's input_id judges one it tags ID_INPUT_JOYSTICK: one with a
    controller's buttons, or with X and Y axes and nothing of a pointer, a touch surface or a pen. A motion sensor -
    the node a game pad's accelerometer is given beside the pad's own - is never one, though it has X and Y.
    @param capabilities  what the node said of itself
    @return              true for a controller */
inline bool IsController(const EvdevCapabilities &capabilities)
{
    if (capabilities.properties.test(kPropertyAccelerometer))
    {
        return false;
    }
    for (std::size_t code = kButtonJoystick; code < kKeyCount; ++code)
    {
        if (capabilities.keys.test(code) && IsControllerButton(code))
        {
            return true;
        }
    }
    const bool has_x_and_y  = capabilities.absolutes.test(kAbsoluteX) && capabilities.absolutes.test(kAbsoluteY);
    const bool is_pointing  = capabilities.properties.test(kPropertyPointer) ||
                              capabilities.properties.test(kPropertyDirect) || capabilities.keys.test(kButtonLeft);
    const bool is_touch_pen = capabilities.keys.test(kButtonTouch) || capabilities.keys.test(kButtonToolFinger) ||
                              capabilities.keys.test(kButtonStylus) || capabilities.keys.test(kButtonDigitiser);
    return has_x_and_y && !is_pointing && !is_touch_pen;
}

/** Whether an absolute axis code is one of the hats' (ABS_HAT0X to ABS_HAT3Y).
    @param code  the axis code
    @return      true for a hat's */
constexpr bool IsHatCode(std::size_t code) noexcept
{
    return code >= kAbsoluteHat0X && code <= kAbsoluteHat3Y;
}

/** Whether an absolute axis code is taken as an axis: every code up to ABS_MISC that is not a hat's. Past it are a
    touch surface's slots and contacts, which no controller has.
    @param code  the axis code
    @return      true for an axis */
constexpr bool IsAxisCode(std::size_t code) noexcept
{
    return code <= kAbsoluteMisc && !IsHatCode(code);
}

/** A device node's number, from its name: 12 for "event12". Nodes are opened in this order, so devices present when
    the program starts appear in the order the kernel made them.
    @param name  the node's file name
    @return      its number; -1 for a name that is not "event" and a number */
constexpr long NodeNumber(std::string_view name) noexcept
{
    constexpr std::string_view kPrefix = "event";
    if (name.size() <= kPrefix.size() || name.substr(0, kPrefix.size()) != kPrefix)
    {
        return -1;
    }
    long number = 0;
    for (const char digit : name.substr(kPrefix.size()))
    {
        if (digit < '0' || digit > '9' || number > 100000000)
        {
            return -1;
        }
        number = number * 10 + (digit - '0');
    }
    return number;
}

/**
    Where each of a controller's codes goes: the index of each button, axis and hat, made once as the device is
    opened. The buttons are its codes from BTN_JOYSTICK up, then those from BTN_MISC to BTN_JOYSTICK, in code order -
    so a game pad's south button and a joystick's trigger are each button 0, as other programs on Linux number them;
    the axes are its absolute codes in code order, the hats' left out; a hat is there when either of its two codes is.
*/
class EvdevLayout
{
  public:
    /** The layout of a controller.
        @param capabilities  what the node said of itself */
    explicit EvdevLayout(const EvdevCapabilities &capabilities)
    {
        button_of_code_.fill(-1);
        axis_of_code_.fill(-1);
        for (std::size_t code = kButtonJoystick; code < kKeyCount; ++code)
        {
            AddButton(capabilities, code);
        }
        for (std::size_t code = kButtonMisc; code < kButtonJoystick; ++code)
        {
            AddButton(capabilities, code);
        }
        for (std::size_t code = 0; code < kAbsoluteCount; ++code)
        {
            if (capabilities.absolutes.test(code) && IsAxisCode(code))
            {
                axis_of_code_[code] = static_cast<std::int16_t>(axes_.size());
                axes_.push_back(GamepadAxisRange{capabilities.absolute_info[code].minimum,
                                                 capabilities.absolute_info[code].maximum,
                                                 GamepadAxisRest::kFirstReading});
                axis_codes_.push_back(static_cast<std::uint16_t>(code));
            }
        }
        for (int hat = 0; hat < kHatCount; ++hat)
        {
            const std::size_t x_code = kAbsoluteHat0X + 2u * static_cast<std::size_t>(hat);
            if (capabilities.absolutes.test(x_code) || capabilities.absolutes.test(x_code + 1))
            {
                hat_codes_.push_back(static_cast<std::uint16_t>(x_code));
            }
        }
    }

    /** What the device is, for the roster: its name, its stable id, and its controls.
        @param capabilities  what the node said of itself
        @return              the layout */
    GamepadLayout Layout(const EvdevCapabilities &capabilities) const
    {
        std::string distinguisher = capabilities.unique_id;
        if (!capabilities.physical_path.empty())
        {
            distinguisher += "@" + capabilities.physical_path;
        }
        return GamepadLayout{capabilities.name,
                             GamepadStableId(capabilities.vendor_id, capabilities.product_id, distinguisher),
                             static_cast<int>(button_codes_.size()), axes_, static_cast<int>(hat_codes_.size())};
    }

    /** One event applied to the device: a button's, an axis's or a hat's reading. An event of any other type, or of a
        code the device does not have, is passed over.
        @param event   the event
        @param device  the device
        @param edges   where the edges it makes are appended */
    void Apply(const EvdevEvent &event, GamepadDevice &device, std::vector<GamepadEdge> &edges) const
    {
        if (event.type == kKey && event.code < kKeyCount)
        {
            device.SetButton(button_of_code_[event.code], event.value != 0, edges);
        }
        else if (event.type == kAbsolute && event.code < kAbsoluteCount)
        {
            if (IsHatCode(event.code))
            {
                ApplyHat(event.code, event.value, device, edges);
            }
            else
            {
                device.SetAxis(axis_of_code_[event.code], event.value, edges);
            }
        }
    }

    /** Everything a device holds now applied to it: what it held as it was opened (the capabilities' readings) or what
        it holds after the kernel dropped events.
        @param snapshot  the device's keys and readings
        @param device    the device
        @param edges     where the edges it makes are appended */
    void ApplySnapshot(const EvdevSnapshot &snapshot, GamepadDevice &device, std::vector<GamepadEdge> &edges) const
    {
        for (std::size_t button = 0; button < button_codes_.size(); ++button)
        {
            device.SetButton(static_cast<int>(button), snapshot.keys.test(button_codes_[button]), edges);
        }
        for (std::size_t axis = 0; axis < axis_codes_.size(); ++axis)
        {
            device.SetAxis(static_cast<int>(axis), snapshot.absolute_values[axis_codes_[axis]], edges);
        }
        for (std::size_t hat = 0; hat < hat_codes_.size(); ++hat)
        {
            const std::uint16_t x_code = hat_codes_[hat];
            device.SetHat(static_cast<int>(hat),
                          HatOf(snapshot.absolute_values[x_code], snapshot.absolute_values[x_code + 1u]), edges);
        }
    }

    /** A snapshot of a device's readings as it was opened, from its capabilities: no key down, since the capabilities
        carry none (the back end reads them with the snapshot proper), and every axis's reading.
        @param capabilities  what the node said of itself
        @return              its readings */
    static EvdevSnapshot ReadingsOf(const EvdevCapabilities &capabilities)
    {
        EvdevSnapshot snapshot;
        for (std::size_t code = 0; code < kAbsoluteCount; ++code)
        {
            snapshot.absolute_values[code] = capabilities.absolute_info[code].value;
        }
        return snapshot;
    }

  private:
    /** A key code given the next button index, if the device has it.
        @param capabilities  what the node said of itself
        @param code          the key code */
    void AddButton(const EvdevCapabilities &capabilities, std::size_t code)
    {
        if (capabilities.keys.test(code))
        {
            button_of_code_[code] = static_cast<std::int16_t>(button_codes_.size());
            button_codes_.push_back(static_cast<std::uint16_t>(code));
        }
    }

    /** Where a hat points from its two codes' readings: below zero is up or left, above zero down or right.
        @param x  its left and right reading
        @param y  its up and down reading
        @return   where it points */
    static constexpr GamepadHat HatOf(std::int32_t x, std::int32_t y) noexcept
    {
        return GamepadHat{y < 0, x > 0, y > 0, x < 0};
    }

    /** One of a hat's two codes read: the other direction kept as the device last said it.
        @param code    ABS_HATnX or ABS_HATnY
        @param value   the reading
        @param device  the device
        @param edges   where the edges it makes are appended */
    void ApplyHat(std::uint16_t code, std::int32_t value, GamepadDevice &device, std::vector<GamepadEdge> &edges) const
    {
        const std::uint16_t x_code = static_cast<std::uint16_t>(code - ((code - kAbsoluteHat0X) % 2u));
        for (std::size_t hat = 0; hat < hat_codes_.size(); ++hat)
        {
            if (hat_codes_[hat] == x_code)
            {
                GamepadHat direction = device.Hat(static_cast<int>(hat));
                if (code == x_code)
                {
                    direction.is_left  = value < 0;
                    direction.is_right = value > 0;
                }
                else
                {
                    direction.is_up   = value < 0;
                    direction.is_down = value > 0;
                }
                device.SetHat(static_cast<int>(hat), direction, edges);
                return;
            }
        }
    }

    std::array<std::int16_t, kKeyCount>      button_of_code_{}; ///< each key code's button index; -1 for none
    std::array<std::int16_t, kAbsoluteCount> axis_of_code_{};   ///< each axis code's axis index; -1 for none
    std::vector<std::uint16_t>               button_codes_;     ///< each button's key code, by index
    std::vector<std::uint16_t>               axis_codes_;       ///< each axis's code, by index
    std::vector<GamepadAxisRange>            axes_;             ///< each axis's range, by index
    std::vector<std::uint16_t>               hat_codes_;        ///< each hat's ABS_HATnX code, by index
};
} // namespace reckless::input::evdev
