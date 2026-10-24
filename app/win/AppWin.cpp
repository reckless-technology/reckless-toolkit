// The Standalone's platform on Windows (app/AppPlatform.h): top-level windows of a class of the app's own, sized
// so their client area is the design size at the DPI of the monitor they are on and holding the child windows the
// panel and the settings are drawn in (window/win/WindowWin.cpp); a message-only window for the timers, the calls
// after the current message and the calls other threads post (a message posted to it wakes the loop for them); and
// the message loop, ended by WM_QUIT. The process declares itself aware of each
// monitor's DPI, as a JUCE Standalone does, so its windows are drawn at the display's pixels rather than stretched: a
// window opens at the DPI of the monitor it opens on and is sized again when it moves to one of another
// (WM_DPICHANGED). A window is put back wholly on the monitor it is most on, a dialog centred over its owner and kept
// inside that monitor (WindowPlace.h). The main window is resized by its frame, each size dragged to held to its
// owner's constraint on the content (WM_SIZING) - and a size given it any other way, a snap to a side of the screen,
// held as JUCE's window holds one (WM_WINDOWPOSCHANGING) - and each size taken told (WM_SIZE). The end of a session
// reaches the top-level windows alone (WM_QUERYENDSESSION, WM_ENDSESSION), which hand it to the app's ending. The
// window's icon is the executable's own (the recipe compiles the series' app icon into it as resource 1).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AppPlatform.h"
#include "WindowPlace.h"

// windows.h last: it defines macros (min, max, CreateWindow) that would reach into the headers above.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>

#include "../../window/win/ClipboardWin.h" // the child window's too: not on an include path
#include "../../window/win/HandedKeys.h"   // the child window's too

namespace reckless::app
{
namespace
{
/** The window class of the app's top-level windows. */
constexpr wchar_t kWindowClass[] = L"RecklessStandaloneWindow";

/** The window class of the message-only window that carries the timers and the calls after the current message. */
constexpr wchar_t kMessageClass[] = L"RecklessStandaloneMessages";

/** The message posted to the message-only window to run the queued calls. */
constexpr UINT kCallSoon = WM_APP + 31;

/** The message posted to the message-only window, from any thread, to run the calls other threads posted. */
constexpr UINT kPosted = WM_APP + 32;

/** The id of the icon resource the recipe compiles into the executable. */
constexpr int kIconResource = 1;

/** Text in the platform's wide form.
    @param utf8  the text, in UTF-8
    @return      the same text in UTF-16; empty for empty text */
std::wstring WideOf(const std::string &utf8)
{
    if (utf8.empty())
    {
        return {};
    }
    const int    length = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

/** The id of the menu bar's first command, its command number 1 (MenuBarController); the others follow it. Clear of
    the dialog ids (IDOK, IDCANCEL) and far under the system menu's (SC_ from 0xF000). */
constexpr UINT kFirstCommand = 1000;

/** The key a virtual-key code names, with the modifiers the thread's keyboard holds as it is read: letters and
    digits by their character, the function keys and the editing and moving keys by name, and any other key that
    types a character by the character it types unshifted (MapVirtualKey); a dead key, or a key that types nothing,
    names none.
    @param vk  the virtual-key code, a key-down message's wparam
    @return    the key pressed, as the shortcuts match it; empty for a key that names none */
Shortcut StrokeOf(WPARAM vk)
{
    Shortcut stroke;
    stroke.primary = GetKeyState(VK_CONTROL) < 0;
    stroke.shift   = GetKeyState(VK_SHIFT) < 0;
    stroke.alt     = GetKeyState(VK_MENU) < 0;
    if (vk >= 'A' && vk <= 'Z')
    {
        stroke.key       = ShortcutKey::kCharacter;
        stroke.character = static_cast<char32_t>(U'a' + (vk - 'A'));
        return stroke;
    }
    if (vk >= '0' && vk <= '9')
    {
        stroke.key       = ShortcutKey::kCharacter;
        stroke.character = static_cast<char32_t>(U'0' + (vk - '0'));
        return stroke;
    }
    if (vk >= VK_F1 && vk <= VK_F12)
    {
        stroke.key = static_cast<ShortcutKey>(static_cast<int>(ShortcutKey::kF1) + static_cast<int>(vk - VK_F1));
        return stroke;
    }
    switch (vk)
    {
    case VK_ESCAPE:
        stroke.key = ShortcutKey::kEscape;
        return stroke;
    case VK_RETURN:
        stroke.key = ShortcutKey::kReturn;
        return stroke;
    case VK_TAB:
        stroke.key = ShortcutKey::kTab;
        return stroke;
    case VK_SPACE:
        stroke.key = ShortcutKey::kSpace;
        return stroke;
    case VK_BACK:
        stroke.key = ShortcutKey::kBackspace;
        return stroke;
    case VK_DELETE:
        stroke.key = ShortcutKey::kDelete;
        return stroke;
    case VK_LEFT:
        stroke.key = ShortcutKey::kLeft;
        return stroke;
    case VK_RIGHT:
        stroke.key = ShortcutKey::kRight;
        return stroke;
    case VK_UP:
        stroke.key = ShortcutKey::kUp;
        return stroke;
    case VK_DOWN:
        stroke.key = ShortcutKey::kDown;
        return stroke;
    case VK_HOME:
        stroke.key = ShortcutKey::kHome;
        return stroke;
    case VK_END:
        stroke.key = ShortcutKey::kEnd;
        return stroke;
    case VK_PRIOR:
        stroke.key = ShortcutKey::kPageUp;
        return stroke;
    case VK_NEXT:
        stroke.key = ShortcutKey::kPageDown;
        return stroke;
    default:
        break;
    }
    const UINT typed = MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_CHAR);
    if ((typed & 0x80000000u) == 0 && typed >= 32) // the top bit marks a dead key
    {
        stroke.key       = ShortcutKey::kCharacter;
        stroke.character = Shortcut::Lowered(static_cast<char32_t>(typed));
    }
    return stroke;
}

/** Whether a key types a character - a character key, or the space bar - whose WM_CHAR follows its key down.
    @param stroke  the key
    @return        true for a key that types */
bool TypesCharacter(const Shortcut &stroke) noexcept
{
    return stroke.key == ShortcutKey::kCharacter || stroke.key == ShortcutKey::kSpace;
}

/** A title as a menu item's text: an ampersand doubled, which Windows would otherwise read as a mnemonic's mark.
    @param utf8  the title, UTF-8
    @return      the text, UTF-16 */
std::wstring MenuText(const std::string &utf8)
{
    std::wstring text;
    for (const wchar_t c : WideOf(utf8))
    {
        text.push_back(c);
        if (c == L'&')
        {
            text.push_back(L'&');
        }
    }
    return text;
}

/** A menu's entries appended to `menu`: each item tagged with its command's id, its shortcut's text after a tab, as
    Windows right-aligns it; each submenu a popup of its own; each rule a separator.
    @param menu        the menu, a popup
    @param entries     the entries, held by `controller`
    @param controller  the bar, which numbers the entries */
void AppendEntries(HMENU menu, const std::vector<MenuEntry> &entries, const MenuBarController &controller)
{
    for (const MenuEntry &entry : entries)
    {
        if (entry.separator)
        {
            AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
            continue;
        }
        const UINT state = (entry.enabled ? MF_ENABLED : MF_GRAYED) | (entry.checked ? MF_CHECKED : MF_UNCHECKED);
        if (!entry.submenu.empty())
        {
            HMENU submenu = CreatePopupMenu();
            AppendEntries(submenu, entry.submenu, controller);
            AppendMenuW(menu, MF_POPUP | state, reinterpret_cast<UINT_PTR>(submenu), MenuText(entry.title).c_str());
            continue;
        }
        std::wstring text = MenuText(entry.title);
        if (const std::string shortcut = ShortcutText(entry.shortcut, false); !shortcut.empty())
        {
            text += L"\t" + WideOf(shortcut);
        }
        AppendMenuW(menu, MF_STRING | state, kFirstCommand + static_cast<UINT>(controller.CommandOf(entry)),
                    text.c_str());
    }
}

/** The bar the controller holds as a window's menu bar, a popup for each of its menus.
    @param controller  the bar
    @return            the menu, for a window to own; null for a bar with no menus */
HMENU MenuBarOf(const MenuBarController &controller)
{
    if (controller.bar().menus.empty())
    {
        return nullptr;
    }
    HMENU bar = CreateMenu();
    for (const MenuEntry &menu : controller.bar().menus)
    {
        HMENU popup = CreatePopupMenu();
        AppendEntries(popup, menu.submenu, controller);
        AppendMenuW(bar, MF_POPUP | (menu.enabled ? MF_ENABLED : MF_GRAYED), reinterpret_cast<UINT_PTR>(popup),
                    MenuText(menu.title).c_str());
    }
    return bar;
}

/** Text in UTF-8 from the platform's wide form.
    @param wide  the text, in UTF-16
    @return      the same text in UTF-8; empty for empty text */
std::string Utf8Of(const std::wstring &wide)
{
    if (wide.empty())
    {
        return {};
    }
    const int length =
        WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), utf8.data(), length, nullptr, nullptr);
    return utf8;
}

/** The process aware of each monitor's DPI (Windows 10 1703 on), else of the system's: looked up, not linked. */
void BecomeDpiAware()
{
    using SetContext             = BOOL(WINAPI *)(HANDLE);
    HMODULE          user32      = GetModuleHandleW(L"user32.dll");
    const SetContext set_context = user32 != nullptr ? reinterpret_cast<SetContext>(reinterpret_cast<void *>(
                                                           GetProcAddress(user32, "SetProcessDpiAwarenessContext")))
                                                     : nullptr;
    // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2, spelled as the SDK defines it, for an SDK too old to.
    if (set_context == nullptr || !set_context(reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(-4))))
    {
        SetProcessDPIAware();
    }
}

/** The system's DPI: the main monitor's when the session started.
    @return the dots per inch, 96 where the system gives none */
UINT SystemDpi()
{
    HDC       screen = GetDC(nullptr);
    const int dpi    = GetDeviceCaps(screen, LOGPIXELSX);
    ReleaseDC(nullptr, screen);
    return dpi > 0 ? static_cast<UINT>(dpi) : 96u;
}

/** A window's DPI (GetDpiForWindow, Windows 10 1607 on), else the system's.
    @param hwnd  the window; may be null, which gives the system's
    @return      the dots per inch */
UINT DpiOfWindow(HWND hwnd)
{
    using GetDpi              = UINT(WINAPI *)(HWND);
    static const auto get_dpi = reinterpret_cast<GetDpi>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    const UINT dpi = get_dpi != nullptr && hwnd != nullptr ? get_dpi(hwnd) : 0u;
    return dpi > 0 ? dpi : SystemDpi();
}

/** A monitor's DPI (GetDpiForMonitor's effective DPI, Windows 8.1 on), else the system's: looked up, not linked.
    @param monitor  the monitor; may be null, which gives the system's
    @return         the dots per inch */
UINT DpiOfMonitor(HMONITOR monitor)
{
    using GetDpi              = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
    static const auto get_dpi = []() -> GetDpi {
        HMODULE shcore = LoadLibraryW(L"shcore.dll");
        return shcore != nullptr
                   ? reinterpret_cast<GetDpi>(reinterpret_cast<void *>(GetProcAddress(shcore, "GetDpiForMonitor")))
                   : nullptr;
    }();
    UINT x = 0;
    UINT y = 0;
    if (get_dpi != nullptr && monitor != nullptr && get_dpi(monitor, 0 /* MDT_EFFECTIVE_DPI */, &x, &y) == S_OK &&
        x > 0)
    {
        return x;
    }
    return SystemDpi();
}

/** Every monitor's work area - what the task bar and docked toolbars leave of it - in screen pixels: the callback
    EnumDisplayMonitors calls for each monitor.
    @param monitor  the monitor
    @param areas    a pointer to the `std::vector<gfx::RectI>` the work area is appended to, as an LPARAM
    @return         TRUE, to go on to the next monitor */
BOOL CALLBACK AddWorkArea(HMONITOR monitor, HDC, LPRECT, LPARAM areas)
{
    MONITORINFO info{};
    info.cbSize = sizeof(info);
    if (GetMonitorInfoW(monitor, &info))
    {
        const RECT &r = info.rcWork;
        reinterpret_cast<std::vector<gfx::RectI> *>(areas)->push_back(
            {static_cast<int>(r.left), static_cast<int>(r.top), static_cast<int>(r.right - r.left),
             static_cast<int>(r.bottom - r.top)});
    }
    return TRUE;
}

/** The work area of every monitor.
    @return the work areas, in screen pixels, in the order the system enumerates the monitors */
std::vector<gfx::RectI> WorkAreas()
{
    std::vector<gfx::RectI> areas;
    EnumDisplayMonitors(nullptr, nullptr, AddWorkArea, reinterpret_cast<LPARAM>(&areas));
    return areas;
}

/** A system rectangle as the suite's.
    @param r  the rectangle, as left, top, right and bottom
    @return   the same rectangle as x, y, width and height */
gfx::RectI RectOf(const RECT &r)
{
    return {static_cast<int>(r.left), static_cast<int>(r.top), static_cast<int>(r.right - r.left),
            static_cast<int>(r.bottom - r.top)};
}

/** The outer size of a window of `style` whose client area is `width` x `height` pixels at `dpi`.
    @param style     the window style
    @param ex_style  the extended window style
    @param width     the client area's width, in pixels
    @param height    the client area's height, in pixels
    @param dpi       the dots per inch the frame's metrics are taken at
    @param menu      true for a window with a menu bar: a row of it over the client area, inside the frame
    @return          the window's width and height, in pixels, border, title bar and menu bar included */
SIZE OuterSize(DWORD style, DWORD ex_style, int width, int height, UINT dpi, bool menu)
{
    using Adjust             = BOOL(WINAPI *)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto adjust = reinterpret_cast<Adjust>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "AdjustWindowRectExForDpi")));
    RECT r{0, 0, width, height};
    if (adjust == nullptr || !adjust(&r, style, menu ? TRUE : FALSE, ex_style, dpi))
    {
        AdjustWindowRectEx(&r, style, menu ? TRUE : FALSE, ex_style);
    }
    return {r.right - r.left, r.bottom - r.top};
}

/** The style a top-level window of `options` has: captioned, a system menu, minimisable unless a dialog, a sizing
    frame where it is given a constraint.
    @param options  the window's options
    @return         the window style */
DWORD StyleOf(const WindowOptions &options)
{
    return WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN |
           (options.dialog ? 0u : static_cast<DWORD>(WS_MINIMIZEBOX)) |
           (options.constrain ? static_cast<DWORD>(WS_THICKFRAME) : 0u);
}

/** The extended style a top-level window of `options` has: a dialog's modal frame.
    @param options  the window's options
    @return         the extended window style */
DWORD ExStyleOf(const WindowOptions &options)
{
    return options.dialog ? static_cast<DWORD>(WS_EX_DLGMODALFRAME) : 0u;
}

/** The monitor a window of `options` opens on: the one where it stood, else the main one (an owner's is the caller's).
    @param options  the window's options
    @return         the monitor */
HMONITOR OpeningMonitor(const WindowOptions &options)
{
    return options.position ? MonitorFromPoint(POINT{options.position->first + 24, options.position->second + 12},
                                               MONITOR_DEFAULTTONEAREST)
                            : MonitorFromPoint(POINT{0, 0}, MONITOR_DEFAULTTOPRIMARY);
}

/** Whether a mouse button is down, as the thread's input reads it: the frame being dragged, which WM_SIZING holds.
    @return true while the left, right or middle button is down */
bool MouseButtonDown()
{
    return GetKeyState(VK_LBUTTON) < 0 || GetKeyState(VK_RBUTTON) < 0 || GetKeyState(VK_MBUTTON) < 0;
}

/** The procedure of the app's windows: hands each message to the WinWindow the window holds in GWLP_USERDATA.
    @param hwnd    the window
    @param message the message
    @param wparam  the message's first parameter
    @param lparam  the message's second parameter
    @return        the message's result */
LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);

/** The procedure of the message-only window: hands each message to the WinPlatform it holds in GWLP_USERDATA.
    @param hwnd    the window
    @param message the message
    @param wparam  the message's first parameter
    @param lparam  the message's second parameter
    @return        the message's result */
LRESULT CALLBACK MessageProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
} // namespace

/** What a window asks of its platform beyond its options: the app's ending, the shortcuts and the menu bar. */
struct WindowHooks
{
    std::function<void()> session_ending; ///< the app's ending, run when the session ends (WM_ENDSESSION)
    /** A key no view took offered to the menu bar's shortcuts: true when it was one. */
    std::function<bool(const Shortcut &)> shortcut;
    std::function<void(int command)>      command;       ///< a menu bar item chosen: its command number
    std::function<void()>                 gone;          ///< the window is being destroyed
    HMENU                                 menu{nullptr}; ///< the menu bar the window opens with, which it owns; null
};

/** A top-level window of the app: its client area the design size at the DPI of the monitor it is on, the content a
    child window is made in, sized again when it moves to a monitor of another DPI and held to its owner's constraint
    when resized. Made by WinPlatform::OpenWindow; its HWND is null when Windows would not make one. Main thread. */
class WinWindow final : public AppWindow
{
  public:
    /** Create the window and show it: where it stood, else centred over its owner, else on the main monitor.
        @param options  the title, the content's design size, the owner, the constraint and the callbacks
        @param hooks    the app's ending, the shortcuts, the menu bar's commands and the menu bar it opens with */
    WinWindow(const WindowOptions &options, WindowHooks hooks)
        : closing_(options.closing), rescaled_(options.rescaled), constrain_(options.constrain),
          resized_(options.resized), session_ending_(std::move(hooks.session_ending)),
          shortcut_(std::move(hooks.shortcut)), command_(std::move(hooks.command)), gone_(std::move(hooks.gone)),
          dialog_(options.dialog), design_width_(options.width), design_height_(options.height),
          style_(StyleOf(options)), ex_style_(ExStyleOf(options)), has_menu_(hooks.menu != nullptr)
    {
        HWND owner = nullptr;
        if (const auto *o = dynamic_cast<const WinWindow *>(options.owner))
        {
            owner = o->hwnd_; // an owned window: over its owner, and gone with it
        }

        // The monitor it opens on, and so its DPI and its size: the one where it stood, else its owner's, else the
        // main one. Then its place: where it stood, wholly on the monitor it is most on; else centred over its owner,
        // inside the owner's monitor; else centred on the main monitor's work area.
        const HMONITOR monitor              = options.position || owner == nullptr
                                                  ? OpeningMonitor(options)
                                                  : MonitorFromWindow(owner, MONITOR_DEFAULTTONEAREST);
        dpi_                                = DpiOfMonitor(monitor);
        const SIZE                    outer = Outer();
        const std::vector<gfx::RectI> areas = WorkAreas();
        gfx::RectI                    frame{0, 0, static_cast<int>(outer.cx), static_cast<int>(outer.cy)};
        if (options.position)
        {
            frame = PlaceWithin(frame.WithPosition(options.position->first, options.position->second), areas);
        }
        else if (owner != nullptr)
        {
            RECT over{};
            GetWindowRect(owner, &over);
            frame = CentredOver(RectOf(over), {frame.width(), frame.height()}, areas);
        }
        else
        {
            MONITORINFO info{};
            info.cbSize = sizeof(info);
            GetMonitorInfoW(monitor, &info);
            frame = frame.WithPosition(static_cast<int>((info.rcWork.left + info.rcWork.right - outer.cx) / 2),
                                       static_cast<int>((info.rcWork.top + info.rcWork.bottom - outer.cy) / 2));
        }
        const std::wstring title = WideOf(options.title);
        hwnd_ = CreateWindowExW(ex_style_, kWindowClass, title.c_str(), style_, frame.x(), frame.y(), frame.width(),
                                frame.height(), owner, hooks.menu, GetModuleHandleW(nullptr), this);
        if (hwnd_ == nullptr)
        {
            if (hooks.menu != nullptr)
            {
                DestroyMenu(hooks.menu); // no window took it
            }
            return;
        }
        // A window takes the DPI of the monitor it is created on: sized again where that is not the one planned for
        // (a place pulled onto another monitor).
        if (const UINT dpi = DpiOfWindow(hwnd_); dpi != dpi_)
        {
            dpi_ = dpi;
            SetOwnSize(0, 0, Outer(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        normal_position_ = {frame.x(), frame.y()};
        ShowWindow(hwnd_, SW_SHOW);
        SetForegroundWindow(hwnd_);
    }

    /** Tell the platform it goes, detach the window procedure's user data and destroy the window, its menu bar with
        it. */
    ~WinWindow() override
    {
        if (gone_)
        {
            gone_();
        }
        if (hwnd_ != nullptr)
        {
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
        }
    }

    void *Handle() const override
    {
        return hwnd_;
    }

    float Scale() const override
    {
        return static_cast<float>(dpi_) / 96.0f;
    }

    std::pair<int, int> Position() const override
    {
        if (IsIconic(hwnd_))
        {
            return normal_position_; // minimised, it stands at -32000: where it stood before
        }
        RECT r{};
        GetWindowRect(hwnd_, &r);
        return {static_cast<int>(r.left), static_cast<int>(r.top)};
    }

    void BringToFront() override
    {
        if (IsIconic(hwnd_))
        {
            ShowWindow(hwnd_, SW_RESTORE);
        }
        SetForegroundWindow(hwnd_);
    }

    void SetContentSize(int width, int height) override
    {
        design_width_  = width;
        design_height_ = height;
        SetOwnSize(0, 0, Outer(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void SetContentSizeOnScreen(int width, int height) override
    {
        if (IsZoomed(hwnd_) || IsIconic(hwnd_))
        {
            ShowWindow(hwnd_, SW_RESTORE); // a maximised or minimised window's size is not its own to set
        }
        // The work area the window is most on now, before it grows: the frame at the new size moved onto it, in from
        // any edge it would cross. WM_SIZE tells the owner the size taken, as a drag's does.
        const gfx::RectI area = WorkAreaOfWindow();
        RECT             now{};
        GetWindowRect(hwnd_, &now);
        design_width_          = width;
        design_height_         = height;
        const SIZE       outer = Outer();
        const gfx::RectI frame = MovedOnto({static_cast<int>(now.left), static_cast<int>(now.top),
                                            static_cast<int>(outer.cx), static_cast<int>(outer.cy)},
                                           area);
        SetOwnSize(frame.x(), frame.y(), outer, SWP_NOZORDER | SWP_NOACTIVATE);
    }

    gfx::PointI LargestContent() const override
    {
        // The work area less the frame's own border, title bar and menu bar, in the monitor's pixels; in design units
        // at the window's scale, rounded down so the content it gives fits.
        const gfx::RectI area   = WorkAreaOfWindow();
        const SIZE       border = OuterSize(style_, ex_style_, 0, 0, dpi_, has_menu_);
        const float      scale  = Scale();
        return {std::max(1, static_cast<int>(std::floor(static_cast<float>(area.width() - border.cx) / scale))),
                std::max(1, static_cast<int>(std::floor(static_cast<float>(area.height() - border.cy) / scale)))};
    }

    /** The window's menu bar replaced by `menu`, the one before destroyed, and the frame sized again round the
        content, which keeps its size.
        @param menu  the new bar, which the window owns from now on; null for none */
    void SetMenuBar(HMENU menu)
    {
        if (hwnd_ == nullptr)
        {
            if (menu != nullptr)
            {
                DestroyMenu(menu);
            }
            return;
        }
        HMENU old = GetMenu(hwnd_);
        ::SetMenu(hwnd_, menu);
        if (old != nullptr)
        {
            DestroyMenu(old);
        }
        if (has_menu_ != (menu != nullptr))
        {
            has_menu_ = menu != nullptr;
            SetOwnSize(0, 0, Outer(), SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
        DrawMenuBar(hwnd_);
    }

    void SetTitle(const std::string &title) override
    {
        SetWindowTextW(hwnd_, WideOf(title).c_str());
    }

    std::string Title() const override
    {
        const int    length = GetWindowTextLengthW(hwnd_);
        std::wstring wide(static_cast<std::size_t>(length) + 1, L'\0');
        wide.resize(static_cast<std::size_t>(GetWindowTextW(hwnd_, wide.data(), length + 1)));
        return Utf8Of(wide);
    }

    /** The window's messages: a close asked for, Escape on a dialog, a move, a monitor of another DPI, the session's
        end; the rest Windows'.
        @param message  the message
        @param wparam   the message's first parameter
        @param lparam   the message's second parameter
        @return         the message's result */
    LRESULT Message(UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_CLOSE:
            Closing();
            return 0; // the owner destroys the window when it is done with it
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN:
            if (KeyDown(wparam))
            {
                return 0;
            }
            break; // a key down with Alt, unused, is the window's own: Alt and F10 to the bar, Alt+F4
        case WM_CHAR:
            if (Character(wparam, lparam))
            {
                return 0;
            }
            break;
        case window::win::kHandedKeyDown:
            return KeyDown(wparam) ? 1 : 0; // from the child window, which keeps the key where this answers 0
        case window::win::kHandedChar:
            return Character(wparam, lparam) ? 1 : 0;
        case WM_COMMAND:
            // A menu bar item (a command's id, from a menu: lparam 0, the high word 0), else Windows'.
            if (lparam == 0 && HIWORD(wparam) == 0 && LOWORD(wparam) >= kFirstCommand && command_)
            {
                const auto command = command_;
                command(static_cast<int>(LOWORD(wparam) - kFirstCommand));
                return 0;
            }
            break;
        case WM_ERASEBKGND:
            return 1; // the child window covers the client area
        case WM_SIZING:
            if (constrain_)
            {
                // The frame dragged to, held, the edge or corner being dragged moving and its opposite staying.
                auto      *frame = reinterpret_cast<RECT *>(lparam);
                const SIZE held  = Held(frame->right - frame->left, frame->bottom - frame->top);
                const auto edge  = static_cast<int>(wparam);
                if (edge == WMSZ_LEFT || edge == WMSZ_TOPLEFT || edge == WMSZ_BOTTOMLEFT)
                {
                    frame->left = frame->right - held.cx;
                }
                else
                {
                    frame->right = frame->left + held.cx;
                }
                if (edge == WMSZ_TOP || edge == WMSZ_TOPLEFT || edge == WMSZ_TOPRIGHT)
                {
                    frame->top = frame->bottom - held.cy;
                }
                else
                {
                    frame->bottom = frame->top + held.cy;
                }
                return TRUE;
            }
            break;
        case WM_WINDOWPOSCHANGING:
            if (constrain_ && !setting_size_)
            {
                // A size given some other way - a snap to a side of the screen, another program's - held as a drag is,
                // its top left where it was asked for, as a JUCE Standalone's window holds it: not while a mouse button
                // is down (WM_SIZING holds the drag), nor minimised, nor a size of the window's own.
                auto *place = reinterpret_cast<WINDOWPOS *>(lparam);
                if ((place->flags & SWP_NOSIZE) == 0 && place->x > -32000 && place->y > -32000 && !MouseButtonDown())
                {
                    const SIZE held = Held(place->cx, place->cy);
                    place->cx       = held.cx;
                    place->cy       = held.cy;
                }
            }
            break;
        case WM_SIZE:
            if (resized_ && static_cast<int>(wparam) != SIZE_MINIMIZED)
            {
                // The client area the window took, in design units at its scale: the content's new size.
                const float scale = Scale();
                design_width_     = std::max(1, static_cast<int>(std::lround(LOWORD(lparam) / scale)));
                design_height_    = std::max(1, static_cast<int>(std::lround(HIWORD(lparam) / scale)));
                const auto f      = resized_;
                f(design_width_, design_height_);
                return 0;
            }
            break;
        case WM_MOVE:
            if (!IsIconic(hwnd_))
            {
                RECT r{};
                GetWindowRect(hwnd_, &r);
                normal_position_ = {static_cast<int>(r.left), static_cast<int>(r.top)};
            }
            break;
        case WM_DPICHANGED: {
            // Moved onto a monitor of another DPI: the client area the design size at it, the frame's top left where
            // Windows suggests (so a window dragged across stays under the pointer), and the drawing told.
            dpi_                  = LOWORD(wparam);
            const auto *suggested = reinterpret_cast<const RECT *>(lparam);
            SetOwnSize(suggested->left, suggested->top, Outer(), SWP_NOZORDER | SWP_NOACTIVATE);
            if (rescaled_)
            {
                const auto f = rescaled_;
                f(Scale());
            }
            return 0;
        }
        case WM_QUERYENDSESSION:
            return TRUE; // the app saves what it keeps when the session does end
        case WM_ENDSESSION:
            // A log off, a shut down, an update's restart: sent to the top-level windows alone, and the process may
            // end as soon as it is answered, so the app writes what it keeps before it returns.
            if (wparam != FALSE && session_ending_)
            {
                const auto f = session_ending_;
                f();
            }
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    /** Take the handle Windows is creating for this window, before CreateWindowExW returns (WM_NCCREATE).
        @param hwnd  the window */
    void Adopt(HWND hwnd) noexcept
    {
        hwnd_ = hwnd;
    }

  private:
    /** Tell the app the person closed the window, when it asked to be told. */
    void Closing()
    {
        if (closing_)
        {
            const auto f = closing_;
            f();
        }
    }

    /** A key down for this window, its own or handed on by the child window in it: Escape closing a dialog, else the
        key offered to the shortcuts - but a key that types, with neither Ctrl nor Alt, waits for its character
        (Character), which the views may take first.
        @param vk  the virtual-key code
        @return    true when the key was used */
    bool KeyDown(WPARAM vk)
    {
        if (dialog_ && vk == VK_ESCAPE && GetKeyState(VK_MENU) >= 0)
        {
            Closing();
            return true;
        }
        if (!shortcut_)
        {
            return false;
        }
        const Shortcut stroke = StrokeOf(vk);
        if (stroke.empty() || (TypesCharacter(stroke) && !stroke.primary && !stroke.alt))
        {
            return false;
        }
        const auto shortcut = shortcut_;
        return shortcut(stroke);
    }

    /** A character for this window, its own or handed on: the key that typed it offered to the shortcuts, read back
        from its scan code so it is named as its key down would be - but not with Ctrl or Alt held, whose key down
        was offered already (and with both, AltGr typing a character of its own).
        @param wparam  the character, UTF-16
        @param lparam  the message's flags, the scan code in bits 16 to 23
        @return        true when the key was used */
    bool Character(WPARAM wparam, LPARAM lparam)
    {
        if (!shortcut_ || wparam < 32 || GetKeyState(VK_CONTROL) < 0 || GetKeyState(VK_MENU) < 0)
        {
            return false;
        }
        const UINT     scan   = static_cast<UINT>((lparam >> 16) & 0xFF);
        const Shortcut stroke = StrokeOf(MapVirtualKeyW(scan, MAPVK_VSC_TO_VK));
        if (!TypesCharacter(stroke))
        {
            return false;
        }
        const auto shortcut = shortcut_;
        return shortcut(stroke);
    }

    /** The work area of the monitor the window is most on: what the task bar and docked toolbars leave of it.
        @return the area, in screen pixels */
    gfx::RectI WorkAreaOfWindow() const
    {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        GetMonitorInfoW(MonitorFromWindow(hwnd_, MONITOR_DEFAULTTONEAREST), &info);
        return RectOf(info.rcWork);
    }

    /** The outer size of a client area the design size at the window's DPI.
        @return the window's width and height, in pixels, border and title bar included */
    SIZE Outer() const
    {
        const float scale = static_cast<float>(dpi_) / 96.0f;
        return OuterSize(style_, ex_style_, static_cast<int>(std::lround(static_cast<float>(design_width_) * scale)),
                         static_cast<int>(std::lround(static_cast<float>(design_height_) * scale)), dpi_, has_menu_);
    }

    /** A frame `width` x `height` pixels held: the client area it would hold, held to the owner's constraint in design
        units at the window's scale, and the frame round that.
        @param width   the frame's width, in pixels
        @param height  the frame's height, in pixels
        @return        the frame's width and height held to the constraint, in pixels */
    SIZE Held(LONG width, LONG height) const
    {
        const SIZE        border = OuterSize(style_, ex_style_, 0, 0, dpi_, has_menu_);
        const float       scale  = Scale();
        const gfx::PointF held =
            constrain_({static_cast<float>(width - border.cx) / scale, static_cast<float>(height - border.cy) / scale});
        return {border.cx + static_cast<LONG>(std::lround(held.x * scale)),
                border.cy + static_cast<LONG>(std::lround(held.y * scale))};
    }

    /** The window at a size of its own - the owner's content size, or the design size at a new DPI - which the
        constraint does not hold again (WM_WINDOWPOSCHANGING): its rounding could move it by a pixel.
        @param x      the frame's left, in screen pixels; ignored under SWP_NOMOVE
        @param y      the frame's top, in screen pixels; ignored under SWP_NOMOVE
        @param outer  the frame's width and height, in pixels
        @param flags  SetWindowPos's flags */
    void SetOwnSize(int x, int y, SIZE outer, UINT flags)
    {
        setting_size_ = true;
        SetWindowPos(hwnd_, nullptr, x, y, outer.cx, outer.cy, flags);
        setting_size_ = false;
    }

    HWND                       hwnd_{nullptr}; ///< the window; null when it was not made.
    UINT                       dpi_{96};       ///< the dots per inch of the monitor the window is on.
    std::function<void()>      closing_;       ///< what the app does when the person closes the window; may be empty.
    std::function<void(float)> rescaled_;      ///< told the new scale after a move to a monitor of another DPI.
    /** The owner's constraint on the content size, in design units; empty for a window of one size. */
    std::function<gfx::PointF(gfx::PointF)> constrain_;
    std::function<void(int, int)>           resized_;        ///< told the content's design size after each resize.
    std::function<void()>                   session_ending_; ///< the app's ending, run when the session ends.
    std::function<bool(const Shortcut &)>   shortcut_; ///< a key offered to the menu bar's shortcuts; may be empty.
    std::function<void(int)>                command_;  ///< a menu bar item chosen, by its command number; may be empty.
    std::function<void()>                   gone_;     ///< told as the window is destroyed; may be empty.
    bool                                    dialog_;   ///< a dialog: Escape closes it.
    int                                     design_width_;          ///< the content's width, in design pixels.
    int                                     design_height_;         ///< the content's height, in design pixels.
    DWORD                                   style_;                 ///< the window style.
    DWORD                                   ex_style_;              ///< the extended window style.
    bool                                    has_menu_;              ///< the window carries a menu bar.
    std::pair<int, int>                     normal_position_{0, 0}; ///< where it stood when last it was not minimised
    bool                                    setting_size_{false};   ///< inside SetOwnSize
};

/** The Standalone's platform on Windows: the process made DPI aware, a class for the app's windows, a message-only
    window for the timers and the queued calls, and the message loop. Made by AppPlatform::Create. Main thread. */
class WinPlatform final : public AppPlatform
{
  public:
    /** Declare the process DPI aware, register the two window classes and make the message-only window. */
    WinPlatform()
    {
        BecomeDpiAware();
        HINSTANCE   instance = GetModuleHandleW(nullptr);
        WNDCLASSEXW windows{};
        windows.cbSize      = sizeof(windows);
        windows.lpfnWndProc = WindowProcedure;
        windows.hInstance   = instance;
        windows.hCursor     = LoadCursorW(nullptr, IDC_ARROW);
        windows.hIcon       = LoadIconW(instance, MAKEINTRESOURCEW(kIconResource));
        windows.hIconSm = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(kIconResource), IMAGE_ICON,
                                                        GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON),
                                                        LR_DEFAULTCOLOR));
        windows.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
        windows.lpszClassName = kWindowClass;
        RegisterClassExW(&windows);

        WNDCLASSEXW messages{};
        messages.cbSize        = sizeof(messages);
        messages.lpfnWndProc   = MessageProcedure;
        messages.hInstance     = instance;
        messages.lpszClassName = kMessageClass;
        RegisterClassExW(&messages);
        messages_ = CreateWindowExW(0, kMessageClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, instance, this);
        if (messages_ != nullptr)
        {
            // Under the queue's lock, so never once Close has returned: the window outlives every wake.
            posts_->SetWake([window = messages_] { PostMessageW(window, kPosted, 0, 0); });
        }
    }

    /** Close the posts' queue, kill every timer and destroy the message-only window the app's timers and calls ride
        on. */
    ~WinPlatform() override
    {
        posts_->Close();
        if (messages_ != nullptr)
        {
            for (const auto &timer : timers_)
            {
                KillTimer(messages_, timer.first);
            }
            SetWindowLongPtrW(messages_, GWLP_USERDATA, 0);
            DestroyWindow(messages_);
        }
    }

    gfx::PointI LargestContent(const WindowOptions &options) const override
    {
        // In the monitor's pixels, where the frame's metrics are; the content back in design units at its DPI.
        const HMONITOR monitor = OpeningMonitor(options);
        MONITORINFO    info{};
        info.cbSize = sizeof(info);
        if (monitor == nullptr || !GetMonitorInfoW(monitor, &info))
        {
            return {0, 0};
        }
        const UINT        dpi   = DpiOfMonitor(monitor);
        const float       scale = static_cast<float>(dpi) / 96.0f;
        const gfx::PointI content{static_cast<int>(std::lround(static_cast<float>(options.width) * scale)),
                                  static_cast<int>(std::lround(static_cast<float>(options.height) * scale))};
        const SIZE        outer = OuterSize(StyleOf(options), ExStyleOf(options), content.x, content.y, dpi,
                                            options.menu_bar && !menu_->bar().menus.empty());
        const gfx::PointI fitted =
            FittedContent(content, {static_cast<int>(outer.cx) - content.x, static_cast<int>(outer.cy) - content.y},
                          RectOf(info.rcWork));
        return {static_cast<int>(static_cast<float>(fitted.x) / scale),
                static_cast<int>(static_cast<float>(fitted.y) / scale)};
    }

    std::unique_ptr<AppWindow> OpenWindow(const WindowOptions &options) override
    {
        WindowHooks hooks;
        hooks.session_ending = [this] {
            if (quit_handler_)
            {
                const auto f = quit_handler_;
                f();
            }
        };
        // Every window takes the shortcuts, as a Mac's menu bar serves every window of the app; the bar itself rides
        // on the windows that ask for it.
        const std::shared_ptr<MenuBarController> menu = menu_;
        hooks.shortcut                  = [menu](const Shortcut &pressed) { return menu->PressShortcut(pressed); };
        hooks.command                   = [menu](int command) { menu->Choose(command); };
        auto                      made  = std::make_shared<WinWindow *>(nullptr);
        const std::weak_ptr<bool> alive = alive_;
        hooks.gone                      = [this, made, alive] {
            if (!alive.expired())
            {
                std::erase(menu_windows_, *made);
            }
        };
        hooks.menu  = options.menu_bar ? MenuBarOf(*menu_) : nullptr;
        auto window = std::make_unique<WinWindow>(options, std::move(hooks));
        *made       = window.get();
        if (window->Handle() == nullptr)
        {
            return nullptr;
        }
        if (options.menu_bar)
        {
            menu_windows_.push_back(window.get());
        }
        return window;
    }

    /** The bar set, and shown anew on every window that carries it.
        @param bar     the menus
        @param chosen  told each chosen entry's id */
    void SetMenuBar(const MenuBar &bar, std::function<void(int id)> chosen) override
    {
        menu_->Set(bar, std::move(chosen));
        for (WinWindow *window : menu_windows_)
        {
            window->SetMenuBar(MenuBarOf(*menu_));
        }
    }

    /** Windows shows the bar on each window that asks for it.
        @return  true */
    bool HasNativeMenuBar() const override
    {
        return true;
    }

    std::unique_ptr<window::RunLoop> ChildLoop() override
    {
        return nullptr; // the message loop runs the child windows' messages and timers
    }

    void StartTimer(int milliseconds, std::function<void()> tick) override
    {
        const UINT_PTR id = next_timer_++;
        timers_[id]       = std::move(tick);
        SetTimer(messages_, id, static_cast<UINT>(std::max(milliseconds, 1)), nullptr);
    }

    void CallSoon(std::function<void()> call) override
    {
        calls_.push_back(std::move(call));
        PostMessageW(messages_, kCallSoon, 0, 0);
    }

    void SetMenu(const std::vector<view::MenuItem> &, std::function<void(int)>) override
    {
        // The window's bar holds the app's commands.
    }

    std::shared_ptr<core::PostQueue> Posts() override
    {
        return posts_;
    }

    bool SetClipboardText(const std::string &text) override
    {
        return messages_ != nullptr && window::win::WriteClipboardText(messages_, text);
    }

    std::string ClipboardText() override
    {
        return messages_ != nullptr ? window::win::ReadClipboardText(messages_) : std::string();
    }

    int Run() override
    {
        MSG message{};
        while (GetMessageW(&message, nullptr, 0, 0) > 0)
        {
            TranslateMessage(&message);
            DispatchMessageW(&message);
        }
        posts_->Close(); // the app has quit: what other threads post from here on never runs
        return static_cast<int>(message.wParam);
    }

    void Quit() override
    {
        PostQuitMessage(0);
    }

    void SetQuitHandler(std::function<void()> handler) override
    {
        quit_handler_ = std::move(handler);
    }

    /** The message-only window's messages: a timer's tick, the queued calls; the rest Windows'.
        @param hwnd     the message-only window
        @param message  the message
        @param wparam   the message's first parameter
        @param lparam   the message's second parameter
        @return         the message's result */
    LRESULT HandleMessage(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_TIMER: {
            const auto it = timers_.find(static_cast<UINT_PTR>(wparam));
            if (it != timers_.end())
            {
                it->second();
            }
            return 0;
        }
        case kCallSoon:
            while (!calls_.empty())
            {
                auto call = std::move(calls_.front());
                calls_.pop_front();
                call();
            }
            return 0;
        case kPosted:
            posts_->RunPending();
            return 0;
        default:
            break;
        }
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }

  private:
    /** The calls other threads post (Posts): woken by kPosted on the message-only window, closed when Run returns. */
    std::shared_ptr<core::PostQueue>          posts_{std::make_shared<core::PostQueue>()};
    HWND                                      messages_{nullptr}; ///< the message-only window.
    std::map<UINT_PTR, std::function<void()>> timers_;            ///< the timers StartTimer made, by timer id.
    UINT_PTR                                  next_timer_{1};     ///< the id the next timer takes.
    std::deque<std::function<void()>>         calls_;             ///< the calls CallSoon queued, in order.
    std::function<void()> quit_handler_; ///< the app's ending, run when the session ends; may be empty.
    /** The menu bar and its choosing, shared with every window's shortcuts and commands. */
    std::shared_ptr<MenuBarController> menu_{std::make_shared<MenuBarController>()};
    std::vector<WinWindow *>           menu_windows_; ///< the windows that carry the bar, not owned
    std::shared_ptr<bool>              alive_{std::make_shared<bool>(true)}; ///< expires with the platform
};

namespace
{
LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
        auto       *window = static_cast<WinWindow *>(create->lpCreateParams);
        window->Adopt(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(window));
    }
    if (auto *window = reinterpret_cast<WinWindow *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
    {
        return window->Message(message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}

LRESULT CALLBACK MessageProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        const auto *create = reinterpret_cast<const CREATESTRUCTW *>(lparam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    if (auto *platform = reinterpret_cast<WinPlatform *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA)))
    {
        return platform->HandleMessage(hwnd, message, wparam, lparam);
    }
    return DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace

std::unique_ptr<AppPlatform> AppPlatform::Create(const std::string &, std::span<const std::uint8_t>)
{
    return std::make_unique<WinPlatform>(); // the windows' icon is the executable's
}
} // namespace reckless::app
