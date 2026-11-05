#pragma once

// The kernel's own event types beside the back end's (input/linux/EvdevLayout.h): every number that header copies
// held to the kernel's, and a kernel event in the back end's words. Linux only - it includes the kernel's headers -
// so it is included by the port on the kernel (EvdevSystemPort.cpp) and by the Linux suite, and nowhere else.

#include <cstdint>

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

// The kernel's names for an event's seconds and microseconds since Linux 4.16, which keep struct input_event the same
// size on a 32-bit machine past 2038; a kernel's headers older than that name only the timeval.
#ifndef input_event_sec
#define input_event_sec  time.tv_sec
#define input_event_usec time.tv_usec
#endif

/** A kernel event in the back end's words, its time stamp with it where that is on the steady clock: a node whose
    clock the port has set to CLOCK_MONOTONIC (EVIOCSCLOCKID), which is the clock std::chrono::steady_clock reads on
    Linux in both libstdc++ and libc++. A node left on the kernel's default, CLOCK_REALTIME, stamps the wall clock,
    which a change of the time moves under it, and its time is left at 0 for the back end to stamp on reading.
    @param event              the event as read from a node
    @param is_time_monotonic  whether the node's events are stamped on CLOCK_MONOTONIC
    @return                   its type, code, value and, where it is on the steady clock, time */
inline EvdevEvent EvdevEventOf(const input_event &event, bool is_time_monotonic) noexcept
{
    const std::int64_t nanoseconds = static_cast<std::int64_t>(event.input_event_sec) * 1'000'000'000 +
                                     static_cast<std::int64_t>(event.input_event_usec) * 1'000;
    return EvdevEvent{event.type, event.code, event.value, is_time_monotonic ? nanoseconds : 0};
}
} // namespace reckless::input::evdev
