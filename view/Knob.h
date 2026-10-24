#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <numbers>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "Canvas.h"
#include "Label.h"
#include "ParameterControl.h"
#include "View.h"

namespace reckless::view
{
/**
    The series' knob: a rotary slider with its value in a box under it and its caption, a label of its own, over it,
    drawn and played as the knob the releases built on JUCE drew and played, so a panel lands on the same pixels and
    turns as it did.

    Drawn: the dial fills the knob over its value box - the box `box_width` by `box_height`, held to the knob's width
    and to all but kMinDialHeight of its height, centred at its bottom - the panel's painter handed that area and the
    angle the parameter's position puts the pointer at, kStartAngle to kEndAngle radians clockwise from twelve; the
    box is a Label (Label.h) showing the parameter's text with the knob's suffix; the caption is a Label the panel
    places over the knob as a sibling of it. Dimmed, the knob and its caption fade to the dimmed alpha, the box's text
    to half of that again, and the knob turns no more.

    Played: a press and a drag turn it a full sweep in kPixelsForFullDrag pixels, right or up raising it, measured
    from where the press began (dragged past an end and back, it returns at once; back where it began, it is on the
    value it began on, so a press that does not move moves nothing); with Command or Alt held the drag is a fine one
    instead, each move's pixels through the slider's velocity curve. The press and the release are one gesture round
    the moves. A double click, or an Alt-click, puts it back to its default - or to the value the panel gives a double
    click in the default's place (SetDoubleClickValue: the Mod's mode starting points) - as a gesture of its own after
    the release. The wheel moves it 0.15 of its sweep every 100 of the views' pixels (a unit of JUCE's wheel delta;
    the child windows hand a notch on as 20 pixels on Windows and Linux, 0.03 of the sweep), and a stepped parameter
    at least one step, as one gesture, unless a button is held. A click on the value box opens it to type into;
    Return or a click elsewhere sets what was typed, read through the parameter's readout with the suffix taken off,
    as one gesture when it moves the knob, and text the readout cannot read sets nothing; Escape leaves it. The wheel
    over the box turns the knob, the box handing it on as the JUCE slider's label did.
    Each of these is the slider's arithmetic, in the parameter's own units (ParameterControl's dial) where the slider
    works in them, so a stepped parameter lands on its steps. A knob given a secondary click (on_secondary_click: the
    Pedalboard's macros and floor knobs open a menu on a right click, as the JUCE slider that hands a right click to
    its owner does) runs it on a right press on the dial instead of turning, dimmed or not, since that slider hands the
    press on before it asks whether it is enabled; a right click on the value box does nothing, as on the JUCE box.

    A knob a tempo division overrides follows it (FollowSync): dimmed, and showing the value the division gives
    rather than its own, which it takes back when the division lets go - its parameter untouched throughout.

    Left out: the JUCE slider hides the pointer through a fine drag and lets it run past the screen's edge, and its
    value box holds the panel's input while it is open, so a press elsewhere only ends the edit; here the press ends
    the edit and goes on to what it landed on.
*/
class Knob final : public View
{
  public:
    static constexpr float kStartAngle =
        std::numbers::pi_v<float> * 1.2f;                                ///< the pointer's angle at position 0, radians
    static constexpr float kEndAngle = std::numbers::pi_v<float> * 2.8f; ///< the pointer's angle at position 1, radians
    static constexpr double kPixelsForFullDrag = 250.0; ///< the drag, in design pixels, that turns a full sweep
    static constexpr int    kMinDialHeight     = 15;    ///< the dial's height at least, in design pixels

    /** Paints the dial: the film strip's frame for `angle` in `area`, the slider's area over its box.
        @param canvas  the canvas, in the knob's local coordinates
        @param area    the dial's area, in design pixels
        @param angle   the pointer's angle, kStartAngle to kEndAngle radians clockwise from twelve */
    using DialPainter = std::function<void(gfx::Canvas &, gfx::RectI area, float angle)>;

    /** The knob's look: its two labels and the value box's size. */
    struct Style
    {
        Label::Style caption;            ///< the caption's look
        Label::Style value;              ///< the value box's look
        int          box_width{62};      ///< the value box's width at most, in design pixels
        int          box_height{17};     ///< the value box's height at most, in design pixels
        float        dimmed_alpha{0.4f}; ///< the alpha a dimmed knob and its caption fade to
    };

    /** @param parameter  the parameter it drives, held by reference and outliving the knob
        @param caption    the caption's text
        @param painter    draws the dial; may be empty for none
        @param style      the look
        @param suffix     text after the value in the box, taken off typed text; empty for none */
    Knob(ParameterControl &parameter, std::string caption, DialPainter painter, const Style &style,
         std::string suffix = {})
        : parameter_(parameter), painter_(std::move(painter)), caption_(style.caption), box_(style.value),
          box_width_(style.box_width), box_height_(style.box_height), dimmed_alpha_(style.dimmed_alpha),
          suffix_(std::move(suffix)), shown_(parameter.Normalized())
    {
        caption_.SetText(std::move(caption));
        box_.SetText(parameter_.Text() + suffix_);
        box_.SetEditable(true);
        box_.on_edited = [this](const std::string &typed) { Type(typed); };
        AddChild(box_);
    }

    /** What a right press on the dial does in place of turning the knob, when set: a menu of the panel's (the
        Pedalboard's macros). It runs whether or not the knob is dimmed. */
    std::function<void()> on_secondary_click;

    /** The caption: the panel adds it beside the knob, over it.
        @return  the caption, a label of the knob's own */
    Label &caption() noexcept
    {
        return caption_;
    }

    /** @return  the value box, a label of the knob's own */
    Label &value_box() noexcept
    {
        return box_;
    }

    /** @return  the parameter the knob drives */
    ParameterControl &parameter() noexcept
    {
        return parameter_;
    }

    /** The knob and its caption faded and still, for a setting that has no use for it; false brings it back.
        @param dimmed  true to fade it and stop it turning */
    void SetDimmed(bool dimmed)
    {
        SetEnabled(!dimmed);
        SetAlpha(dimmed ? dimmed_alpha_ : 1.0f);
        caption_.SetAlpha(dimmed ? dimmed_alpha_ : 1.0f);
    }

    /** @return  true while the knob is dimmed */
    bool dimmed() const noexcept
    {
        return !IsEnabled();
    }

    /** The knob and its caption hidden whole, for a setting whose panel has no place for it; true brings it back.
        @param shown  true to show them */
    void SetShown(bool shown)
    {
        SetVisible(shown);
        caption_.SetVisible(shown);
    }

    /** What a double click (or an Alt-click) puts the knob at, in the parameter's units, in place of its default: a
        setting's own starting point (the Mod's RATE in tremolo). Outside the dial it does nothing, as the slider's.
        @param value  the value, in the parameter's own units */
    void SetDoubleClickValue(double value) noexcept
    {
        double_click_value_ = value;
    }

    /**
        A knob a tempo division overrides shows the value the division produces - `synced_value`, in the parameter's
        own units, held to the dial - dimmed, so it reads but does not turn, and takes its parameter's value back the
        moment the division lets go: a greyed-out knob left at a stale setting tells the player nothing. The parameter
        never moves, so the setting is still there when the division returns to Free and the host's automation never
        sees the division's number. The panel calls it once a display frame with whether a division is in force.
        @param synced        true while a division is in force
        @param synced_value  the value the division gives, in the parameter's own units; read only while `synced`
    */
    void FollowSync(bool synced, double synced_value)
    {
        if (synced != following_)
        {
            following_ = synced;
            SetDimmed(synced);
            if (!synced)
            {
                Refresh();
            }
        }
        if (synced)
        {
            const double position = parameter_.PositionOf(parameter_.Snapped(synced_value));
            if (position < shown_ || position > shown_)
            {
                box_.HideEditor(false);
                shown_ = position;
                Repaint();
            }
            box_.SetText(parameter_.TextAt(position) + suffix_);
        }
    }

    /** Whether a tempo division drives the knob (FollowSync).
        @return  true while one does */
    bool following() const noexcept
    {
        return following_;
    }

    /** Once a display frame: the knob and its box follow the parameter where the host moved it - unless a tempo
        division has it (FollowSync), whose value it shows instead. */
    void Refresh()
    {
        if (following_)
        {
            return;
        }
        const double now = parameter_.Normalized();
        if (now < shown_ || now > shown_)
        {
            shown_ = now;
            Repaint();
        }
        box_.SetText(parameter_.Text() + suffix_);
    }

    /** The slider's area over its value box, which the dial fills.
        @return  the area, in the knob's local coordinates */
    gfx::RectI DialArea() const
    {
        return gfx::RectI{width(), height() - BoxSize().second};
    }

    /** The value box's place in the knob.
        @return  the box's bounds, in the knob's local coordinates, centred at the bottom */
    gfx::RectI BoxBounds() const
    {
        const auto [w, h] = BoxSize();
        return {(width() - w) / 2, height() - h, w, h};
    }

    /** The angle the dial is drawn at: the shown position, as a float, along the sweep.
        @return  the angle, kStartAngle to kEndAngle radians */
    float Angle() const noexcept
    {
        const auto position = static_cast<float>(shown_);
        return kStartAngle + position * (kEndAngle - kStartAngle);
    }

    /** Places the value box. */
    void Resized() override
    {
        box_.SetBounds(BoxBounds());
    }

    /** Draws the dial through the painter at the shown angle.
        @param c  the canvas, in the knob's local coordinates */
    void Paint(gfx::Canvas &c) override
    {
        if (painter_)
        {
            painter_(c, DialArea(), Angle());
        }
    }

    /** A right press runs on_secondary_click when set; an Alt-click resets to the default; any other press begins the
        gesture and drag. Nothing while dimmed, but the secondary click.
        @param e  the press: its position, button and modifiers */
    void MouseDown(const MouseEvent &e) override
    {
        if (e.right && on_secondary_click)
        {
            box_.HideEditor(false);
            const auto action = on_secondary_click; // a copy: the action may change the knob
            action();
            return;
        }
        if (!IsEnabled())
        {
            return;
        }
        box_.HideEditor(false);
        if (e.modifiers.alt && !e.modifiers.shift && !e.modifiers.command)
        {
            Reset();
            return;
        }
        dragging_      = true;
        start_         = e.position;
        last_          = e.position;
        on_press_      = parameter_.Value();
        dragged_value_ = on_press_;
        parameter_.BeginGesture();
        MouseDrag(e);
    }

    /** Turns the knob: a full sweep in kPixelsForFullDrag pixels from where the press began, or a fine drag under
        Command or Alt.
        @param e  the drag: its position and modifiers */
    void MouseDrag(const MouseEvent &e) override
    {
        if (!dragging_)
        {
            return;
        }
        if (!e.modifiers.command && !e.modifiers.alt)
        {
            // Where the press began, the value it began on: the dial's position of a value taken back to a value can
            // land a double's rounding off it.
            const float  moved    = (e.position.x - start_.x) + (start_.y - e.position.y);
            const double position = parameter_.PositionOf(on_press_) + static_cast<double>(moved) / kPixelsForFullDrag;
            dragged_value_ =
                moved < 0.0f || moved > 0.0f ? parameter_.ValueAt(std::clamp(position, 0.0, 1.0)) : on_press_;
        }
        else
        {
            // The slider's velocity drag: each move's pixels, up to 200, through a quarter of a sine wave, on from
            // where the drag last left the value.
            const float  moved     = (e.position.x - last_.x) + (last_.y - e.position.y);
            const double max_speed = 200.0;
            double       speed     = std::clamp(static_cast<double>(std::abs(moved)), 0.0, max_speed);
            if (speed > 0.0)
            {
                speed =
                    0.2 *
                    (1.0 + std::sin(std::numbers::pi * (1.5 + std::min(0.5, std::max(0.0, speed - 1.0) / max_speed))));
                const double position = parameter_.PositionOf(dragged_value_) + (moved < 0.0f ? -speed : speed);
                dragged_value_        = parameter_.ValueAt(std::clamp(position, 0.0, 1.0));
            }
        }
        last_ = e.position;
        parameter_.SetValue(dragged_value_);
        Refresh();
    }

    /** Ends the gesture a press began. */
    void MouseUp(const MouseEvent &) override
    {
        if (dragging_)
        {
            dragging_ = false;
            parameter_.EndGesture();
        }
    }

    /** Home, a gesture of its own: the router hands a double click on after its release, as JUCE hands the slider
        one, and none for a second press dragged or held.
        @param e  the double click: its button */
    void MouseDoubleClick(const MouseEvent &e) override
    {
        if (IsEnabled() && !(e.right && on_secondary_click)) // a right double press is the secondary click's twice
        {
            Reset();
        }
    }

    /** The wheel: the knob's, as the JUCE slider's is, while it is enabled - a dimmed one hands it on - and while held
        taken with nothing moved, as the slider takes it with a button down.
        @param e  the wheel event: its deltas, in the views' pixels
        @return   true when the knob took it; false while dimmed */
    bool MouseWheel(const MouseEvent &e) override
    {
        if (!IsEnabled())
        {
            return false;
        }
        if (dragging_)
        {
            return true;
        }
        box_.HideEditor(true);
        // A notch's 0.15 of the sweep from where the value stands, and on a stepped parameter at least one step.
        const float  dx     = e.wheel_x / 100.0f;
        const float  dy     = e.wheel_y / 100.0f;
        const float  amount = (std::abs(dx) > std::abs(dy) ? -dx : dy) * (e.wheel_reversed ? -1.0f : 1.0f);
        const double value  = parameter_.Value();
        const double moved  = std::clamp(parameter_.PositionOf(value) + static_cast<double>(amount) * 0.15, 0.0, 1.0);
        const double delta  = parameter_.ValueAt(moved) - value;
        if (std::abs(delta) > std::numeric_limits<double>::min())
        {
            parameter_.BeginGesture();
            parameter_.SetValue(value + std::max(parameter_.Interval(), std::abs(delta)) * (delta < 0.0 ? -1.0 : 1.0));
            Refresh();
            parameter_.EndGesture();
        }
        return true;
    }

  private:
    /** @return  the value box's width and height, in design pixels, held to the knob's size less kMinDialHeight */
    std::pair<int, int> BoxSize() const
    {
        return {std::max(0, std::min(box_width_, width())),
                std::max(0, std::min(box_height_, height() - kMinDialHeight))};
    }

    /** The default - or the value the panel gave a double click (SetDoubleClickValue) - as one gesture; nothing for a
        value outside the dial. */
    void Reset()
    {
        const double value = double_click_value_.value_or(parameter_.DefaultValue());
        const double low   = parameter_.ValueAt(0.0);
        const double high  = parameter_.ValueAt(1.0);
        if (value < std::min(low, high) || value > std::max(low, high))
        {
            return;
        }
        parameter_.BeginGesture();
        parameter_.SetValue(value);
        Refresh();
        parameter_.EndGesture();
    }

    /** What the box was given: its text with the suffix taken off, read through the parameter's readout.
        @param typed  the text the box was given */
    void Type(std::string typed)
    {
        typed.erase(0, typed.find_first_not_of(" \t"));
        if (!suffix_.empty() && typed.size() >= suffix_.size() &&
            typed.compare(typed.size() - suffix_.size(), suffix_.size(), suffix_) == 0)
        {
            typed.erase(typed.size() - suffix_.size());
        }
        if (const auto value = parameter_.ValueFromText(typed))
        {
            const double now = parameter_.Value();
            if (*value < now || *value > now)
            {
                parameter_.BeginGesture();
                parameter_.SetValue(*value);
                parameter_.EndGesture();
            }
        }
        Refresh(); // the box shows the value as its readout writes it
    }

    ParameterControl     &parameter_;          ///< the parameter it drives
    DialPainter           painter_;            ///< draws the dial
    Label                 caption_;            ///< the caption, added to the tree by the panel
    Label                 box_;                ///< the value box
    int                   box_width_;          ///< the value box's width at most
    int                   box_height_;         ///< the value box's height at most
    float                 dimmed_alpha_;       ///< the alpha a dimmed knob fades to
    std::string           suffix_;             ///< text after the value in the box
    double                shown_;              ///< the position the dial shows, 0 to 1
    bool                  dragging_{false};    ///< a press has begun a gesture that is not yet released
    bool                  following_{false};   ///< a tempo division drives it (FollowSync)
    std::optional<double> double_click_value_; ///< what a double click sets, in place of the default
    gfx::PointF           start_{};            ///< where the press began
    gfx::PointF           last_{};             ///< where the last drag event was
    double                on_press_{0.0};      ///< the value when the press began
    double                dragged_value_{0.0}; ///< where the drag last put the value, before the snap
};
} // namespace reckless::view
