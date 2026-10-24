// The child window on Windows: a child HWND of the host's window, drawing a view tree through Skia into a raster the
// size of the window and putting it on screen with SetDIBitsToDevice in WM_PAINT, the mouse, the keys and dropped
// files handed to a MouseRouter, and a window timer for the frame callback. A popup is the same surface in a
// no-activate tool window over the host's, watched by a thread mouse hook for presses outside it. The window class is
// the module's own, registered with the first window and unregistered with the last, so two plug-ins' copies of this
// code never share a window procedure. Screen points are the screen's pixels, opaque to the views.
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cwchar>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "include/core/SkCanvas.h"
#include "include/core/SkSurface.h"

#include "SkiaCanvas.h"
#include "Window.h"

// windows.h last: it defines macros (DrawText, CreateWindow, min, max) that would reach into the headers above.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
// The wide forms throughout: the calls here name them, and the cursor ids (IDC_ARROW) become the wide resource names
// LoadCursorW takes.
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>

#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>

#include "ClipboardWin.h"
#include "HandedKeys.h"

namespace reckless::window
{
namespace
{
/** The id of the window timer that drives the frame callback. */
constexpr UINT_PTR kFrameTimer = 1;

/** The frame callback's tick, in milliseconds: about 60 Hz. */
constexpr UINT kFrameMilliseconds = 16;

/** One click of a wheel (WHEEL_DELTA), in the views' pixels. */
constexpr float kNotchPixels = 20.0f;

/** The message posted to the window when a CallSoon's call is waiting. */
constexpr UINT kCallSoonMessage = WM_APP + 17;

/** The message posted to the window when a file dialog is waiting to open. */
constexpr UINT kChooseFileMessage = WM_APP + 18;

/** The module this code is linked into: its window class is registered against it.
    @return the module's instance handle */
HINSTANCE ModuleInstance() noexcept
{
    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&ModuleInstance), &module);
    return module;
}

/** The class name, unique to the module, so another plug-in's copy of this code registers a class of its own.
    @return the name, made on the first call and kept for the life of the process */
const wchar_t *ClassName() noexcept
{
    static wchar_t name[64]{};
    if (name[0] == L'\0')
    {
        std::swprintf(name, std::size(name), L"RecklessPanel%p", static_cast<void *>(ModuleInstance()));
    }
    return name;
}

/** The windows of the module alive: the class is registered with the first and unregistered with the last. */
std::atomic<int> window_count{0};

/** The modifier keys held, as the thread's input reads them.
    @return the Shift, Control and Alt states */
view::Modifiers CurrentModifiers() noexcept
{
    return {GetKeyState(VK_SHIFT) < 0, GetKeyState(VK_CONTROL) < 0, GetKeyState(VK_MENU) < 0};
}

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

/** Text from the platform's wide form.
    @param wide  the text in UTF-16, null-terminated; may be null
    @return      the same text in UTF-8; empty for null or empty text */
std::string Utf8Of(const wchar_t *wide)
{
    if (wide == nullptr || *wide == L'\0')
    {
        return {};
    }
    const int   length = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    std::string utf8(static_cast<std::size_t>(length > 0 ? length - 1 : 0), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8.data(), length, nullptr, nullptr);
    return utf8;
}

/** The system cursor a view's cursor kind stands for.
    @param cursor  the cursor a view asked for
    @return        the system cursor's resource id; the arrow for kArrow, the hand for the grab kinds */
LPCWSTR CursorFor(view::Cursor cursor)
{
    switch (cursor)
    {
    case view::Cursor::kPointingHand:
    case view::Cursor::kGrab:
    case view::Cursor::kGrabbing:
        return IDC_HAND;
    case view::Cursor::kIBeam:
        return IDC_IBEAM;
    case view::Cursor::kUpDown:
        return IDC_SIZENS;
    case view::Cursor::kLeftRight:
        return IDC_SIZEWE;
    case view::Cursor::kCrosshair:
        return IDC_CROSS;
    case view::Cursor::kArrow:
        break;
    }
    return IDC_ARROW;
}

/** A named key from its virtual-key code; Key::kNone for one that types a character (WM_CHAR brings that).
    @param code  the virtual-key code, a key-down message's wparam
    @return      the key, or Key::kNone */
view::Key NamedKey(WPARAM code)
{
    switch (code)
    {
    case VK_ESCAPE:
        return view::Key::kEscape;
    case VK_RETURN:
        return view::Key::kReturn;
    case VK_TAB:
        return view::Key::kTab;
    case VK_BACK:
        return view::Key::kBackspace;
    case VK_DELETE:
        return view::Key::kDelete;
    case VK_LEFT:
        return view::Key::kLeft;
    case VK_RIGHT:
        return view::Key::kRight;
    case VK_UP:
        return view::Key::kUp;
    case VK_DOWN:
        return view::Key::kDown;
    case VK_HOME:
        return view::Key::kHome;
    case VK_END:
        return view::Key::kEnd;
    case VK_PRIOR:
        return view::Key::kPageUp;
    case VK_NEXT:
        return view::Key::kPageDown;
    default:
        return view::Key::kNone;
    }
}

/** A surface's life as a file dialog's caller reads it: the dialog runs a modal loop of its own, in which the host
    may close the editor - the surface, the panel and every view between them gone when it returns - so the caller
    holds this, which outlives the surface, and reads it before touching anything. The surface marks itself gone here,
    and closes the dialog up, as it goes. */
struct SurfaceLife
{
    bool         gone{false};        ///< true once the surface has let go.
    IFileDialog *dialog_up{nullptr}; ///< the file dialog while it shows, else null.
};

/** The shell's file dialog for `request`, modal over `owner`; the path chosen, or empty. `life.dialog_up` is the
    dialog while it shows.
    @param request  the title, the patterns, the folder and whether it saves
    @param owner    the window the dialog is modal over
    @param life     the surface's life, whose dialog_up is set for the dialog's loop
    @return         the path chosen, in UTF-8; empty for a cancel or a failure */
std::string RunFileDialog(const view::FileRequest &request, HWND owner, SurfaceLife &life)
{
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    std::string   result;
    IFileDialog  *dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(request.save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                                   CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    {
        if (!request.title.empty())
        {
            dialog->SetTitle(WideOf(request.title).c_str());
        }
        std::wstring spec;
        for (const auto &pattern : request.patterns)
        {
            spec += (spec.empty() ? L"" : L";") + WideOf(pattern);
        }
        if (!spec.empty())
        {
            const COMDLG_FILTERSPEC filter{L"Files", spec.c_str()};
            dialog->SetFileTypes(1, &filter);
        }
        if (request.save && !request.file_name.empty())
        {
            dialog->SetFileName(WideOf(request.file_name).c_str());
        }
        if (!request.folder.empty())
        {
            IShellItem *folder = nullptr;
            if (SUCCEEDED(SHCreateItemFromParsingName(WideOf(request.folder).c_str(), nullptr, IID_PPV_ARGS(&folder))))
            {
                dialog->SetFolder(folder);
                folder->Release();
            }
        }
        life.dialog_up      = dialog;
        const HRESULT shown = dialog->Show(owner);
        life.dialog_up      = nullptr;
        if (SUCCEEDED(shown))
        {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)))
            {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)))
                {
                    result = Utf8Of(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }
        dialog->Release();
    }
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
    return result;
}

/** The window procedure of the module's class: hands each message to the WinSurface the window holds in
    GWLP_USERDATA.
    @param hwnd     the window
    @param message  the message
    @param wparam   the message's first parameter
    @param lparam   the message's second parameter
    @return         the message's result */
LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam);
} // namespace

class WinPopup;

/** A view tree drawn in an HWND: the raster, the drawing, the mouse, keys and drops to a router, and the host the tree
    lives in. The child window and every popup are one. */
class WinSurface : public view::Host
{
  public:
    /** A surface for `root`, with no window until CreateSurface.
        @param root   the view tree to draw; outlives the surface
        @param scale  window pixels per design pixel */
    WinSurface(view::View &root, float scale) : root_(root), router_(root, this), scale_(scale)
    {
    }

    /** The surface released (Release): its popups closed, the tree's host dropped, the window gone. */
    ~WinSurface() override
    {
        Release();
    }

    /** Every popup opened over the surface closed, and theirs, each `closed_by_window` gathered into `heard` for
        whoever let the surface go to run once everything is closed.
        @param heard  the `closed_by_window` callbacks of the popups closed, appended to */
    void EndPopups(std::vector<std::function<void()>> &heard);

    /** `popup`, going on its own, off the list of those opened over the surface.
        @param popup  the popup being destroyed */
    void Forget(const WinPopup *popup)
    {
        popups_.erase(std::remove(popups_.begin(), popups_.end(), popup), popups_.end());
    }

    /** The window, with `style` and `ex_style`, at `x`, `y` in `parent`'s client (or the screen for a popup), the
        design size times the scale; false when Windows refuses it.
        @param parent    the parent or owner window
        @param style     the window style
        @param ex_style  the extended window style
        @param x         the window's left, in `parent`'s client pixels or screen pixels for a popup
        @param y         the window's top, in the same units
        @return          true when the window was made */
    bool CreateSurface(HWND parent, DWORD style, DWORD ex_style, int x, int y)
    {
        if (window_count++ == 0)
        {
            WNDCLASSEXW window_class{};
            window_class.cbSize        = sizeof(window_class);
            window_class.style         = CS_DBLCLKS; // a second press soon after the first arrives as a double click
            window_class.lpfnWndProc   = WindowProcedure;
            window_class.hInstance     = ModuleInstance();
            window_class.hCursor       = nullptr; // WM_SETCURSOR sets the views' shape
            window_class.lpszClassName = ClassName();
            RegisterClassExW(&window_class);
        }
        registered_ = true;
        ResizeRaster();
        hwnd_ = CreateWindowExW(ex_style, ClassName(), L"", style, x, y, pixel_width_, pixel_height_, parent, nullptr,
                                ModuleInstance(), this);
        if (hwnd_ == nullptr)
        {
            return false;
        }
        DragAcceptFiles(hwnd_, TRUE);
        root_.SetHost(this);
        return true;
    }

    /** The handle, from the window's first message on: messages arrive before CreateWindowExW returns it.
        @param hwnd  the window being created */
    void Adopt(HWND hwnd) noexcept
    {
        hwnd_ = hwnd;
    }

    // ---- view::Host -----------------------------------------------------------------------------------------------

    /** The pixels `area` covers marked for painting; nothing once the surface has let go.
        @param area  the area to repaint, in design pixels */
    void Invalidate(gfx::RectI area) override
    {
        if (hwnd_ == nullptr)
        {
            return; // let go: no window, and a null one would invalidate every window on the desktop
        }
        const gfx::RectI pixels = ToPixels(area);
        const RECT       r{pixels.x(), pixels.y(), pixels.right(), pixels.bottom()};
        InvalidateRect(hwnd_, &r, FALSE);
    }

    float TextWidth(const gfx::Font &font, std::string_view text) const override
    {
        return gfx::MeasureText(font, text);
    }

    /** The display's scale, which the window draws its root at: the zoom a JUCE editor's DPI transform gives it.
        @return window pixels per design pixel */
    float Zoom() const override
    {
        return scale_;
    }

    void CallSoon(std::function<void()> call) override
    {
        if (hwnd_ == nullptr)
        {
            return; // let go: nothing it defers runs
        }
        calls_.push_back(std::move(call));
        PostMessageW(hwnd_, kCallSoonMessage, 0, 0);
    }

    void SetCursor(view::Cursor cursor) override
    {
        cursor_ = cursor;
        ::SetCursor(LoadCursorW(nullptr, CursorFor(cursor)));
    }

    void RequestKeyboardFocus(view::View *view) override
    {
        router_.Focus(view);
    }

    void KeyboardFocusChanged(bool a_view_has_it) override
    {
        if (hwnd_ == nullptr)
        {
            return;
        }
        if (a_view_has_it)
        {
            previous_focus_ = GetFocus();
            focus_taken_    = true;
            SetFocus(hwnd_);
        }
        else if (focus_taken_)
        {
            focus_taken_ = false;
            if (GetFocus() == hwnd_ && previous_focus_ != nullptr && IsWindow(previous_focus_))
            {
                SetFocus(previous_focus_); // the keyboard back to whoever had it
            }
            previous_focus_ = nullptr;
        }
    }

    void ViewLeaving(const view::View &view) override
    {
        router_.Forget(view);
    }

    /** A popup of `content` in a no-activate tool window, kept whole on the monitor it opens on (Host::OpenPopup).
        @param content          the popup's root view; outlives the popup
        @param at               the popup's top left, in this surface's design pixels
        @param pressed_outside  runs with the screen point of every press outside the popup
        @param takes_mouse      false for a popup that only shows, which clicks fall through
        @return                 the popup; null once the surface has let go or Windows refuses the window */
    std::unique_ptr<view::Popup> OpenPopup(view::View &content, gfx::PointI at,
                                           std::function<void(gfx::PointI)> pressed_outside, bool takes_mouse) override;

    /** The clipboard's Unicode text, for a paste: opened against this window and closed again at once.
        @return  the text in UTF-8, or empty when it holds none or another program has it open */
    std::string ClipboardText() override
    {
        std::string text;
        if (hwnd_ == nullptr || !OpenClipboard(hwnd_))
        {
            return text;
        }
        if (HANDLE data = GetClipboardData(CF_UNICODETEXT); data != nullptr)
        {
            if (const auto *wide = static_cast<const wchar_t *>(GlobalLock(data)); wide != nullptr)
            {
                text = Utf8Of(wide);
                GlobalUnlock(data);
            }
        }
        CloseClipboard();
        return text;
    }

    /** `text` on the clipboard, for a copy (win::WriteClipboardText): opened against this window, which owns it until
        another copy, and closed again at once; nothing once the surface is let go.
        @param text  the text, UTF-8
        @return      true when the clipboard took it */
    bool SetClipboardText(const std::string &text) override
    {
        return hwnd_ != nullptr && win::WriteClipboardText(hwnd_, text);
    }

    /** The shell's own dialog, modal over the host's window, opened from a message of its own once the event that
        asked for it is done: never inside that event, whose views' frames would lie below the dialog's loop on the
        stack and be returned into after the host had closed the editor in it. `picked` hears the answer while the
        window lives, and nothing once it has gone.
        @param request  the title, the patterns, the folder and whether it saves
        @param picked   told the path chosen, in UTF-8, or an empty string for a cancel; runs on the message thread */
    void ChooseFile(const view::FileRequest &request, std::function<void(const std::string &)> picked) override
    {
        if (hwnd_ == nullptr)
        {
            return; // let go: nothing hears an answer
        }
        dialogs_.push_back({request, std::move(picked)});
        PostMessageW(hwnd_, kChooseFileMessage, 0, 0);
    }

    // ---- The window's messages ------------------------------------------------------------------------------------

    /** The window's messages: painting, the cursor, the mouse, the wheel, the keys, dropped files, the frame timer, the
        queued calls and the file dialogs; the rest Windows'.
        @param message  the message
        @param wparam   the message's first parameter
        @param lparam   the message's second parameter
        @return         the message's result */
    LRESULT Handle(UINT message, WPARAM wparam, LPARAM lparam)
    {
        switch (message)
        {
        case WM_PAINT: {
            PAINTSTRUCT paint{};
            HDC         dc = BeginPaint(hwnd_, &paint);
            Paint(dc, paint.rcPaint);
            EndPaint(hwnd_, &paint);
            return 0;
        }
        case WM_ERASEBKGND:
            return 1; // the raster covers every pixel
        case WM_SETCURSOR:
            if (LOWORD(lparam) == HTCLIENT)
            {
                ::SetCursor(LoadCursorW(nullptr, CursorFor(cursor_)));
                return TRUE;
            }
            break;
        case WM_MOUSEACTIVATE:
            if (no_activate_)
            {
                return MA_NOACTIVATE; // a popup never takes the host's window's place
            }
            break;
        case WM_LBUTTONDOWN:
        case WM_LBUTTONDBLCLK:
            Press(lparam, message == WM_LBUTTONDBLCLK ? 2 : 1, false);
            return 0;
        case WM_RBUTTONDOWN:
        case WM_RBUTTONDBLCLK:
            Press(lparam, 1, true);
            return 0;
        case WM_LBUTTONUP:
        case WM_RBUTTONUP:
            if (pressed_)
            {
                pressed_ = false;
                ReleaseCapture();
                router_.Up(PointOf(lparam), CurrentModifiers());
            }
            return 0;
        case WM_MOUSEMOVE:
            if (!tracking_)
            {
                TRACKMOUSEEVENT track{static_cast<DWORD>(sizeof(TRACKMOUSEEVENT)), TME_LEAVE, hwnd_, 0};
                tracking_ = TrackMouseEvent(&track) != FALSE;
            }
            if (pressed_)
            {
                router_.Drag(PointOf(lparam), CurrentModifiers());
            }
            else
            {
                router_.Move(PointOf(lparam), CurrentModifiers());
            }
            return 0;
        case WM_MOUSELEAVE:
            tracking_ = false;
            if (!pressed_)
            {
                router_.Exit();
            }
            return 0;
        case WM_MOUSEWHEEL:
        case WM_MOUSEHWHEEL: {
            // The wheel's position comes in screen coordinates.
            POINT point{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd_, &point);
            const float       notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / WHEEL_DELTA;
            const gfx::PointF at{static_cast<float>(point.x) / scale_, static_cast<float>(point.y) / scale_};
            if (message == WM_MOUSEWHEEL)
            {
                router_.Wheel(at, 0.0f, notches * kNotchPixels, CurrentModifiers());
            }
            else
            {
                router_.Wheel(at, -notches * kNotchPixels, 0.0f, CurrentModifiers()); // a tilt right is positive
            }
            return 0;
        }
        case WM_CAPTURECHANGED:
            if (pressed_ && reinterpret_cast<HWND>(lparam) != hwnd_)
            {
                pressed_ = false; // taken by someone else mid-drag: the press ends where it was
                router_.Exit();
            }
            return 0;
        case WM_GETDLGCODE:
            if (router_.focused() != nullptr)
            {
                return DLGC_WANTALLKEYS | DLGC_WANTCHARS; // a dialog-like host must not take the arrows or Tab
            }
            break;
        case WM_KEYDOWN:
        case WM_SYSKEYDOWN: {
            const view::Key       key       = NamedKey(wparam);
            const view::Modifiers modifiers = CurrentModifiers();
            if (key != view::Key::kNone && router_.Key({key, 0, modifiers}))
            {
                return 0;
            }
            // Ctrl with a letter as the letter with the primary modifier, as a Mac's Command-letter reaches the views:
            // its WM_CHAR is a control character, which no view reads. Not with Alt, where Ctrl+Alt may be AltGr
            // typing a character of its own, which WM_CHAR brings.
            if (key == view::Key::kNone && modifiers.command && !modifiers.alt && wparam >= 'A' && wparam <= 'Z' &&
                router_.Key({view::Key::kCharacter, static_cast<char32_t>(U'a' + (wparam - 'A')), modifiers}))
            {
                return 0;
            }
            if (key_target_ != nullptr && SendMessageW(key_target_, win::kHandedKeyDown, wparam, lparam) != 0)
            {
                return 0; // no view's: the window it sits in used it
            }
            break;
        }
        case WM_CHAR:
            if (wparam >= 32 && router_.Key({view::Key::kCharacter, static_cast<char32_t>(wparam), CurrentModifiers()}))
            {
                return 0;
            }
            if (wparam >= 32 && key_target_ != nullptr &&
                SendMessageW(key_target_, win::kHandedChar, wparam, lparam) != 0)
            {
                return 0;
            }
            break;
        case WM_KILLFOCUS:
            if (focus_taken_)
            {
                focus_taken_ = false;
                router_.Focus(nullptr); // the keyboard went elsewhere
            }
            return 0;
        case WM_DROPFILES: {
            HDROP                    drop  = reinterpret_cast<HDROP>(wparam);
            const UINT               count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
            std::vector<std::string> paths;
            for (UINT i = 0; i < count; ++i)
            {
                wchar_t path[MAX_PATH * 4]{};
                DragQueryFileW(drop, i, path, static_cast<UINT>(std::size(path)));
                paths.push_back(Utf8Of(path));
            }
            POINT point{};
            DragQueryPoint(drop, &point);
            DragFinish(drop);
            router_.DropFiles({static_cast<float>(point.x) / scale_, static_cast<float>(point.y) / scale_}, paths);
            return 0;
        }
        case WM_TIMER:
            if (wparam == kFrameTimer && frame_)
            {
                frame_();
            }
            return 0;
        case kCallSoonMessage: {
            // One posted message per call; each runs the oldest waiting.
            if (!calls_.empty())
            {
                auto call = std::move(calls_.front());
                calls_.pop_front();
                call();
            }
            return 0;
        }
        case kChooseFileMessage: {
            if (!dialogs_.empty())
            {
                PendingDialog pending = std::move(dialogs_.front());
                dialogs_.pop_front();
                const std::shared_ptr<SurfaceLife> life = life_;
                const std::string path = RunFileDialog(pending.request, GetAncestor(hwnd_, GA_ROOT), *life);
                if (!life->gone) // else the surface went in the dialog's loop: nothing of it may be touched again
                {
                    pending.picked(path);
                }
            }
            return 0;
        }
        default:
            break;
        }
        return DefWindowProcW(hwnd_, message, wparam, lparam);
    }

    /** The window.
        @return the handle; null before it is made and after the surface lets go */
    HWND hwnd() const noexcept
    {
        return hwnd_;
    }

  protected:
    /** The surface let go, once - what its destruction does, and what a popup's closing with the window it opened
        over does early. A file dialog's caller, below this on the stack when the host closes the editor inside the
        dialog's loop, answers nothing from here on, and the dialog goes; the popups opened over the surface close, and
        theirs; the tree loses its host and the window goes. Then each closed popup's `closed_by_window` runs - a menu
        ends dismissed, its owner free to drop it there - when nothing of the surface is left for it to reach. */
    void Release()
    {
        std::vector<std::function<void()>> heard;
        ReleaseInto(heard);
        for (const auto &closed : heard)
        {
            closed();
        }
    }

    /** Release's letting go, the closed popups' `closed_by_window` gathered into `heard` for the caller to run.
        @param heard  the `closed_by_window` callbacks of the popups closed, appended to */
    void ReleaseInto(std::vector<std::function<void()>> &heard)
    {
        if (released_)
        {
            return;
        }
        released_   = true;
        life_->gone = true;
        if (life_->dialog_up != nullptr)
        {
            life_->dialog_up->Close(HRESULT_FROM_WIN32(ERROR_CANCELLED));
        }
        EndPopups(heard);
        root_.SetHost(nullptr);
        if (hwnd_ != nullptr)
        {
            if (focus_taken_ && GetFocus() == hwnd_ && previous_focus_ != nullptr && IsWindow(previous_focus_))
            {
                SetFocus(previous_focus_);
            }
            KillTimer(hwnd_, kFrameTimer);
            SetWindowLongPtrW(hwnd_, GWLP_USERDATA, 0);
            DestroyWindow(hwnd_);
            hwnd_ = nullptr;
        }
        if (registered_ && --window_count == 0)
        {
            UnregisterClassW(ClassName(), ModuleInstance());
        }
        registered_ = false;
    }

    /** The raster made again at the root's size times the scale, cleared, with the Skia surface over it. */
    void ResizeRaster()
    {
        pixel_width_  = std::max(1, static_cast<int>(std::lround(root_.width() * scale_)));
        pixel_height_ = std::max(1, static_cast<int>(std::lround(root_.height() * scale_)));
        pixels_.assign(static_cast<std::size_t>(pixel_width_) * static_cast<std::size_t>(pixel_height_), 0u);
        surface_ = SkSurfaces::WrapPixels(
            SkImageInfo::Make(pixel_width_, pixel_height_, kBGRA_8888_SkColorType, kPremul_SkAlphaType), pixels_.data(),
            static_cast<std::size_t>(pixel_width_) * 4u);
    }

    /** The design pixels `area` covers, in the window's pixels, rounded out.
        @param area  an area, in design pixels
        @return      the smallest whole-pixel rectangle that holds it, in window pixels */
    gfx::RectI ToPixels(gfx::RectI area) const noexcept
    {
        return gfx::RectF{area.x() * scale_, area.y() * scale_, area.width() * scale_, area.height() * scale_}
            .SmallestIntegerContainer();
    }

    view::View                &root_;            ///< the tree drawn.
    view::MouseRouter          router_;          ///< hands the mouse, keys and drops to the tree.
    float                      scale_;           ///< window pixels per design pixel.
    HWND                       hwnd_{nullptr};   ///< the window; null before it is made and after the surface lets go.
    int                        pixel_width_{1};  ///< the raster's width, in pixels.
    int                        pixel_height_{1}; ///< the raster's height, in pixels.
    std::vector<std::uint32_t> pixels_;          ///< the raster, B G R A with premultiplied alpha, top-down.
    sk_sp<SkSurface>           surface_;         ///< Skia's surface over the raster.
    std::function<void()>      frame_;           ///< the frame callback, run on the frame timer; may be empty.
    bool                       no_activate_{false}; ///< a click does not activate the window: a popup's.
    /** The window a key no view took is handed on to (ChildWindow::HandOnKeysTo); null to hand on none. */
    HWND key_target_{nullptr};

  private:
    /** A press to the router, the mouse captured until its release.
        @param lparam  the message's client position
        @param clicks  1 for a press, 2 for a double click
        @param right   true for the right button */
    void Press(LPARAM lparam, int clicks, bool right)
    {
        pressed_ = true;
        SetCapture(hwnd_);
        router_.Down(PointOf(lparam), clicks, right, CurrentModifiers());
    }

    /** A mouse message's position as the views see it.
        @param lparam  the message's client position, in window pixels
        @return        the position, in design pixels */
    gfx::PointF PointOf(LPARAM lparam) const noexcept
    {
        return {static_cast<float>(GET_X_LPARAM(lparam)) / scale_, static_cast<float>(GET_Y_LPARAM(lparam)) / scale_};
    }

    /** The tree drawn over `area` (the window's pixels), and the raster put on screen, which the paint's own clip
        holds to the part that changed.
        @param dc    the device context WM_PAINT gave
        @param area  the part to draw again, in window pixels */
    void Paint(HDC dc, const RECT &area)
    {
        if (!surface_)
        {
            return;
        }
        const gfx::RectI dirty = gfx::RectI::LeftTopRightBottom(area.left, area.top, area.right, area.bottom)
                                     .Intersection({pixel_width_, pixel_height_});
        if (!dirty.IsEmpty())
        {
            // Only the dirty part is drawn again: the rest of the raster still holds the last frame.
            const gfx::RectI design =
                gfx::RectF{dirty.x() / scale_, dirty.y() / scale_, dirty.width() / scale_, dirty.height() / scale_}
                    .SmallestIntegerContainer();
            SkCanvas *sk = surface_->getCanvas();
            sk->save();
            sk->resetMatrix();
            sk->scale(scale_, scale_);
            {
                gfx::SkiaCanvas canvas(*sk, 1.0f);
                canvas.ClipToRect(design);
                view::PaintTree(root_, canvas);
            }
            sk->restore();
        }
        BITMAPINFO info{};
        info.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
        info.bmiHeader.biWidth       = pixel_width_;
        info.bmiHeader.biHeight      = -pixel_height_; // top-down, as the raster lies
        info.bmiHeader.biPlanes      = 1;
        info.bmiHeader.biBitCount    = 32;
        info.bmiHeader.biCompression = BI_RGB;
        SetDIBitsToDevice(dc, 0, 0, static_cast<DWORD>(pixel_width_), static_cast<DWORD>(pixel_height_), 0, 0, 0,
                          static_cast<UINT>(pixel_height_), pixels_.data(), &info, DIB_RGB_COLORS);
    }

    /** A file dialog asked for and waiting for its message to open. */
    struct PendingDialog
    {
        view::FileRequest                        request; ///< the dialog's title, patterns and folder.
        std::function<void(const std::string &)> picked;  ///< told the path chosen.
    };

    std::deque<std::function<void()>> calls_;   ///< the calls CallSoon queued, one run per kCallSoonMessage.
    std::deque<PendingDialog>         dialogs_; ///< the file dialogs asked for, one opened per kChooseFileMessage.
    std::shared_ptr<SurfaceLife>      life_{std::make_shared<SurfaceLife>()}; ///< what a file dialog's caller reads.
    std::vector<WinPopup *>           popups_;                  ///< the popups opened over it, which go with it
    bool                              released_{false};         ///< true once the surface has let go.
    bool                              registered_{false};       ///< true while this window counts in window_count.
    bool                              pressed_{false};          ///< a press is down, the mouse captured.
    bool                              tracking_{false};         ///< a WM_MOUSELEAVE is asked for.
    bool                              focus_taken_{false};      ///< the keyboard focus was taken for a view.
    HWND                              previous_focus_{nullptr}; ///< the window that had the focus before it was taken.
    view::Cursor                      cursor_{view::Cursor::kArrow}; ///< the cursor the views last asked for.
};

namespace
{
/** The open popups, for the thread's mouse hook, which tells each of a press outside it. */
std::vector<WinPopup *> open_popups;

/** The thread's mouse hook, installed with the first open popup and removed with the last; null when none is open. */
HHOOK mouse_hook = nullptr;

/** The mouse hook's procedure: each button press, told to every open popup as a screen point.
    @param code    HC_ACTION for a message to handle
    @param wparam  the mouse message
    @param lparam  a MOUSEHOOKSTRUCT with the press's screen point
    @return        the next hook's result */
LRESULT CALLBACK PopupMouseHook(int code, WPARAM wparam, LPARAM lparam);
} // namespace

/** A popup: the surface in a no-activate tool window over everything, and the thread's mouse hook telling its opener
    of every press outside it. It goes with the surface it opened over (CloseInto) if that goes first. */
class WinPopup final : public view::Popup, public WinSurface
{
  public:
    /** A popup over `opener`, not yet shown.
        @param opener   the surface it opens over, which lets it go if it goes first; outlives the popup or lets it go
        @param content  the popup's root view; outlives the popup
        @param scale    window pixels per design pixel
        @param outside  runs with the screen point of every press outside the popup */
    WinPopup(WinSurface &opener, view::View &content, float scale, std::function<void(gfx::PointI)> outside)
        : WinSurface(content, scale), opener_(&opener), outside_(std::move(outside))
    {
        no_activate_ = true;
    }

    /** The popup taken off its opener's list and off the mouse hook's. */
    ~WinPopup() override
    {
        if (opener_ != nullptr)
        {
            opener_->Forget(this);
        }
        Unhook();
    }

    /** Closed with the surface it opened over: off the mouse hook's list, then the surface let go - its own popups
        with it - and its `closed_by_window` gathered into `heard` after theirs, the handle left inert for its owner to
        drop, its destruction then reaching nothing.
        @param heard  the `closed_by_window` callbacks, this popup's after those of the popups opened over it */
    void CloseInto(std::vector<std::function<void()>> &heard)
    {
        if (closed_)
        {
            return;
        }
        closed_ = true;
        opener_ = nullptr;
        Unhook();
        ReleaseInto(heard);
        if (closed_by_window)
        {
            heard.push_back(closed_by_window);
        }
    }

    /** Make the window, show it without activating it and put it on the mouse hook's list.
        @param owner        the top-level window it is owned by
        @param screen       its top left, in screen pixels
        @param takes_mouse  false for a popup clicks fall through
        @return             true when the window was made */
    bool Open(HWND owner, POINT screen, bool takes_mouse)
    {
        DWORD ex_style = WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE;
        if (!takes_mouse)
        {
            ex_style |= WS_EX_TRANSPARENT | WS_EX_LAYERED; // clicks fall through it
        }
        if (!CreateSurface(owner, WS_POPUP, ex_style, screen.x, screen.y))
        {
            return false;
        }
        if (!takes_mouse)
        {
            SetLayeredWindowAttributes(hwnd_, 0, 255, LWA_ALPHA);
        }
        ShowWindow(hwnd_, SW_SHOWNOACTIVATE);
        open_popups.push_back(this);
        if (mouse_hook == nullptr)
        {
            mouse_hook = SetWindowsHookExW(WH_MOUSE, PopupMouseHook, nullptr, GetCurrentThreadId());
        }
        return true;
    }

    bool Contains(gfx::PointI screen) const override
    {
        if (hwnd_ == nullptr)
        {
            return false; // closed with the surface it opened over
        }
        RECT r{};
        GetWindowRect(hwnd_, &r);
        return screen.x >= r.left && screen.x < r.right && screen.y >= r.top && screen.y < r.bottom;
    }

    /** A press anywhere on the thread: the popup's opener hears of it when it lands outside the popup.
        @param screen  the press, in screen pixels */
    void PressedAt(gfx::PointI screen)
    {
        if (!Contains(screen) && outside_)
        {
            outside_(screen);
        }
    }

  private:
    /** Off the thread's mouse hook's list, the hook going with the last popup on it. */
    void Unhook()
    {
        const auto listed = std::find(open_popups.begin(), open_popups.end(), this);
        if (listed != open_popups.end())
        {
            open_popups.erase(listed);
        }
        if (open_popups.empty() && mouse_hook != nullptr)
        {
            UnhookWindowsHookEx(mouse_hook);
            mouse_hook = nullptr;
        }
    }

    WinSurface                      *opener_;        ///< the surface it opened over; null once closed with it.
    std::function<void(gfx::PointI)> outside_;       ///< told the screen point of each press outside the popup.
    bool                             closed_{false}; ///< true once CloseInto has run.
};

void WinSurface::EndPopups(std::vector<std::function<void()>> &heard)
{
    for (WinPopup *popup : std::exchange(popups_, {}))
    {
        popup->CloseInto(heard);
    }
}

namespace
{
LRESULT CALLBACK PopupMouseHook(int code, WPARAM wparam, LPARAM lparam)
{
    if (code == HC_ACTION && (wparam == WM_LBUTTONDOWN || wparam == WM_RBUTTONDOWN || wparam == WM_MBUTTONDOWN ||
                              wparam == WM_NCLBUTTONDOWN || wparam == WM_NCRBUTTONDOWN))
    {
        const auto                   *mouse = reinterpret_cast<const MOUSEHOOKSTRUCT *>(lparam);
        const gfx::PointI             at{mouse->pt.x, mouse->pt.y};
        const std::vector<WinPopup *> now = open_popups; // a popup may close as it hears
        for (WinPopup *popup : now)
        {
            if (std::find(open_popups.begin(), open_popups.end(), popup) != open_popups.end())
            {
                popup->PressedAt(at);
            }
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}
} // namespace

std::unique_ptr<view::Popup> WinSurface::OpenPopup(view::View &content, gfx::PointI at,
                                                   std::function<void(gfx::PointI)> pressed_outside, bool takes_mouse)
{
    if (released_ || hwnd_ == nullptr)
    {
        return nullptr;
    }
    POINT screen{static_cast<LONG>(std::lround(at.x * scale_)), static_cast<LONG>(std::lround(at.y * scale_))};
    ClientToScreen(hwnd_, &screen);
    // Kept whole on the monitor it opens on: moved in from an edge it would cross.
    MONITORINFO monitor{};
    monitor.cbSize = sizeof(monitor);
    if (GetMonitorInfoW(MonitorFromPoint(screen, MONITOR_DEFAULTTONEAREST), &monitor))
    {
        const LONG width  = std::lround(content.width() * scale_);
        const LONG height = std::lround(content.height() * scale_);
        screen.x          = std::max(monitor.rcWork.left, std::min(screen.x, monitor.rcWork.right - width));
        screen.y          = std::max(monitor.rcWork.top, std::min(screen.y, monitor.rcWork.bottom - height));
    }
    auto popup = std::make_unique<WinPopup>(*this, content, scale_, std::move(pressed_outside));
    if (!popup->Open(GetAncestor(hwnd_, GA_ROOT), screen, takes_mouse))
    {
        return nullptr;
    }
    popups_.push_back(popup.get());
    return popup;
}

/** The child window inside the host's editor: the surface as a child HWND, with the frame timer. */
class WinChildWindow final : public ChildWindow, public WinSurface
{
  public:
    /** A child window for `root`, not yet made.
        @param root   the view tree to draw; outlives the window
        @param scale  window pixels per design pixel */
    WinChildWindow(view::View &root, float scale) : WinSurface(root, scale)
    {
    }

    /** Make the window as a visible child of the host's.
        @param parent  the host's window
        @return        true when the window was made */
    bool Create(HWND parent)
    {
        return CreateSurface(parent, WS_CHILD | WS_VISIBLE | WS_CLIPSIBLINGS, 0, 0, 0);
    }

    void SetScale(float scale) override
    {
        scale_ = scale;
        ResizeRaster();
        SetWindowPos(hwnd_, nullptr, 0, 0, pixel_width_, pixel_height_, SWP_NOZORDER | SWP_NOMOVE | SWP_NOACTIVATE);
        InvalidateRect(hwnd_, nullptr, FALSE);
    }

    void SetFrameCallback(std::function<void()> frame) override
    {
        frame_ = std::move(frame);
        KillTimer(hwnd_, kFrameTimer);
        if (frame_)
        {
            SetTimer(hwnd_, kFrameTimer, kFrameMilliseconds, nullptr);
        }
    }

    /** Visible with every window above it, and its top-level window not minimised.
        @return true when the window shows */
    bool Showing() const override
    {
        return hwnd_ != nullptr && IsWindowVisible(hwnd_) != FALSE && IsIconic(GetAncestor(hwnd_, GA_ROOT)) == FALSE;
    }

    /** Keys no view takes handed on to `top_level`, the window this one sits in, as win::kHandedKeyDown and
        win::kHandedChar: Windows gives a key to the window with the focus alone, and this one holds the focus while a
        view has the keyboard.
        @param top_level  the HWND of the window this one sits in; null to hand on none */
    void HandOnKeysTo(void *top_level) override
    {
        key_target_ = static_cast<HWND>(top_level);
    }
};

namespace
{
LRESULT CALLBACK WindowProcedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
{
    if (message == WM_NCCREATE)
    {
        auto *created = static_cast<WinSurface *>(reinterpret_cast<const CREATESTRUCTW *>(lparam)->lpCreateParams);
        created->Adopt(hwnd);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
        return DefWindowProcW(hwnd, message, wparam, lparam);
    }
    auto *surface = reinterpret_cast<WinSurface *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    return surface != nullptr ? surface->Handle(message, wparam, lparam)
                              : DefWindowProcW(hwnd, message, wparam, lparam);
}
} // namespace

void RevealInFileManager(const std::string &path)
{
    // As JUCE's File::revealToUser: the item selected in the file manager's window of its folder, through the shell's
    // item list, with COM up for the call as the shell asks.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (PIDLIST_ABSOLUTE item = ILCreateFromPathW(WideOf(path).c_str()))
    {
        SHOpenFolderAndSelectItems(item, 0, nullptr, 0);
        ILFree(item);
    }
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
}

void OpenInBrowser(const std::string &url)
{
    if (url.rfind("https://", 0) != 0 && url.rfind("http://", 0) != 0)
    {
        return;
    }
    // The shell hands the URL to the default browser, with COM up for the call as the shell asks.
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ShellExecuteW(nullptr, L"open", WideOf(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    if (SUCCEEDED(com))
    {
        CoUninitialize();
    }
}

float DisplayScaleOf(void *parent)
{
    // GetDpiForWindow is Windows 10's (1607 on), so it is looked up rather than linked.
    using GetDpiForWindowFunction = UINT(WINAPI *)(HWND);
    static const auto get_dpi     = reinterpret_cast<GetDpiForWindowFunction>(
        reinterpret_cast<void *>(GetProcAddress(GetModuleHandleW(L"user32.dll"), "GetDpiForWindow")));
    HWND window = static_cast<HWND>(parent);
    if (get_dpi == nullptr || window == nullptr)
    {
        return 1.0f;
    }
    const UINT dpi = get_dpi(window);
    return dpi > 0 ? static_cast<float>(dpi) / 96.0f : 1.0f;
}

std::unique_ptr<ChildWindow> CreateChildWindow(void *parent, view::View &root, int width, int height, float scale,
                                               std::unique_ptr<RunLoop>)
{
    HWND parent_window = static_cast<HWND>(parent);
    if (parent_window == nullptr || !IsWindow(parent_window))
    {
        return nullptr;
    }
    root.SetBounds({width, height});
    auto child = std::make_unique<WinChildWindow>(root, scale);
    if (!child->Create(parent_window))
    {
        return nullptr;
    }
    return child;
}
} // namespace reckless::window
