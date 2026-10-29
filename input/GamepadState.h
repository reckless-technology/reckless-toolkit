#pragma once

// What turns a platform's readings of a device into the edges Gamepads promises, with none of any platform's headers,
// so the suites hold all of it on every platform - as window/RawKey.h holds the keys a window has said are down. A
// back end reads its platform and says what it read: a button is pressed or not, an axis reads a number in its range,
// a hat points somewhere. Everything else is here: an axis's two halves through a threshold with hysteresis, an axis
// that rests at one end told apart from one that rests in the middle, a hat's four directions, the controls a device
// has said are down - which is what makes every edge alternate and lets a device that goes let go of everything - and
// the ids devices are known by.

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "Gamepads.h"

namespace reckless::input
{
/** Where a hat points: any of its four directions, two at once on a diagonal, none when it is centred. */
struct GamepadHat
{
    bool is_up{false};    ///< pushed up, alone or on a diagonal
    bool is_right{false}; ///< pushed right, alone or on a diagonal
    bool is_down{false};  ///< pushed down, alone or on a diagonal
    bool is_left{false};  ///< pushed left, alone or on a diagonal

    /** Whether two hats point the same way.
        @param other  the other hat
        @return       true when every direction is the same */
    constexpr bool operator==(const GamepadHat &other) const = default;
};

/** Where an axis rests when nothing pushes it, which decides what its controls are. */
enum class GamepadAxisRest : std::uint8_t
{
    /** Not known yet: judged from the first reading the back end gives (JudgeAxisRest), which a back end gives as the
        device appears wherever its platform can say where the axis is then. */
    kFirstReading,
    kCentre,  ///< in the middle of its range, as a stick does: both halves are controls
    kMinimum, ///< at the bottom of its range, as a trigger does: only its positive half is a control
    kMaximum, ///< at the top of its range, as an inverted trigger does: only its negative half is a control
};

/** An axis's range, as its platform gives it, and where it rests. */
struct GamepadAxisRange
{
    std::int32_t    minimum{0};                           ///< the lowest reading
    std::int32_t    maximum{0};                           ///< the highest reading; an axis with no range has no edges
    GamepadAxisRest rest{GamepadAxisRest::kFirstReading}; ///< where it rests, where the back end knows

    /** Whether two ranges are the same.
        @param other  the other range
        @return       true when the bounds and the rest are equal */
    constexpr bool operator==(const GamepadAxisRange &other) const = default;
};

/** What a back end found a device to be: what a GamepadInfo says of it, and its axes' ranges. */
struct GamepadLayout
{
    std::string                   name;            ///< what the device calls itself (GamepadInfo::name)
    std::string                   stable_id;       ///< GamepadInfo::stable_id, made by GamepadStableId
    int                           button_count{0}; ///< how many buttons
    std::vector<GamepadAxisRange> axes;            ///< every axis's range, in the device's order
    int                           hat_count{0};    ///< how many hats
};

/** How far a reading is from the middle of an axis's range, towards either end, in thousandths of the half range:
    -1000 at the minimum, 1000 at the maximum, 0 in the middle. A reading outside the range is taken as the end it is
    past; an axis with no range reads 0. Whole numbers throughout, so every platform gives the same answer.
    @param range  the axis's range
    @param value  the reading
    @return       the deflection, from -1000 to 1000 */
constexpr std::int32_t CentredDeflection(const GamepadAxisRange &range, std::int32_t value) noexcept
{
    if (range.maximum <= range.minimum)
    {
        return 0;
    }
    const std::int64_t span        = static_cast<std::int64_t>(range.maximum) - range.minimum;
    const std::int64_t offset      = 2 * static_cast<std::int64_t>(value) - range.minimum - range.maximum;
    const std::int64_t thousandths = offset * 1000 / span;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(thousandths, -1000, 1000));
}

/** How far a reading is from one end of an axis's range towards the other, in thousandths of the whole range: 0 at
    `from_maximum`'s end, 1000 at the other. A reading outside the range is taken as the end it is past; an axis with
    no range reads 0.
    @param range         the axis's range
    @param value         the reading
    @param from_maximum  true to measure from the maximum down, false from the minimum up
    @return              the deflection, from 0 to 1000 */
constexpr std::int32_t OneSidedDeflection(const GamepadAxisRange &range, std::int32_t value, bool from_maximum) noexcept
{
    if (range.maximum <= range.minimum)
    {
        return 0;
    }
    const std::int64_t span     = static_cast<std::int64_t>(range.maximum) - range.minimum;
    const std::int64_t distance = from_maximum ? static_cast<std::int64_t>(range.maximum) - value
                                               : static_cast<std::int64_t>(value) - range.minimum;
    return static_cast<std::int32_t>(std::clamp<std::int64_t>(distance * 1000 / span, 0, 1000));
}

/** Where an axis rests, judged from a reading taken while nothing pushes it: in the middle unless the reading is so far
    to one side that a stick's control would be down there - which is a trigger at rest, or a pedal, and which, judged
    a stick, would hold a control down for as long as the device was plugged in. Judged at that end instead, the axis
    has the one control that moving away from it gives.
    @param range       the axis's range
    @param value       the reading at rest
    @param thresholds  the thresholds in use (UsableThresholds)
    @return            kCentre, kMinimum or kMaximum */
constexpr GamepadAxisRest JudgeAxisRest(const GamepadAxisRange &range, std::int32_t value,
                                        const GamepadAxisThresholds &thresholds) noexcept
{
    const std::int32_t deflection = CentredDeflection(range, value);
    if (deflection <= -thresholds.press_thousandths)
    {
        return GamepadAxisRest::kMinimum;
    }
    if (deflection >= thresholds.press_thousandths)
    {
        return GamepadAxisRest::kMaximum;
    }
    return GamepadAxisRest::kCentre;
}

/** Whether a control of an axis is down after a reading: one that is down stays down until the deflection comes back
    under the release, and one that is up goes down only at the press - the hysteresis that keeps a stick resting near
    one threshold from chattering.
    @param is_down     whether the control is down before the reading
    @param deflection  how far the axis is pushed towards this control, in thousandths
    @param thresholds  the thresholds in use (UsableThresholds)
    @return            whether it is down after */
constexpr bool IsAxisControlDown(bool is_down, std::int32_t deflection,
                                 const GamepadAxisThresholds &thresholds) noexcept
{
    return is_down ? deflection >= thresholds.release_thousandths : deflection >= thresholds.press_thousandths;
}

/** The id a device is given as it appears, from one count for the whole binary, so no two devices - in one Gamepads
    or in two - are ever given the same id, and 0 is never given. Safe from any thread.
    @return  the next id, from 1 */
inline std::uint32_t NextGamepadDeviceId() noexcept
{
    static std::atomic<std::uint32_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

/** A GamepadInfo::stable_id: the vendor and product ids as four lower-case hexadecimal digits each, a colon between,
    then a slash and `distinguisher` where the platform gave one.
    @param vendor_id      the vendor's USB id
    @param product_id     the product's USB id
    @param distinguisher  what tells two devices of the model apart - a serial number, a port - or empty
    @return               the id */
inline std::string GamepadStableId(std::uint16_t vendor_id, std::uint16_t product_id, const std::string &distinguisher)
{
    constexpr char kDigits[] = "0123456789abcdef";
    std::string    id;
    for (const std::uint16_t number : {vendor_id, product_id})
    {
        if (!id.empty())
        {
            id += ':';
        }
        for (int shift = 12; shift >= 0; shift -= 4)
        {
            id += kDigits[(number >> shift) & 0xFu];
        }
    }
    if (!distinguisher.empty())
    {
        id += '/';
        id += distinguisher;
    }
    return id;
}

/**
    The controls one device has said are down, in the order they went down: whether a control turning on is a down to
    tell (it was not down already), whether one turning off is an up to tell (its down was told), and what is still
    down when the device goes and must be let go. Every edge a device tells passes through here, which is what makes
    the edges of one control alternate - as window::RawKeysHeld does for a window's keys.
*/
class GamepadControlsHeld
{
  public:
    /** `control` noted as down, and whether that is news.
        @param control  the control turning on
        @return         true when it was not down, so a down is to be told */
    bool NoteDown(GamepadControl control)
    {
        if (IsHeld(control))
        {
            return false;
        }
        held_.push_back(control);
        return true;
    }

    /** `control` noted as let go, and whether its owner is owed the up.
        @param control  the control turning off
        @return         true when it was down, so an up is to be told */
    bool NoteUp(GamepadControl control)
    {
        const auto found = std::find(held_.begin(), held_.end(), control);
        if (found == held_.end())
        {
            return false;
        }
        held_.erase(found);
        return true;
    }

    /** Whether `control` is down.
        @param control  the control
        @return         true while it is down */
    bool IsHeld(GamepadControl control) const
    {
        return std::find(held_.begin(), held_.end(), control) != held_.end();
    }

    /** An up for every control still down, oldest first, and nothing down afterwards.
        @param device_id  the device the ups are of
        @return           the ups; empty when nothing is down */
    std::vector<GamepadEdge> TakeUps(std::uint32_t device_id)
    {
        std::vector<GamepadEdge> ups;
        ups.reserve(held_.size());
        for (const GamepadControl control : held_)
        {
            ups.push_back(GamepadEdge{device_id, control, false});
        }
        held_.clear();
        return ups;
    }

    /** How many controls are down.
        @return  the count */
    std::size_t count() const noexcept
    {
        return held_.size();
    }

  private:
    /** The controls down, oldest first: a device holds a handful at once, so this is searched rather than indexed. */
    std::vector<GamepadControl> held_;
};

/**
    One device as its back end reads it: told each reading, it tells the edges the reading makes. A reading that
    changes nothing tells nothing, so a back end may tell a device everything it read - a whole report, a whole
    snapshot - without first working out what changed. Where one reading moves one control off and another on - an
    axis swung from one end to the other, a hat turned from up to right - the up is told before the down. A reading of
    a button, axis or hat the device does not have is passed over.
*/
class GamepadDevice
{
  public:
    /** A device with nothing down.
        @param device_id   its id (NextGamepadDeviceId)
        @param layout      what the back end found it to be
        @param thresholds  the thresholds in use (UsableThresholds) */
    GamepadDevice(std::uint32_t device_id, GamepadLayout layout, const GamepadAxisThresholds &thresholds)
        : device_id_(device_id), layout_(std::move(layout)), thresholds_(thresholds)
    {
        layout_.button_count = std::max(layout_.button_count, 0);
        layout_.hat_count    = std::max(layout_.hat_count, 0);
    }

    /** A button's reading.
        @param index    the button
        @param is_down  whether it is pressed
        @param edges    where an edge it makes is appended */
    void SetButton(int index, bool is_down, std::vector<GamepadEdge> &edges)
    {
        if (index < 0 || index >= layout_.button_count)
        {
            return;
        }
        Change(Control(GamepadControlKind::kButton, index), is_down, edges);
    }

    /** An axis's reading. The first reading of an axis whose rest is not known judges where it rests
        (JudgeAxisRest), and tells nothing if that is where it is.
        @param index  the axis
        @param value  the reading, in the axis's range
        @param edges  where the edges it makes are appended */
    void SetAxis(int index, std::int32_t value, std::vector<GamepadEdge> &edges)
    {
        if (index < 0 || static_cast<std::size_t>(index) >= layout_.axes.size())
        {
            return;
        }
        GamepadAxisRange &range = layout_.axes[static_cast<std::size_t>(index)];
        if (range.rest == GamepadAxisRest::kFirstReading)
        {
            range.rest = JudgeAxisRest(range, value, thresholds_);
        }
        const GamepadControl negative     = Control(GamepadControlKind::kAxisNegative, index);
        const GamepadControl positive     = Control(GamepadControlKind::kAxisPositive, index);
        bool                 is_negative  = false;
        bool                 is_positive  = false;
        const bool           was_negative = held_.IsHeld(negative);
        const bool           was_positive = held_.IsHeld(positive);
        const std::int32_t   centred      = CentredDeflection(range, value);
        switch (range.rest)
        {
        case GamepadAxisRest::kMinimum:
            is_positive = IsAxisControlDown(was_positive, OneSidedDeflection(range, value, false), thresholds_);
            break;
        case GamepadAxisRest::kMaximum:
            is_negative = IsAxisControlDown(was_negative, OneSidedDeflection(range, value, true), thresholds_);
            break;
        case GamepadAxisRest::kCentre:
        case GamepadAxisRest::kFirstReading:
            is_negative = IsAxisControlDown(was_negative, -centred, thresholds_);
            is_positive = IsAxisControlDown(was_positive, centred, thresholds_);
            break;
        }
        ChangeUpsFirst({{negative, is_negative}, {positive, is_positive}}, edges);
    }

    /** A hat's reading.
        @param index  the hat
        @param hat    where it points
        @param edges  where the edges it makes are appended */
    void SetHat(int index, GamepadHat hat, std::vector<GamepadEdge> &edges)
    {
        if (index < 0 || index >= layout_.hat_count)
        {
            return;
        }
        ChangeUpsFirst({{Control(GamepadControlKind::kHatUp, index), hat.is_up},
                        {Control(GamepadControlKind::kHatRight, index), hat.is_right},
                        {Control(GamepadControlKind::kHatDown, index), hat.is_down},
                        {Control(GamepadControlKind::kHatLeft, index), hat.is_left}},
                       edges);
    }

    /** Where a hat points, as its readings have said.
        @param index  the hat
        @return       its directions; centred for a hat the device does not have */
    GamepadHat Hat(int index) const
    {
        return GamepadHat{held_.IsHeld(Control(GamepadControlKind::kHatUp, index)),
                          held_.IsHeld(Control(GamepadControlKind::kHatRight, index)),
                          held_.IsHeld(Control(GamepadControlKind::kHatDown, index)),
                          held_.IsHeld(Control(GamepadControlKind::kHatLeft, index))};
    }

    /** Every control still down let go, oldest first: what is told as the device goes.
        @param edges  where the ups are appended */
    void ReleaseAll(std::vector<GamepadEdge> &edges)
    {
        const std::vector<GamepadEdge> ups = held_.TakeUps(device_id_);
        edges.insert(edges.end(), ups.begin(), ups.end());
    }

    /** Whether a control is down.
        @param control  the control
        @return         true while it is down */
    bool IsHeld(GamepadControl control) const
    {
        return held_.IsHeld(control);
    }

    /** How many controls are down.
        @return  the count */
    std::size_t HeldCount() const noexcept
    {
        return held_.count();
    }

    /** The device's id.
        @return  the id */
    std::uint32_t device_id() const noexcept
    {
        return device_id_;
    }

    /** The device's axes' ranges, each rest as judged so far.
        @return  the ranges */
    const std::vector<GamepadAxisRange> &axes() const noexcept
    {
        return layout_.axes;
    }

    /** What a program is told of the device.
        @return  its info */
    GamepadInfo Info() const
    {
        return GamepadInfo{device_id_,
                           layout_.name,
                           layout_.stable_id,
                           layout_.button_count,
                           static_cast<int>(layout_.axes.size()),
                           layout_.hat_count};
    }

  private:
    /** A control of a kind at an index.
        @param kind   the kind
        @param index  the index, which the callers have checked
        @return       the control */
    static GamepadControl Control(GamepadControlKind kind, int index) noexcept
    {
        return GamepadControl{kind, static_cast<std::uint16_t>(index)};
    }

    /** A control turned on or off, the edge it makes told.
        @param control  the control
        @param is_down  whether it is on after the reading
        @param edges    where an edge is appended */
    void Change(GamepadControl control, bool is_down, std::vector<GamepadEdge> &edges)
    {
        const bool is_news = is_down ? held_.NoteDown(control) : held_.NoteUp(control);
        if (is_news)
        {
            edges.push_back(GamepadEdge{device_id_, control, is_down});
        }
    }

    /** Several controls of one reading turned on or off, every one turning off before any turning on.
        @param changes  each control and whether it is on after the reading
        @param edges    where the edges are appended */
    void ChangeUpsFirst(std::initializer_list<std::pair<GamepadControl, bool>> changes, std::vector<GamepadEdge> &edges)
    {
        for (const auto &[control, is_down] : changes)
        {
            if (!is_down)
            {
                Change(control, false, edges);
            }
        }
        for (const auto &[control, is_down] : changes)
        {
            if (is_down)
            {
                Change(control, true, edges);
            }
        }
    }

    std::uint32_t         device_id_;  ///< the device's id
    GamepadLayout         layout_;     ///< what it is, its axes' rests judged as they are read
    GamepadAxisThresholds thresholds_; ///< the thresholds in use
    GamepadControlsHeld   held_;       ///< the controls it has said are down
};

/**
    The devices a back end has open, in the order they appeared: each given its id as it is added, and each told to
    let go of everything as it is taken out. A back end keeps its platform's handles beside the ids; this keeps the
    devices.
*/
class GamepadRoster
{
  public:
    /** No devices.
        @param thresholds  the thresholds a program gave, held as UsableThresholds holds them */
    explicit GamepadRoster(const GamepadAxisThresholds &thresholds) : thresholds_(UsableThresholds(thresholds))
    {
    }

    /** A device that has appeared, given a new id.
        @param layout  what the back end found it to be
        @return        the device, which lives until it is removed */
    GamepadDevice &Add(GamepadLayout layout)
    {
        devices_.push_back(std::make_unique<GamepadDevice>(NextGamepadDeviceId(), std::move(layout), thresholds_));
        return *devices_.back();
    }

    /** A device by its id.
        @param device_id  the id
        @return           the device; null for an id this roster does not hold */
    GamepadDevice *Find(std::uint32_t device_id) const
    {
        for (const std::unique_ptr<GamepadDevice> &device : devices_)
        {
            if (device->device_id() == device_id)
            {
                return device.get();
            }
        }
        return nullptr;
    }

    /** A device that has gone, every control it held let go.
        @param device_id  the id
        @param edges      where the ups are appended
        @return           true when the roster held it */
    bool Remove(std::uint32_t device_id, std::vector<GamepadEdge> &edges)
    {
        for (auto device = devices_.begin(); device != devices_.end(); ++device)
        {
            if ((*device)->device_id() == device_id)
            {
                (*device)->ReleaseAll(edges);
                devices_.erase(device);
                return true;
            }
        }
        return false;
    }

    /** Every device as a program is told of it, in the order they appeared.
        @return  the devices */
    std::vector<GamepadInfo> Devices() const
    {
        std::vector<GamepadInfo> devices;
        devices.reserve(devices_.size());
        for (const std::unique_ptr<GamepadDevice> &device : devices_)
        {
            devices.push_back(device->Info());
        }
        return devices;
    }

    /** How many devices it holds.
        @return  the count */
    std::size_t count() const noexcept
    {
        return devices_.size();
    }

    /** The thresholds in use.
        @return  the thresholds, as UsableThresholds held them */
    const GamepadAxisThresholds &thresholds() const noexcept
    {
        return thresholds_;
    }

  private:
    GamepadAxisThresholds                       thresholds_; ///< the thresholds in use
    std::vector<std::unique_ptr<GamepadDevice>> devices_;    ///< the devices, oldest first; each lives on the heap so
                                                             ///< a reference to one outlives another's addition
};
} // namespace reckless::input
