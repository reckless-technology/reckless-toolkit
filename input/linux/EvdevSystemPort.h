#pragma once

// The kernel's own event types beside the back end's (input/linux/EvdevLayout.h): every number that header copies
// held to the kernel's, and a kernel event in the back end's words. Linux only - it includes the kernel's headers -
// so it is included by the port on the kernel (EvdevSystemPort.cpp) and by the Linux suite, and nowhere else.

#include <linux/input.h>

#include "EvdevLayout.h"

namespace reckless::input::evdev
{
static_assert(kSynchronise == EV_SYN && kKey == EV_KEY && kAbsolute == EV_ABS);
static_assert(kSynchroniseReport == SYN_REPORT && kSynchroniseDropped == SYN_DROPPED);
static_assert(kKeyCount == KEY_CNT && kAbsoluteCount == ABS_CNT && kPropertyCount == INPUT_PROP_CNT);
static_assert(kButtonMisc == BTN_MISC && kButtonLeft == BTN_LEFT && kButtonJoystick == BTN_JOYSTICK);
static_assert(kButtonGamepad == BTN_GAMEPAD && kButtonDigitiser == BTN_DIGI && kButtonToolFinger == BTN_TOOL_FINGER);
static_assert(kButtonTouch == BTN_TOUCH && kButtonStylus == BTN_STYLUS);
static_assert(kButtonTriggerHappy == BTN_TRIGGER_HAPPY1 && kButtonTriggerHappyEnd == BTN_TRIGGER_HAPPY40 + 1);
static_assert(kAbsoluteX == ABS_X && kAbsoluteY == ABS_Y && kAbsoluteMisc == ABS_MISC);
static_assert(kAbsoluteHat0X == ABS_HAT0X && kAbsoluteHat3Y == ABS_HAT3Y);
static_assert(kPropertyPointer == INPUT_PROP_POINTER && kPropertyDirect == INPUT_PROP_DIRECT);
static_assert(kPropertyAccelerometer == INPUT_PROP_ACCELEROMETER);

/** A kernel event in the back end's words, its time stamp left behind.
    @param event  the event as read from a node
    @return       its type, code and value */
inline EvdevEvent EvdevEventOf(const input_event &event) noexcept
{
    return EvdevEvent{event.type, event.code, event.value};
}
} // namespace reckless::input::evdev
