// This build's platform showing the menu bar (AppPlatform::SetMenuBar) and taking its shortcuts. On Windows: the bar
// a window carries, its items' text with their shortcuts, a tick, the content keeping its size under the bar, a click
// (WM_COMMAND) and the keys - Ctrl with a letter at its key down, a key that types at its character - choosing the
// entries, a bar set anew replacing the window's, and a child window in it handing on to it the keys its views do not
// take: a text view's Ctrl+C its own copy, Ctrl+N the bar's. On Linux, on a display: a drawn bar from the platform, and
// a key press sent to the window choosing its entry from Run's loop. The macOS half is test_app_menu_mac.mm.
#include <gtest/gtest.h>

#include <cstdlib>
#include <memory>
#include <string>
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
