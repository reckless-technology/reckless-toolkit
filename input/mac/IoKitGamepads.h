#pragma once

// The macOS back end (IoKitGamepads.cpp): the IOKit HID manager, matching joysticks, game pads and multi-axis
// controllers, scheduled on the main run loop. Its callbacks only queue what they are told; Poll drains the queue.

#include <memory>

#include "Gamepads.h"

namespace reckless::input
{
/** The controllers macOS has: every HID joystick, game pad and multi-axis controller. Made on the main thread, whose
    run loop delivers the devices and their values: nothing arrives until that run loop has run.
    @param axis_thresholds  where an axis's controls go down and are let go
    @return                 the controllers; never null */
std::unique_ptr<Gamepads> MakeIoKitGamepads(const GamepadAxisThresholds &axis_thresholds);
} // namespace reckless::input
