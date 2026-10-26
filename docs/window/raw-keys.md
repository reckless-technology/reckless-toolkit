# The raw keys a window delivers

The views' key path carries a key down and a typed character and nothing else. That is enough for the panels and the
dialogs it was written for - a text field, a menu's shortcut, Escape on a card - and not enough for a program that
reads the keyboard as a control surface rather than as typing. Such a program needs three things the views' path has
never had:

- **A key let go.** `view::View::KeyPressed` has no counterpart. Nothing in the toolkit told anyone a key had been
  released, on any platform.
- **Auto-repeat told apart from a tap.** A key held sends a run of key downs at the system's repeat rate - on a
  Windows desk, one after half a second and then thirty a second - and nothing in the event said which were the
  system's and which were the person's.
- **The physical key.** `view::Key` names fourteen keys: Escape, Return, Tab, Backspace, Delete, the four arrows,
  Home, End, Page Up and Page Down. It has no function keys and no space bar - a press of the space bar reaches the
  views as the character it types, and the keys of the keypad reach them as whatever their symbols say.

Every platform under the toolkit delivers all three. The window threw them away: Windows' `WM_KEYUP` fell through to
`DefWindowProcW` and bit 30 of its `lparam` was never read, the Linux window's `XSelectInput` mask held no
`KeyReleaseMask`, and the macOS view's class was given `keyDown:` and neither `keyUp:` nor `flagsChanged:`.

So a window now carries an **opt-in raw key callback** beside the views' path, and a product that wants the keyboard
asks for it.

## The seam

`window::ChildWindow::SetRawKeyCallback` ([window/Window.h](../../window/Window.h)) takes a
`window::RawKeyCallback`, and [window/RawKey.h](../../window/RawKey.h) is everything it hands over. An app on the
shell reaches its child window through `app::AppRunHooks::on_child` ([app/AppRun.h](../../app/AppRun.h)), which is
called with the window once it is made and before the window is opened to the person:

```cpp
reckless::app::AppRunHooks hooks;
hooks.on_child = [&](reckless::window::ChildWindow &child) {
    child.SetRawKeyCallback([&](const reckless::window::RawKeyEvent &key) { controls.Heard(key); });
};
```

A plug-in's editor, which makes its own child window, calls it on the window `window::CreateChildWindow` gave it.

A `RawKeyEvent` is one edge of one key:

| Member | What it is |
|---|---|
| `is_down` | the key going down, or being let go |
| `is_repeat` | a down of a key the window has already said is down: auto-repeat's |
| `named_key` | the toolkit's own name for the key (`view::Key`) where it has one, else `Key::kNone` |
| `platform_key_code` | the platform's own code: the virtual-key code on Windows, the X11 keycode on Linux, an `NSEvent`'s `keyCode` on macOS |
| `physical_key_code` | the physical key, whatever the layout makes it type - **the identity of a key** |
| `modifiers` | the modifier keys held as the edge arrived |

`physical_key_code` is what a consumer keys its own set of held keys on. On Windows it is the scan code the keyboard
sent, with `0xE0` before an extended key's, so the two shift keys, the two control keys, the keypad's enter and the
arrow block above the keypad all differ - where `platform_key_code` gives one `VK_SHIFT` for both shift keys and
follows the keyboard layout for the letters. On Linux and macOS the platform's own code already names the physical
key, and the two members are equal.

The toolkit names no physical key beyond the fourteen `view::Key` has, and does not try to: a program that wants W,
A, S and D, or F1, carries its own table of the three platforms' codes for them. That is the honest place for it -
the codes are the platform's, and a keyboard's layout is the product's business.

## What the window promises

- **Nothing until it is asked.** A window whose owner has not called `SetRawKeyCallback` reads nothing of the raw
  keys and delivers nothing. On Linux it does not even ask the server for the key releases, so its event stream is
  the one it always had; on Windows `WM_KEYUP` falls through to `DefWindowProcW` as before; on macOS `keyUp:`,
  `flagsChanged:` and `resignFirstResponder` call `NSView`'s own implementations, which is all that happened when the
  methods were not there.
- **The views are untouched.** The callback is told of an edge; it cannot keep the edge from the views, change what
  they are given, or be consulted about it. `view::KeyEvent`, `view::View::KeyPressed` and the router's focus-only
  delivery are exactly what they were, and the key downs the views are given are exactly the key downs they were
  given before.
- **The edges of one key alternate.** Every down is followed by exactly one up, and no up arrives for a key whose
  down did not - a key held as the window takes the keyboard sends an up the window never promised, and that up is
  dropped. So a set of held keys built from these events cannot go wrong.
- **A key held as the window loses the keyboard is let go.** Every key still held is delivered as an up, oldest
  first, carrying the codes and the name of the down it answers and no modifiers - nothing says what is held once
  the keyboard has gone. Nothing a consumer holds down can stay down.
- **A platform's own repeat flag is not used.** Windows' bit 30 and an `NSEvent`'s `isARepeat` are both set for a key
  that was already down when the window took the keyboard, whose down nothing was told of, which would leave a
  consumer holding a repeat for a key it has never seen pressed. `is_repeat` is the window's own answer from what it
  has said is down (`window::RawKeysHeld`), so the first down a window delivers is never a repeat.
- **Raw keys arrive while the window holds the platform's keyboard**, which it does while a view of its tree holds
  the router's focus (`view::Host::RequestKeyboardFocus`, `view::View::GrabKeyboardFocus`). A game's root view asks
  for the focus as it is shown and has the keyboard from then on. A window no view has asked for the keyboard in is
  sent no keys by the platform at all, and there is nothing for the callback to hear.
- **No tuning number is the toolkit's.** There is no repeat interval, no dead time and no key list here: the edges
  are reported as the platform gives them and the product decides what they mean.

## Each platform

| | Windows | Linux (X11) | macOS |
|---|---|---|---|
| Key down | `WM_KEYDOWN`, `WM_SYSKEYDOWN` | `KeyPress` | `keyDown:` |
| Key let go | `WM_KEYUP`, `WM_SYSKEYUP` | `KeyRelease`, asked for with the first callback | `keyUp:` |
| Modifier keys | a key down and up of their own | a `KeyPress` and `KeyRelease` of their own | `flagsChanged:` alone |
| The keyboard lost | `WM_KILLFOCUS` | `FocusOut` (`NotifyNormal`) | the router's focus dropped, `resignFirstResponder`, or the window resigning key |
| Physical key | the scan code, `0xE0` before an extended key's | the X keycode | the virtual key code |

Two things are worth knowing beyond the table.

**A Mac reports a modifier key no other way.** A shift, control, option or command key sends no key down at all, only
`flagsChanged:`, which says which key changed and never which way it went - and its flag stays set while the other
key of the pair is held. So the window reads the edge as a down where the key's flag is set and the key is not held
already, and as an up otherwise. That is also right for caps lock, whose flag follows the lamp and so is set by one
press and cleared by the next. Without this the macOS raw path would have been the only one of the three with no
modifier keys in it.

**X's auto-repeat sends a release before each repeated press.** A window asking for raw keys therefore also asks the
server for *detectable auto-repeat* (`XkbSetDetectableAutoRepeat`), which has the server leave that release out, so a
key held arrives as a run of presses as it does on the other two platforms. Where the server refuses - which no
server since XFree86 4 does - the release is recognised instead by the press that follows it: auto-repeat's two
events name the same key, in the same window, at the same millisecond
(`window::IsX11AutoRepeatRelease`), and the release is passed over.

## What is held, and where

The policy needs no window, no display and no keyboard, and it is in one place for that reason
([window/RawKey.h](../../window/RawKey.h)) - as Windows Audio's arithmetic is
([docs/audio/native-backends.md](../audio/native-backends.md)). `tests/test_raw_keys.cpp`, in the `view` suite, holds
all of it on **every platform, in the builds with no GUI too**: the keys a window has said are down and the three
answers they give (whether a down is a repeat, whether an up is owed, what is to be let go), the scan code and the
extended prefix out of a Windows key message's flags, and X11's auto-repeat release.

`tests/test_app_shell.cpp`, in the `app` suite, holds the opt-in seam through the app's shell on a child window of
the suite's own, on **every platform**: the child window handed to the product before the window is opened, a window
left with no callback at all where the product asks for nothing, and every edge the window delivers reaching the
product's callback.

The windows themselves are held by the child window's own suite - `skia/tests/test_child_window.cpp` and
`skia/tests/test_child_window_mac.mm`, the `child_window` CTest, label `window` - each platform driving its own key
events into a real child window with no keyboard touched: `SendMessageW` on Windows, `XSendEvent` on Linux (on a
display, under `xvfb-run` in CI) and `NSEvent` key events sent to the view on macOS. Each holds the same run - a key
held until it repeats and let go, a key the views have no name for, two keys at once, everything let go as the
keyboard goes, and a callback taken back - and each also holds that the views were given the key downs they were
always given and nothing else.
