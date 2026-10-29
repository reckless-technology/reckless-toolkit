// The one place that knows which controller back end this build carries, and the one a program calls through: the
// back end is chosen here by hand, under the same definitions that compile it (CMakeLists.txt's
// reckless_input_devices), and deliberately not by self-registration - a registrar in a translation unit nothing
// references is stripped from a static library by the linker, as audio/DeviceBackends.h says at length.

#include "Gamepads.h"

#include <memory>

#if defined(RECKLESS_INPUT_EVDEV)
#include "linux/EvdevGamepads.h"
#endif
#if defined(RECKLESS_INPUT_WINDOWS)
#include "win/WindowsGamepads.h"
#endif
#if defined(RECKLESS_INPUT_IOKIT)
#include "mac/IoKitGamepads.h"
#endif

namespace reckless::input
{
std::unique_ptr<Gamepads> MakeGamepads(const GamepadAxisThresholds &axis_thresholds)
{
#if defined(RECKLESS_INPUT_EVDEV)
    return std::make_unique<evdev::EvdevGamepads>(evdev::MakeSystemEvdevPort(), axis_thresholds);
#elif defined(RECKLESS_INPUT_WINDOWS)
    return MakeWindowsGamepads(axis_thresholds);
#elif defined(RECKLESS_INPUT_IOKIT)
    return MakeIoKitGamepads(axis_thresholds);
#else
    static_cast<void>(axis_thresholds);
    return std::make_unique<NoGamepads>();
#endif
}
} // namespace reckless::input
