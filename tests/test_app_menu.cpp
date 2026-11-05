// This build's platform showing the menu bar (AppPlatform::SetMenuBar) and taking its shortcuts. On Windows: the bar
// a window carries, its items' text with their shortcuts, a tick, the content keeping its size under the bar, a click
// (WM_COMMAND) and the keys - Ctrl with a letter at its key down, a key that types at its character - choosing the
// entries, a bar set anew replacing the window's, and a child window in it handing on to it the keys its views do not
// take: a text view's Ctrl+C its own copy, Ctrl+N the bar's. The first bar a window opened with none is given - set
// once it is open, or while it is full screen - growing the frame round the content rather than taking its row out of
// it, and a maximised window's frame staying; Alt+Enter, taken by a shortcut or by none, never looked for in the bar
// (WM_MENUCHAR, which beeps), nor beeping in a window with no bar, while Alt with a title's first letter still opens
// its menu. On Linux, on a display: a drawn bar from the platform, and a key press sent to the window choosing its
// entry from Run's loop. The macOS half is test_app_menu_mac.mm.
#include <gtest/gtest.h>

#include <cmath>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AppPlatform.h"
#include "TestHostWindow.h"
#include "TextView.h"
#include "View.h"
#include "Window.h"

namespace
{
using reckless::app::MenuBar;
using reckless::app::MenuEntry;
using reckless::app::Shortcut;

/** A bar of File (New Game, Ctrl+N; a disabled item) and View (Flip Board, F, ticked), each counted as chosen. */
MenuBar CountingBar(int &new_games, int &flips, int &copies)
{
    MenuBar   bar;
    MenuEntry gone = MenuEntry::Item("Save", 3, Shortcut::Primary('s'));
    gone.enabled   = false;
    bar.menus.push_back(
        MenuEntry::Menu("File", {MenuEntry::Item("New Game", 1, Shortcut::Primary('n'), [&] { ++new_games; }),
                                 MenuEntry::Item("Copy FEN", 2, Shortcut::Primary('c'), [&] { ++copies; }),
                                 MenuEntry::Separator(), gone}));
    MenuEntry flip = MenuEntry::Item("Flip Board", 4, Shortcut::Plain('f'), [&] { ++flips; });
    flip.checked   = true;
    bar.menus.push_back(MenuEntry::Menu("View", {flip}));
    return bar;
}
} // namespace

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace
{
/** A key held down, or let go, in this thread's keyboard state, which GetKeyState reads: a key message sent with
    SendMessage carries none of its own.
    @param vk    the virtual key
    @param down  true to hold it */
void Hold(BYTE vk, bool down)
{
    BYTE state[256]{};
    GetKeyboardState(state);
    state[vk] = down ? 0x80 : 0;
    SetKeyboardState(state);
}

/** A menu item's text, by its place.
    @param menu   the menu
    @param place  the item's place, from 0
    @return       its text */
std::wstring TextOf(HMENU menu, UINT place)
{
    wchar_t text[128]{};
    GetMenuStringW(menu, place, text, 128, MF_BYPOSITION);
    return text;
}

/** A key's lparam as Windows writes it: its scan code in bits 16 to 23.
    @param vk  the virtual key
    @return    the lparam */
LPARAM ScanOf(UINT vk)
{
    return static_cast<LPARAM>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC)) << 16;
}
} // namespace

TEST(AppMenuBar, AWindowCarriesTheBarAndItsShortcutsChooseFromAnyKeyNoViewTakes)
{
    int        new_games = 0;
    int        flips     = 0;
    int        copies    = 0;
    const auto platform  = reckless::app::AppPlatform::Create("RecklessMenuTest", {});
    EXPECT_TRUE(platform->HasNativeMenuBar());
    EXPECT_EQ(platform->MakeMenuBarView({}), nullptr) << "Windows shows the bar itself";
    platform->SetMenuBar(CountingBar(new_games, flips, copies), nullptr);

    reckless::app::WindowOptions options;
    options.title     = "Menu";
    options.width     = 300;
    options.height    = 200;
    options.menu_bar  = true;
    const auto window = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);
    const auto hwnd = static_cast<HWND>(window->Handle());
    HMENU      bar  = GetMenu(hwnd);
    ASSERT_NE(bar, nullptr) << "the window carries the bar";
    ASSERT_EQ(GetMenuItemCount(bar), 2);
    EXPECT_EQ(TextOf(bar, 0), L"File");
    HMENU file = GetSubMenu(bar, 0);
    EXPECT_EQ(TextOf(file, 0), L"New Game\tCtrl+N") << "the shortcut after a tab, which Windows right-aligns";
    EXPECT_NE(GetMenuState(file, 3, MF_BYPOSITION) & MF_GRAYED, 0u) << "a disabled item shown dim";
    EXPECT_NE(GetMenuState(GetSubMenu(bar, 1), 0, MF_BYPOSITION) & MF_CHECKED, 0u) << "a ticked item";
    RECT client{};
    GetClientRect(hwnd, &client);
    EXPECT_EQ(client.right - client.left, static_cast<LONG>(300 * window->Scale() + 0.5f));
    EXPECT_EQ(client.bottom - client.top, static_cast<LONG>(200 * window->Scale() + 0.5f)) << "the bar over it";

    SendMessageW(hwnd, WM_COMMAND, MAKEWPARAM(GetMenuItemID(file, 0), 0), 0);
    EXPECT_EQ(new_games, 1) << "a click on the item";

    Hold(VK_CONTROL, true);
    SendMessageW(hwnd, WM_KEYDOWN, 'N', ScanOf('N'));
    SendMessageW(hwnd, WM_KEYDOWN, 'S', ScanOf('S'));
    Hold(VK_CONTROL, false);
    EXPECT_EQ(new_games, 2) << "Ctrl+N at its key down";
    SendMessageW(hwnd, WM_KEYDOWN, 'F', ScanOf('F'));
    EXPECT_EQ(flips, 0) << "a key that types waits for its character, which a view may take";
    SendMessageW(hwnd, WM_CHAR, 'f', ScanOf('F'));
    EXPECT_EQ(flips, 1);

    // A child window holding a text view with the keyboard: its Ctrl+C is the view's, Ctrl+N handed on to the bar.
    reckless::view::View     root;
    reckless::view::TextView text{reckless::view::TextView::Style{}};
    root.SetBounds({300, 200});
    text.SetBounds({0, 0, 300, 200});
    root.AddChild(text);
    auto child = reckless::window::CreateChildWindow(hwnd, root, 300, 200, window->Scale());
    ASSERT_NE(child, nullptr);
    child->HandOnKeysTo(hwnd);
    HWND child_hwnd = GetWindow(hwnd, GW_CHILD);
    ASSERT_NE(child_hwnd, nullptr);
    std::vector<std::string> copied;
    text.SetText("1. d4");
    text.on_copy = [&](std::string copy) { copied.push_back(std::move(copy)); };
    root.host()->RequestKeyboardFocus(&text);
    text.SelectAll();
    Hold(VK_CONTROL, true);
    SendMessageW(child_hwnd, WM_KEYDOWN, 'C', ScanOf('C'));
    SendMessageW(child_hwnd, WM_KEYDOWN, 'N', ScanOf('N'));
    Hold(VK_CONTROL, false);
    EXPECT_EQ(copied, (std::vector<std::string>{"1. d4"})) << "Ctrl+C reached the view as the letter with Command";
    EXPECT_EQ(copies, 0) << "and was the view's, not the bar's";
    EXPECT_EQ(new_games, 3) << "Ctrl+N, no view's, handed on to the window's shortcuts";

    // The bar set anew: the window's replaced, the content's size kept.
    MenuBar next;
    next.menus.push_back(MenuEntry::Menu("Game", {MenuEntry::Item("Resign", 9)}));
    platform->SetMenuBar(next, nullptr);
    bar = GetMenu(hwnd);
    ASSERT_NE(bar, nullptr);
    EXPECT_EQ(GetMenuItemCount(bar), 1);
    EXPECT_EQ(TextOf(bar, 0), L"Game");
    RECT after{};
    GetClientRect(hwnd, &after);
    EXPECT_EQ(after.bottom - after.top, client.bottom - client.top);
    child.reset();
}

namespace
{
/** A window's client area, in pixels.
    @param hwnd  the window
    @return      its width and height */
std::pair<LONG, LONG> ClientOf(HWND hwnd)
{
    RECT client{};
    GetClientRect(hwnd, &client);
    return {client.right - client.left, client.bottom - client.top};
}

/** A window's frame, in screen pixels.
    @param hwnd  the window
    @return      its left, top, right and bottom */
std::vector<LONG> FrameOf(HWND hwnd)
{
    RECT frame{};
    GetWindowRect(hwnd, &frame);
    return {frame.left, frame.top, frame.right, frame.bottom};
}
} // namespace

TEST(AppMenuBar, AWindowsFirstBarGrowsTheFrameRoundTheContentWhicheverComesFirst)
{
    // An app sets its bar once its window is open (Knect 4's and Sudoku's on_open do): the window opened with none -
    // an empty bar is no menu - and the bar's row, when it comes, is the frame's, not taken out of the content.
    const auto                       platform = reckless::app::AppPlatform::Create("RecklessMenuTest", {});
    std::vector<std::pair<int, int>> told;
    reckless::app::WindowOptions     options;
    options.title     = "Menu";
    options.width     = 300;
    options.height    = 200;
    options.menu_bar  = true;
    options.constrain = [](reckless::gfx::PointF proposed) { return proposed; };
    options.resized   = [&told](int width, int height) { told.emplace_back(width, height); };
    const auto window = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);
    const auto hwnd = static_cast<HWND>(window->Handle());
    EXPECT_EQ(GetMenu(hwnd), nullptr) << "no bar set yet";
    const std::pair<LONG, LONG> design{std::lround(300 * window->Scale()), std::lround(200 * window->Scale())};
    ASSERT_EQ(ClientOf(hwnd), design);

    int new_games = 0;
    int flips     = 0;
    int copies    = 0;
    platform->SetMenuBar(CountingBar(new_games, flips, copies), nullptr);
    ASSERT_NE(GetMenu(hwnd), nullptr);
    EXPECT_EQ(ClientOf(hwnd), design) << "the frame grew by the bar's row";
    for (const auto &size : told)
    {
        EXPECT_EQ(size, (std::pair<int, int>{300, 200})) << "never told the content was the bar's row short";
    }

    // The bar taken away: the frame shrinks round the content, which keeps its size again.
    platform->SetMenuBar(MenuBar{}, nullptr);
    EXPECT_EQ(GetMenu(hwnd), nullptr);
    EXPECT_EQ(ClientOf(hwnd), design) << "the frame shrank by the bar's row";
    for (const auto &size : told)
    {
        EXPECT_EQ(size, (std::pair<int, int>{300, 200})) << "never told the content was the bar's row tall";
    }

    // Maximised, the frame is the work area's and stays: the row comes out of the content, told as a resize.
    ShowWindow(hwnd, SW_MAXIMIZE);
    const std::vector<LONG>     frame     = FrameOf(hwnd);
    const std::pair<LONG, LONG> maximised = ClientOf(hwnd);
    told.clear();
    platform->SetMenuBar(CountingBar(new_games, flips, copies), nullptr);
    EXPECT_EQ(FrameOf(hwnd), frame) << "a maximised window's frame is not its own to grow";
    EXPECT_EQ(ClientOf(hwnd).first, maximised.first);
    EXPECT_LT(ClientOf(hwnd).second, maximised.second);
    ASSERT_FALSE(told.empty());
    EXPECT_EQ(told.back().second, std::lround(static_cast<float>(ClientOf(hwnd).second) / window->Scale()));
}

TEST(AppMenuBar, ABarThatFirstComesWhileFullScreenIsTheFramesOnTheWayBack)
{
    const auto                   platform = reckless::app::AppPlatform::Create("RecklessMenuTest", {});
    reckless::app::WindowOptions options;
    options.title              = "Menu";
    options.width              = 300;
    options.height             = 200;
    options.menu_bar           = true;
    options.may_go_full_screen = true;
    options.constrain          = [](reckless::gfx::PointF proposed) { return proposed; };
    options.resized            = [](int, int) {};
    const auto window          = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);
    const auto                  hwnd = static_cast<HWND>(window->Handle());
    const std::pair<LONG, LONG> design{std::lround(300 * window->Scale()), std::lround(200 * window->Scale())};
    ASSERT_TRUE(window->EnterFullScreen());
    int new_games = 0;
    int flips     = 0;
    int copies    = 0;
    platform->SetMenuBar(CountingBar(new_games, flips, copies), nullptr);
    EXPECT_EQ(GetMenu(hwnd), nullptr) << "held off the window while it is full screen";
    ASSERT_TRUE(window->LeaveFullScreen());
    EXPECT_NE(GetMenu(hwnd), nullptr) << "put on as it comes back";
    EXPECT_EQ(ClientOf(hwnd), design) << "the frame it comes back to grown by the row it had none of";
}

namespace
{
/** What reached a window's menu handling: WM_MENUCHAR (a key with Alt the menu found nothing for, whose default
    answer, MNC_IGNORE, beeps) and the platform's answer to it, and WM_ENTERMENULOOP (a menu opened, or looked
    through), heard by a procedure of the suite's put in front of the platform's. */
struct MenuWatch
{
    static inline WNDPROC platforms{nullptr}; ///< the platform's procedure, which every message goes on to
    static inline int     menu_chars{0};      ///< WM_MENUCHAR heard
    static inline int     menu_loops{0};      ///< WM_ENTERMENULOOP heard
    static inline int     beeps{0};           ///< WM_MENUCHAR the platform answered MNC_IGNORE

    /** Count, and hand on to the platform's.
        @param hwnd     the window
        @param message  the message
        @param wparam   its first parameter
        @param lparam   its second parameter
        @return         the platform's answer */
    static LRESULT CALLBACK Procedure(HWND hwnd, UINT message, WPARAM wparam, LPARAM lparam)
    {
        menu_chars += message == WM_MENUCHAR ? 1 : 0;
        menu_loops += message == WM_ENTERMENULOOP ? 1 : 0;
        const LRESULT answer = CallWindowProcW(platforms, hwnd, message, wparam, lparam);
        beeps += message == WM_MENUCHAR && HIWORD(answer) == MNC_IGNORE ? 1 : 0;
        return answer;
    }
};

/** A key with Alt, pressed in `hwnd` as Windows delivers it: its key down, then the WM_SYSCHAR TranslateMessage
    makes of it (bit 29, Alt held), with Alt held in the thread's keyboard state; a menu the key opens is closed a
    fifth of a second later (EndMenu), from the menu's own loop.
    @param hwnd       the window with the keyboard
    @param vk         the virtual key
    @param character  the character it types */
void PressWithAlt(HWND hwnd, UINT vk, wchar_t character)
{
    const LPARAM flags = 1 | ScanOf(vk) | (LPARAM{1} << 29);
    SetTimer(GetAncestor(hwnd, GA_ROOT), 77, 200, [](HWND top, UINT, UINT_PTR id, DWORD) {
        KillTimer(top, id);
        EndMenu();
    });
    Hold(VK_MENU, true);
    SendMessageW(hwnd, WM_SYSKEYDOWN, vk, flags);
    SendMessageW(hwnd, WM_SYSCHAR, character, flags);
    Hold(VK_MENU, false);
    KillTimer(GetAncestor(hwnd, GA_ROOT), 77);
}
} // namespace

TEST(AppMenuBar, AltEnterIsSilentWhetherAShortcutTakesItOrNotAndAMenusLetterStillOpensIt)
{
    int        new_games    = 0;
    int        flips        = 0;
    int        copies       = 0;
    int        full_screens = 0;
    const auto platform     = reckless::app::AppPlatform::Create("RecklessMenuTest", {});
    platform->SetMenuBar(CountingBar(new_games, flips, copies), nullptr);
    reckless::app::WindowOptions options;
    options.title     = "Menu";
    options.width     = 300;
    options.height    = 200;
    options.menu_bar  = true;
    const auto window = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);
    const auto           hwnd = static_cast<HWND>(window->Handle());
    reckless::view::View root;
    root.SetBounds({300, 200});
    auto child = reckless::window::CreateChildWindow(hwnd, root, 300, 200, window->Scale());
    ASSERT_NE(child, nullptr);
    child->HandOnKeysTo(hwnd);
    HWND child_hwnd = GetWindow(hwnd, GW_CHILD);
    ASSERT_NE(child_hwnd, nullptr);
    MenuWatch::platforms = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(MenuWatch::Procedure)));

    // No shortcut has it: Windows would look for a carriage return among the bar's titles and beep at finding none.
    for (HWND keyboard : {child_hwnd, hwnd})
    {
        MenuWatch::menu_chars = 0;
        MenuWatch::menu_loops = 0;
        PressWithAlt(keyboard, VK_RETURN, L'\r');
        EXPECT_EQ(MenuWatch::menu_chars, 0) << "no WM_MENUCHAR, which beeps";
        EXPECT_EQ(MenuWatch::menu_loops, 0) << "no menu looked through";
    }

    // A shortcut has it, as the full-screen item's does on Windows (FullScreenOtherShortcuts): chosen, and silent.
    MenuBar   bar = CountingBar(new_games, flips, copies);
    MenuEntry full =
        MenuEntry::Item("Full screen", 5, Shortcut::Plain(reckless::app::ShortcutKey::kF11), [&] { ++full_screens; });
    full.other_shortcuts = {Shortcut::Plain(reckless::app::ShortcutKey::kReturn, false, true)};
    bar.menus.back().submenu.push_back(full);
    platform->SetMenuBar(bar, nullptr);
    MenuWatch::menu_chars = 0;
    MenuWatch::menu_loops = 0;
    PressWithAlt(child_hwnd, VK_RETURN, L'\r');
    EXPECT_EQ(full_screens, 1) << "Alt+Enter chose its entry, from the child window with the keyboard";
    EXPECT_EQ(MenuWatch::menu_chars, 0) << "no WM_MENUCHAR, which beeps";
    EXPECT_EQ(MenuWatch::menu_loops, 0) << "no menu looked through";

    // A key with Alt no shortcut has is still the bar's: Alt+F opens File by its first letter, as Windows takes a
    // title with no mnemonic of its own (the toolkit doubles every ampersand).
    MenuWatch::menu_chars = 0;
    MenuWatch::menu_loops = 0;
    PressWithAlt(child_hwnd, 'F', L'f');
    EXPECT_EQ(MenuWatch::menu_loops, 1) << "the File menu opened";
    EXPECT_EQ(MenuWatch::menu_chars, 0) << "found by its letter";
    EXPECT_EQ(new_games, 0) << "and nothing in it chosen";
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(MenuWatch::platforms));
    child.reset();
}

TEST(AppMenuBar, AltEnterIsSilentInAWindowWithNoBarWhoseChildHandsOnNoKeys)
{
    // A window opened with no bar, its child window handing on nothing (RunApp asks for that only for an app with a
    // bar, as IR Capture has none): Windows looks through the system menu for the carriage return from the child
    // itself, and asks the window what to do at finding none.
    const auto                   platform = reckless::app::AppPlatform::Create("RecklessMenuTest", {});
    reckless::app::WindowOptions options;
    options.title     = "Menu";
    options.width     = 300;
    options.height    = 200;
    const auto window = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);
    const auto           hwnd = static_cast<HWND>(window->Handle());
    reckless::view::View root;
    root.SetBounds({300, 200});
    auto child = reckless::window::CreateChildWindow(hwnd, root, 300, 200, window->Scale());
    ASSERT_NE(child, nullptr);
    HWND child_hwnd = GetWindow(hwnd, GW_CHILD);
    ASSERT_NE(child_hwnd, nullptr);
    MenuWatch::platforms = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(MenuWatch::Procedure)));
    MenuWatch::menu_chars = 0;
    MenuWatch::beeps      = 0;
    PressWithAlt(child_hwnd, VK_RETURN, L'\r');
    EXPECT_EQ(MenuWatch::menu_chars, 1) << "asked, as Windows asks a window with no bar";
    EXPECT_EQ(MenuWatch::beeps, 0) << "and answered MNC_CLOSE, which closes the menu without a beep";
    SetWindowLongPtrW(hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(MenuWatch::platforms));
    child.reset();
}
#endif

#if defined(__linux__)
#include <chrono>
#include <cstdint>

// Xlib last: its macros (None, Bool, Status) would reach into the headers above.
#include <X11/Xlib.h>
#include <X11/keysym.h>

TEST(AppMenuBar, LinuxDrawsTheBarAndAKeyTheWindowHasChoosesItsEntry)
{
    const char *display_name = std::getenv("DISPLAY");
    if (display_name == nullptr || *display_name == '\0')
    {
        if (reckless::test::WindowRequired())
        {
            FAIL() << "a window was promised, and there is no display";
        }
        GTEST_SKIP() << "no display";
    }
    int        new_games = 0;
    int        flips     = 0;
    int        copies    = 0;
    const auto platform  = reckless::app::AppPlatform::Create("RecklessMenuTest", {});
    EXPECT_FALSE(platform->HasNativeMenuBar());
    platform->SetMenuBar(CountingBar(new_games, flips, copies), [&](int id) {
        if (id == 1)
        {
            platform->Quit();
        }
    });
    const std::unique_ptr<reckless::app::MenuBarView> drawn = platform->MakeMenuBarView({});
    ASSERT_NE(drawn, nullptr) << "X has no bar of its own: the app places this one";

    reckless::app::WindowOptions options;
    options.title     = "Menu";
    options.width     = 300;
    options.height    = 200;
    const auto window = platform->OpenWindow(options);
    ASSERT_NE(window, nullptr);

    // Ctrl+N pressed in the window, sent as the server would deliver it.
    Display *display = XOpenDisplay(nullptr);
    ASSERT_NE(display, nullptr);
    const auto id = static_cast<::Window>(reinterpret_cast<std::uintptr_t>(window->Handle()));
    XEvent     press{};
    press.xkey.type        = KeyPress;
    press.xkey.display     = display;
    press.xkey.window      = id;
    press.xkey.root        = DefaultRootWindow(display);
    press.xkey.state       = ControlMask;
    press.xkey.keycode     = XKeysymToKeycode(display, XK_n);
    press.xkey.same_screen = True;
    XSendEvent(display, id, False, KeyPressMask, &press);
    XFlush(display);
    platform->StartTimer(5000, [&] { platform->Quit(); });
    platform->Run();
    XCloseDisplay(display);
    EXPECT_EQ(new_games, 1) << "Ctrl+N, from Run's loop";
}
#endif
