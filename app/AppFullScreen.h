#pragma once

#include <string>
#include <vector>

#include "AppPlatform.h"
#include "MenuBar.h"

namespace reckless::app
{
/**
    A window's full screen as a menu shows it, written once: the item's text, its keys and its tick as each platform has
    them, choosing AppWindow::ToggleFullScreen. Menus are an app's own (MenuBar), so an app that opens its window with
    full screen allowed (AppRunHooks::may_go_full_screen, WindowOptions::may_go_full_screen) places
    FullScreenMenuEntry in its View menu and sets the bar again whenever full screen changes
    (AppRunHooks::on_full_screen), as it does for any tick. Escape leaving full screen is the app's choice, not this
    entry's: a game may want Escape for itself.
*/

/** The platforms a menu's full-screen item is written for, each its own way. */
enum class MenuPlatform
{
    kWindows, ///< F11, and Alt+Enter as games have it; "Full screen", ticked while full screen
    kMac,     ///< Ctrl+Cmd+F; "Enter Full Screen" and "Exit Full Screen", as every Mac app's View menu has it
    kLinux,   ///< F11; "Full screen", ticked while full screen
};

/** The platform this build is for. */
inline constexpr MenuPlatform kThisMenuPlatform =
#if defined(__APPLE__)
    MenuPlatform::kMac;
#elif defined(_WIN32)
    MenuPlatform::kWindows;
#else
    MenuPlatform::kLinux;
#endif

/** The key that takes a window full screen and back on `platform`: F11 on Windows and Linux, as their browsers and
    file managers have it; Control-Command-F on a Mac, AppKit's own for Enter Full Screen.
    @param platform  the platform; this build's when left out
    @return          the shortcut */
inline Shortcut FullScreenShortcut(MenuPlatform platform = kThisMenuPlatform)
{
    if (platform == MenuPlatform::kMac)
    {
        Shortcut shortcut = Shortcut::Primary('f');
        shortcut.control  = true;
        return shortcut;
    }
    return Shortcut::Plain(ShortcutKey::kF11);
}

/** The further keys that do the same, shown nowhere: Alt+Enter on Windows, where games and media players take it;
    none on a Mac or Linux, where it is no convention.
    @param platform  the platform; this build's when left out
    @return          the keys */
inline std::vector<Shortcut> FullScreenOtherShortcuts(MenuPlatform platform = kThisMenuPlatform)
{
    if (platform == MenuPlatform::kWindows)
    {
        return {Shortcut::Plain(ShortcutKey::kReturn, false, true)};
    }
    return {};
}

/** The text of the full-screen item on `platform` for a window that is full screen or not: a Mac's two items in one,
    "Enter Full Screen" and "Exit Full Screen", as AppKit names them; elsewhere "Full screen", ticked while it is.
    @param is_full_screen  whether the window is full screen
    @param platform        the platform; this build's when left out
    @return                the text, UTF-8 */
inline std::string FullScreenTitle(bool is_full_screen, MenuPlatform platform = kThisMenuPlatform)
{
    if (platform == MenuPlatform::kMac)
    {
        return is_full_screen ? "Exit Full Screen" : "Enter Full Screen";
    }
    return "Full screen";
}

/** The menu item that takes `window` full screen and back, as `platform` writes one: its text, its key and the keys
    beside it, ticked while full screen where the platform ticks it, and enabled only where the window may go full
    screen (AppWindow::CanGoFullScreen). Choosing it toggles the window's full screen, then reports `id` to the bar's
    `chosen` as any item does. The window must outlive the bar's use of the item: an app sets the bar again, or no
    more, once the window is gone.
    @param window    the window it takes full screen; null for none yet, when the item is dim and does nothing
    @param id        what choosing it reports; 0 for nothing
    @param platform  the platform; this build's when left out
    @return          the entry */
inline MenuEntry FullScreenMenuEntry(AppWindow *window, int id = 0, MenuPlatform platform = kThisMenuPlatform)
{
    const bool is_full_screen = window != nullptr && window->IsFullScreen();
    MenuEntry  entry =
        MenuEntry::Item(FullScreenTitle(is_full_screen, platform), id, FullScreenShortcut(platform), [window] {
            if (window != nullptr)
            {
                window->ToggleFullScreen();
            }
        });
    entry.other_shortcuts = FullScreenOtherShortcuts(platform);
    entry.checked         = platform != MenuPlatform::kMac && is_full_screen;
    entry.enabled         = window != nullptr && window->CanGoFullScreen();
    return entry;
}
} // namespace reckless::app
