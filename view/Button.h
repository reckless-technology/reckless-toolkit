#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <utility>

#include "View.h"

namespace reckless::view
{
/**
    The series' button: a plate with its text, lit a shade under the pointer and more while held, which does its
    action on a release over it (a press dragged off and released elsewhere does nothing). A toggle keeps its state
    and flips it on each click. How the plate looks is the panel's (the series' button plate, look), handed in
    as a painter. A button whose click already acts may take a second action on a right click (on_secondary_click).
*/
class Button final : public View
{
  public:
    /** What the painter is told of the button's state. */
    struct Look
    {
        bool on{false};     ///< a toggle that is on
        bool over{false};   ///< the pointer over it
        bool held{false};   ///< pressed, the pointer still over it
        bool dimmed{false}; ///< disabled: drawn faded, and a press does nothing
        bool lamp{false};   ///< a toggle that shows whether it is on by a lamp on its plate, lit or dark
    };

    /** How the plate is drawn: the panel's, normally the series' button plate.
        @param canvas  the canvas to draw on, in the button's local coordinates
        @param bounds  the button's bounds, in design pixels, origin zero
        @param text    the button's text
        @param look    the button's state */
    using Painter = std::function<void(gfx::Canvas &, gfx::RectI bounds, std::string_view text, Look look)>;

    /** @param text     the button's text
        @param painter  draws the plate; called on every paint
        @param clicked  the action, run on a release over the button; may be empty for none
        @param toggle   true for a toggle, which flips its state on each click before `clicked` runs */
    Button(std::string text, Painter painter, std::function<void()> clicked, bool toggle = false)
        : text_(std::move(text)), painter_(std::move(painter)), clicked_(std::move(clicked)), toggle_(toggle)
    {
        cursor     = Cursor::kPointingHand;
        look_.lamp = toggle;
    }

    /** A second action, for a button whose click already acts (the Cab's CABRACADABRA and the Space's GOOD
        VERBRATIONS roll a design, and take a typed seed on a right click): set, a press of the right button - or a
        control-click on a Mac, which a window hands on as one - holds no plate down and runs this on its release over
        the button in place of the click; a release elsewhere runs nothing. Unset, a right press clicks as a left one
        does. A double click could not carry it: the first click of the pair would act before the second arrived. */
    std::function<void()> on_secondary_click;

    /** Change the text; repaints when it differs.
        @param text  the new text */
    void SetText(std::string text)
    {
        if (text != text_)
        {
            text_ = std::move(text);
            Repaint();
        }
    }

    /** @return  the button's text, valid until the next SetText */
    const std::string &text() const noexcept
    {
        return text_;
    }

    /** Set a toggle's state; repaints when it differs. The click handler is not run.
        @param on  true for lit */
    void SetOn(bool on)
    {
        if (on != look_.on)
        {
            look_.on = on;
            Repaint();
        }
    }

    /** Show the button's on state by a lamp on its plate, for a button whose owner sets it on and off (SetOn) - a
        drawer's button lit while its drawer is open - as a toggle shows its own; repaints when it differs.
        @param lamp  true to carry the lamp */
    void SetShowsLamp(bool lamp)
    {
        if (lamp != look_.lamp)
        {
            look_.lamp = lamp;
            Repaint();
        }
    }

    /** @return  true when the toggle is on */
    bool on() const noexcept
    {
        return look_.on;
    }

    /** Dim or restore the button. A dimmed button ignores presses and shows an arrow cursor.
        @param dimmed  true to disable it */
    void SetDimmed(bool dimmed)
    {
        if (dimmed != look_.dimmed)
        {
            look_.dimmed = dimmed;
            cursor       = dimmed ? Cursor::kArrow : Cursor::kPointingHand;
            Repaint();
        }
    }

    /** @return  true while the button is dimmed: drawn faded, ignoring presses */
    bool dimmed() const noexcept
    {
        return look_.dimmed;
    }

    /** `changed` is told the button's held state - pressed with the pointer over it - each time it changes: what a
        momentary control acts on (the Comp's LISTEN, which listens while held). A press dragged off lets go and
        dragged back holds again.
        @param changed  called with the new held state, on the message thread; replaces any earlier one */
    void OnHeldChanged(std::function<void(bool held)> changed)
    {
        held_changed_ = std::move(changed);
    }

    /** @return  true while pressed with the pointer over the button */
    bool held() const noexcept
    {
        return look_.held;
    }

    /** Draws the plate through the painter with the current look.
        @param c  the canvas, in the button's local coordinates */
    void Paint(gfx::Canvas &c) override
    {
        painter_(c, LocalBounds(), text_, look_);
    }

    /** The pointer is over the plate: lit a shade. */
    void MouseEnter(const MouseEvent &) override
    {
        Set(true, look_.held);
    }

    /** The pointer left: the plate goes dark and any hold is let go. */
    void MouseExit(const MouseEvent &) override
    {
        Set(false, false);
    }

    /** A press holds the plate down, unless the button is dimmed; a right press with a second action set waits for
        its release instead.
        @param e  the press: its position and button */
    void MouseDown(const MouseEvent &e) override
    {
        if (look_.dimmed)
        {
            return;
        }
        if (e.right && on_secondary_click)
        {
            secondary_pending_ = true; // the plate stays as it was: the second action's press holds nothing down
            return;
        }
        pressed_ = true;
        Set(true, true);
    }

    /** A press dragged off the plate lets go; dragged back it holds again.
        @param e  the drag: its position, in the button's local coordinates */
    void MouseDrag(const MouseEvent &e) override
    {
        if (pressed_)
        {
            const bool inside = LocalBounds().ToFloat().Contains(e.position);
            Set(inside, inside);
        }
    }

    /** A release over the button flips a toggle and runs the click (or the second action, for a right press); a
        release elsewhere does nothing.
        @param e  the release: its position, in the button's local coordinates */
    void MouseUp(const MouseEvent &e) override
    {
        if (secondary_pending_)
        {
            secondary_pending_ = false;
            if (LocalBounds().ToFloat().Contains(e.position) && on_secondary_click)
            {
                const auto action = on_secondary_click; // a copy: the action may destroy the button it belongs to
                action();
            }
            return;
        }
        const bool was_pressed = pressed_;
        pressed_               = false;
        const bool inside      = LocalBounds().ToFloat().Contains(e.position);
        Set(inside, false);
        if (was_pressed && inside && !look_.dimmed)
        {
            if (toggle_)
            {
                SetOn(!look_.on);
            }
            if (clicked_)
            {
                const auto action = clicked_; // a copy: the action may destroy the button it belongs to
                action();
            }
        }
    }

  private:
    /** Move the look's over and held flags, repainting and telling OnHeldChanged's callback when held moved.
        @param over  the pointer is over the button
        @param held  pressed with the pointer over */
    void Set(bool over, bool held)
    {
        if (over != look_.over || held != look_.held)
        {
            const bool held_moved = held != look_.held;
            look_.over            = over;
            look_.held            = held;
            Repaint();
            if (held_moved && held_changed_)
            {
                held_changed_(held);
            }
        }
    }

    std::string           text_;                     ///< the button's text
    Painter               painter_;                  ///< draws the plate
    std::function<void()> clicked_;                  ///< the action, run on a release over the button
    bool                  toggle_;                   ///< a toggle: each click flips `look_.on`
    bool                  pressed_{false};           ///< a left press began here and has not been released
    bool                  secondary_pending_{false}; ///< a right press began here: its release is the second action
    Look                  look_;                     ///< what the painter is told

    std::function<void(bool)> held_changed_; ///< OnHeldChanged's
};
} // namespace reckless::view
