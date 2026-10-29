#pragma once

// What a USB HID controller's report descriptor says, read the way the Windows and macOS back ends both read it: which
// collections are controllers, which of their values are axes and which are hats, a logical range a careless
// descriptor got wrong, a value's sign, and where a hat switch points. The numbers are the HID Usage Tables' own
// (version 1.5: the Generic Desktop page 0x01, the Simulation Controls page 0x02, the Button page 0x09), so none of
// either platform's headers is needed and the suites hold all of it on every platform.

#include <cstdint>

#include "GamepadState.h"

namespace reckless::input::hid
{
inline constexpr std::uint16_t kGenericDesktopPage = 0x01; ///< the Generic Desktop usage page
inline constexpr std::uint16_t kSimulationPage     = 0x02; ///< the Simulation Controls usage page
inline constexpr std::uint16_t kButtonPage         = 0x09; ///< the Button usage page: usage n is button n
inline constexpr std::uint16_t kJoystickUsage      = 0x04; ///< Generic Desktop: a joystick's application collection
inline constexpr std::uint16_t kGamePadUsage       = 0x05; ///< Generic Desktop: a game pad's application collection
inline constexpr std::uint16_t kMultiAxisUsage     = 0x08; ///< Generic Desktop: a multi-axis controller's collection
inline constexpr std::uint16_t kXUsage             = 0x30; ///< Generic Desktop: X, the first of the axes
inline constexpr std::uint16_t kWheelUsage         = 0x38; ///< Generic Desktop: Wheel, the last of the axes
inline constexpr std::uint16_t kHatSwitchUsage     = 0x39; ///< Generic Desktop: a hat switch
inline constexpr std::uint16_t kRudderUsage        = 0xBA; ///< Simulation: Rudder
inline constexpr std::uint16_t kThrottleUsage      = 0xBB; ///< Simulation: Throttle
inline constexpr std::uint16_t kAcceleratorUsage   = 0xC4; ///< Simulation: Accelerator
inline constexpr std::uint16_t kBrakeUsage         = 0xC5; ///< Simulation: Brake
inline constexpr std::uint16_t kSteeringUsage      = 0xC8; ///< Simulation: Steering

/** Whether a top-level collection is a controller this layer reads: a joystick, a game pad or a multi-axis controller.
    @param usage_page  the collection's usage page
    @param usage       its usage
    @return            true for a controller */
constexpr bool IsControllerCollection(std::uint16_t usage_page, std::uint16_t usage) noexcept
{
    return usage_page == kGenericDesktopPage &&
           (usage == kJoystickUsage || usage == kGamePadUsage || usage == kMultiAxisUsage);
}

/** What a value of a controller's report is to this layer. */
enum class ValueRole : std::uint8_t
{
    kNone, ///< nothing this layer reads: a vendor's value, a battery level, a motion sensor's axis
    kAxis, ///< an axis: X, Y, Z, Rx, Ry, Rz, a slider, a dial, a wheel, or a simulation control
    kHat,  ///< a hat switch
};

/** What a value of a controller's report is, by its usage.
    @param usage_page  the value's usage page
    @param usage       its usage
    @return            its role */
constexpr ValueRole RoleOfValue(std::uint16_t usage_page, std::uint16_t usage) noexcept
{
    if (usage_page == kGenericDesktopPage)
    {
        if (usage >= kXUsage && usage <= kWheelUsage)
        {
            return ValueRole::kAxis;
        }
        return usage == kHatSwitchUsage ? ValueRole::kHat : ValueRole::kNone;
    }
    if (usage_page == kSimulationPage)
    {
        const bool is_axis = usage == kRudderUsage || usage == kThrottleUsage || usage == kAcceleratorUsage ||
                             usage == kBrakeUsage || usage == kSteeringUsage;
        return is_axis ? ValueRole::kAxis : ValueRole::kNone;
    }
    return ValueRole::kNone;
}

/** A logical range as it was meant. A descriptor that gives an unsigned field's maximum in a byte too short for it -
    255 as the one byte 0xFF, which the item's rules read as -1 - leaves a maximum under its minimum; such a range is
    the field's whole unsigned range instead, as every driver that reads such devices takes it.
    @param minimum   the logical minimum the descriptor gave
    @param maximum   the logical maximum it gave
    @param bit_size  the field's size in bits
    @return          the range, its rest left to be judged */
constexpr GamepadAxisRange RepairedLogicalRange(std::int32_t minimum, std::int32_t maximum, int bit_size) noexcept
{
    if (maximum >= minimum || minimum < 0 || bit_size <= 0 || bit_size > 31)
    {
        return GamepadAxisRange{minimum, maximum, GamepadAxisRest::kFirstReading};
    }
    return GamepadAxisRange{minimum, static_cast<std::int32_t>((std::uint32_t{1} << bit_size) - 1u),
                            GamepadAxisRest::kFirstReading};
}

/** A field's bits as the number its descriptor means: sign-extended from its top bit where the logical minimum is
    negative, as the bits are then two's complement, and taken as they are otherwise.
    @param raw              the field's bits, low-aligned
    @param bit_size         the field's size in bits
    @param logical_minimum  the field's logical minimum
    @return                 the value */
constexpr std::int32_t SignedValue(std::uint32_t raw, int bit_size, std::int32_t logical_minimum) noexcept
{
    if (logical_minimum >= 0 || bit_size <= 0 || bit_size >= 32)
    {
        return static_cast<std::int32_t>(raw);
    }
    const std::uint32_t mask     = (std::uint32_t{1} << bit_size) - 1u;
    const std::uint32_t sign_bit = std::uint32_t{1} << (bit_size - 1);
    const std::uint32_t bits     = raw & mask;
    return (bits & sign_bit) != 0 ? static_cast<std::int32_t>(bits) - static_cast<std::int32_t>(mask) - 1
                                  : static_cast<std::int32_t>(bits);
}

/** Where a hat switch points. A hat of eight positions counts them clockwise from up - up, up and right, right, down
    and right, down, down and left, left, up and left - from its logical minimum; one of four counts up, right, down
    and left. Any value outside its logical range is the hat centred, which is how a hat says it is: its null state.
    @param value            the hat's value
    @param logical_minimum  its logical minimum, the value for up
    @param logical_maximum  its logical maximum
    @return                 where it points; centred for a value outside the range or a hat neither of eight
                            positions nor of four */
constexpr GamepadHat HatOfSwitch(std::int32_t value, std::int32_t logical_minimum,
                                 std::int32_t logical_maximum) noexcept
{
    const std::int64_t positions = static_cast<std::int64_t>(logical_maximum) - logical_minimum + 1;
    if (value < logical_minimum || value > logical_maximum || (positions != 8 && positions != 4))
    {
        return GamepadHat{};
    }
    // As a position of eight: a hat of four steps two at a time.
    const std::int64_t eighth = (static_cast<std::int64_t>(value) - logical_minimum) * (positions == 4 ? 2 : 1);
    return GamepadHat{eighth == 7 || eighth <= 1, eighth >= 1 && eighth <= 3, eighth >= 3 && eighth <= 5,
                      eighth >= 5 && eighth <= 7};
}
} // namespace reckless::input::hid
