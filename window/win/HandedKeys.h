#pragma once

// The two messages a child window hands a key on with, to the window it sits in, when no view took the key and its
// owner asked for that (ChildWindow::HandOnKeysTo): the child window (window/win/WindowWin.cpp) sends them and the
// app's top-level window (app/win/AppWin.cpp) answers them, each including this after <windows.h>. A key the child
// window holds the focus for never reaches its parent as Windows delivers keys, which is why it is handed on - as a
// Mac's view hands a key up its responder chain and an X11 child window sends it to its parent.

namespace reckless::window::win
{
/** A key down (WM_KEYDOWN or WM_SYSKEYDOWN) no view took: its wparam and lparam as they came; answered nonzero when
    the receiver used it (a shortcut chose its entry). */
constexpr UINT kHandedKeyDown = WM_APP + 40;

/** A character (WM_CHAR) no view took: its wparam and lparam as they came; answered nonzero when it was used. */
constexpr UINT kHandedChar = WM_APP + 41;
} // namespace reckless::window::win
