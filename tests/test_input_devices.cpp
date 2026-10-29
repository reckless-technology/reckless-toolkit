// The controller back end this build carries, on this machine (MakeGamepads): made, polled and let go of whatever
// controllers there are or are not, its devices well formed and its edges alternating. On Linux also the kernel's own
// struct input_event in the back end's words, and - where /dev/uinput is writable and the node the kernel makes for a
// virtual device is readable - a controller of the suite's own made with uinput and read from its plugging in to its
// pulling out. Where either is not so, those cases skip and say why: a desk whose user has not been given uinput, a CI
// runner. The back end's every decision is held without a kernel by tests/test_evdev_gamepads.cpp.
#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include "Gamepads.h"

#if defined(__linux__)
#include <cerrno>
#include <cstring>
#include <string>

#include <fcntl.h>
#include <linux/uinput.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "linux/EvdevSystemPort.h"
#endif

namespace
{
using reckless::input::GamepadControl;
using reckless::input::GamepadControlKind;
using reckless::input::GamepadEdge;
using reckless::input::GamepadInfo;

/** Down at half the travel, let go under three tenths. */
constexpr reckless::input::GamepadAxisThresholds kThresholds{500, 300};

/** Whether every control's edges alternate, beginning with a down.
    @param edges  the edges
    @return       true when they do */
bool Alternates(const std::vector<GamepadEdge> &edges)
{
    std::set<std::pair<std::uint32_t, std::pair<int, int>>> down;
    for (const GamepadEdge &edge : edges)
    {
        const auto key = std::pair(
            edge.device_id, std::pair(static_cast<int>(edge.control.kind), static_cast<int>(edge.control.index)));
        if (edge.is_down == (down.count(key) != 0))
        {
            return false;
        }
        edge.is_down ? static_cast<void>(down.insert(key)) : static_cast<void>(down.erase(key));
    }
    return true;
}
} // namespace

TEST(MakeGamepads, ThisMachinesControllersArePolledAndLetGo)
{
    std::unique_ptr<reckless::input::Gamepads> gamepads = reckless::input::MakeGamepads(kThresholds);
    ASSERT_NE(gamepads, nullptr) << "never null";
    std::vector<GamepadEdge> edges;
    for (int frame = 0; frame < 5; ++frame)
    {
        gamepads->Poll(edges);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    const std::vector<GamepadInfo> devices = gamepads->Devices();
    std::set<std::uint32_t>        ids;
    for (const GamepadInfo &device : devices)
    {
        EXPECT_NE(device.device_id, 0u);
        EXPECT_TRUE(ids.insert(device.device_id).second) << "each id once";
        EXPECT_GE(device.button_count, 0);
        EXPECT_GE(device.axis_count, 0);
        EXPECT_GE(device.hat_count, 0);
        EXPECT_FALSE(device.stable_id.empty());
        std::cout << "[ device  ] " << device.device_id << " \"" << device.name << "\" " << device.stable_id << ": "
                  << device.button_count << " buttons, " << device.axis_count << " axes, " << device.hat_count
                  << " hats\n";
    }
    for (const GamepadEdge &edge : edges)
    {
        EXPECT_EQ(ids.count(edge.device_id), 1u) << "every edge is of a device listed";
    }
    EXPECT_TRUE(Alternates(edges));
    std::cout << "[ machine ] " << devices.size() << " controller(s), " << edges.size() << " edge(s)\n";
}

TEST(MakeGamepads, IsNeverNullAndTwoAreIndependent)
{
    const auto first  = reckless::input::MakeGamepads(kThresholds);
    const auto second = reckless::input::MakeGamepads(reckless::input::GamepadAxisThresholds{0, 0});
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    EXPECT_EQ(first->Devices().size(), second->Devices().size()) << "one machine";
    for (const GamepadInfo &one : first->Devices())
    {
        for (const GamepadInfo &other : second->Devices())
        {
            EXPECT_NE(one.device_id, other.device_id) << "ids are never given twice";
        }
    }
}

#if defined(__linux__)
namespace
{
/** A virtual controller made with uinput: four buttons, X and Y from 0 to 255, and a hat - what an arcade encoder
    presents itself as - made as this is constructed and pulled out as it is destroyed. */
class VirtualController
{
  public:
    /** The controller made, where /dev/uinput is writable. */
    VirtualController()
    {
        handle_ = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
        if (handle_ < 0)
        {
            reason_ = std::string("/dev/uinput cannot be opened for writing: ") + std::strerror(errno);
            return;
        }
        bool is_made = ioctl(handle_, UI_SET_EVBIT, EV_KEY) == 0 && ioctl(handle_, UI_SET_EVBIT, EV_ABS) == 0 &&
                       ioctl(handle_, UI_SET_EVBIT, EV_SYN) == 0;
        for (const int button : {BTN_TRIGGER, BTN_THUMB, BTN_THUMB2, BTN_TOP})
        {
            is_made = is_made && ioctl(handle_, UI_SET_KEYBIT, button) == 0;
        }
        for (const auto &[code, minimum, maximum] : {std::tuple(ABS_X, 0, 255), std::tuple(ABS_Y, 0, 255),
                                                     std::tuple(ABS_HAT0X, -1, 1), std::tuple(ABS_HAT0Y, -1, 1)})
        {
            uinput_abs_setup axis{};
            axis.code            = static_cast<std::uint16_t>(code);
            axis.absinfo.minimum = minimum;
            axis.absinfo.maximum = maximum;
            axis.absinfo.value   = maximum == 255 ? 128 : 0;
            is_made = is_made && ioctl(handle_, UI_SET_ABSBIT, code) == 0 && ioctl(handle_, UI_ABS_SETUP, &axis) == 0;
        }
        uinput_setup setup{};
        setup.id.bustype = BUS_USB;
        setup.id.vendor  = 0x1209;
        setup.id.product = 0x7e57;
        std::strncpy(setup.name, "Reckless toolkit virtual controller", UINPUT_MAX_NAME_SIZE - 1);
        is_made = is_made && ioctl(handle_, UI_DEV_SETUP, &setup) == 0 && ioctl(handle_, UI_DEV_CREATE) == 0;
        if (!is_made)
        {
            reason_ = std::string("uinput would not make the controller: ") + std::strerror(errno);
            close(handle_);
            handle_ = -1;
        }
    }

    /** The controller pulled out. */
    ~VirtualController()
    {
        PullOut();
    }

    VirtualController(const VirtualController &)            = delete; ///< not copied: it owns the device
    VirtualController &operator=(const VirtualController &) = delete; ///< not copied: it owns the device

    /** The controller pulled out, if it is in. */
    void PullOut()
    {
        if (handle_ >= 0)
        {
            ioctl(handle_, UI_DEV_DESTROY);
            close(handle_);
            handle_ = -1;
        }
    }

    /** One event sent, and the frame ended.
        @param type   its type
        @param code   its code
        @param value  its value */
    void Send(int type, int code, int value) const
    {
        for (const auto &[event_type, event_code, event_value] :
             {std::tuple(type, code, value), std::tuple(EV_SYN, 0, 0)})
        {
            input_event event{};
            event.type  = static_cast<std::uint16_t>(event_type);
            event.code  = static_cast<std::uint16_t>(event_code);
            event.value = event_value;
            ASSERT_EQ(write(handle_, &event, sizeof(event)), static_cast<ssize_t>(sizeof(event)));
        }
    }

    /** Whether it was made.
        @return  true when it is plugged in */
    bool is_made() const noexcept
    {
        return handle_ >= 0;
    }

    /** Why it was not made.
        @return  the reason */
    const std::string &reason() const noexcept
    {
        return reason_;
    }

  private:
    int         handle_{-1}; ///< /dev/uinput, open while the controller is in
    std::string reason_;     ///< why it was not made
};

/** Polls until `is_done` says so or two seconds have gone.
    @param gamepads  the controllers
    @param edges     where the edges are appended
    @param is_done   whether to stop
    @return          true when it stopped because `is_done` said so */
template <typename Done>
bool PollUntil(reckless::input::Gamepads &gamepads, std::vector<GamepadEdge> &edges, Done is_done)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < end)
    {
        gamepads.Poll(edges);
        if (is_done())
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}
} // namespace

TEST(EvdevSystemPort, AKernelEventIsItsTypeCodeAndValue)
{
    input_event event{};
    event.type                                    = EV_ABS;
    event.code                                    = ABS_HAT0Y;
    event.value                                   = -1;
    const reckless::input::evdev::EvdevEvent ours = reckless::input::evdev::EvdevEventOf(event);
    EXPECT_EQ(ours.type, reckless::input::evdev::kAbsolute);
    EXPECT_EQ(ours.code, 0x11);
    EXPECT_EQ(ours.value, -1);
}

TEST(EvdevSystemPort, AVirtualControllerIsFoundReadAndLetGoOf)
{
    auto              gamepads = reckless::input::MakeGamepads(kThresholds);
    VirtualController controller;
    if (!controller.is_made())
    {
        GTEST_SKIP() << controller.reason();
    }
    std::vector<GamepadEdge> edges;
    const auto               find = [&gamepads]() -> std::uint32_t {
        for (const GamepadInfo &device : gamepads->Devices())
        {
            if (device.stable_id.rfind("1209:7e57", 0) == 0)
            {
                return device.device_id;
            }
        }
        return 0;
    };
    if (!PollUntil(*gamepads, edges, [&find]() { return find() != 0; }))
    {
        GTEST_SKIP() << "the kernel made the controller, but its node is not readable here: udev has not given this "
                        "user the event node (run as a member of the input group, or where udev's uaccess applies)";
    }
    const std::uint32_t id = find();
    EXPECT_TRUE(edges.empty()) << "a controller at rest holds nothing";
    const GamepadInfo info = [&gamepads, id]() {
        for (const GamepadInfo &device : gamepads->Devices())
        {
            if (device.device_id == id)
            {
                return device;
            }
        }
        return GamepadInfo{};
    }();
    EXPECT_EQ(info.name, "Reckless toolkit virtual controller");
    EXPECT_EQ(info.button_count, 4);
    EXPECT_EQ(info.axis_count, 2);
    EXPECT_EQ(info.hat_count, 1);

    controller.Send(EV_KEY, BTN_THUMB, 1);
    controller.Send(EV_ABS, ABS_X, 0);
    controller.Send(EV_ABS, ABS_HAT0Y, 1);
    ASSERT_TRUE(PollUntil(*gamepads, edges, [&edges]() { return edges.size() >= 3; }));
    ASSERT_EQ(edges.size(), 3u);
    EXPECT_EQ(edges[0].control, (GamepadControl{GamepadControlKind::kButton, 1}));
    EXPECT_EQ(edges[1].control, (GamepadControl{GamepadControlKind::kAxisNegative, 0}));
    EXPECT_EQ(edges[2].control, (GamepadControl{GamepadControlKind::kHatDown, 0}));
    for (const GamepadEdge &edge : edges)
    {
        EXPECT_EQ(edge.device_id, id);
        EXPECT_TRUE(edge.is_down);
    }

    edges.clear();
    controller.PullOut();
    ASSERT_TRUE(PollUntil(*gamepads, edges, [&find]() { return find() == 0; })) << "pulled out, it goes";
    ASSERT_EQ(edges.size(), 3u) << "and lets go of all three";
    for (const GamepadEdge &edge : edges)
    {
        EXPECT_FALSE(edge.is_down);
    }
    EXPECT_EQ(edges[0].control, (GamepadControl{GamepadControlKind::kButton, 1})) << "oldest first";
}
#endif
