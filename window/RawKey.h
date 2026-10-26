#pragma once

// The raw keys a child window delivers to an owner that asks for them (ChildWindow::SetRawKeyCallback): one edge of
// one physical key, which the views' KeyPressed path cannot carry - it has a key down and a typed character and
// nothing else, so a key held is a run of presses no program can tell from a run of taps, a key let go is silent, and
// a key the view::Key enum does not name (a function key, the space bar) arrives as the character it types or not at
// all. Everything here is the platform's answer in the toolkit's words, and the parts of each platform's answer that
// need none of that platform's headers live here too - the keys a window has said are down, the scan code inside a
// Windows key message's flags, and X11's auto-repeat release - so the suites hold them on every platform, as
// audio/win/WasapiFormats.h holds Windows Audio's arithmetic.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <vector>

#include "View.h"

namespace reckless::window
{
/**
    One edge of one key: the key going down or being let go, and which key it was. A window tells its owner of every
    edge it receives while it holds the platform's keyboard (which it does while a view of its tree holds the router's
    focus, view::Host::RequestKeyboardFocus), and the edges of one key alternate - every down is followed by exactly
    one up, and no up arrives for a key whose down did not. A key still held when the window loses the keyboard is let
    go, so nothing a consumer holds down stays down.

    Two edges are of the same key when their `physical_key_code` is the same: that, and not `platform_key_code`, is
    what a consumer keys its own set of held keys on, since Windows gives one virtual-key code for both shift keys and
    one for both control keys while their scan codes differ.
*/
struct RawKeyEvent
{
    bool      is_down{false};   ///< true for the key going down, false for it being let go
    bool      is_repeat{false}; ///< true for a down of a key the window has already said is down: auto-repeat's
    view::Key named_key{view::Key::kNone}; ///< the toolkit's name for the key, where it has one; else Key::kNone

    /** The platform's own code for the key: the virtual-key code on Windows (a key message's `wparam`), the X11
        keycode on Linux, and an NSEvent's `keyCode` on macOS. The letter and punctuation codes follow the keyboard
        layout on Windows alone; on Linux and macOS this code already names the physical key. */
    std::uint32_t platform_key_code{0};

    /** The physical key, whatever the layout says it types: the scan code on Windows (set 1, with 0xE0 before an
        extended key's, so the two shift keys, the two control keys and the keypad's own keys differ), and the same as
        `platform_key_code` on Linux and macOS, whose codes already name the physical key. 0 where the platform named
        no key - a key message another program synthesised with no scan code in it. */
    std::uint32_t physical_key_code{0};

    view::Modifiers modifiers{}; ///< the modifier keys held as the edge arrived; empty for a key let go by LetGo
};

/** Told of every raw key edge a window receives, on the window's thread, for as long as the window lives.
    @param key  the edge */
using RawKeyCallback = std::function<void(const RawKeyEvent &key)>;

/**
    The keys a window has told its owner are down, by physical key code, in the order they went down. The window keeps
    one of these so that it can answer three things no platform answers the same way: whether a key down is one of
    auto-repeat's (the key is held already), whether an up is one its owner is owed (the down was told), and what is
    still held when the window loses the keyboard and must be let go.

    A platform's own repeat flag is not used for the first of those - Windows' bit 30 and an NSEvent's `isARepeat` are
    both set for a key that was already down when the window took the keyboard, whose down the window never told
    anyone about, which would leave a consumer holding a repeat for a key it has never seen pressed. Held here, the
    first down a window delivers is never a repeat.
*/
class RawKeysHeld
{
  public:
    /** `key` noted as held, and whether it already was - the repeat flag of the down being told. What is noted is the
        up this key is owed, so an edge kept here never carries a stale repeat flag or stale modifiers.
        @param key  the key going down, its `physical_key_code` its identity
        @return     true when the key was already held, so the down is one of auto-repeat's */
    bool NoteDown(const RawKeyEvent &key)
    {
        for (const RawKeyEvent &held : held_)
        {
            if (held.physical_key_code == key.physical_key_code)
            {
                return true;
            }
        }
        RawKeyEvent up = key;
        up.is_down     = false;
        up.is_repeat   = false;
        up.modifiers   = {};
        held_.push_back(up);
        return false;
    }

    /** The key `physical_key_code` noted as let go, and whether it was held - whether its owner is owed the up. An up
        for a key that was never noted down is no one's: the key went down before the window had the keyboard.
        @param physical_key_code  the key's physical code
        @return                   true when the key was held, so the up is to be told */
    bool NoteUp(std::uint32_t physical_key_code)
    {
        for (std::size_t i = 0; i < held_.size(); ++i)
        {
            if (held_[i].physical_key_code == physical_key_code)
            {
                held_.erase(held_.begin() + static_cast<std::ptrdiff_t>(i));
                return true;
            }
        }
        return false;
    }

    /** Whether the key `physical_key_code` is held.
        @param physical_key_code  the key's physical code
        @return                   true while it is held */
    bool IsHeld(std::uint32_t physical_key_code) const
    {
        for (const RawKeyEvent &held : held_)
        {
            if (held.physical_key_code == physical_key_code)
            {
                return true;
            }
        }
        return false;
    }

    /** An up for every key still held, in the order they went down, and nothing held afterwards: what a window tells
        its owner as it loses the keyboard, so no key it said was down stays down. Each carries the codes and the name
        of the down it answers, and no modifiers - nothing says what is held once the keyboard has gone.
        @return  the ups to tell, oldest first; empty when nothing is held */
    std::vector<RawKeyEvent> TakeUps()
    {
        std::vector<RawKeyEvent> ups;
        ups.swap(held_);
        return ups;
    }

    /** How many keys are held.
        @return  the count */
    std::size_t count() const noexcept
    {
        return held_.size();
    }

  private:
    /** The up owed for each key held, oldest first: a keyboard holds a handful of keys at once, so this is searched
        through rather than indexed. */
    std::vector<RawKeyEvent> held_;
};

/** The physical key a Windows key message names, from the low 32 bits of its `lparam`: the scan code the keyboard sent
    (bits 16 to 23), with 0xE0 before it where the extended bit (bit 24) is set, as the keyboard itself sends 0xE0
    before the right-hand modifiers, the keypad's enter and divide, the arrow and editing keys of the block between the
    letters and the keypad, and the Windows and menu keys. Nothing of the platform's headers is needed to read it, so
    every suite holds this.
    @param key_message_flags  the low 32 bits of a WM_KEYDOWN, WM_KEYUP, WM_SYSKEYDOWN or WM_SYSKEYUP `lparam`
    @return                   the scan code, 0xE0 before an extended key's; 0 where the message carried none */
constexpr std::uint32_t WindowsPhysicalKeyOf(std::uint32_t key_message_flags) noexcept
{
    const std::uint32_t scan_code = (key_message_flags >> 16) & 0xFFu;
    const bool          extended  = (key_message_flags & (1u << 24)) != 0;
    return extended ? (0xE000u | scan_code) : scan_code;
}

/** A key edge as X11 stamps it: which window it was delivered to, which key, and when - all the auto-repeat test
    below needs, and none of it X11's own types, so every suite holds that test. */
struct X11KeyStamp
{
    unsigned long window{0};   ///< the X window id the event was delivered to (XKeyEvent::window)
    unsigned int  key_code{0}; ///< the X keycode (XKeyEvent::keycode)
    unsigned long time{0};     ///< the server time it is stamped with, in milliseconds (XKeyEvent::time)
};

/** Whether a key release is one X's auto-repeat made rather than a person letting the key go: auto-repeat sends a
    release and a press of the same key, to the same window, stamped at the same millisecond, where the server does not
    give detectable auto-repeat - which suppresses the release instead, and which a window asks for whenever it asks
    for raw keys. So a release is auto-repeat's when the next event waiting is a press that matches it in all three.
    @param released        the release in hand
    @param is_next_a_press whether the next event waiting is a key press; false where none waits or it is anything else
    @param next            that next press's stamp, read only when `is_next_a_press`
    @return                true when the release is auto-repeat's and is to be passed over */
constexpr bool IsX11AutoRepeatRelease(const X11KeyStamp &released, bool is_next_a_press,
                                      const X11KeyStamp &next) noexcept
{
    return is_next_a_press && next.window == released.window && next.key_code == released.key_code &&
           next.time == released.time;
}
} // namespace reckless::window
