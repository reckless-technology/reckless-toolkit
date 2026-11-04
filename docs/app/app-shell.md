# A desktop app's shell

A product that is a desktop app rather than a plug-in - a window of its own, opened by the person, laid out to its size -
builds on the platform seam (`app/AppPlatform.h`) through the shell in `app/`: the window and its loop, the root that
holds the app's views, the app's colours and fonts, the widgets its window is made of, what its views draw with on Skia,
and its update check. The shell is written against no platform and no renderer but for `AppResources`, so a product's
suite drives the whole app on the suites' own platform (`tests/FakeAppPlatform.h`) with a child window of its own.

| Header | Target | What it is |
|---|---|---|
| `app/AppRun.h` | `reckless_app_shell` | `ParseAppOptions` and `RunApp`: the command line's `--smoke`, the window opened round the app's views, its frames, its loop |
| `app/AppRoot.h` | `reckless_app_shell` | `AppRoot`: the views at the window's size in a zoom, zoomed down below the layout's least or held at a fixed zoom the app sets, and the series' tooltip |
| `app/AppWindowSizes.h` | `reckless_app_shell` | `WindowSizes`: a View menu's sizes, 50 % to 150 % of the design, held on the root - the size in force, those the screen takes, the next up and down, kept through full screen |
| `app/AppFullScreen.h` | `reckless_app_headers` | `FullScreenMenuEntry`: a View menu's full-screen item as each platform writes it - F11 (and Alt+Enter on Windows), Control-Command-F on a Mac - toggling the window's full screen |
| `app/AppTheme.h` | `reckless_app_shell` | `Theme`, the app's colours, its palette and tooltip; `fonts`, the series' serif at the series' sizes |
| `app/AppWidgets.h` | `reckless_app_shell` | `Section`, `TextLine`, `Slider`, `ChoicePlate` and `MakeButton`, in the theme |
| `app/AppUpdates.h` | `reckless_app_shell` | `MakeUpdateChecker`: a product's update check (`update::MakeChecker`), none where `RECKLESS_UPDATE_CHECK=off`; `UpdatesMenuEntry`, its Updates menu as a menu of the menu bar |
| `app/AppResources.h` | `reckless_app_resources` (with the GUI) | `AppResources`: the series' context on Skia, the look's images, the company mark |

## The window and its loop: `RunApp`

```cpp
int main(int argc, char **argv)
{
    reckless::app::AppOptions options = reckless::app::ParseAppOptions({argv + 1, argv + argc});
    // ... the product takes its own flags out of options.unknown ...
    const auto platform = reckless::app::AppPlatform::Create(name, reckless::embedded::Find("RecklessAppIcon256.png"));
    const reckless::app::AppResources resources(version);
    MainView view(resources);
    reckless::app::AppRoot root(view, theme.Tooltip(), {}, {kLayoutWidth, kLayoutHeight});
    root.SetBounds({width, height});
    reckless::app::AppRunHooks hooks;
    hooks.on_frame = [&](double seconds) { view.Frame(seconds); root.Frame(); };
    hooks.on_end   = [&](auto position) { view.End(position); };
    return reckless::app::RunApp(*platform, root, {kMinimumWidth, kMinimumHeight}, options, name, version,
                                 &reckless::window::CreateChildWindow, hooks, saved_position);
}
```

The platform comes first because making it starts Skia for the processor ([platform.md](platform.md)), before the
resources decode anything.

`ParseAppOptions` reads `--smoke [frames]` and keeps every other argument in `unknown`, in order; the product takes its
own flags from there, and `RunApp` reports what is left as ignored. `RunApp` opens the window, titled `<name>  v<version>`,
round the root at the root's size - held first to what fits the display it opens on (`AppPlatform::LargestContent`), never
under the minimum - where it stood last or centred; the person may resize it down to the minimum, and the root follows
each resize, the child window following each change of the display's scale. It makes the child window through the maker
it is given (`window::CreateChildWindow`, or a suite's own), hands it the files a desktop drags over the window, calls
`on_open` with the window, then `on_frame` once a display frame with the seconds since the last (`view::FrameSteps`),
runs the loop until the window is closed or the app is quit, calls `on_end` with where the window stood, and closes
it. A smoke run prints the name and the version, quits once the window has drawn its frames (thirty unless the count
says) and returns 3 where it drew fewer - none in twenty seconds means no display; 1 is no window at all.

## The root: `AppRoot`

The app's views are laid out at the window's own size - an app's window is a document, not a face plate - down to the
least its layout takes; a window smaller than that shows the layout at that size zoomed down to fit (`view::ZoomView`),
so a small screen shows all of it, smaller, rather than a layout squeezed past its least. With no least the views are
always at the window's size. In front of them is the series' tooltip, shown, moved and hidden by the series' tracker from
what the zoom's watch hears (`view::TooltipTracker`); `Frame()` ticks it once a display frame.

An app that offers sizes of its own - a View menu of 50 % to 150 % - holds the content at one size and zoom instead:
`SetFixedZoom(design, zoom)` gives the size the root holds it at (the design size times the zoom), the app sizes the
window to it with `AppWindow::SetContentSizeOnScreen` - its top left kept unless the window would cross an edge of the
work area it is most on, when it is moved in until all of it stands there, and the size taken told to `resized` on
every platform, so the root and the child window follow it as they follow a person's resize - and the content is laid
out at its design size and drawn at exactly that zoom. `fixed_zoom()` says the zoom in force, the one a menu ticks; the
first time the root is sized to anything else - the person dragging the frame, a display too small for the size - it
lets the zoom go and lays the content out at the window's size again. `AppWindow::LargestContent()` is the most the
window's screen takes where it stands now, which such a menu holds its sizes to. A menu item may answer to more than
one key: `MenuEntry::other_shortcuts` choose it as its `shortcut` does and show nowhere (Ctrl++ beside Ctrl+=).

`WindowSizes` (`app/AppWindowSizes.h`) is that menu's logic written once: the sizes `kWindowSizes` (50, 75, 100, 125
and 150 %), `Restore(percent)` before the window opens - the root held at the size the app kept and its bounds set to it
- `SetWindow` once it is open, `Choose(percent)` for a size chosen (refused for one the window's screen does not take,
which the menu shows dim rather than shrinking it, so a ticked size is always the size drawn), `Frame` to ask the
screen's room again every half a second, since the window may be moved to another screen, and `Menu()` for what the
menu shows: the size in force, those that fit, and the next size up and down - from a dragged window, the next past the
scale it is at. Where the size is kept between launches is the app's: `InForce()` is what to keep, 0 for a window
dragged to another size.

## Full screen

**Opt-in, per app.** A window goes full screen only where the app opened it so - `AppRunHooks::may_go_full_screen`,
which `RunApp` hands the platform as `WindowOptions::may_go_full_screen` - and every app that does not ask keeps the
window it always had: on macOS out of every full-screen space, its green button zooming it. A dialog never goes. The
calls are the window's (`app/AppPlatform.h`):

```cpp
bool AppWindow::CanGoFullScreen() const;           // opened with it allowed, on a platform that has it
bool AppWindow::IsFullScreen() const;              // full screen, or on its way there: the state last asked for
bool AppWindow::SetFullScreen(bool is_full_screen); // made or begun; false where nothing was done
bool AppWindow::EnterFullScreen();                 // SetFullScreen(true)
bool AppWindow::LeaveFullScreen();                 // SetFullScreen(false)
bool AppWindow::ToggleFullScreen();                // SetFullScreen(!IsFullScreen())
```

Full screen covers the display the window is most on. The app hears it as it hears any resize - `resized` at the
display's size in design units, the child window following at the display's scale, so the root and everything in it are
laid out at the new size - and then `AppRunHooks::on_full_screen(bool)` (`WindowOptions::full_screen_changed`), whoever
asked: the app, the green button on a Mac, a window manager's own key on Linux. Back from full screen the window has the
frame, the place and the size it had, to the pixel. While it is full screen `Position()` is where it stood and
`LargestContent()` what the display takes of a window, so what an app keeps and offers is the window it will come back
to; a size of the app's own (`SetContentSize`, `SetContentSizeOnScreen`) leaves full screen first. How each platform
does it is in [platform.md](platform.md#full-screen).

**The menu item** is `FullScreenMenuEntry(window, id)` (`app/AppFullScreen.h`), for the app's View menu: "Full screen",
ticked while it is, on F11 - and on Windows Alt+Enter too, as games take it - and on a Mac "Enter Full Screen" and "Exit
Full Screen" on Control-Command-F, as AppKit names them. It is dim where the window may not go. Menus are an app's own,
so the app places the item and sets its bar again when `on_full_screen` says the state changed, as it does for any tick.
**Escape is the app's choice**, not the toolkit's: a game may want the key for itself, and an app that wants Escape to
leave full screen gives itself that key. The Mac's shortcut is written with `Shortcut::control`, a modifier of the
Mac's own beside Command, which only a shortcut written with it matches and which Windows and Linux never set.

**Remembering it is the app's choice too.** The toolkit keeps no placement between runs: an app keeps where the window
stood (`on_end`'s position, which is the windowed place even when it ends full screen) and may keep whether it was full
screen, reading `IsFullScreen()` from the window it was given in `on_open` before the window closes. To open that way
again it sets `AppRunHooks::opens_full_screen`: the window opens at its own place and size, which it comes back to,
and goes full screen as the loop starts - never in a smoke run, which ends before a Mac's animation would.

`WindowSizes` keeps its size through full screen: while full screen the size it had is the one in force, ticked and
dim, and every size is refused; back from full screen the root holds that size again, at once or once the window gets
there, so `InForce()` read at the end is the window's size, not the display's.

```cpp
hooks.may_go_full_screen = true;
hooks.opens_full_screen  = settings.was_full_screen;
hooks.on_open            = [&](reckless::app::AppWindow &opened) { window = &opened; sizes.SetWindow(&opened); };
hooks.on_full_screen     = [&](bool) { SetMenuBarAgain(); };    // FullScreenMenuEntry(window) is ticked anew
hooks.on_end             = [&](auto position) { settings.was_full_screen = window->IsFullScreen(); /* ... */ };
```

## The theme and the widgets

`Theme` is the product's: the window's colour, the rules and frames, the plates and the plate under the pointer, the ink
and its two fainter shades, the accent and the accent at rest, a good and a bad state. `Palette()` is the series'
palette in it, which the toolkit's own views are dressed in (`panel/PanelStyle.h`, `panel/ListStyles.h`); `Tooltip()`
the tooltip's style. The widgets each take the theme and copy it:

- `Section` - a frame in the track colour with its title in the accent on its top edge; the owner lays its controls out
  in `ContentArea()`.
- `TextLine` - a line, or a few, of text in a colour and a font; it takes no mouse.
- `Slider` - whole numbers on a groove; a press jumps, a drag follows, the wheel steps, `on_change` hears the person.
- `ChoicePlate` - the series' legend plate with a menu of named choices and a caption beside it; `on_choose` hears the
  person, `SetCurrent` tells no one.
- `MakeButton` - the series' button in the theme, a toggle lit by a lamp.

## What the views draw with: `AppResources`

The series' context on Skia (`window/ModulePanel.h`'s `ModulePanelContext`: the serif handed to Skia, images decoded,
cut and resampled, the files the binary carries), the look's image backend on it and the company mark, decoded from the
binary's own `RecklessRtLogo.png`; and Steinberg's ASIO-compatible logo where the binary carries
`AsioCompatibleLogo.png`. An app has no host parameters, so every control asked for by id is the still one. The binary
carries its files through `reckless_module_assets`, and the monospaced family where it asks for it
(`reckless_monospace_font_files`; [svg-and-monospace.md](../gfx/svg-and-monospace.md)).

## The update check

`MakeUpdateChecker` makes an app's update check from its description (`update::Product`: the name its request
carries, its version, the manifest the website writes from its newest public release and the file of its own it keeps
what it found in) through the toolkit's one factory, `update::MakeChecker`, or none where `RECKLESS_UPDATE_CHECK=off`
(the CI runners set it), so a window offers no update menu that could only say it has not checked. The menu itself is
the toolkit's Updates menu (`update/UpdatesMenu.h`), the window polling the check from its frame;
[update-check.md](../update/update-check.md) has the whole service. `EnvironmentVariable` reads a variable as each
runtime reads it safely (`core/ProcessEnvironment.h`).

## Where it came from

The shell is Reckless IR Capture's window's generic part, lifted so that no product copies it: its `AppRun`, `AppRoot`,
`Theme`, `Widgets`, `Resources` and `Updates`. Its behaviour is theirs, with the product's own parts made arguments: the
name in the title and the messages, the tooltip's style and the widgets' colours from a `Theme` value rather than a
namespace of constants, the update check's manifest, file and name from an `update::Product`, and the hooks in an
`AppRunHooks` (with `on_open` beside the frame and the end). IR Capture's `--simulate`, which its `--smoke` implies, is
its own flag, read from `unknown`; its capture button stays its own.
