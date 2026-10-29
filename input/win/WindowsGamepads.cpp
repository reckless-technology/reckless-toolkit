// The Windows back end (WindowsGamepads.h says what it is): XInput, loaded at run time, for the controllers XInput
// serves; Windows' HID API for every other joystick, game pad and multi-axis controller - found with SetupDi, read
// with overlapped ReadFile and parsed with the HidP_ functions on the device's preparsed data - passing over every HID
// path of an XInput controller, which XInput reads already. A configuration manager notification, which needs no
// window, says when a HID interface arrives or goes, so the devices are looked for again only then; XInput's empty
// slots are asked about once a second, since asking an empty slot is slow.
#include "WindowsGamepads.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cfgmgr32.h>
#include <hidsdi.h>
#include <setupapi.h>
#include <xinput.h>

#include "GamepadState.h"
#include "HidPaths.h"
#include "HidUsages.h"
#include "XInputPads.h"

namespace reckless::input
{
namespace
{
static_assert(xinput::kDpadUp == XINPUT_GAMEPAD_DPAD_UP && xinput::kDpadDown == XINPUT_GAMEPAD_DPAD_DOWN);
static_assert(xinput::kDpadLeft == XINPUT_GAMEPAD_DPAD_LEFT && xinput::kDpadRight == XINPUT_GAMEPAD_DPAD_RIGHT);
static_assert(xinput::kStart == XINPUT_GAMEPAD_START && xinput::kBack == XINPUT_GAMEPAD_BACK);
static_assert(xinput::kLeftThumb == XINPUT_GAMEPAD_LEFT_THUMB && xinput::kRightThumb == XINPUT_GAMEPAD_RIGHT_THUMB);
static_assert(xinput::kLeftShoulder == XINPUT_GAMEPAD_LEFT_SHOULDER);
static_assert(xinput::kRightShoulder == XINPUT_GAMEPAD_RIGHT_SHOULDER);
static_assert(xinput::kA == XINPUT_GAMEPAD_A && xinput::kB == XINPUT_GAMEPAD_B);
static_assert(xinput::kX == XINPUT_GAMEPAD_X && xinput::kY == XINPUT_GAMEPAD_Y);
static_assert(xinput::kSlotCount == XUSER_MAX_COUNT);

/** How long between asking XInput's empty slots, and between looks for HID devices where no notification comes. */
constexpr ULONGLONG kLookIntervalMilliseconds = 1000;

/** The most reports one device's read takes in one poll, so a device that never stops sending cannot hold a poll. */
constexpr int kMostReportsPerPoll = 256;

/** UTF-16 as UTF-8.
    @param wide  the text, null-terminated
    @return      the UTF-8 text */
std::string Utf8(const wchar_t *wide)
{
    if (wide == nullptr || *wide == L'\0')
    {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
    {
        return {};
    }
    std::string text(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), bytes, nullptr, nullptr);
    text.resize(static_cast<std::size_t>(bytes - 1));
    return text;
}

/** XInput, loaded at run time from the system's directory - xinput1_4 where it is, the older xinput9_1_0 otherwise -
    so the toolkit links no import library for it and a machine without it has no XInput controllers. */
class XInputLibrary
{
  public:
    /** XInput loaded, where the system has it. */
    XInputLibrary()
    {
        module_ = LoadLibraryExW(L"xinput1_4.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (module_ == nullptr)
        {
            module_ = LoadLibraryExW(L"xinput9_1_0.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        }
        if (module_ != nullptr)
        {
            get_state_ =
                reinterpret_cast<GetState>(reinterpret_cast<void *>(GetProcAddress(module_, "XInputGetState")));
        }
    }

    /** XInput unloaded. */
    ~XInputLibrary()
    {
        if (module_ != nullptr)
        {
            FreeLibrary(module_);
        }
    }

    XInputLibrary(const XInputLibrary &)            = delete; ///< not copied: it owns the module
    XInputLibrary &operator=(const XInputLibrary &) = delete; ///< not copied: it owns the module

    /** A slot's state.
        @param slot   the slot, 0 to 3
        @param state  where its state is written
        @return       true when a controller is in the slot */
    bool State(DWORD slot, XINPUT_STATE &state) const
    {
        return get_state_ != nullptr && get_state_(slot, &state) == ERROR_SUCCESS;
    }

  private:
    using GetState = DWORD(WINAPI *)(DWORD, XINPUT_STATE *); ///< XInputGetState

    HMODULE  module_{nullptr};    ///< the library; null where the system has none
    GetState get_state_{nullptr}; ///< XInputGetState; null where it is not there
};

/** One of a HID controller's values this layer reads: an axis or a hat. */
struct HidValue
{
    USAGE          page{0};                     ///< its usage page
    USAGE          usage{0};                    ///< its usage
    USHORT         link_collection{0};          ///< the collection it is in
    UCHAR          report_id{0};                ///< the report it comes in
    int            bit_size{0};                 ///< its field's size in bits
    LONG           logical_minimum{0};          ///< its logical minimum, as the descriptor gave it
    LONG           logical_maximum{0};          ///< its logical maximum, as the descriptor gave it
    hid::ValueRole role{hid::ValueRole::kNone}; ///< axis or hat
    int            index{0};                    ///< its axis's or hat's index
};

/** One of a HID controller's buttons. */
struct HidButton
{
    USAGE usage{0};     ///< its usage on the Button page: button n is usage n
    UCHAR report_id{0}; ///< the report it comes in
};

/** One HID controller, open and being read. */
class HidPad
{
  public:
    /** A HID interface opened, if it is a controller.
        @param path    the interface's path
        @param layout  where what the controller is, is written
        @return        the controller; null where the path cannot be opened or is not a controller */
    static std::unique_ptr<HidPad> Open(const std::wstring &path, GamepadLayout &layout)
    {
        std::unique_ptr<HidPad> pad(new HidPad(path));
        if (!pad->OpenHandle() || !pad->ReadDescriptor() || !pad->ReadLayout(layout))
        {
            return nullptr;
        }
        return pad;
    }

    /** Its read cancelled and waited for, and everything closed. */
    ~HidPad()
    {
        if (handle_ != INVALID_HANDLE_VALUE)
        {
            if (is_reading_)
            {
                CancelIoEx(handle_, &overlapped_);
                DWORD bytes = 0;
                GetOverlappedResult(handle_, &overlapped_, &bytes, TRUE);
            }
            CloseHandle(handle_);
        }
        if (overlapped_.hEvent != nullptr)
        {
            CloseHandle(overlapped_.hEvent);
        }
        if (preparsed_ != nullptr)
        {
            HidD_FreePreparsedData(preparsed_);
        }
    }

    HidPad(const HidPad &)            = delete; ///< not copied: the read in flight points at it
    HidPad &operator=(const HidPad &) = delete; ///< not copied: the read in flight points at it

    /** Every report waiting read and told to the device.
        @param device  the controller's device
        @param edges   where the edges are appended
        @return        false when the controller has gone */
    bool Drain(GamepadDevice &device, std::vector<GamepadEdge> &edges)
    {
        for (int report = 0; report < kMostReportsPerPoll; ++report)
        {
            if (!is_reading_)
            {
                const BOOL is_done =
                    ReadFile(handle_, report_.data(), static_cast<DWORD>(report_.size()), nullptr, &overlapped_);
                if (!is_done && GetLastError() != ERROR_IO_PENDING)
                {
                    return false;
                }
                is_reading_ = true;
            }
            DWORD bytes = 0;
            if (!GetOverlappedResult(handle_, &overlapped_, &bytes, FALSE))
            {
                return GetLastError() == ERROR_IO_INCOMPLETE;
            }
            is_reading_ = false;
            Apply(static_cast<ULONG>(bytes), device, edges);
        }
        return true;
    }

    /** Its interface's path.
        @return  the path */
    const std::wstring &path() const noexcept
    {
        return path_;
    }

    std::uint32_t device_id{0}; ///< its device's id in the roster

  private:
    /** A controller not yet opened.
        @param path  its interface's path */
    explicit HidPad(std::wstring path) : path_(std::move(path))
    {
    }

    /** The interface opened for overlapped reads - for reading and writing where Windows allows it, as some drivers
        ask, else for reading alone - and its read's event made.
        @return  true when it is open */
    bool OpenHandle()
    {
        constexpr DWORD kShare = FILE_SHARE_READ | FILE_SHARE_WRITE;
        handle_ = CreateFileW(path_.c_str(), GENERIC_READ | GENERIC_WRITE, kShare, nullptr, OPEN_EXISTING,
                              FILE_FLAG_OVERLAPPED, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
        {
            handle_ =
                CreateFileW(path_.c_str(), GENERIC_READ, kShare, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
        }
        if (handle_ == INVALID_HANDLE_VALUE)
        {
            return false;
        }
        overlapped_.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        return overlapped_.hEvent != nullptr;
    }

    /** The descriptor read, and whether its top-level collection is a controller.
        @return  true for a controller */
    bool ReadDescriptor()
    {
        if (!HidD_GetPreparsedData(handle_, &preparsed_) || HidP_GetCaps(preparsed_, &caps_) != HIDP_STATUS_SUCCESS)
        {
            return false;
        }
        if (!hid::IsControllerCollection(caps_.UsagePage, caps_.Usage) || caps_.InputReportByteLength == 0)
        {
            return false;
        }
        report_.assign(caps_.InputReportByteLength, '\0');
        usages_.assign(HidP_MaxUsageListLength(HidP_Input, hid::kButtonPage, preparsed_), 0);
        return true;
    }

    /** The controller's buttons, axes and hats found, and what it is written to `layout`.
        @param layout  where what it is, is written
        @return        true */
    bool ReadLayout(GamepadLayout &layout)
    {
        ReadButtons();
        ReadValues(layout);
        HIDD_ATTRIBUTES attributes{};
        attributes.Size = sizeof(attributes);
        if (!HidD_GetAttributes(handle_, &attributes))
        {
            attributes.VendorID  = 0;
            attributes.ProductID = 0;
        }
        wchar_t product[256]{};
        layout.name = HidD_GetProductString(handle_, product, static_cast<ULONG>(sizeof(product))) ? Utf8(product)
                                                                                                   : std::string();
        if (layout.name.empty())
        {
            layout.name = "HID controller";
        }
        layout.stable_id =
            GamepadStableId(attributes.VendorID, attributes.ProductID, hidpath::InstanceOf(std::wstring_view(path_)));
        layout.button_count = static_cast<int>(buttons_.size());
        return true;
    }

    /** The Button page's usages read into buttons_, in usage order, each once. */
    void ReadButtons()
    {
        USHORT                        count = caps_.NumberInputButtonCaps;
        std::vector<HIDP_BUTTON_CAPS> button_caps(count);
        if (count == 0 || HidP_GetButtonCaps(HidP_Input, button_caps.data(), &count, preparsed_) != HIDP_STATUS_SUCCESS)
        {
            return;
        }
        for (USHORT cap = 0; cap < count; ++cap)
        {
            const HIDP_BUTTON_CAPS &button = button_caps[cap];
            if (button.UsagePage != hid::kButtonPage)
            {
                continue;
            }
            const USAGE first = button.IsRange ? button.Range.UsageMin : button.NotRange.Usage;
            const USAGE last  = button.IsRange ? button.Range.UsageMax : button.NotRange.Usage;
            for (unsigned usage = first; usage <= last && usage <= 0xFFFFu; ++usage)
            {
                buttons_.push_back(HidButton{static_cast<USAGE>(usage), button.ReportID});
            }
        }
        std::stable_sort(buttons_.begin(), buttons_.end(),
                         [](const HidButton &left, const HidButton &right) { return left.usage < right.usage; });
        buttons_.erase(
            std::unique(buttons_.begin(), buttons_.end(),
                        [](const HidButton &left, const HidButton &right) { return left.usage == right.usage; }),
            buttons_.end());
    }

    /** The axes, in usage order, and the hats, each once; every axis's range written to `layout`.
        @param layout  where the axes' ranges and the hat count are written */
    void ReadValues(GamepadLayout &layout)
    {
        USHORT                       count = caps_.NumberInputValueCaps;
        std::vector<HIDP_VALUE_CAPS> value_caps(count);
        if (count == 0 || HidP_GetValueCaps(HidP_Input, value_caps.data(), &count, preparsed_) != HIDP_STATUS_SUCCESS)
        {
            return;
        }
        for (USHORT cap = 0; cap < count; ++cap)
        {
            const HIDP_VALUE_CAPS &value = value_caps[cap];
            const USAGE            first = value.IsRange ? value.Range.UsageMin : value.NotRange.Usage;
            const USAGE            last  = value.IsRange ? value.Range.UsageMax : value.NotRange.Usage;
            for (unsigned usage = first; usage <= last && usage <= 0xFFFFu; ++usage)
            {
                const hid::ValueRole role = hid::RoleOfValue(value.UsagePage, static_cast<std::uint16_t>(usage));
                if (role == hid::ValueRole::kNone || value.ReportCount > 1)
                {
                    continue;
                }
                values_.push_back(HidValue{value.UsagePage, static_cast<USAGE>(usage), value.LinkCollection,
                                           value.ReportID, static_cast<int>(value.BitSize), value.LogicalMin,
                                           value.LogicalMax, role, 0});
            }
        }
        // In collection order, then usage order: a pad that gives each stick its own collection, X and Y in each,
        // has its left stick's two axes before its right's, as one that calls the right stick Rx and Ry does. A
        // value is the same value only where its collection and report are the same too - two hats share a usage.
        const auto key = [](const HidValue &value) {
            return std::tuple(value.link_collection, value.page, value.usage, value.report_id);
        };
        std::stable_sort(values_.begin(), values_.end(),
                         [&key](const HidValue &left, const HidValue &right) { return key(left) < key(right); });
        values_.erase(
            std::unique(values_.begin(), values_.end(),
                        [&key](const HidValue &left, const HidValue &right) { return key(left) == key(right); }),
            values_.end());
        int hat_count = 0;
        for (HidValue &value : values_)
        {
            if (value.role == hid::ValueRole::kAxis)
            {
                value.index = static_cast<int>(layout.axes.size());
                layout.axes.push_back(
                    hid::RepairedLogicalRange(value.logical_minimum, value.logical_maximum, value.bit_size));
            }
            else
            {
                value.index = hat_count++;
            }
        }
        layout.hat_count = hat_count;
    }

    /** One report told to the device: the buttons and values it carries, and nothing of those another report
        carries.
        @param length  the report's length in bytes
        @param device  the controller's device
        @param edges   where the edges are appended */
    void Apply(ULONG length, GamepadDevice &device, std::vector<GamepadEdge> &edges)
    {
        const UCHAR report_id = length > 0 ? static_cast<UCHAR>(report_[0]) : 0;
        ULONG       pressed   = static_cast<ULONG>(usages_.size());
        const bool  has_usages =
            !usages_.empty() && HidP_GetUsages(HidP_Input, hid::kButtonPage, 0, usages_.data(), &pressed, preparsed_,
                                               report_.data(), length) == HIDP_STATUS_SUCCESS;
        for (std::size_t button = 0; has_usages && button < buttons_.size(); ++button)
        {
            if (buttons_[button].report_id == report_id)
            {
                const auto end = usages_.begin() + static_cast<std::ptrdiff_t>(pressed);
                device.SetButton(static_cast<int>(button),
                                 std::find(usages_.begin(), end, buttons_[button].usage) != end, edges);
            }
        }
        for (const HidValue &value : values_)
        {
            ULONG raw = 0;
            if (value.report_id != report_id ||
                HidP_GetUsageValue(HidP_Input, value.page, value.link_collection, value.usage, &raw, preparsed_,
                                   report_.data(), length) != HIDP_STATUS_SUCCESS)
            {
                continue;
            }
            const std::int32_t reading = hid::SignedValue(raw, value.bit_size, value.logical_minimum);
            if (value.role == hid::ValueRole::kAxis)
            {
                device.SetAxis(value.index, reading, edges);
            }
            else
            {
                device.SetHat(value.index, hid::HatOfSwitch(reading, value.logical_minimum, value.logical_maximum),
                              edges);
            }
        }
    }

    std::wstring           path_;                         ///< its interface's path
    HANDLE                 handle_{INVALID_HANDLE_VALUE}; ///< the interface, opened for overlapped reads
    OVERLAPPED             overlapped_{};                 ///< the read in flight
    bool                   is_reading_{false};            ///< whether a read is in flight
    PHIDP_PREPARSED_DATA   preparsed_{nullptr};           ///< its descriptor, parsed
    HIDP_CAPS              caps_{};                       ///< its top-level collection
    std::vector<char>      report_;                       ///< the read's buffer, one input report long
    std::vector<USAGE>     usages_;                       ///< the buttons a report says are pressed
    std::vector<HidButton> buttons_;                      ///< its buttons, by index
    std::vector<HidValue>  values_;                       ///< its axes and hats
};

/** Every HID interface present.
    @param hid_guid  the HID interface class
    @return          each interface's path */
std::vector<std::wstring> HidPaths(const GUID &hid_guid)
{
    std::vector<std::wstring> paths;
    const HDEVINFO set = SetupDiGetClassDevsW(&hid_guid, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE)
    {
        return paths;
    }
    SP_DEVICE_INTERFACE_DATA interface_data{};
    interface_data.cbSize = sizeof(interface_data);
    for (DWORD member = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hid_guid, member, &interface_data); ++member)
    {
        DWORD size = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &interface_data, nullptr, 0, &size, nullptr);
        if (size < sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W))
        {
            continue;
        }
        // DWORDs, so the detail's alignment is its own.
        std::vector<DWORD> buffer((size + sizeof(DWORD) - 1) / sizeof(DWORD));
        auto              *detail = reinterpret_cast<SP_DEVICE_INTERFACE_DETAIL_DATA_W *>(buffer.data());
        detail->cbSize            = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (SetupDiGetDeviceInterfaceDetailW(set, &interface_data, detail, size, nullptr, nullptr))
        {
            paths.emplace_back(detail->DevicePath);
        }
    }
    SetupDiDestroyDeviceInfoList(set);
    return paths;
}

/** XInput's state as the platform-free mapping takes it.
    @param pad  XInput's state of the pad
    @return     the same, field for field */
xinput::PadState PadStateOf(const XINPUT_GAMEPAD &pad)
{
    return xinput::PadState{pad.wButtons, pad.bLeftTrigger, pad.bRightTrigger, pad.sThumbLX,
                            pad.sThumbLY, pad.sThumbRX,     pad.sThumbRY};
}

/** The controllers Windows has. */
class WindowsGamepads final : public Gamepads
{
  public:
    /** Every controller there is, opened, and the notification of HID interfaces asked for.
        @param thresholds  where an axis's controls go down and are let go */
    explicit WindowsGamepads(const GamepadAxisThresholds &thresholds) : roster_(thresholds)
    {
        HidD_GetHidGuid(&hid_guid_);
        CM_NOTIFY_FILTER filter{};
        filter.cbSize                      = sizeof(filter);
        filter.FilterType                  = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
        filter.u.DeviceInterface.ClassGuid = hid_guid_;
        if (CM_Register_Notification(&filter, &has_changed_, &OnChange, &notification_) != CR_SUCCESS)
        {
            notification_ = nullptr;
        }
        LookForHid(pending_);
        PollXInput(pending_, true);
        last_look_ = GetTickCount64();
    }

    /** The notification withdrawn - which waits for one in progress - and every controller closed. */
    ~WindowsGamepads() override
    {
        if (notification_ != nullptr)
        {
            CM_Unregister_Notification(notification_);
        }
    }

    WindowsGamepads(const WindowsGamepads &)            = delete; ///< not copied: the notification points at it
    WindowsGamepads &operator=(const WindowsGamepads &) = delete; ///< not copied: the notification points at it

    /** Every edge since the last call, and every controller plugged in or pulled out found (Gamepads::Poll).
        @param edges  where the edges are appended */
    void Poll(std::vector<GamepadEdge> &edges) override
    {
        edges.insert(edges.end(), pending_.begin(), pending_.end());
        pending_.clear();
        for (std::size_t pad = 0; pad < pads_.size();)
        {
            if (pads_[pad]->Drain(*roster_.Find(pads_[pad]->device_id), edges))
            {
                ++pad;
            }
            else
            {
                ClosePad(pad, edges);
            }
        }
        const bool      has_changed     = has_changed_.exchange(false);
        const ULONGLONG now             = GetTickCount64();
        const bool      has_second_gone = now - last_look_ >= kLookIntervalMilliseconds;
        if (has_changed || (notification_ == nullptr && has_second_gone))
        {
            LookForHid(edges);
        }
        PollXInput(edges, has_changed || has_second_gone);
        if (has_changed || has_second_gone)
        {
            last_look_ = now;
        }
    }

    /** The controllers open (Gamepads::Devices).
        @return  the devices, in the order they were found */
    std::vector<GamepadInfo> Devices() const override
    {
        return roster_.Devices();
    }

  private:
    /** One of XInput's slots. */
    struct XInputSlot
    {
        std::uint32_t device_id{0}; ///< the controller in it; 0 where it is empty
        DWORD         packet{0};    ///< the state's packet number last told
    };

    /** Told by the configuration manager, on a thread of its own, that a HID interface arrived or went.
        @param notification  the registration
        @param context       the flag to set
        @param action        what happened
        @param event_data    what it happened to
        @param event_size    its size
        @return              ERROR_SUCCESS */
    static DWORD CALLBACK OnChange(HCMNOTIFICATION notification, PVOID context, CM_NOTIFY_ACTION action,
                                   PCM_NOTIFY_EVENT_DATA event_data, DWORD event_size)
    {
        static_cast<void>(notification);
        static_cast<void>(event_data);
        static_cast<void>(event_size);
        if (action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL || action == CM_NOTIFY_ACTION_DEVICEINTERFACEREMOVAL)
        {
            static_cast<std::atomic<bool> *>(context)->store(true);
        }
        return ERROR_SUCCESS;
    }

    /** A HID controller closed and its device let go of.
        @param pad    its index in pads_
        @param edges  where its ups are appended */
    void ClosePad(std::size_t pad, std::vector<GamepadEdge> &edges)
    {
        roster_.Remove(pads_[pad]->device_id, edges);
        pads_.erase(pads_.begin() + static_cast<std::ptrdiff_t>(pad));
    }

    /** The HID interfaces looked at: controllers gone closed, new ones opened, XInput's passed over.
        @param edges  where the ups of controllers gone are appended */
    void LookForHid(std::vector<GamepadEdge> &edges)
    {
        const std::vector<std::wstring> paths = HidPaths(hid_guid_);
        for (std::size_t pad = 0; pad < pads_.size();)
        {
            if (std::find(paths.begin(), paths.end(), pads_[pad]->path()) == paths.end())
            {
                ClosePad(pad, edges);
            }
            else
            {
                ++pad;
            }
        }
        std::erase_if(not_controllers_, [&paths](const std::wstring &path) {
            return std::find(paths.begin(), paths.end(), path) == paths.end();
        });
        for (const std::wstring &path : paths)
        {
            const bool is_known =
                hidpath::IsXInputPath(std::wstring_view(path)) ||
                std::find(not_controllers_.begin(), not_controllers_.end(), path) != not_controllers_.end() ||
                std::any_of(pads_.begin(), pads_.end(), [&path](const auto &pad) { return pad->path() == path; });
            if (is_known)
            {
                continue;
            }
            GamepadLayout           layout;
            std::unique_ptr<HidPad> pad = HidPad::Open(path, layout);
            if (pad == nullptr)
            {
                // Passed over for good only where it opened and was not a controller: one that would not open - a
                // keyboard Windows keeps to itself, a controller another program holds - is tried at the next look.
                const HANDLE probe = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                                 OPEN_EXISTING, 0, nullptr);
                if (probe != INVALID_HANDLE_VALUE)
                {
                    PHIDP_PREPARSED_DATA preparsed = nullptr;
                    HIDP_CAPS            caps{};
                    if (HidD_GetPreparsedData(probe, &preparsed))
                    {
                        if (HidP_GetCaps(preparsed, &caps) == HIDP_STATUS_SUCCESS &&
                            !hid::IsControllerCollection(caps.UsagePage, caps.Usage))
                        {
                            not_controllers_.push_back(path);
                        }
                        HidD_FreePreparsedData(preparsed);
                    }
                    CloseHandle(probe);
                }
                continue;
            }
            pad->device_id = roster_.Add(std::move(layout)).device_id();
            pads_.push_back(std::move(pad));
        }
    }

    /** XInput's slots read: each controller's state told where it changed, a controller gone let go of, and - where
        `should_look_in_empty_slots` - the empty slots asked whether a controller has come.
        @param edges                       where the edges are appended
        @param should_look_in_empty_slots  whether to ask the empty slots */
    void PollXInput(std::vector<GamepadEdge> &edges, bool should_look_in_empty_slots)
    {
        for (DWORD slot = 0; slot < static_cast<DWORD>(xinput::kSlotCount); ++slot)
        {
            XInputSlot &held = slots_[slot];
            if (held.device_id == 0 && !should_look_in_empty_slots)
            {
                continue;
            }
            XINPUT_STATE state{};
            if (!xinput_.State(slot, state))
            {
                if (held.device_id != 0)
                {
                    roster_.Remove(held.device_id, edges);
                    held.device_id = 0;
                }
                continue;
            }
            const bool is_new = held.device_id == 0;
            if (is_new)
            {
                held.device_id = roster_.Add(xinput::PadLayout(static_cast<int>(slot))).device_id();
            }
            if (is_new || state.dwPacketNumber != held.packet)
            {
                held.packet = state.dwPacketNumber;
                xinput::ApplyPadState(PadStateOf(state.Gamepad), *roster_.Find(held.device_id), edges);
            }
        }
    }

    GamepadRoster                              roster_;             ///< every controller open
    XInputLibrary                              xinput_;             ///< XInput, where the system has it
    std::array<XInputSlot, xinput::kSlotCount> slots_{};            ///< XInput's slots
    GUID                                       hid_guid_{};         ///< the HID interface class
    std::vector<std::unique_ptr<HidPad>>       pads_;               ///< the HID controllers open
    std::vector<std::wstring>                  not_controllers_;    ///< HID interfaces opened and found not controllers
    std::vector<GamepadEdge>                   pending_;            ///< what was found down as this was made
    std::atomic<bool>                          has_changed_{false}; ///< set by the notification: look for HID again
    HCMNOTIFICATION                            notification_{nullptr}; ///< the notification; null where none was given
    ULONGLONG                                  last_look_{0};          ///< when the empty slots were last asked
};
} // namespace

std::unique_ptr<Gamepads> MakeWindowsGamepads(const GamepadAxisThresholds &axis_thresholds)
{
    return std::make_unique<WindowsGamepads>(axis_thresholds);
}
} // namespace reckless::input
