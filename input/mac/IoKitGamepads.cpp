// The macOS back end (IoKitGamepads.h says what it is): an IOHIDManager matching the Generic Desktop page's joystick,
// game pad and multi-axis controller collections, scheduled on the main run loop. Its three callbacks - a device
// matched, a device removed, an input value - run on that run loop and only queue what they are told, a matched
// device retained so that it outlives the queue; Poll, on the same thread, drains the queue in order. A matched
// device's elements are read once into its buttons (the Button page, in usage order), axes (in usage order) and hats,
// and each element's value as the device appears is read too, so that an axis's rest is judged from where it is
// then and a button already down is told.
#include "IoKitGamepads.h"

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/hid/IOHIDLib.h>

#include "GamepadState.h"
#include "HidUsages.h"

namespace reckless::input
{
namespace
{
/** A CFString as UTF-8.
    @param text  the string; may be null
    @return      the UTF-8 text */
std::string Utf8(CFStringRef text)
{
    if (text == nullptr)
    {
        return {};
    }
    const CFIndex     length = CFStringGetLength(text);
    const CFIndex     most   = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::vector<char> bytes(static_cast<std::size_t>(std::max<CFIndex>(most, 1)), '\0');
    if (!CFStringGetCString(text, bytes.data(), most, kCFStringEncodingUTF8))
    {
        return {};
    }
    return std::string(bytes.data());
}

/** A device's string property.
    @param device  the device
    @param key     the property's key
    @return        its value as UTF-8; empty where it has none */
std::string StringProperty(IOHIDDeviceRef device, CFStringRef key)
{
    const CFTypeRef value = IOHIDDeviceGetProperty(device, key);
    return value != nullptr && CFGetTypeID(value) == CFStringGetTypeID() ? Utf8(static_cast<CFStringRef>(value))
                                                                         : std::string();
}

/** A device's number property.
    @param device  the device
    @param key     the property's key
    @return        its value; 0 where it has none */
long long NumberProperty(IOHIDDeviceRef device, CFStringRef key)
{
    const CFTypeRef value  = IOHIDDeviceGetProperty(device, key);
    long long       number = 0;
    if (value != nullptr && CFGetTypeID(value) == CFNumberGetTypeID())
    {
        CFNumberGetValue(static_cast<CFNumberRef>(value), kCFNumberLongLongType, &number);
    }
    return number;
}

/** A matching dictionary for one Generic Desktop collection.
    @param usage  the collection's usage
    @return       the dictionary, owned by the caller */
CFMutableDictionaryRef MatchingOf(int usage)
{
    CFMutableDictionaryRef matching = CFDictionaryCreateMutable(kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
                                                                &kCFTypeDictionaryValueCallBacks);
    const int              page     = hid::kGenericDesktopPage;
    CFNumberRef            page_number  = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &page);
    CFNumberRef            usage_number = CFNumberCreate(kCFAllocatorDefault, kCFNumberIntType, &usage);
    CFDictionarySetValue(matching, CFSTR(kIOHIDDeviceUsagePageKey), page_number);
    CFDictionarySetValue(matching, CFSTR(kIOHIDDeviceUsageKey), usage_number);
    CFRelease(page_number);
    CFRelease(usage_number);
    return matching;
}

/** What one of a controller's elements is to this layer. */
struct Element
{
    IOHIDElementCookie cookie{};                    ///< the element, within its device
    IOHIDElementRef    element{nullptr};            ///< the element itself, alive while its device is retained
    std::uint32_t      page{0};                     ///< its usage page
    std::uint32_t      usage{0};                    ///< its usage
    hid::ValueRole     role{hid::ValueRole::kNone}; ///< axis or hat; kNone for a button
    int                index{0};                    ///< its button's, axis's or hat's index
    CFIndex            logical_minimum{0};          ///< its logical minimum
    CFIndex            logical_maximum{0};          ///< its logical maximum
};

/** A controller open. */
struct OpenDevice
{
    IOHIDDeviceRef       device{nullptr}; ///< the device, retained
    std::uint32_t        device_id{0};    ///< its id in the roster
    std::vector<Element> elements;        ///< its buttons, axes and hats
};

/** One thing a callback was told. */
struct Happening
{
    /** What it was. */
    enum class Kind : std::uint8_t
    {
        kMatched, ///< a device matched, retained by the callback
        kRemoved, ///< a device removed
        kValue,   ///< an element's value
    };

    Kind               kind{Kind::kValue}; ///< what it was
    IOHIDDeviceRef     device{nullptr};    ///< the device
    IOHIDElementCookie cookie{};           ///< the element, for a value
    CFIndex            value{0};           ///< the value
};

/** The controllers macOS has. */
class IoKitGamepads final : public Gamepads
{
  public:
    /** The manager made, matching controllers, its callbacks registered and it scheduled on the main run loop.
        @param thresholds  where an axis's controls go down and are let go */
    explicit IoKitGamepads(const GamepadAxisThresholds &thresholds)
        : roster_(thresholds), manager_(IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone))
    {
        if (manager_ == nullptr)
        {
            return;
        }
        CFMutableArrayRef matching = CFArrayCreateMutable(kCFAllocatorDefault, 0, &kCFTypeArrayCallBacks);
        for (const int usage : {hid::kJoystickUsage, hid::kGamePadUsage, hid::kMultiAxisUsage})
        {
            CFMutableDictionaryRef one = MatchingOf(usage);
            CFArrayAppendValue(matching, one);
            CFRelease(one);
        }
        IOHIDManagerSetDeviceMatchingMultiple(manager_, matching);
        CFRelease(matching);
        IOHIDManagerRegisterDeviceMatchingCallback(manager_, &OnMatched, this);
        IOHIDManagerRegisterDeviceRemovalCallback(manager_, &OnRemoved, this);
        IOHIDManagerRegisterInputValueCallback(manager_, &OnValue, this);
        IOHIDManagerScheduleWithRunLoop(manager_, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
        IOHIDManagerOpen(manager_, kIOHIDOptionsTypeNone);
    }

    /** The manager unscheduled and closed, so no callback runs after this, and every device released. */
    ~IoKitGamepads() override
    {
        if (manager_ != nullptr)
        {
            IOHIDManagerUnscheduleFromRunLoop(manager_, CFRunLoopGetMain(), kCFRunLoopDefaultMode);
            IOHIDManagerClose(manager_, kIOHIDOptionsTypeNone);
            CFRelease(manager_);
        }
        for (const Happening &happening : queue_)
        {
            if (happening.kind == Happening::Kind::kMatched)
            {
                CFRelease(happening.device);
            }
        }
        for (const OpenDevice &open : open_)
        {
            CFRelease(open.device);
        }
    }

    IoKitGamepads(const IoKitGamepads &)            = delete; ///< not copied: the callbacks point at it
    IoKitGamepads &operator=(const IoKitGamepads &) = delete; ///< not copied: the callbacks point at it

    /** Everything the callbacks queued since the last call, in order (Gamepads::Poll).
        @param edges  where the edges are appended */
    void Poll(std::vector<GamepadEdge> &edges) override
    {
        std::vector<Happening> happenings;
        happenings.swap(queue_);
        for (const Happening &happening : happenings)
        {
            switch (happening.kind)
            {
            case Happening::Kind::kMatched:
                Open(happening.device, edges);
                break;
            case Happening::Kind::kRemoved:
                Close(happening.device, edges);
                break;
            case Happening::Kind::kValue:
                Apply(happening, edges);
                break;
            }
        }
        // Kept for the next poll's swap, so a frame's queue allocates nothing once it has grown.
        happenings.clear();
        if (queue_.empty())
        {
            queue_.swap(happenings);
        }
    }

    /** The controllers open (Gamepads::Devices).
        @return  the devices, in the order they were matched */
    std::vector<GamepadInfo> Devices() const override
    {
        return roster_.Devices();
    }

  private:
    /** A device matched: retained and queued.
        @param context  the back end
        @param result   unused
        @param sender   unused
        @param device   the device */
    static void OnMatched(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
    {
        static_cast<void>(result);
        static_cast<void>(sender);
        CFRetain(device);
        static_cast<IoKitGamepads *>(context)->queue_.push_back(
            Happening{Happening::Kind::kMatched, device, IOHIDElementCookie{}, 0});
    }

    /** A device removed: queued.
        @param context  the back end
        @param result   unused
        @param sender   unused
        @param device   the device */
    static void OnRemoved(void *context, IOReturn result, void *sender, IOHIDDeviceRef device)
    {
        static_cast<void>(result);
        static_cast<void>(sender);
        static_cast<IoKitGamepads *>(context)->queue_.push_back(
            Happening{Happening::Kind::kRemoved, device, IOHIDElementCookie{}, 0});
    }

    /** An input value: queued with its device and element.
        @param context  the back end
        @param result   unused
        @param sender   unused
        @param value    the value */
    static void OnValue(void *context, IOReturn result, void *sender, IOHIDValueRef value)
    {
        static_cast<void>(result);
        static_cast<void>(sender);
        const IOHIDElementRef element = IOHIDValueGetElement(value);
        static_cast<IoKitGamepads *>(context)->queue_.push_back(
            Happening{Happening::Kind::kValue, IOHIDElementGetDevice(element), IOHIDElementGetCookie(element),
                      IOHIDValueGetIntegerValue(value)});
    }

    /** A matched device's elements read, the device added to the roster and each element's value as it appears
        told; a device with no buttons, axes or hats released.
        @param device  the device, retained by the callback
        @param edges   where the edges are appended */
    void Open(IOHIDDeviceRef device, std::vector<GamepadEdge> &edges)
    {
        OpenDevice    open{device, 0, Elements(device)};
        GamepadLayout layout;
        for (const Element &element : open.elements)
        {
            if (element.role == hid::ValueRole::kNone)
            {
                ++layout.button_count;
            }
            else if (element.role == hid::ValueRole::kAxis)
            {
                layout.axes.push_back(
                    hid::RepairedLogicalRange(static_cast<std::int32_t>(element.logical_minimum),
                                              static_cast<std::int32_t>(element.logical_maximum),
                                              static_cast<int>(IOHIDElementGetReportSize(element.element))));
            }
            else
            {
                ++layout.hat_count;
            }
        }
        if (open.elements.empty())
        {
            CFRelease(device);
            return;
        }
        layout.name = StringProperty(device, CFSTR(kIOHIDProductKey));
        if (layout.name.empty())
        {
            layout.name = "HID controller";
        }
        layout.stable_id = GamepadStableId(
            static_cast<std::uint16_t>(NumberProperty(device, CFSTR(kIOHIDVendorIDKey))),
            static_cast<std::uint16_t>(NumberProperty(device, CFSTR(kIOHIDProductIDKey))), Distinguisher(device));
        open.device_id         = roster_.Add(std::move(layout)).device_id();
        GamepadDevice &gamepad = *roster_.Find(open.device_id);
        for (const Element &element : open.elements)
        {
            IOHIDValueRef value = nullptr;
            if (IOHIDDeviceGetValue(device, element.element, &value) == kIOReturnSuccess && value != nullptr)
            {
                Tell(element, IOHIDValueGetIntegerValue(value), gamepad, edges);
            }
        }
        open_.push_back(std::move(open));
    }

    /** A removed device let go of and released.
        @param device  the device
        @param edges   where its ups are appended */
    void Close(IOHIDDeviceRef device, std::vector<GamepadEdge> &edges)
    {
        const auto found = std::find_if(open_.begin(), open_.end(),
                                        [device](const OpenDevice &open) { return open.device == device; });
        if (found == open_.end())
        {
            return;
        }
        roster_.Remove(found->device_id, edges);
        CFRelease(found->device);
        open_.erase(found);
    }

    /** An input value told to its device, where the device is open and the element one of its controls.
        @param happening  the value
        @param edges      where the edges are appended */
    void Apply(const Happening &happening, std::vector<GamepadEdge> &edges)
    {
        const auto open = std::find_if(open_.begin(), open_.end(), [&happening](const OpenDevice &device) {
            return device.device == happening.device;
        });
        if (open == open_.end())
        {
            return;
        }
        const auto element = std::find_if(open->elements.begin(), open->elements.end(),
                                          [&happening](const Element &one) { return one.cookie == happening.cookie; });
        if (element != open->elements.end())
        {
            Tell(*element, happening.value, *roster_.Find(open->device_id), edges);
        }
    }

    /** One element's value told to a device as its button's, axis's or hat's reading.
        @param element  the element
        @param value    its value
        @param device   the device
        @param edges    where the edges are appended */
    static void Tell(const Element &element, CFIndex value, GamepadDevice &device, std::vector<GamepadEdge> &edges)
    {
        switch (element.role)
        {
        case hid::ValueRole::kNone:
            device.SetButton(element.index, value != 0, edges);
            break;
        case hid::ValueRole::kAxis:
            device.SetAxis(element.index, static_cast<std::int32_t>(value), edges);
            break;
        case hid::ValueRole::kHat:
            device.SetHat(element.index,
                          hid::HatOfSwitch(static_cast<std::int32_t>(value),
                                           static_cast<std::int32_t>(element.logical_minimum),
                                           static_cast<std::int32_t>(element.logical_maximum)),
                          edges);
            break;
        }
    }

    /** A device's buttons, axes and hats: its input elements on the Button page, in usage order, then its axes in
        usage order and its hats, each element once, each numbered within its kind.
        @param device  the device
        @return        the elements */
    static std::vector<Element> Elements(IOHIDDeviceRef device)
    {
        std::vector<Element> elements;
        CFArrayRef           all = IOHIDDeviceCopyMatchingElements(device, nullptr, kIOHIDOptionsTypeNone);
        if (all == nullptr)
        {
            return elements;
        }
        for (CFIndex at = 0; at < CFArrayGetCount(all); ++at)
        {
            auto *const element = static_cast<IOHIDElementRef>(const_cast<void *>(CFArrayGetValueAtIndex(all, at)));
            const IOHIDElementType type = IOHIDElementGetType(element);
            if (type != kIOHIDElementTypeInput_Misc && type != kIOHIDElementTypeInput_Button &&
                type != kIOHIDElementTypeInput_Axis)
            {
                continue;
            }
            const std::uint32_t  page  = IOHIDElementGetUsagePage(element);
            const std::uint32_t  usage = IOHIDElementGetUsage(element);
            const hid::ValueRole role  = page == hid::kButtonPage ? hid::ValueRole::kNone
                                                                  : hid::RoleOfValue(static_cast<std::uint16_t>(page),
                                                                                     static_cast<std::uint16_t>(usage));
            if (page != hid::kButtonPage && role == hid::ValueRole::kNone)
            {
                continue;
            }
            elements.push_back(Element{IOHIDElementGetCookie(element), element, page, usage, role, 0,
                                       IOHIDElementGetLogicalMin(element), IOHIDElementGetLogicalMax(element)});
        }
        CFRelease(all);
        Number(elements);
        return elements;
    }

    /** The elements put in order - buttons, then axes, then hats, each in usage order - each cookie once, and each
        given its index within its kind.
        @param elements  the elements */
    static void Number(std::vector<Element> &elements)
    {
        const auto key = [](const Element &element) {
            const int rank = element.role == hid::ValueRole::kNone ? 0 : element.role == hid::ValueRole::kAxis ? 1 : 2;
            return std::tuple(rank, element.page, element.usage, element.cookie);
        };
        std::sort(elements.begin(), elements.end(),
                  [&key](const Element &left, const Element &right) { return key(left) < key(right); });
        elements.erase(
            std::unique(elements.begin(), elements.end(),
                        [](const Element &left, const Element &right) { return left.cookie == right.cookie; }),
            elements.end());
        int counts[3] = {0, 0, 0};
        for (Element &element : elements)
        {
            element.index = counts[std::get<0>(key(element))]++;
        }
    }

    /** What tells two devices of a model apart: the serial number where the device has one, and the port it is
        plugged into (its location id, in hexadecimal).
        @param device  the device
        @return        the distinguisher */
    static std::string Distinguisher(IOHIDDeviceRef device)
    {
        std::string distinguisher = StringProperty(device, CFSTR(kIOHIDSerialNumberKey));
        const auto  location      = static_cast<unsigned long long>(NumberProperty(device, CFSTR(kIOHIDLocationIDKey)));
        if (location != 0)
        {
            char       digits[24]{};
            const auto written = std::to_chars(digits, digits + sizeof(digits), location, 16);
            distinguisher += "@" + std::string(digits, written.ptr);
        }
        return distinguisher;
    }

    GamepadRoster           roster_;  ///< every controller open
    IOHIDManagerRef         manager_; ///< the manager; null where it could not be made
    std::vector<OpenDevice> open_;    ///< the controllers open, retained
    std::vector<Happening>  queue_;   ///< what the callbacks were told since the last poll
};
} // namespace

std::unique_ptr<Gamepads> MakeIoKitGamepads(const GamepadAxisThresholds &axis_thresholds)
{
    return std::make_unique<IoKitGamepads>(axis_thresholds);
}
} // namespace reckless::input
