#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <string_view>

namespace reckless::view
{
/**
    A parameter as a control on a panel drives it, through whatever format holds it: its position 0..1 and its
    default's, the text its readout gives the current value, and a change as one gesture the host records - begun, any
    number of positions, ended. A choice parameter also names its choices and the position of each; any parameter reads
    text typed for it through its readout. A format's layer provides one per parameter (vst-plugins' common/panel's
    HostParameterControl); a knob or a plate holds a reference and asks for the position when it paints.

    The dial in the parameter's own units - hertz, decibels, a stepped count - is the knob's where a slider's
    arithmetic works in them: the wheel's step of at least one interval, a value a tempo division shows, a default
    other than the parameter's own, text typed in. A control whose format gives it no units reads its position as its
    value, and the conversions are then the identity.
*/
class ParameterControl
{
  public:
    virtual ~ParameterControl() = default;

    /** @return  the parameter's position along its dial, 0 to 1 */
    virtual double Normalized() const = 0;

    /** @return  the default's position along the dial, 0 to 1 */
    virtual double DefaultNormalized() const = 0;

    /** @return  the text the parameter's readout gives the current value, without a suffix */
    virtual std::string Text() const = 0;

    /** Begin a gesture: the host records what follows as one edit. Message thread. */
    virtual void BeginGesture() = 0;

    /** Set the parameter's position, inside a gesture. Message thread.
        @param value  the position along the dial, 0 to 1 */
    virtual void SetNormalized(double value) = 0;

    /** End the gesture BeginGesture began. Message thread. */
    virtual void EndGesture() = 0;

    /** A choice parameter's number of choices; 0 for any other parameter.
        @return  the count of choices, 0 for none */
    virtual int ChoiceCount() const
    {
        return 0;
    }

    /** Choice `index`'s name, as its plate shows it.
        @param index  the choice, from 0 to ChoiceCount() - 1
        @return       its name; an empty string for an index out of range or a parameter with no choices */
    virtual std::string ChoiceText(int index) const
    {
        (void)index;
        return {};
    }

    /** The choice the parameter is on, from its position: the nearest of the evenly spaced positions.
        @return  the choice's index, 0 for a parameter with fewer than two choices */
    int ChoiceIndex() const
    {
        const int count = ChoiceCount();
        if (count <= 1)
        {
            return 0;
        }
        return std::clamp(static_cast<int>(std::lround(Normalized() * static_cast<double>(count - 1))), 0, count - 1);
    }

    /** Choice `index`'s position: the choices spread evenly from 0 to 1.
        @param index  the choice, from 0 to ChoiceCount() - 1
        @return       its position along the dial, 0 to 1; 0 for a parameter with fewer than two choices */
    double NormalizedForChoice(int index) const
    {
        const int count = ChoiceCount();
        return count > 1 ? static_cast<double>(index) / static_cast<double>(count - 1) : 0.0;
    }

    /** The position `text` typed for the parameter names, through its readout; nothing for text it cannot read.
        @param text  the text typed
        @return      the position along the dial, 0 to 1; an empty optional for text it cannot read, the default */
    virtual std::optional<double> NormalizedFromText(std::string_view text) const
    {
        (void)text;
        return std::nullopt;
    }

    // ---- The dial in the parameter's own units --------------------------------------------------------------------

    /** The parameter's value, in its own units.
        @return  the value; the position itself for a control with no units */
    virtual double Value() const
    {
        return Normalized();
    }

    /** The parameter's default, in its own units.
        @return  the default; its position itself for a control with no units */
    virtual double DefaultValue() const
    {
        return DefaultNormalized();
    }

    /** The value `position` along the dial stands for: the dial's taper, the position held to 0..1.
        @param position  the position along the dial; held to 0 to 1
        @return          the value, in the parameter's own units */
    virtual double ValueAt(double position) const
    {
        return std::clamp(position, 0.0, 1.0);
    }

    /** Where `value` sits along the dial, 0..1: the taper's inverse, held to the dial.
        @param value  the value, in the parameter's own units
        @return       the position along the dial, 0 to 1 */
    virtual double PositionOf(double value) const
    {
        return std::clamp(value, 0.0, 1.0);
    }

    /** A stepped parameter's step, in its own units (the Sequence's LENGTH, whole steps); 0 for a continuous one.
        @return  the step, in the parameter's own units; 0 for none */
    virtual double Interval() const
    {
        return 0.0;
    }

    /** `value` as the parameter can hold it: on the nearest step from the dial's bottom, when it is stepped, and
        inside the dial's ends.
        @param value  the value, in the parameter's own units
        @return       the nearest value the parameter can hold */
    virtual double Snapped(double value) const
    {
        const double low  = ValueAt(0.0);
        const double high = ValueAt(1.0);
        const double step = Interval();
        if (step > 0.0)
        {
            value = low + step * std::floor((value - low) / step + 0.5);
        }
        return std::clamp(value, std::min(low, high), std::max(low, high));
    }

    /** The parameter to `value`, in its own units, snapped as Snapped puts it: what a slider sets its parameter to
        from a value of its own (a double click's, a typed one, a wheel's step). Inside a gesture.
        @param value  the value, in the parameter's own units */
    virtual void SetValue(double value)
    {
        SetNormalized(PositionOf(Snapped(value)));
    }

    /** The text the parameter's readout gives the value at `position`: what a knob shows of a value that is not its
        parameter's own (a tempo division's). A control that can read only its current text gives that.
        @param position  the position along the dial, 0 to 1
        @return          the readout's text for the value there, without a suffix */
    virtual std::string TextAt(double position) const
    {
        (void)position;
        return Text();
    }

    /** The value `text` typed for the parameter names, in its own units, through its readout; nothing for text it
        cannot read.
        @param text  the text typed
        @return      the value, in the parameter's own units; an empty optional for text it cannot read */
    virtual std::optional<double> ValueFromText(std::string_view text) const
    {
        const auto position = NormalizedFromText(text);
        return position.has_value() ? std::optional<double>(ValueAt(*position)) : std::nullopt;
    }

    /** The parameter to what `text` typed for it names, for a box that types a parameter's value without a knob of its
        own (the tone stack's parts, typed on its schematic): the value the readout reads, set. Inside a gesture. A
        control that cannot read text sets nothing.
        @param text  the text typed */
    void SetFromText(std::string_view text)
    {
        if (const auto value = ValueFromText(text))
        {
            SetValue(*value);
        }
    }
};

/** The control a panel is handed for an id its plugin's parameters lack, a panel's slip: it holds still at nought,
    reads as nothing and tells no one. */
class StillControl final : public ParameterControl
{
  public:
    /** @return  0 */
    double Normalized() const override
    {
        return 0.0;
    }

    /** @return  0 */
    double DefaultNormalized() const override
    {
        return 0.0;
    }

    /** @return  an empty string */
    std::string Text() const override
    {
        return {};
    }

    /** Does nothing. */
    void BeginGesture() override
    {
    }

    /** Does nothing: the position stays 0. */
    void SetNormalized(double) override
    {
    }

    /** Does nothing. */
    void EndGesture() override
    {
    }
};
} // namespace reckless::view
