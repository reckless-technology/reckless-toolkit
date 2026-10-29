// The Linux back end whole (input/linux/EvdevGamepads.h) on a port of the suite's own, so on every platform and with no
// kernel: the controllers among the nodes opened as it is made and what they hold told at the first poll, a node that
// is not a controller or cannot be opened passed over until it changes, a controller plugged in and pulled out between
// polls, everything it held let go as it goes, the kernel's dropped events recovered from, and the look once a second
// where the nodes are not watched. The port on the kernel is held by tests/test_input_devices.cpp.
#include <gtest/gtest.h>

#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "linux/EvdevGamepads.h"

namespace
{
using reckless::input::GamepadControl;
using reckless::input::GamepadControlKind;
using reckless::input::GamepadEdge;
using reckless::input::GamepadInfo;
namespace evdev = reckless::input::evdev;

/** A node of the suite's port. */
struct FakeNode
{
    evdev::EvdevCapabilities       capabilities;       ///< what it says of itself
    evdev::EvdevSnapshot           snapshot;           ///< what it holds now
    std::vector<evdev::EvdevEvent> waiting;            ///< the events waiting to be read
    evdev::EvdevNodeStamp          stamp{1, 1};        ///< its stamp
    bool                           can_open{true};     ///< whether it opens
    bool                           can_snapshot{true}; ///< whether its snapshot can be read
    bool                           is_gone{false};     ///< whether a read finds it gone
    bool                           is_listed{true};    ///< whether the directory lists it
};

/** What the suite's port has, shared with the case that drives it. */
struct FakeSystem
{
    std::map<std::string, FakeNode> nodes;              ///< the nodes, by path
    bool                            can_watch{true};    ///< whether the port watches the nodes
    bool                            has_changed{false}; ///< what TakeNodesChanged says next
    std::int64_t                    now{0};             ///< the clock
    int                             opens{0};           ///< how many opens were tried
    std::map<int, std::string>      open;               ///< the handles open, to their paths
    int                             next_handle{3};     ///< the next handle given
};

/** The suite's port, on a FakeSystem the case keeps. */
class FakePort final : public evdev::EvdevPort
{
  public:
    /** A port on `system`.
        @param system  what it has; outlives the port */
    explicit FakePort(FakeSystem &system) : system_(system)
    {
    }

    /** Whether the nodes are watched. @return the system's say */
    bool CanWatchNodes() const override
    {
        return system_.can_watch;
    }

    /** Whether the nodes changed, once. @return the system's say, then false */
    bool TakeNodesChanged() override
    {
        return std::exchange(system_.has_changed, false);
    }

    /** The clock. @return the system's */
    std::int64_t NowMilliseconds() const override
    {
        return system_.now;
    }

    /** The nodes. @return their paths, in reverse, so the back end's own order is what is held */
    std::vector<std::string> ListNodes() const override
    {
        std::vector<std::string> paths;
        for (auto node = system_.nodes.rbegin(); node != system_.nodes.rend(); ++node)
        {
            if (node->second.is_listed)
            {
                paths.push_back(node->first);
            }
        }
        return paths;
    }

    /** A node's stamp. @param path the node @return its stamp */
    evdev::EvdevNodeStamp StampOf(const std::string &path) const override
    {
        const auto node = system_.nodes.find(path);
        return node == system_.nodes.end() ? evdev::EvdevNodeStamp{} : node->second.stamp;
    }

    /** A node opened. @param path the node @param capabilities its capabilities @return a handle, or -1 */
    int Open(const std::string &path, evdev::EvdevCapabilities &capabilities) override
    {
        ++system_.opens;
        const auto node = system_.nodes.find(path);
        if (node == system_.nodes.end() || !node->second.can_open)
        {
            return -1;
        }
        capabilities         = node->second.capabilities;
        const int handle     = system_.next_handle++;
        system_.open[handle] = path;
        return handle;
    }

    /** A node's snapshot. @param handle the node @param snapshot where it goes @return whether it was read */
    bool ReadSnapshot(int handle, evdev::EvdevSnapshot &snapshot) override
    {
        const FakeNode &node = system_.nodes.at(system_.open.at(handle));
        if (!node.can_snapshot)
        {
            return false;
        }
        snapshot = node.snapshot;
        return true;
    }

    /** A node's waiting events. @param handle the node @param events where they go @return kGone where it went */
    evdev::EvdevReadResult Read(int handle, std::vector<evdev::EvdevEvent> &events) override
    {
        const auto node = system_.nodes.find(system_.open.at(handle));
        if (node == system_.nodes.end() || node->second.is_gone)
        {
            return evdev::EvdevReadResult::kGone;
        }
        events.insert(events.end(), node->second.waiting.begin(), node->second.waiting.end());
        node->second.waiting.clear();
        return evdev::EvdevReadResult::kRead;
    }

    /** A node closed. @param handle the node */
    void Close(int handle) override
    {
        EXPECT_EQ(system_.open.erase(handle), 1u) << "closed once, and only what was opened";
    }

  private:
    FakeSystem &system_; ///< what it has
};

/** A joystick of `buttons` buttons and an X and Y from 0 to 255, at rest.
    @param name     what it calls itself
    @param buttons  how many buttons, from BTN_TRIGGER up
    @return         the node */
FakeNode Joystick(const std::string &name, int buttons)
{
    FakeNode node;
    for (int button = 0; button < buttons; ++button)
    {
        node.capabilities.keys.set(evdev::kButtonJoystick + static_cast<std::size_t>(button));
    }
    for (const std::size_t code : {evdev::kAbsoluteX, evdev::kAbsoluteY})
    {
        node.capabilities.absolutes.set(code);
        node.capabilities.absolute_info[code] = evdev::EvdevAbsoluteInfo{128, 0, 255};
        node.snapshot.absolute_values[code]   = 128;
    }
    node.capabilities.vendor_id  = 0x0079;
    node.capabilities.product_id = 0x0006;
    node.capabilities.name       = name;
    return node;
}

/** A keyboard: no controller.
    @return  the node */
FakeNode Keyboard()
{
    FakeNode node;
    node.capabilities.keys.set(30);
    node.capabilities.name = "Keyboard";
    return node;
}

/** An event.
    @param type   its type
    @param code   its code
    @param value  its value
    @return       the event */
evdev::EvdevEvent Event(std::uint16_t type, std::uint16_t code, std::int32_t value)
{
    return evdev::EvdevEvent{type, code, value};
}

/** One button control.
    @param index  its index
    @return       the control */
GamepadControl Button(std::uint16_t index)
{
    return GamepadControl{GamepadControlKind::kButton, index};
}

/** The back end on `system`.
    @param system  the system
    @return        the back end */
std::unique_ptr<evdev::EvdevGamepads> Make(FakeSystem &system)
{
    return std::make_unique<evdev::EvdevGamepads>(std::make_unique<FakePort>(system),
                                                  reckless::input::GamepadAxisThresholds{500, 300});
}
} // namespace

TEST(EvdevGamepads, ControllersAreFoundAsItIsMadeInTheKernelsOrder)
{
    FakeSystem system;
    system.nodes["/dev/input/event10"] = Joystick("Later", 2);
    system.nodes["/dev/input/event2"]  = Joystick("Earlier", 4);
    system.nodes["/dev/input/event3"]  = Keyboard();
    system.nodes["/dev/input/event2"].snapshot.keys.set(evdev::kButtonJoystick + 1);
    auto gamepads = Make(system);

    const std::vector<GamepadInfo> devices = gamepads->Devices();
    ASSERT_EQ(devices.size(), 2u) << "found as it is made, before any poll; the keyboard passed over";
    EXPECT_EQ(devices[0].name, "Earlier") << "event2 before event10";
    EXPECT_EQ(devices[0].button_count, 4);
    EXPECT_EQ(devices[0].axis_count, 2);
    EXPECT_EQ(devices[0].stable_id, "0079:0006");
    EXPECT_EQ(devices[1].name, "Later");
    EXPECT_EQ(system.open.size(), 2u) << "the keyboard closed again";

    std::vector<GamepadEdge> edges;
    gamepads->Poll(edges);
    ASSERT_EQ(edges.size(), 1u) << "a button down as the device appears is told at the first poll";
    EXPECT_EQ(edges[0].device_id, devices[0].device_id);
    EXPECT_EQ(edges[0].control, Button(1));
    EXPECT_TRUE(edges[0].is_down);
    edges.clear();
    gamepads->Poll(edges);
    EXPECT_TRUE(edges.empty()) << "told once";
}

TEST(EvdevGamepads, EventsAreReadEveryPoll)
{
    FakeSystem system;
    system.nodes["/dev/input/event4"] = Joystick("Stick", 2);
    auto                     gamepads = Make(system);
    std::vector<GamepadEdge> edges;
    gamepads->Poll(edges);
    EXPECT_TRUE(edges.empty());
    system.nodes["/dev/input/event4"].waiting = {Event(evdev::kKey, evdev::kButtonJoystick, 1),
                                                 Event(evdev::kAbsolute, evdev::kAbsoluteX, 0),
                                                 Event(evdev::kSynchronise, evdev::kSynchroniseReport, 0)};
    gamepads->Poll(edges);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_EQ(edges[0].control, Button(0));
    EXPECT_EQ(edges[1].control, (GamepadControl{GamepadControlKind::kAxisNegative, 0}));
}

TEST(EvdevGamepads, AControllerPluggedInIsFoundWhenTheNodesChange)
{
    FakeSystem system;
    auto       gamepads = Make(system);
    EXPECT_TRUE(gamepads->Devices().empty());
    system.nodes["/dev/input/event7"] = Joystick("Arcade", 8);
    std::vector<GamepadEdge> edges;
    gamepads->Poll(edges);
    EXPECT_TRUE(gamepads->Devices().empty()) << "nothing changed, as far as the port says";
    system.has_changed = true;
    gamepads->Poll(edges);
    ASSERT_EQ(gamepads->Devices().size(), 1u);
    EXPECT_EQ(gamepads->Devices()[0].button_count, 8);
}

TEST(EvdevGamepads, AControllerPulledOutLetsGoOfEverythingItHeld)
{
    FakeSystem system;
    system.nodes["/dev/input/event5"] = Joystick("Pad", 3);
    auto                     gamepads = Make(system);
    const std::uint32_t      id       = gamepads->Devices()[0].device_id;
    std::vector<GamepadEdge> edges;
    system.nodes["/dev/input/event5"].waiting = {Event(evdev::kKey, evdev::kButtonJoystick + 2, 1),
                                                 Event(evdev::kAbsolute, evdev::kAbsoluteY, 255)};
    gamepads->Poll(edges);
    ASSERT_EQ(edges.size(), 2u);
    edges.clear();

    system.nodes["/dev/input/event5"].is_gone = true;
    gamepads->Poll(edges);
    ASSERT_EQ(edges.size(), 2u) << "ups for both, oldest first";
    EXPECT_EQ(edges[0].control, Button(2));
    EXPECT_FALSE(edges[0].is_down);
    EXPECT_EQ(edges[1].control, (GamepadControl{GamepadControlKind::kAxisPositive, 1}));
    EXPECT_FALSE(edges[1].is_down);
    EXPECT_EQ(edges[1].device_id, id);
    EXPECT_TRUE(gamepads->Devices().empty());
    EXPECT_TRUE(system.open.empty()) << "closed";

    system.nodes["/dev/input/event5"].is_gone = false;
    system.has_changed                        = true;
    gamepads->Poll(edges);
    ASSERT_EQ(gamepads->Devices().size(), 1u) << "plugged in again";
    EXPECT_NE(gamepads->Devices()[0].device_id, id) << "as a new device";
}

TEST(EvdevGamepads, ANodeThatGoesFromTheDirectoryIsClosedAtTheNextLook)
{
    FakeSystem system;
    system.nodes["/dev/input/event5"] = Joystick("Pad", 1);
    auto                     gamepads = Make(system);
    std::vector<GamepadEdge> edges;
    system.nodes["/dev/input/event5"].waiting = {Event(evdev::kKey, evdev::kButtonJoystick, 1)};
    gamepads->Poll(edges);
    ASSERT_EQ(edges.size(), 1u);
    // The node gone from the listing while its handle still reads: the look closes it.
    system.nodes["/dev/input/event5"].is_listed = false;
    system.has_changed                          = true;
    gamepads->Poll(edges);
    ASSERT_EQ(edges.size(), 2u);
    EXPECT_FALSE(edges[1].is_down);
    EXPECT_TRUE(gamepads->Devices().empty());
}

TEST(EvdevGamepads, ANodePassedOverIsLookedAtAgainOnlyOnceItChanges)
{
    FakeSystem system;
    system.nodes["/dev/input/event1"]          = Keyboard();
    system.nodes["/dev/input/event9"]          = Joystick("Not yet ours", 2);
    system.nodes["/dev/input/event9"].can_open = false;
    auto gamepads                              = Make(system);
    EXPECT_EQ(system.opens, 2);
    EXPECT_TRUE(gamepads->Devices().empty());

    std::vector<GamepadEdge> edges;
    system.has_changed = true;
    gamepads->Poll(edges);
    EXPECT_EQ(system.opens, 2) << "nothing changed on either node: neither is opened again";

    // udev grants the seat's user the controller: its access list, and so its stamp, changes.
    system.nodes["/dev/input/event9"].can_open = true;
    system.nodes["/dev/input/event9"].stamp    = evdev::EvdevNodeStamp{1, 2};
    system.has_changed                         = true;
    gamepads->Poll(edges);
    EXPECT_EQ(system.opens, 3);
    ASSERT_EQ(gamepads->Devices().size(), 1u);

    // A node passed over that leaves the directory is forgotten, so a node of that name later is looked at afresh.
    system.nodes.erase("/dev/input/event1");
    system.has_changed = true;
    gamepads->Poll(edges);
    system.nodes["/dev/input/event1"] = Joystick("New at an old name", 1);
    system.has_changed                = true;
    gamepads->Poll(edges);
    EXPECT_EQ(gamepads->Devices().size(), 2u);
}

TEST(EvdevGamepads, WhatTheKernelDroppedIsReadAgainAtTheFramesEnd)
{
    FakeSystem system;
    system.nodes["/dev/input/event6"] = Joystick("Pad", 4);
    auto                     gamepads = Make(system);
    std::vector<GamepadEdge> edges;
    FakeNode                &node = system.nodes["/dev/input/event6"];
    node.waiting                  = {
        Event(evdev::kKey, evdev::kButtonJoystick, 1), Event(evdev::kSynchronise, evdev::kSynchroniseDropped, 0),
        Event(evdev::kKey, evdev::kButtonJoystick + 3, 1), // lost in the drop: passed over
        Event(evdev::kSynchronise, evdev::kSynchroniseReport, 0), Event(evdev::kKey, evdev::kButtonJoystick + 1, 1)};
    // What the device holds once the frame ends: button 0 let go in the events the kernel dropped, button 2 down.
    node.snapshot.keys.reset();
    node.snapshot.keys.set(evdev::kButtonJoystick + 2);
    gamepads->Poll(edges);
    ASSERT_EQ(edges.size(), 4u);
    EXPECT_EQ(edges[0].control, Button(0));
    EXPECT_TRUE(edges[0].is_down);
    EXPECT_EQ(edges[1].control, Button(0)) << "the snapshot lets go what the drop hid";
    EXPECT_FALSE(edges[1].is_down);
    EXPECT_EQ(edges[2].control, Button(2));
    EXPECT_TRUE(edges[2].is_down);
    EXPECT_EQ(edges[3].control, Button(1)) << "events after the frame's end are read again";

    // A snapshot that cannot be read leaves the device as its events last said.
    edges.clear();
    node.can_snapshot = false;
    node.waiting      = {Event(evdev::kSynchronise, evdev::kSynchroniseDropped, 0),
                         Event(evdev::kSynchronise, evdev::kSynchroniseReport, 0)};
    gamepads->Poll(edges);
    EXPECT_TRUE(edges.empty());
}

TEST(EvdevGamepads, AControllerWhoseSnapshotCannotBeReadIsJudgedFromItsCapabilities)
{
    FakeSystem system;
    system.nodes["/dev/input/event6"]                                                     = Joystick("Pad", 1);
    system.nodes["/dev/input/event6"].can_snapshot                                        = false;
    system.nodes["/dev/input/event6"].capabilities.absolute_info[evdev::kAbsoluteY].value = 0;
    auto                     gamepads                                                     = Make(system);
    std::vector<GamepadEdge> edges;
    system.nodes["/dev/input/event6"].waiting = {Event(evdev::kAbsolute, evdev::kAbsoluteY, 255)};
    gamepads->Poll(edges);
    ASSERT_EQ(edges.size(), 1u);
    EXPECT_EQ(edges[0].control, (GamepadControl{GamepadControlKind::kAxisPositive, 1}))
        << "Y read at 0 as it was opened: a trigger at rest, whose control is its positive half";
}

TEST(EvdevGamepads, WhereTheNodesAreNotWatchedItLooksOnceASecond)
{
    FakeSystem system;
    system.can_watch                  = false;
    system.now                        = 10'000;
    auto gamepads                     = Make(system);
    system.nodes["/dev/input/event8"] = Joystick("Pad", 1);
    std::vector<GamepadEdge> edges;
    system.now += 999;
    gamepads->Poll(edges);
    EXPECT_TRUE(gamepads->Devices().empty()) << "under a second";
    system.now += 1;
    gamepads->Poll(edges);
    EXPECT_EQ(gamepads->Devices().size(), 1u) << "a second";
}

TEST(EvdevGamepads, LettingItGoClosesEveryNodeAndTellsNothing)
{
    FakeSystem system;
    system.nodes["/dev/input/event1"] = Joystick("One", 1);
    system.nodes["/dev/input/event2"] = Joystick("Two", 1);
    {
        auto gamepads = Make(system);
        EXPECT_EQ(system.open.size(), 2u);
    }
    EXPECT_TRUE(system.open.empty());
}
