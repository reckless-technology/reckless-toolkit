#pragma once

// An XInput controller's state as a device's controls, with none of Windows' headers: XINPUT_GAMEPAD's fields and its
// button bits are XInput's documented interface, copied here so that the suites hold the mapping on every platform;
// input/win/WindowsGamepads.cpp, which includes xinput.h, holds the bits to Windows' own with a static_assert. Here too
// is what the back end's XInput thread decides and queues, and the queue told to the devices, so that a tap between
// two polls - a down and an up the thread saw and Poll had not yet read - is held by the suites without a thread or a
// controller.

#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <string>
#include <vector>

#include "GamepadState.h"

namespace reckless::input::xinput
{
inline constexpr std::uint16_t kDpadUp        = 0x0001; ///< XINPUT_GAMEPAD_DPAD_UP
inline constexpr std::uint16_t kDpadDown      = 0x0002; ///< XINPUT_GAMEPAD_DPAD_DOWN
inline constexpr std::uint16_t kDpadLeft      = 0x0004; ///< XINPUT_GAMEPAD_DPAD_LEFT
inline constexpr std::uint16_t kDpadRight     = 0x0008; ///< XINPUT_GAMEPAD_DPAD_RIGHT
inline constexpr std::uint16_t kStart         = 0x0010; ///< XINPUT_GAMEPAD_START
inline constexpr std::uint16_t kBack          = 0x0020; ///< XINPUT_GAMEPAD_BACK
inline constexpr std::uint16_t kLeftThumb     = 0x0040; ///< XINPUT_GAMEPAD_LEFT_THUMB: the left stick pressed
inline constexpr std::uint16_t kRightThumb    = 0x0080; ///< XINPUT_GAMEPAD_RIGHT_THUMB: the right stick pressed
inline constexpr std::uint16_t kLeftShoulder  = 0x0100; ///< XINPUT_GAMEPAD_LEFT_SHOULDER
inline constexpr std::uint16_t kRightShoulder = 0x0200; ///< XINPUT_GAMEPAD_RIGHT_SHOULDER
inline constexpr std::uint16_t kA             = 0x1000; ///< XINPUT_GAMEPAD_A
inline constexpr std::uint16_t kB             = 0x2000; ///< XINPUT_GAMEPAD_B
inline constexpr std::uint16_t kX             = 0x4000; ///< XINPUT_GAMEPAD_X
inline constexpr std::uint16_t kY             = 0x8000; ///< XINPUT_GAMEPAD_Y
inline constexpr int           kSlotCount     = 4;      ///< XUSER_MAX_COUNT: how many controllers XInput serves

/** The buttons in the order they are numbered: as Windows' own HID driver for the pad numbers them, A to the right
    stick, so a pad is numbered the same whether a program reaches it through XInput or not. */
inline constexpr std::uint16_t kButtonOrder[] = {kA,    kB,     kX,         kY,         kLeftShoulder, kRightShoulder,
                                                 kBack, kStart, kLeftThumb, kRightThumb};

/** XINPUT_GAMEPAD, field for field. */
struct PadState
{
    std::uint16_t buttons{0};       ///< wButtons: the button bits above
    std::uint8_t  left_trigger{0};  ///< bLeftTrigger, 0 to 255
    std::uint8_t  right_trigger{0}; ///< bRightTrigger, 0 to 255
    std::int16_t  left_x{0};        ///< sThumbLX: right is positive
    std::int16_t  left_y{0};        ///< sThumbLY: up is positive
    std::int16_t  right_x{0};       ///< sThumbRX: right is positive
    std::int16_t  right_y{0};       ///< sThumbRY: up is positive
};

/** A stick's vertical reading as every other controller gives one, down positive: XInput's own is up positive. The
    range is kept - -32768 to 32767 both ways - by turning each reading into its one's complement.
    @param up_positive  XInput's reading
    @return             the reading, down positive */
constexpr std::int32_t DownPositive(std::int16_t up_positive) noexcept
{
    return -1 - static_cast<std::int32_t>(up_positive);
}

/** What an XInput controller is: ten buttons; six axes in the order Linux's driver for these pads gives them - the
    left stick across and down, the left trigger, the right stick across and down, the right trigger - the sticks
    resting in the middle and the triggers at the bottom; and the pad's cross as one hat.
    @param slot  which of XInput's four controllers it is, from 0
    @return      the layout */
inline GamepadLayout PadLayout(int slot)
{
    constexpr GamepadAxisRange kStick{-32768, 32767, GamepadAxisRest::kCentre};
    constexpr GamepadAxisRange kTrigger{0, 255, GamepadAxisRest::kMinimum};
    return GamepadLayout{"XInput controller " + std::to_string(slot + 1),
                         "xinput/" + std::to_string(slot),
                         static_cast<int>(std::size(kButtonOrder)),
                         {kStick, kStick, kTrigger, kStick, kStick, kTrigger},
                         1};
}

/** An XInput controller's state told to its device: every button, axis and the hat.
    @param state   the state XInputGetState gave
    @param device  the device
    @param edges   where the edges it makes are appended */
inline void ApplyPadState(const PadState &state, GamepadDevice &device, std::vector<GamepadEdge> &edges)
{
    int index = 0;
    for (const std::uint16_t bit : kButtonOrder)
    {
        device.SetButton(index++, (state.buttons & bit) != 0, edges);
    }
    device.SetAxis(0, state.left_x, edges);
    device.SetAxis(1, DownPositive(state.left_y), edges);
    device.SetAxis(2, state.left_trigger, edges);
    device.SetAxis(3, state.right_x, edges);
    device.SetAxis(4, DownPositive(state.right_y), edges);
    device.SetAxis(5, state.right_trigger, edges);
    device.SetHat(0,
                  GamepadHat{(state.buttons & kDpadUp) != 0, (state.buttons & kDpadRight) != 0,
                             (state.buttons & kDpadDown) != 0, (state.buttons & kDpadLeft) != 0},
                  edges);
}

/** What the thread that reads XInput knows of one slot between its reads. */
struct WatchedSlot
{
    bool          is_connected{false}; ///< whether a controller was in it at the last read
    std::uint32_t packet{0};           ///< the state's packet number at the last read: XInput's "something changed"
};

/** Whether a read of a slot is news to queue for Poll - a controller arrived, gone, or its state changed - and the
    slot as the thread now knows it. A packet number that has not moved is no news: XInput moves it whenever the
    state changes.
    @param watched       the slot as the thread knew it, updated to the read
    @param is_connected  whether XInputGetState found a controller
    @param packet        the state's packet number (dwPacketNumber); read only where `is_connected`
    @return              true where the read is to be queued */
constexpr bool IsSlotNews(WatchedSlot &watched, bool is_connected, std::uint32_t packet) noexcept
{
    if (!is_connected)
    {
        const bool was_connected = watched.is_connected;
        watched.is_connected     = false;
        return was_connected;
    }
    const bool is_news   = !watched.is_connected || packet != watched.packet;
    watched.is_connected = true;
    watched.packet       = packet;
    return is_news;
}

/** One read of a slot the thread found to be news, queued for Poll. */
struct SlotReading
{
    int          slot{0};             ///< the slot, 0 to kSlotCount - 1
    bool         is_connected{false}; ///< false for the controller in it gone
    PadState     state{};             ///< its state, where connected
    std::int64_t time_nanoseconds{0}; ///< when it was read, on the steady clock (GamepadEdge::time_nanoseconds)
};

/** The device in each of XInput's slots, as Poll keeps them: 0 for an empty slot. */
using SlotDevices = std::array<std::uint32_t, kSlotCount>;

/** The readings the thread queued told to the devices, in the order they were read: a slot's controller added at its
    first reading, every state told (ApplyPadState) - so a down and an up read between two polls are both told, in
    order - and a controller gone let go of. Every edge a reading makes carries the reading's time.
    @param readings  the readings, oldest first
    @param devices   the device in each slot, updated as controllers come and go
    @param roster    the devices
    @param edges     where the edges are appended */
inline void ApplySlotReadings(const std::vector<SlotReading> &readings, SlotDevices &devices, GamepadRoster &roster,
                              std::vector<GamepadEdge> &edges)
{
    for (const SlotReading &reading : readings)
    {
        if (reading.slot < 0 || reading.slot >= kSlotCount)
        {
            continue;
        }
        std::uint32_t    &device = devices[static_cast<std::size_t>(reading.slot)];
        const std::size_t first  = edges.size();
        if (!reading.is_connected)
        {
            if (device != 0)
            {
                roster.Remove(device, edges);
                device = 0;
            }
        }
        else
        {
            if (device == 0)
            {
                device = roster.Add(PadLayout(reading.slot)).device_id();
            }
            ApplyPadState(reading.state, *roster.Find(device), edges);
        }
        StampEdges(edges, first, reading.time_nanoseconds);
    }
}

/** How many readings the queue holds before a slot's newest state is written over its last queued one rather than
    queued beside it: four seconds of a stick moving every millisecond, which only a program that has stopped polling
    reaches. A controller's arrival and going are always queued. */
inline constexpr std::size_t kMostQueuedSlotReadings = 4096;

/** `reading` queued behind the readings already waiting - or, where kMostQueuedSlotReadings are waiting and the slot's
    last queued reading is a state, written over that one, so a program that has stopped polling holds a bounded queue
    and still reads each slot's newest state when it starts again.
    @param queue    the readings waiting for Poll
    @param reading  the reading to queue */
inline void QueueSlotReading(std::vector<SlotReading> &queue, const SlotReading &reading)
{
    if (queue.size() >= kMostQueuedSlotReadings && reading.is_connected)
    {
        for (auto queued = queue.rbegin(); queued != queue.rend(); ++queued)
        {
            if (queued->slot == reading.slot)
            {
                if (queued->is_connected)
                {
                    *queued = reading;
                    return;
                }
                break;
            }
        }
    }
    queue.push_back(reading);
}
} // namespace reckless::input::xinput
