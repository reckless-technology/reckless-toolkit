#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "ParameterControl.h"

namespace reckless::view
{
/**
    A control on a value the panel owns rather than a host's parameter - a drawer's knob or plate whose value is the
    plugin's own state, which no host sees (the Cab's Cabinet Maker, the Space Craft) - played as JUCE's slider or
    combo box on a value of its own plays, in the value's units and in double: on a linear dial from `min` to `max`,
    stepped by `interval` when it is not 0 (juce::Slider::setRange); on a geometric one, equal turns equal ratios
    with the geometric mean of the ends at twelve o'clock (a NormalisableRange<double> of a pow and a log); or on a
    list of choices, the value a choice's index (a plain juce::ComboBox). Every number is the JUCE range's: the dial
    both ways, the snap to a step and to the ends.

    Its text is `text` of the value; typed text is `parse` of the text and the value as it stands - the slider's
    valueFromTextFunction - or nothing for text it cannot read. A hand that moves it - a drag, the wheel, a double
    click, typed text, a choice from the plate's menu - lands the value snapped and, when that moved it by more than a
    double's rounding (the slider's own test), runs `moved` with it: the slider's onValueChange, the combo box's
    onChange. The panel's own refresh (SetQuietly) lands a value with nothing run, as dontSendNotification - and, as
    the slider's setValue, only a value that moves it by more than that rounding: a refresh to the recipe a drag just
    sent, rounded on its way, leaves the knob on the value the drag gave. No host hears any of it, so its gestures
    are nothing.
*/
class LocalControl final : public ParameterControl
{
  public:
    /** The value's text.
        @param value  the value, in the control's units
        @return       the text the box shows, without a suffix */
    using TextOf = std::function<std::string(double value)>;

    /** The value typed text names.
        @param text     the text typed
        @param current  the control's value as it stands
        @return         the value, or an empty optional for text it cannot read */
    using ParseOf = std::function<std::optional<double>(std::string_view text, double current)>;

    /** What a hand's move of the value is told to.
        @param value  the new value, snapped */
    using Moved = std::function<void(double value)>;

    /** A linear dial from `min` to `max`, stepped by `interval` when it is not 0; `value` and the default where it
        starts.
        @param min       the value at the dial's start
        @param max       the value at the dial's end
        @param interval  the step, in the value's units; 0 for a continuous dial
        @param value     the starting value, snapped; also the default
        @param text      the value's text
        @param parse     the value typed text names; may be empty, when typed text reads as nothing
        @param moved     runs when a hand moves the value; may be empty
        @return          the control */
    static LocalControl Linear(double min, double max, double interval, double value, TextOf text, ParseOf parse,
                               Moved moved)
    {
        return LocalControl(min, max, interval, false, {}, value, std::move(text), std::move(parse), std::move(moved));
    }

    /** A geometric dial from `min` to `max`, both positive.
        @param min    the value at the dial's start, above 0
        @param max    the value at the dial's end, above `min`
        @param value  the starting value; also the default
        @param text   the value's text
        @param parse  the value typed text names; may be empty, when typed text reads as nothing
        @param moved  runs when a hand moves the value; may be empty
        @return       the control */
    static LocalControl Geometric(double min, double max, double value, TextOf text, ParseOf parse, Moved moved)
    {
        return LocalControl(min, max, 0.0, true, {}, value, std::move(text), std::move(parse), std::move(moved));
    }

    /** A plate's choices, on choice `index`.
        @param choices  the choices' names, in order; the value is a choice's index
        @param index    the starting choice; also the default
        @param moved    runs when a hand moves the value, with the new index; may be empty
        @return         the control */
    static LocalControl Choices(std::vector<std::string> choices, int index, Moved moved)
    {
        const double last = static_cast<double>(std::max<std::size_t>(choices.size(), 1) - 1);
        return LocalControl(0.0, last, 1.0, false, std::move(choices), static_cast<double>(index), {}, {},
                            std::move(moved));
    }

    // A knob or a plate holds its control by reference: the control stays where it was made.

    /** Not copyable: a knob or a plate holds the control by reference. */
    LocalControl(const LocalControl &) = delete;

    /** Not assignable: a knob or a plate holds the control by reference.
        @return  never */
    LocalControl &operator=(const LocalControl &) = delete;

    /** The panel's own refresh: the value snapped and landed when it moves the control by more than a double's
        rounding, nothing run - juce::Slider::setValue with dontSendNotification.
        @param value  the value to land, in the control's units */
    void SetQuietly(double value)
    {
        value = Snapped(value);
        if (Moves(value))
        {
            value_ = value;
        }
    }

    /** What a double click returns the knob to when the panel gives it no value of its own
        (Knob::SetDoubleClickValue).
        @param value  the default, in the control's units; not snapped */
    void SetDefault(double value)
    {
        default_ = value;
    }

    // ---- ParameterControl ---------------------------------------------------------------------------------------

    /** @return  the value's position along the dial, 0 to 1 */
    double Normalized() const override
    {
        return PositionOf(value_);
    }

    /** @return  the default's position along the dial, 0 to 1 */
    double DefaultNormalized() const override
    {
        return PositionOf(default_);
    }

    /** The value's own text, as the slider prints its value - never a trip through the dial and back, which may move
        a value by its last bit.
        @return  the text function's text for the value, or a choice's name */
    std::string Text() const override
    {
        return TextOfValue(value_);
    }

    /** Does nothing: no host hears a local control. */
    void BeginGesture() override
    {
    }

    /** Set the value at a position along the dial, as SetValue does.
        @param position  the position along the dial, 0 to 1 */
    void SetNormalized(double position) override
    {
        SetValue(ValueAt(position));
    }

    /** Does nothing: no host hears a local control. */
    void EndGesture() override
    {
    }

    /** @return  the number of choices; 0 for a dial */
    int ChoiceCount() const override
    {
        return static_cast<int>(choices_.size());
    }

    /** @param index  the choice, from 0
        @return       its name; an empty string for an index out of range */
    std::string ChoiceText(int index) const override
    {
        return index >= 0 && index < ChoiceCount() ? choices_[static_cast<std::size_t>(index)] : std::string();
    }

    /** @param text  the text typed
        @return      the position of the value `parse` reads from it; an empty optional when it reads nothing */
    std::optional<double> NormalizedFromText(std::string_view text) const override
    {
        const auto value = ValueFromText(text);
        return value.has_value() ? std::optional<double>(PositionOf(*value)) : std::nullopt;
    }

    /** @return  the value, in the control's units */
    double Value() const override
    {
        return value_;
    }

    /** @return  the default, in the control's units */
    double DefaultValue() const override
    {
        return default_;
    }

    /** The JUCE range's convertFrom0to1, the position held to 0..1 first.
        @param position  the position along the dial; held to 0 to 1
        @return          the value, in the control's units */
    double ValueAt(double position) const override
    {
        const double p = std::clamp(position, 0.0, 1.0);
        return geometric_ ? min_ * std::pow(max_ / min_, p) : min_ + (max_ - min_) * p;
    }

    /** The JUCE range's convertTo0to1, held to 0..1; the geometric dial's value held into its ends first, so a typed
        value out of range never reaches the logarithm with a non-positive argument.
        @param value  the value, in the control's units
        @return       the position along the dial, 0 to 1 */
    double PositionOf(double value) const override
    {
        if (geometric_)
        {
            return std::clamp(std::log(std::clamp(value, min_, max_) / min_) / std::log(max_ / min_), 0.0, 1.0);
        }
        return max_ > min_ ? std::clamp((value - min_) / (max_ - min_), 0.0, 1.0) : 0.0;
    }

    /** @return  the step, in the control's units; 0 for a continuous dial */
    double Interval() const override
    {
        return interval_;
    }

    /** The value snapped as the slider constrains one, and moved on when it moved by more than a double's rounding.
        @param value  the value, in the control's units */
    void SetValue(double value) override
    {
        value = Snapped(value);
        if (Moves(value))
        {
            value_ = value;
            if (moved_)
            {
                const auto moved = moved_; // a copy: the callback may remake the control
                moved(value);
            }
        }
    }

    /** @param position  the position along the dial, 0 to 1
        @return          the text of the value there */
    std::string TextAt(double position) const override
    {
        return TextOfValue(ValueAt(position));
    }

    /** @param text  the text typed
        @return      `parse` of the text and the current value; an empty optional with no `parse` or for text it cannot
                     read */
    std::optional<double> ValueFromText(std::string_view text) const override
    {
        return parse_ ? parse_(text, value_) : std::nullopt;
    }

  private:
    /** The control the factories make.
        @param min        the value at the dial's start
        @param max        the value at the dial's end
        @param interval   the step; 0 for none
        @param geometric  true for a geometric dial
        @param choices    the choices' names; empty for a dial
        @param value      the starting value, snapped; also the default
        @param text       the value's text
        @param parse      the value typed text names
        @param moved      runs when a hand moves the value */
    LocalControl(double min, double max, double interval, bool geometric, std::vector<std::string> choices,
                 double value, TextOf text, ParseOf parse, Moved moved)
        : min_(min), max_(max), interval_(interval), geometric_(geometric), choices_(std::move(choices)),
          text_(std::move(text)), parse_(std::move(parse)), moved_(std::move(moved))
    {
        default_ = value_ = Snapped(value);
    }

    /** `value`'s text: a choice's name, or the text function's.
        @param value  the value, in the control's units
        @return       the text */
    std::string TextOfValue(double value) const
    {
        if (!choices_.empty())
        {
            return ChoiceText(static_cast<int>(std::lround(value)));
        }
        return text_ ? text_(value) : std::string();
    }

    /** Whether `value` is another value than the one held, by more than a double's rounding: juce::approximatelyEqual,
        which the slider asks before it moves and tells anyone - for a value that is not finite, whether the two are
        unequal (a NaN always is).
        @param value  the value to compare with the held one
        @return       true when it differs */
    bool Moves(double value) const noexcept
    {
        if (!std::isfinite(value) || !std::isfinite(value_))
        {
            return !(value <= value_ && value >= value_);
        }
        const double difference = std::abs(value - value_);
        return !(difference <= std::numeric_limits<double>::min() ||
                 difference <= std::numeric_limits<double>::epsilon() * std::max(std::abs(value), std::abs(value_)));
    }

    double                   min_;          ///< the value at the dial's start
    double                   max_;          ///< the value at the dial's end
    double                   interval_;     ///< the step; 0 for none
    bool                     geometric_;    ///< equal turns are equal ratios
    std::vector<std::string> choices_;      ///< the choices' names; empty for a dial
    TextOf                   text_;         ///< the value's text
    ParseOf                  parse_;        ///< the value typed text names
    Moved                    moved_;        ///< runs when a hand moves the value
    double                   value_{0.0};   ///< the value, in the control's units
    double                   default_{0.0}; ///< the default, in the control's units
};
} // namespace reckless::view
