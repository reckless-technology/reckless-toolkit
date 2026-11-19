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
| Windows | an HMENU on each window opened with `WindowOptions::menu_bar`, the content keeping its design size under it - the frame grows by the bar's row, the first bar set after the window opened (or while it was full screen) included, and only a maximised window gives the row out of its content, told as a resize; the shortcut's text after a tab | the top-level window's key messages: Ctrl or Alt with a key at its key down, a key that types at its character; a child window hands on the keys its views do not take once `ChildWindow::HandOnKeysTo` is called, and the menu a key with Alt asks for (`WM_SYSCOMMAND`'s `SC_KEYMENU`) with them |
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
window over the monitor is what a browser's F11 is. A child window an app has asked to draw on the GPU
([../window/gpu-drawing.md](../window/gpu-drawing.md)) swaps a GL child window's buffers, which makes nothing exclusive
either, so full screen is the same borderless window for it. The monitor's DPI is the window's - the window is on it - and a
change of DPI or of the display's resolution while full screen (`WM_DPICHANGED`, `WM_DISPLAYCHANGE`) covers the monitor
again. Alt+Enter never reaches the bar, whether a shortcut takes it or not, nor does any other key with Alt that a
shortcut takes: its character's menu (`WM_SYSCOMMAND`'s `SC_KEYMENU`, which a child window with the keyboard hands on to
the window) is swallowed, where Windows would look for it among the bar's titles, or the system menu's with no bar, and
beep at finding none (`WM_MENUCHAR`, answered `MNC_IGNORE`). Where a child window hands on no keys - a window with no
bar, whose app never calls `ChildWindow::HandOnKeysTo` - Windows looks through the system menu from the child itself,
and the window answers its `WM_MENUCHAR` for a carriage return `MNC_CLOSE`, which closes the menu without a sound. Every
other key with Alt is the bar's as before: Alt+F opens File by its first letter, since the bar's titles carry no
mnemonic of their own.

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

## Placing a window, and naming the screens

`AppWindow::SetPosition` puts a window's top left where it is asked, the exact inverse of `AppWindow::Position` on the
same platform, so a place read and put back is where the window stood. `AppPlatform::ScreenAreas` names every display's
usable area - what its panels, docks and bars leave - the main display's first. Together they move a window to another
display: a point inside that display's area places it there. A platform that cannot do either answers false and none,
and a caller must then leave the window where it is.

| Platform | Placed | The screens |
|---|---|---|
| macOS | the frame's top left (`setFrameOrigin:`, against Cocoa's y-up); refused while full screen or minimised | each `NSScreen`'s `visibleFrame`, y down, the main screen's first |
| Linux | the **client area's** top left, as `Position` reads it (`XTranslateCoordinates`): the size hints given the place as an asked one (`USPosition`) with static gravity, then `XMoveWindow`; refused while full screen or minimised (`WM_STATE` iconic) | RandR's monitors (`XRRGetMonitors`, RandR 1.5), the primary first, each held to what the window manager says its panels leave of it |
| Windows | the window rectangle's top left (`SetWindowPos` with no size, z-order or activation change), as `Position` reads it (`GetWindowRect`); put again if a `WM_DPICHANGED` moved it; refused while full screen or minimised | each monitor's work area (`GetMonitorInfo`'s `rcWork`, through `EnumDisplayMonitors`), the primary's first |

**What a window manager says on Linux.** X knows the monitors; only the window manager knows the panels. EWMH's
`_NET_WORKAREA` is one rectangle for the whole desktop, which is exact where every panel sits on an edge of the
desktop's bounding box, and too small for a display that has no panel where another display's panel sits. So a
window manager's work areas display by display come first where it publishes them: Mutter and Muffin (GNOME's and
Cinnamon's) publish `_GTK_WORKAREAS_D<desktop>`, each area belonging to the smallest display it lies inside
(`UsableAreas` in [WindowPlace.h](../../app/WindowPlace.h)). A display with none of its own takes the part of
`_NET_WORKAREA` on it, and with no window manager at all, as under `xvfb-run`, all of it. libXrandr is opened by name,
as the display clock opens it, so no product's link line changes; a server without RandR 1.5 names no screens.

**What space Windows is in.** The process is per-monitor DPI aware (`BecomeDpiAware`), so `GetWindowRect`,
`SetWindowPos` and every monitor's rectangle are in the same physical pixels of the virtual desktop, whatever a
monitor's scaling; nothing is converted. A window put on a monitor of another DPI is sized for it by `WM_DPICHANGED`,
which may move its top left to where Windows suggests, so `SetPosition` puts the position again after.

**A move is asked, not made.** A window manager has the last word on where a window goes, and may keep it on its work
area or move it on; `SetPosition`'s true says the move was asked, and `Position` says where the window went once the
window manager has placed it. Static gravity is what makes the point the client area's under a window manager that
draws a title bar over the window; Muffin honours it (Cinnamon 6.6 on jonny-precision: a window at 800, 440 put back
read 800, 440, and one put at 480, 260 read 480, 260), as a window manager following ICCCM does.

**What the suites prove.** The rules that pick a display's area are held without a display (`tests/test_window_place.cpp`).
On Linux the platform suite (`tests/test_app_platform.cpp`) puts a window back where it stands and moves it, and under
`xvfb-run`, which has no window manager, sets two RandR monitors up within the screen with a panel's work areas on the
root, and holds the order and each display's area; on a person's desktop it only reads them, the monitors and panels
being the person's. A move between two real panels wants a second screen, and is measured rather than asserted.

On Windows the platform suite reads a real window's place, holds it to a work area, puts it back and moves it by an
offset and back; it skips where the session has no monitor, as a runner that is a service on a window station that
shows nothing has none. A move between two monitors of different DPI wants a second screen, and is not asserted.

## Hosting a native view in a window of the app (a plug-in's editor)

What a host that attaches a plug-in's editor to a top-level window of its own needs of the platform - a DAW opening one
`AppWindow` per VST3 `IPlugView` and calling `attached(window->Handle(), ...)` - and what the toolkit gives it. All of it
is additive: no default, signature or behaviour a shipped product has changed.

| Need | Where it is | Notes |
|---|---|---|
| A window with **no child window** in it | `AppPlatform::OpenWindow` | Legal on every platform: it draws nothing itself (Linux's X window has a black background pixel, Windows' class a black brush, macOS' content view nothing), survives a resize and closes by being destroyed. `Handle()` is the NSView, the HWND or the X window's id. A window with no child window is not one the toolkit paints: whatever is attached must cover the client area. |
| The content size, **exactly, in the handle's units** | `AppWindow::ContentSizeInUnits`, `SetContentSizeInUnits` | Units are pixels on Windows and Linux and points on macOS, what `IPlugView::getSize`, `onSize` and `IPlugFrame::resizeView` count in. `SetContentSize` goes through design pixels, which at a fractional scale cannot name every pixel. `SetContentSizeInUnits` **tells neither `resized` nor `resized_in_units`** on any platform: the caller set the size and knows it, and a plug-in told the size it asked for would be asked again. |
| Resizable by the person, **per window and at any time** | `AppWindow::SetUserResizable(is_resizable, least, most)` | The limits are in the handle's units. It replaces what `WindowOptions::constrain` opened the window with: Windows' `WS_THICKFRAME` and `WM_SIZING`, macOS' resizable style mask and `windowWillResize`, and X11's size hints are the ones a window opened with a constraint already used. A window opened with no constraint is one size until this says otherwise. |
| The person's resize, **in units** | `WindowOptions::resized_in_units(width, height)` | For a window with no child window as for one with. `resized` is told too, in design pixels, as it always was; the new callback is the exact size in the handle's units, which is what `onSize` takes. Neither is told for a size the app set. |
| Close requested; close | `WindowOptions::closing`; destroy the `AppWindow` | `closing` is the close button, or Escape on a dialog, and does not close the window. Closing is destroying the `AppWindow`, which tells nothing, so call `IPlugView::removed` first, and **destroy after the event has returned** (`CallSoon`) rather than inside `closing`: macOS is inside `windowShouldClose:` there. |
| The content scale | `AppWindow::Scale()`, `WindowOptions::rescaled` | The window's units per design pixel (Windows: the monitor's DPI over 96; Linux: the desktop's; macOS: 1, a window of points). `rescaled` is told when a window moves to a monitor of another scale, **on Windows only**: Linux reads the desktop's scale once, as the window opens (see below). |
| A repeating timer the app can stop | `AppPlatform::AddTimer`, `CancelTimer` | `StartTimer` has no cancel and keeps it that way. `AddTimer` returns a token (never 0); `CancelTimer` stops it at once, from inside its own tick too. Natively `KillTimer` on Windows, `invalidate` on macOS and the loop's own table on Linux; the default on `AppPlatform` wraps `StartTimer`, so a platform of a suite's own keeps compiling and the cancelled timer stops calling the app. |
| **Linux's VST3 `IRunLoop`**: file descriptors | `AppPlatform::CanWatchFileDescriptors`, `WatchFileDescriptor`, `UnwatchFileDescriptor` | `registerEventHandler(handler, fd)` is `WatchFileDescriptor(fd, [handler] { handler->onFDIsSet(fd); })` and `unregisterEventHandler` is `UnwatchFileDescriptor`; `registerTimer(handler, ms)` is `AddTimer(ms, [handler] { handler->onTimer(); })` and `unregisterTimer` is `CancelTimer`. The loop is the app's own `ppoll` (the same one a child window's `RunLoop::WatchFd` uses), so the handlers run on the main thread between X events. **Level triggered**: a descriptor left readable calls back on every turn, so the handler reads it or unwatches it; unwatching from inside the callback is safe. Elsewhere `CanWatchFileDescriptors` is false and `WatchFileDescriptor` answers 0 - VST3's `IRunLoop` is Linux's alone. |

**What Windows and macOS do that Linux does not.** `WindowOptions::resized` (design pixels) is told for a size the app
set with `SetContentSize` on Windows and macOS, where the platform's resize event is synchronous, and not on Linux, where
the app's own size is known before the window manager's answer comes. `SetContentSizeInUnits` and `SetUserResizable` are
the same on all three - told to no one - and a host should size an editor with those and take the person's with
`resized_in_units`.

**Not here: a live change of scale on Linux.** The window reads the desktop's scale once (`window::DisplayScaleOf`),
and the toolkit has no source of a change; a plug-in is told its factor at `attached` and not again. The design for it
would be: select `PropertyChangeMask` on the root window, re-read `RESOURCE_MANAGER` (`Xft.dpi`, which
`DisplayScaleOf` already parses) when it changes, and tell each window's `rescaled` - and its `Scale()` - the new factor
at the next turn of the loop. It is left out because the toolkit's own child windows would then need to follow too (they
hold the scale they were made with), which is a change to every consumer's drawing and not a thing a host's attach
needs.

**What the suites prove.** On Linux, under `xvfb-run` (`tests/test_app_platform.cpp`): a window with no child window set
to an exact odd size reads it back and tells no one; the person's resize, made by resizing the X window from another
connection as a window manager does, reaches `resized_in_units` in pixels; a `WM_DELETE_WINDOW` reaches `closing`
without closing the window and destroying it removes the X window; a timer cancelled from outside and from inside its own
tick stops; a pipe's descriptor is polled level triggered and unwatched from its own callback. Windows' and macOS' code
for the same is compiled by CI and not run: CI's runners show no window to a service.

## Keys, the mouse and the dialogs a timeline needs

| Need | Where it is | Notes |
|---|---|---|
| Space, Home, End, Delete, Backspace, arrows, F1 to F12, a letter, as **menu shortcuts** | `app::Shortcut::Plain` / `Primary`, `ShortcutKey` | Existed: `kSpace`, `kHome`, `kEnd`, `kDelete`, `kBackspace`, the arrows, `kF1` to `kF12`, and any character. A shortcut with no primary modifier is offered **after** the views: a `TextField` or `TextView` with the keyboard takes every key that types and every key that moves its caret, so `Plain('s')` and `Plain(ShortcutKey::kSpace)` cannot steal typing from a field and are the bar's the moment no view with the keyboard takes them. Held in `tests/test_menu_bar.cpp`. |
| The same keys to **a focused view** | `view::KeyEvent`, `view::Key` | Space is `kCharacter` with `U' '`, a letter its character. **New: `Key::kF1` to `kF12`**, which the views never heard (no name for them: Linux gave a `kCharacter` with no character, Windows nothing). A text field and a text view return false for them, so a function key is the bar's. The raw keys' `named_key` is unchanged: the products that read raw keys see what they always did. |
| The Control key itself | `view::Modifiers::control` | **New**, last in the struct, so `{shift, command, alt}` still reads as it did. `command` is Command on a Mac and Control elsewhere; `control` is the Control key everywhere - on Windows and Linux true with `command`, on a Mac Control alone. A Mac's Control+click is still a right press as it was (`MouseEvent::right`), and now also has `control` set. |
| Modifiers on the wheel; horizontal wheel | `MouseEvent::modifiers`, `wheel_x`, `wheel_y` | Existed on all three: Ctrl+wheel is `modifiers.control` with `wheel_y`, Shift+wheel is `modifiers.shift` (a view makes it horizontal), a tilt or a trackpad's sideways scroll is `wheel_x` (Windows `WM_MOUSEHWHEEL`, macOS `scrollingDeltaX`, X11 buttons 6 and 7). |
| The **middle button** | `MouseEvent::middle`, `View::takes_middle_button`, `MouseRouter::Down(point, clicks, MouseButton, modifiers)` | **New**, and opt-in: the platforms never delivered the middle button, so no view was written to ignore it. A press reaches the view under the pointer or the first of its parents with `takes_middle_button` set, and for every other view it is nothing - not a press, not a focus change - so a knob does not turn on a middle click. `MouseRouter::HasPress` says whether a view took it, so a window captures the pointer only for one that did. Windows `WM_MBUTTON*`, macOS `otherMouse*` button 2, X11 button 2. |
| **A second button** while one is held | `MouseRouter::Down`, `MouseRouter::HasPress` | **Fixed**: the router holds one press at a time. A press that comes while a view holds another - the other button, on that view or another - first ends the held one for its view as a `MouseUp` of its own button at the last place the router was told of the pointer (the same end the release of that button would have given; the toolkit has no cancel event, and a window losing the keyboard ends no press), with no double click after it, and then begins the new press. Every `MouseDown` a view hears is followed by one `MouseUp`; a view that began a drag, an undo group or a take can rely on it. A middle press no view asked for is nothing and leaves the held press alone. The platforms report no button with a release, so the next release of either ends the press then held and the other's finds none. Held in `tests/test_view.cpp`. |
| Cursors | `view::Cursor` | Existed, all eight on all three platforms: `kArrow`, `kPointingHand`, `kIBeam`, `kGrab`, `kGrabbing`, `kUpDown`, `kLeftRight` (horizontal resize: a clip's trim edge), `kCrosshair`. Windows draws the hand for `kGrab` and `kGrabbing`, and X11 `XC_fleur` for both. |
| A dialog for **several files**, and for a **folder** | `view::FileRequest::multiple`, `choose_folder`; `Host::ChooseFiles`, `View::ChooseFiles` | **New**: `ChooseFiles` answers every path chosen, in the dialog's order. `multiple` is an open's; `choose_folder` asks for a folder and ignores the patterns. Windows `FOS_ALLOWMULTISELECT` and `FOS_PICKFOLDERS`, macOS `NSOpenPanel` `allowsMultipleSelection` and `canChooseDirectories`, Linux zenity's `--multiple` (one path to a line) and `--directory`, kdialog's `--multiple --separate-output` and `--getexistingdirectory`. `ChooseFile` is the same dialog answering the first path, and is what it always was for a request that sets neither. A host that has only `ChooseFile` answers a single request through it and a folder with none. |
| Files dropped in | `View::FilesDropped(paths, position)` | Existed: every file of the drop, in order, and the drop's position in the view's own coordinates. |
