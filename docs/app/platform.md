# The app's platform

What an app has of its platform beyond the child window a panel is drawn in, behind one seam
([AppPlatform.h](../../app/AppPlatform.h)): its windows, a timer, the call after the current event, the event loop and
its end - and the three things a desktop app with work on other threads, text to copy and commands to choose needs: a
post to the UI thread from any thread, the clipboard written, and a menu bar with keyboard shortcuts. One implementation
a platform: an NSApplication on macOS (`app/mac/AppMac.mm`), a Win32 message loop on Windows (`app/win/AppWin.cpp`), an
X11 connection on a `ppoll` loop on Linux (`app/linux/AppLinux.cpp`). Every new call has a default, so a product's own
platform of its suites keeps compiling; the suites' own is [FakeAppPlatform.h](../../tests/FakeAppPlatform.h).

The Linux loop waits in **`ppoll`, to the nanosecond, and not in `poll`**, whose timeout is whole milliseconds. A
timer fires at or after its due time, so a wait rounded down to the millisecond ends before the timer is due, fires
nothing, and is made again at once: the sub-millisecond remainder of every tick went in a run of waits that returned
immediately - about three thousand of them a frame at a frame timer's 60 Hz, each with an `XFlush` of the connection
behind it, which is a fraction of a core for nothing. Rounding the wait up instead would end the spin too, at the
cost of firing every tick up to a millisecond late; `ppoll` costs nothing and fires on time.

## Skia started for the processor

**`AppPlatform::Create` starts Skia before it does anything else**, on every platform: `gfx::InitialiseSkia()`
([SkiaCanvas.h](../../gfx/skia/SkiaCanvas.h)), which is `SkGraphics::Init()` called once a process. Until that call
Skia draws with its portable SSE2 routines; it is the call that chooses the processor's own - AVX2 and AVX-512 on an
x86-64 that has them, all carried by the prebuilt Skia - and Skia never makes it of its own accord. Until 2026-11-02
nothing made it, and every Reckless app drew on SSE2 alone. A filtered, scaled image drawn at 4K took 20.2 ms without
the call and 6.1 ms with it (Sentinel's bench, `Legion5i`, Windows x64, 2026-11-01); a whole 4K frame of one
image upscaled bilinearly gains less, 18.7 to 15.5 ms (the same machine, 2026-11-02). Apple silicon is unaffected, its
routines being chosen when Skia is built.

The platform is the place because it is the one thing every app makes first - before its `AppResources` decode the
series' images, before Linux's platform decodes the window's icon, long before `RunApp` opens a window - and because
the call swaps the routines Skia's drawing goes through, so it has to come before any drawing on any thread rather
than with the first. `RunApp` is not the place: the shell is written against no renderer, and an app has drawn by the
time it runs. A later call is a check of a flag; the suites read whether it ran with `gfx::IsSkiaInitialised()`. A
plug-in in a host makes no platform and is not started this way.

## A post to the UI thread

`AppPlatform::Post(call)` from any thread, or `Posts()` - the queue itself, [PostQueue.h](../../core/PostQueue.h) - which
a worker may keep. Calls run on the main thread while `Run` runs, soon after each post and in the order posted, so each
thread's run in its order; they run during the platform's modal loops too (a file dialog, a menu tracked, a window
dragged). The post that finds the queue empty wakes the loop, once a batch:

| Platform | Wake |
|---|---|
| Windows | a message posted to the platform's message-only window |
| macOS | a block dispatched to the main queue, served in the run loop's common modes |
| Linux | a byte written to a non-blocking pipe the poll watches |

When `Run` returns the queue closes for good: what waits is dropped unrun, and every later post is refused (`Post`
returns false) - so a worker joined after the loop ends may post to its last moment and nothing of it runs on objects
the app is tearing down. A call is destroyed outside the queue's lock, so a capture whose destructor posts is safe
whether the call ran, was dropped or was refused; a refused call is destroyed on the posting thread.

## The clipboard

A view copies through its window (`view::Host::SetClipboardText`) and the app of its own accord through the platform
(`AppPlatform::SetClipboardText`, `AppPlatform::ClipboardText`), UTF-8 both ways, ill-formed UTF-8 repaired first.

| Platform | How |
|---|---|
| Windows | `CF_UNICODETEXT`, the clipboard opened against the child window or the platform's message-only window ([ClipboardWin.h](../../window/win/ClipboardWin.h)) |
| macOS | the general pasteboard ([ClipboardMac.h](../../window/mac/ClipboardMac.h)) |
| Linux | the CLIPBOARD selection owned by an unmapped window answering TARGETS, UTF8_STRING and text/plain;charset=utf-8 from its connection's loop, and letting go on SelectionClear ([X11Clipboard.h](../../window/linux/X11Clipboard.h)) |

X keeps no clipboard of its own, so on Linux the copy lives as long as the connection that made it: an app's platform
holds it on its own connection, and a child window on the platform's loop copies through it
(`window::RunLoop::SetClipboardText`), so a copy made in a dialog outlives the dialog; a plug-in's window in a host
holds it on its own connection while it lives. A paste in a process that holds the selection reads its own copy, since
asking an owner whose loop is the very thread waiting would time out. Text longer than one X request (a megabyte or
more) is refused: X's INCR transfer is not done.

## The menu bar and its shortcuts

One description for every platform ([MenuBar.h](../../app/MenuBar.h)): a `MenuBar` of menus, each a `MenuEntry` - a
title, an id, an action, enabled, checked, a separator, a `Shortcut` and a submenu. A shortcut is written once, the
primary modifier standing for Command on macOS and Control elsewhere (`Shortcut::Primary('n')`), with Shift and Alt; a
character key by the character it types unshifted, a letter in lower case. `AppPlatform::SetMenuBar(bar, chosen)` shows
it; choosing an entry - its click or its shortcut - runs its action and then tells `chosen` its id. A disabled entry, or
one under a disabled menu, is chosen by neither. The app sets the bar again to change a tick or an item, an entry's own
action included. The app menu macOS puts before the bar's menus has an About item, the system's standard panel unless
`MenuBar::about_id` names one of the bar's entries, which it then chooses as that entry's own click does: the app's own
About window.

| Platform | The bar | The shortcuts |
|---|---|---|
| macOS | the screen's menu bar, after the app menu; each shortcut the item's key equivalent | the window takes a key that is one of the bar's shortcuts before the bar does (its `performKeyEquivalent`, which AppKit asks first) and hands it to the views as a key down; one no view takes reaches the window's `keyDown`, which chooses the entry; every other key, Command-Q among them, is AppKit's |
| Windows | an HMENU on each window opened with `WindowOptions::menu_bar`, the content keeping its design size under it; the shortcut's text after a tab | the top-level window's key messages: Ctrl or Alt with a key at its key down, a key that types at its character; a child window hands on the keys its views do not take once `ChildWindow::HandOnKeysTo` is called |
| Linux | a drawn bar, `MenuBarView` ([MenuBarView.h](../../app/MenuBarView.h)), which `AppPlatform::MakeMenuBarView` makes and the app places across the top of its window; its menus the views' own popups | the top-level window's key presses, which a child window sends it when no view took the key |

`AppPlatform::HasNativeMenuBar` says whether the platform shows the bar itself. On every platform the view with the
keyboard has a key first: a text view's Ctrl+C or Command+C is its own copy, and only a key no view takes reaches the
shortcuts. Ctrl with a letter reaches the views on Windows and Linux as the letter with the primary modifier, as
Command with a letter does on a Mac, so a view handles both alike. The drawn bar opens a menu on a press on its title
and closes it on a press on the same title or outside the menu; a press on another title while a menu is open closes
the menu, and a second press opens the other.

The old `AppPlatform::SetMenu`, a flat Options menu on macOS and nothing elsewhere, works as it did; setting the bar
replaces it, and it the bar.

## Full screen

`AppWindow::SetFullScreen` and the calls round it ([app-shell.md](app-shell.md#full-screen) has what an app does with
them), for a window opened with `WindowOptions::may_go_full_screen` alone - never a dialog. Every other window is as it
was.

| Platform | Full screen | Back |
|---|---|---|
| Windows | borderless: the caption and the sizing frame taken off and `WS_POPUP` put on, the menu bar held off the window, and the window over the whole of the monitor it is most on (`MonitorFromWindow`, `GetMonitorInfo`'s `rcMonitor`, not its work area) at the top of the z-order, which puts the task bar under it; at once | the style, the extended style, the menu bar and the placement it had (`GetWindowPlacement`, `SetWindowPlacement`), at once |
| macOS | the Mac's own: a full-screen space of the window's own (`toggleFullScreen:`, the collection behaviour `FullScreenPrimary`), which the green button, Control-Command-F and the Globe key's F take it to too; AppKit's animation, so begun when asked and told when it is done | the same, by `toggleFullScreen:` again or the person |
| Linux | the window manager's (EWMH): `_NET_WM_STATE_FULLSCREEN` asked of it by a client message to the root window, the size hints let go, and its word heard back on the window's `_NET_WM_STATE` (`PropertyNotify`), so its own key is followed too; with no window manager that has it (none at all, as under `xvfb-run`, or one without EWMH), the window made undecorated (`_MOTIF_WM_HINTS`) over the whole X screen, at once | the window manager's, the size hints the owner's again; or the decoration, the hints, the place and the size the window had, at once |

**Why borderless on Windows, not exclusive.** Exclusive full screen is a swap chain's, and the child window rasters on
the CPU and hands its pixels to GDI (`SetDIBitsToDevice`): there is no swap chain to make exclusive, and a borderless
window over the monitor is what a browser's F11 is. The monitor's DPI is the window's - the window is on it - and a
change of DPI or of the display's resolution while full screen (`WM_DPICHANGED`, `WM_DISPLAYCHANGE`) covers the monitor
again. A key with Alt that a shortcut takes (Alt+Enter) has its menu mnemonic swallowed (`WM_SYSCOMMAND`'s
`SC_KEYMENU`), which Windows would otherwise look for in the bar and beep at finding none.

**Why the Mac's own, not a borderless window.** It is what every Mac app's full screen is - a space of its own the
person swipes between, the menu bar and the Dock out of the way until the pointer reaches for them, the green button
doing it - and nothing here needs what a borderless window would buy (no animation, staying on the desktop's space). So
an app that opts in gets the platform's behaviour, its window `FullScreenPrimary`; an app that does not keeps
`FullScreenAuxiliary`, its green button zooming as a JUCE Standalone's does. AppKit is asked not to add an Enter Full
Screen item of its own to the app's View menu (`NSFullScreenMenuItemEverywhere`), only once a window that may go full
screen opens, since the app places the toolkit's item there; a change asked for while AppKit's animation runs is
refused, as AppKit refuses it, and a size of the app's own asked for then is set once the window is out.

**Why the window manager's on Linux.** A window manager places and stacks a window itself, and fights a client that
moves one over its panels; `_NET_WM_STATE_FULLSCREEN` is how every EWMH window manager - GNOME's, KDE's, Xfce's and the
rest - is asked. The fallback covers the whole X screen rather than one monitor, which with no window manager is the
screen there is.

**What the suites prove.** Windows' path is driven by the window suite on the real platform
(`tests/test_app_platform.cpp`: the window's rectangle the monitor's, no caption, and back to the frame and the style it
had, to the pixel); Linux's runs the same suite under `xvfb-run` in CI, which has no window manager, so it is the
fallback that CI proves and the window manager's path that a desktop does; a Mac's is AppKit's animation into a space,
which the suite does not run, a machine's spaces being the person's.
