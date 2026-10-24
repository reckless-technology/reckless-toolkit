#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "AppTheme.h"
#include "Button.h"
#include "Canvas.h"
#include "PanelStyle.h"
#include "SettingsWidgets.h"
#include "Switch.h"
#include "View.h"

namespace reckless::app
{
/**
    A section of an app's window: a frame in the theme's track colour, the plate's colour faint inside it, its title in
    the accent on the frame's top edge, as the series frames a bank of knobs. Its children are laid out by its owner
    inside ContentArea().
*/
class Section final : public view::View
{
  public:
    static constexpr int kTitleHeight = 18; ///< the title's band, in design pixels
    static constexpr int kPadding     = 10; ///< the room inside the frame, in design pixels

    /** @param theme  the colours it is drawn in
        @param title  the section's title, shown as given */
    Section(const Theme &theme, std::string title) : theme_(theme), title_(std::move(title))
    {
    }

    /** @return  where the section's controls go: inside the frame, under the title */
    gfx::RectI ContentArea() const noexcept
    {
        return LocalBounds().Reduced(kPadding).WithTrimmedTop(kTitleHeight - kPadding / 2);
    }

    /** @return  the title */
    const std::string &title() const noexcept
    {
        return title_;
    }

    /** The frame, and the title over a gap in its top edge.
        @param c  the canvas, in the section's coordinates */
    void Paint(gfx::Canvas &c) override
    {
        const auto  frame = LocalBounds().ToFloat().Reduced(0.5f).WithTrimmedTop(kTitleHeight / 2.0f);
        const auto  font  = fonts::Title();
        const float width = c.TextWidth(font, title_) + 10.0f;
        c.SetColor(theme_.plate.WithAlpha(0.35f));
        c.FillRoundedRect(frame, 6.0f);
        c.SetColor(theme_.track);
        c.DrawRoundedRect(frame, 6.0f, 1.0f);
        c.SetColor(theme_.panel);
        c.FillRect(gfx::RectF{12.0f, 0.0f, width, static_cast<float>(kTitleHeight)});
        c.SetFont(font);
        c.SetColor(theme_.accent);
        c.DrawText(title_, gfx::RectF{17.0f, 0.0f, width, static_cast<float>(kTitleHeight)}, gfx::Justify::kCentredLeft,
                   false);
    }

  private:
    Theme       theme_; ///< the colours
    std::string title_; ///< the title
};

/** A line or a few of text: a hint, a readout, a caption. It takes no mouse. */
class TextLine final : public view::View
{
  public:
    /** @param colour     the text's colour
        @param font       the text's font
        @param justify    where it sits
        @param max_lines  the most lines it wraps to */
    explicit TextLine(gfx::Color colour, gfx::Font font = fonts::Small(),
                      gfx::Justify justify = gfx::Justify::kCentredLeft, int max_lines = 1)
        : colour_(colour), font_(font), justify_(justify), max_lines_(max_lines)
    {
        SetInterceptsMouse(false, false);
    }

    /** @param text  the text; repaints when it differs */
    void SetText(std::string text)
    {
        if (text != text_)
        {
            text_ = std::move(text);
            Repaint();
        }
    }

    /** @param colour  the text's colour; repaints when it differs */
    void SetColour(gfx::Color colour)
    {
        if (colour.argb() != colour_.argb())
        {
            colour_ = colour;
            Repaint();
        }
    }

    /** @param font  the text's font; repaints */
    void SetFont(gfx::Font font)
    {
        font_ = font;
        Repaint();
    }

    /** @return  the text */
    const std::string &text() const noexcept
    {
        return text_;
    }

    /** @return  the text's colour */
    gfx::Color colour() const noexcept
    {
        return colour_;
    }

    /** The text in its box: on one line cut with an ellipsis, or fitted to the lines it may take.
        @param c  the canvas */
    void Paint(gfx::Canvas &c) override
    {
        c.SetFont(font_);
        c.SetColor(colour_);
        if (max_lines_ <= 1)
        {
            c.DrawText(text_, LocalBounds().ToFloat(), justify_, true);
        }
        else
        {
            c.DrawFittedText(text_, LocalBounds(), justify_, max_lines_, 1.0f);
        }
    }

  private:
    std::string  text_;      ///< the text
    gfx::Color   colour_;    ///< its colour
    gfx::Font    font_;      ///< its font
    gfx::Justify justify_;   ///< where it sits
    int          max_lines_; ///< the most lines
};

/**
    A horizontal slider on whole numbers from `minimum` to `maximum` in the theme: a groove in the track colour, the
    part below the value in the accent at rest, the handle in the accent. A press jumps the value to the pointer and a
    drag follows it; the wheel steps it; `on_change` hears each new value from the person's hand.
*/
class Slider final : public view::View
{
  public:
    /** @param theme    the colours it is drawn in
        @param minimum  the least value
        @param maximum  the greatest
        @param value    where it starts */
    Slider(const Theme &theme, int minimum, int maximum, int value)
        : theme_(theme), minimum_(minimum), maximum_(maximum), value_(std::clamp(value, minimum, maximum))
    {
        cursor = view::Cursor::kPointingHand;
    }

    std::function<void(int value)> on_change; ///< hears each new value from the person's hand

    /** @return  the value */
    int value() const noexcept
    {
        return value_;
    }

    /** The value set, held to the range; nothing is told.
        @param value  the value */
    void SetValue(int value)
    {
        const int held = std::clamp(value, minimum_, maximum_);
        if (held != value_)
        {
            value_ = held;
            Repaint();
        }
    }

    /** The groove, the filled part and the handle.
        @param c  the canvas */
    void Paint(gfx::Canvas &c) override
    {
        const float left  = kInset;
        const float right = static_cast<float>(width()) - kInset;
        const float y     = static_cast<float>(height()) / 2.0f;
        const float x     = XOf(value_, left, right);
        c.SetColor(theme_.track);
        c.FillRoundedRect(gfx::RectF{left, y - 2.0f, right - left, 4.0f}, 2.0f);
        c.SetColor(theme_.accent_dim);
        c.FillRoundedRect(gfx::RectF{left, y - 2.0f, x - left, 4.0f}, 2.0f);
        c.SetColor(IsEnabled() ? theme_.accent : theme_.ink_faint);
        c.FillEllipse(gfx::RectF{x - 7.0f, y - 7.0f, 14.0f, 14.0f});
        c.SetColor(theme_.plate);
        c.DrawEllipse(gfx::RectF{x - 7.0f, y - 7.0f, 14.0f, 14.0f}, 1.0f);
    }

    /** A press jumps to the pointer.
        @param e  the press */
    void MouseDown(const view::MouseEvent &e) override
    {
        Follow(e.position.x);
    }

    /** A drag follows it.
        @param e  the drag */
    void MouseDrag(const view::MouseEvent &e) override
    {
        Follow(e.position.x);
    }

    /** The wheel steps the value by one.
        @param e  the wheel's travel
        @return   true: the slider takes the wheel */
    bool MouseWheel(const view::MouseEvent &e) override
    {
        const float travel = std::abs(e.wheel_y) >= std::abs(e.wheel_x) ? e.wheel_y : e.wheel_x;
        if (travel != 0.0f)
        {
            Move(value_ + (travel > 0.0f ? 1 : -1));
        }
        return true;
    }

  private:
    static constexpr float kInset = 8.0f; ///< the groove's ends from the slider's sides, in design pixels

    /** Where `value` lands between `left` and `right`.
        @param value  a value in the range
        @param left   the x of the least
        @param right  the x of the greatest
        @return       the x it lands at */
    float XOf(int value, float left, float right) const noexcept
    {
        const float t =
            maximum_ > minimum_ ? static_cast<float>(value - minimum_) / static_cast<float>(maximum_ - minimum_) : 0.0f;
        return left + t * (right - left);
    }

    /** The value under `x`, told.
        @param x  the pointer's x, in the slider */
    void Follow(float x)
    {
        const float left  = kInset;
        const float right = static_cast<float>(width()) - kInset;
        const float t     = right > left ? std::clamp((x - left) / (right - left), 0.0f, 1.0f) : 0.0f;
        Move(minimum_ + static_cast<int>(std::lround(t * static_cast<float>(maximum_ - minimum_))));
    }

    /** `value` held to the range, told when it moved.
        @param value  the value wanted */
    void Move(int value)
    {
        const int held = std::clamp(value, minimum_, maximum_);
        if (held != value_)
        {
            value_ = held;
            Repaint();
            if (on_change)
            {
                const auto changed = on_change;
                changed(value_);
            }
        }
    }

    Theme theme_;   ///< the colours
    int   minimum_; ///< the least value
    int   maximum_; ///< the greatest
    int   value_;   ///< the value
};

/**
    A drop-down of named choices in the series' look: the legend plate (view::Switch) on a list of choices (the audio
    settings' ChoiceControl), with its caption as a line of text beside it for the owner to place. Choosing another
    runs `on_choose`. The plate and the caption are views the owner adds and lays out.
*/
class ChoicePlate
{
  public:
    /** @param theme    the colours it is drawn in
        @param caption  the caption; empty for none */
    ChoicePlate(const Theme &theme, std::string caption)
        : plate_(control_, std::move(caption), panel::PlatePainter(theme.Palette()),
                 panel::SwitchStyle(theme.Palette())),
          caption_(theme.ink_dim, fonts::Caption(), gfx::Justify::kCentredLeft)
    {
        caption_.SetText(plate_.caption().text());
        control_.on_choose = [this](int index) {
            if (on_choose)
            {
                const auto action = on_choose;
                action(index);
            }
        };
    }

    /** Not copyable: the plate refers to the control. */
    ChoicePlate(const ChoicePlate &) = delete;
    /** Not assignable: the plate refers to the control.
        @return  never */
    ChoicePlate &operator=(const ChoicePlate &) = delete;

    std::function<void(int index)> on_choose; ///< the person chose another

    /** The choices and the one shown, the plate's legend brought to it.
        @param choices  the legends
        @param current  the one in force */
    void SetChoices(std::vector<std::string> choices, int current)
    {
        control_.SetChoices(std::move(choices), current);
        plate_.Refresh();
        plate_.legend().SetText(control_.ChoiceText(control_.current()));
    }

    /** The choice shown, nothing told.
        @param index  the choice */
    void SetCurrent(int index)
    {
        SetChoices(control_.choices(), index);
    }

    /** @return  the choice in force */
    int current() const noexcept
    {
        return control_.current();
    }

    /** @return  the choices */
    const std::vector<std::string> &choices() const noexcept
    {
        return control_.choices();
    }

    /** A choice as the person makes it: the plate's menu's path, `on_choose` told when it moves.
        @param index  the choice */
    void Choose(int index)
    {
        plate_.Choose(index);
    }

    /** @return  the plate */
    view::Switch &plate() noexcept
    {
        return plate_;
    }

    /** @return  the caption beside it */
    TextLine &caption() noexcept
    {
        return caption_;
    }

  private:
    ChoiceControl control_; ///< the choices
    view::Switch  plate_;   ///< the legend plate
    TextLine      caption_; ///< the caption
};

/** The series' button in the theme, `text` running `clicked`.
    @param theme    the colours it is drawn in
    @param text     the button's text
    @param clicked  its action
    @param toggle   true for a toggle, lit by a lamp while on
    @return         the button */
inline std::unique_ptr<view::Button> MakeButton(const Theme &theme, std::string text, std::function<void()> clicked,
                                                bool toggle = false)
{
    return std::make_unique<view::Button>(std::move(text), panel::ButtonPainter(theme.Palette()), std::move(clicked),
                                          toggle);
}
} // namespace reckless::app
