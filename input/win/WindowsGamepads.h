#pragma once

// The Windows back end (WindowsGamepads.cpp): XInput's four controllers, and every other joystick and game pad
// through Windows' HID API, polled - a program on the toolkit has no window of its own to hand a raw input
// registration, and a poll a frame is what Gamepads asks for anyway.

#include <memory>

#include "Gamepads.h"

namespace reckless::input
{
/** The controllers Windows has: XInput's, and every other HID joystick, game pad and multi-axis controller.
    @param axis_thresholds  where an axis's controls go down and are let go
    @return                 the controllers; never null */
std::unique_ptr<Gamepads> MakeWindowsGamepads(const GamepadAxisThresholds &axis_thresholds);
} // namespace reckless::input
