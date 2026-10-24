#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "DecimalText.h"
#include "Utf8.h"

namespace reckless::core
{
/**
    The series' readouts as text, and their parses: what every knob's box shows and reads back, in one place that knows
    nothing of a plugin format. The format's own layer (HostParameters.h, through each parameter's readout) hands these
    the parameter's value and shows what comes back; every readout that displays a unit parses typed text in the same
    unit, the invariant every box keeps.

    Three things are the series' own and are kept exactly, since a box's text is what a player and the suites read:
    fixed-point text that ignores the process's locale (a host that sets a decimal-comma locale must not turn
    "6.5 kHz" into "6,5 kHz", nor read "6.5" as 6), a whole number rounded half to even (the FPU's default mode,
    which every readout the series wrote rounds through), and a parse that reads a number off the front of the text
    and ignores what follows it ("35 %" is 35, "6.5k" is 6.5 before its prefix is read).
*/

/** To the nearest whole number, halves to even: 2.5 is 2 and 3.5 is 4.
    @param value  the number to round
    @return       the nearest whole number */
inline int RoundToInt(double value) noexcept
{
    return static_cast<int>(std::lrint(value));
}

/** `value` to `places` decimals, fixed, whatever the locale: "-6.0", "0.25". At places 0 or under, the whole number
    RoundToInt gives.
    @param value   the number to print
    @param places  the digits after the decimal point
    @return        the text */
inline std::string FixedString(double value, int places)
{
    if (places <= 0)
    {
        return std::to_string(RoundToInt(value));
    }
    char       buffer[64];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::fixed, places);
    return std::string(buffer, result.ptr);
}

/** Whether `c` is white space as a box's text has it: a space, a tab, a line break.
    @param c  one byte of text
    @return   true for a space, tab, line feed, carriage return, form feed or vertical tab */
constexpr bool IsSpace(char c) noexcept
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

/** `text` without the white space at either end.
    @param text  the text, which must outlive the result
    @return      the part of `text` between the white space, a view into it */
inline std::string_view Trim(std::string_view text) noexcept
{
    std::size_t start = 0;
    while (start < text.size() && IsSpace(text[start]))
    {
        ++start;
    }
    std::size_t end = text.size();
    while (end > start && IsSpace(text[end - 1]))
    {
        --end;
    }
    return text.substr(start, end - start);
}

/** `c` in lower case where it is an ASCII capital, else itself.
    @param c  one byte of text
    @return   the lower-case letter, or `c` */
constexpr char LowerAscii(char c) noexcept
{
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

/** `text` with its ASCII capitals in lower case: what a unit's letters are compared in.
    @param text  the text
    @return      the lower-cased copy */
inline std::string ToLower(std::string_view text)
{
    std::string lower(text);
    for (char &c : lower)
    {
        c = LowerAscii(c);
    }
    return lower;
}

/** The number at the front of `text` - leading white space skipped, a sign read, then decimals with an exponent if
    one follows - or nothing where there is none ("abc", ""): whether typed text names a number at all. What follows
    the number is left for the caller ("6.5k", "1.5 s").
    @param text  the typed text
    @return      the number, or nothing when the text does not begin with one */
inline std::optional<double> NumberAtFront(std::string_view text) noexcept
{
    std::size_t i = 0;
    while (i < text.size() && IsSpace(text[i]))
    {
        ++i;
    }
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-'))
    {
        negative = text[i] == '-';
        ++i;
    }
    const std::string_view rest  = text.substr(i);
    const auto             value = ReadDecimal(rest.substr(0, DecimalLength(rest)));
    if (!value)
    {
        return std::nullopt;
    }
    return negative ? -*value : *value;
}

/** The number at the front of `text` (NumberAtFront), and 0 where there is none: what every readout reads typed text
    with.
    @param text  the typed text
    @return      the number, or 0 */
inline double ParseNumber(std::string_view text) noexcept
{
    return NumberAtFront(text).value_or(0.0);
}

/** Fixed-precision text with no unit; a whole number at places 0.
    @param value   the number to print
    @param places  the digits after the decimal point
    @return        the text */
inline std::string DecimalsString(double value, int places)
{
    return FixedString(value, places);
}

/** Decibels to `places` decimals - "-6.0 dB", and "+3.0 dB" for a gain above unity when `signed_plus` asks for the
    sign (a pedal's gain or volume, a tone shelf, an equaliser band). The sign is decided on the value as shown,
    rounded to `places`, never on the raw value: a 0 dB that a host's automation or a plain linear range sends through
    the normalised round trip can come back a hair off zero, on either side as the machine's arithmetic rounds it, and
    decided on the raw value it would read "+0.0 dB" on one machine and "0.0 dB" on another. A hair either side of zero
    shows as "0.0 dB" everywhere (the rounded negative zero is added to +0.0 so it prints unsigned).
    @param value        the level, in decibels
    @param places       the digits after the decimal point
    @param signed_plus  true to print "+" before a value above zero
    @return             the text */
inline std::string DecibelString(double value, int places, bool signed_plus)
{
    const double      scale = std::pow(10.0, static_cast<double>(places));
    const double      shown = std::round(value * scale) / scale + 0.0;
    const std::string text  = FixedString(shown, places) + " dB";
    return signed_plus && shown > 0.0 ? "+" + text : text;
}

/** "35 %".
    @param value  the percentage, rounded to a whole number
    @return       the text */
inline std::string PercentString(double value)
{
    return std::to_string(RoundToInt(value)) + " %";
}

/** A frequency: whole hertz below a kilohertz, hundredths of a kilohertz above - "700 Hz", "6.50 kHz".
    @param hz  the frequency, in hertz
    @return    the text */
inline std::string FrequencyString(double hz)
{
    return hz >= 1000.0 ? FixedString(hz * 0.001, 2) + " kHz" : std::to_string(RoundToInt(hz)) + " Hz";
}

/** A typed frequency in either unit: "6.5k", "6500", "6.5 kHz" all 6500.
    @param text  the typed text
    @return      the frequency, in hertz */
inline double ParseFrequency(std::string_view text)
{
    const std::string lower  = ToLower(Trim(text));
    const double      number = ParseNumber(lower);
    return lower.find('k') != std::string::npos ? number * 1000.0 : number;
}

/** Hertz to `places` decimals: "1.00 Hz", an LFO's rate.
    @param value   the frequency, in hertz
    @param places  the digits after the decimal point
    @return        the text */
inline std::string HertzString(double value, int places)
{
    return FixedString(value, places) + " Hz";
}

/** The series' one millisecond format: two decimals under a millisecond ("0.25 ms"), one under ten ("3.2 ms"),
    whole below a second ("200 ms"), and seconds to two decimals from there ("1.50 s").
    @param value  the time, in milliseconds
    @return       the text */
inline std::string MillisecondString(double value)
{
    if (value >= 1000.0)
    {
        return FixedString(value * 0.001, 2) + " s";
    }
    if (value < 1.0)
    {
        return FixedString(value, 2) + " ms";
    }
    if (value < 10.0)
    {
        return FixedString(value, 1) + " ms";
    }
    return std::to_string(RoundToInt(value)) + " ms";
}

/** A typed time: seconds when the text ends in "s" but not "ms" ("1.5 s", "1.5s"), milliseconds otherwise - so the
    same knob position reads the same on every plugin.
    @param text  the typed text
    @return      the time, in milliseconds */
inline double ParseMilliseconds(std::string_view text)
{
    const std::string lower = ToLower(Trim(text));
    const double      value = ParseNumber(lower);
    const bool        ms    = lower.size() >= 2 && lower.compare(lower.size() - 2, 2, "ms") == 0;
    const bool        s     = !lower.empty() && lower.back() == 's';
    return s && !ms ? value * 1000.0 : value;
}

/** The prefix a value's decade reads in, as a scale: 1e6 for 1.00 MOhm, 1e3 for 220 kOhm, 1 for ohms, 1e-3 down to
    1e-12 for the farads a tone stack holds.
    @param value  the component's value, in ohms or farads
    @return       the scale of the prefix the value reads in */
inline double BarePrefixScale(double value) noexcept
{
    const double magnitude = std::fabs(value);
    if (magnitude >= 1.0e6)
    {
        return 1.0e6;
    }
    if (magnitude >= 1.0e3)
    {
        return 1.0e3;
    }
    if (magnitude >= 1.0)
    {
        return 1.0;
    }
    if (magnitude >= 1.0e-3)
    {
        return 1.0e-3;
    }
    if (magnitude >= 1.0e-6)
    {
        return 1.0e-6;
    }
    if (magnitude >= 1.0e-9)
    {
        return 1.0e-9;
    }
    return 1.0e-12;
}

/** A component value in engineering notation, three significant figures with the prefix the value's decade calls
    for - "470 pF", "22.0 nF", "220 kOhm", "1.00 MOhm", the ohm and micro signs as UTF-8: the readout of a tone
    stack's parts on the Amp and the Tone.
    @param value   the component's value, in farads or ohms
    @param farads  true for a capacitor, false for a resistor
    @return        the text, UTF-8 */
inline std::string EngineeringString(double value, bool farads)
{
    const char *unit     = farads ? "F" : "\xCE\xA9";
    double      scale    = BarePrefixScale(value);
    double      mantissa = value / scale;
    // Three significant figures: 4.70, 47.0, 470. A mantissa that rounds up to 1000 moves up a prefix.
    if (mantissa >= 999.5)
    {
        scale *= 1.0e3;
        mantissa = value / scale;
    }
    const char *prefix   = scale >= 1.0e6    ? "M"
                           : scale >= 1.0e3  ? "k"
                           : scale >= 1.0    ? ""
                           : scale >= 1.0e-3 ? "m"
                           : scale >= 1.0e-6 ? "\xC2\xB5"
                           : scale >= 1.0e-9 ? "n"
                                             : "p";
    const int   decimals = mantissa < 9.995 ? 2 : (mantissa < 99.95 ? 1 : 0);
    return FixedString(mantissa, decimals) + " " + prefix + unit;
}

/** A typed component value: with a prefix it is read in it - a capacitor's p or pF, n or nF, u or uF (the micro
    sign or the Greek mu too), a resistor's k or kOhm, M or MOhm - and a bare number in `bare_prefix_scale`: 1 for a
    resistor, so a bare number is ohms, and for a capacitor the prefix its box shows at the stock value (1e-12 for a
    treble capacitor, 1e-9 for a slope pair), so a capacitor typed without a suffix is what the box says. Case is
    forgiven where it cannot mislead: a milliohm is no tone-stack value, so on ohms a lowercase m reads as mega too;
    on farads M is no prefix a capacitor takes, so both read as milli.
    @param text              the typed text
    @param farads            true for a capacitor, false for a resistor
    @param bare_prefix_scale the scale a number with no prefix is read in
    @return                  the value, in farads or ohms */
inline double ParseEngineering(std::string_view text, bool farads, double bare_prefix_scale)
{
    const std::string_view trimmed = Trim(text);
    const double           number  = ParseNumber(trimmed);
    std::size_t            i       = 0;
    while (i < trimmed.size())
    {
        const char ch      = trimmed[i];
        const bool numeric = (ch >= '0' && ch <= '9') || ch == '.' || ch == '-' || ch == '+' || ch == ',' ||
                             ch == ' ' || ch == 'e' || ch == 'E';
        if (!numeric)
        {
            break;
        }
        ++i;
    }
    if (i >= trimmed.size())
    {
        return number * bare_prefix_scale;
    }
    switch (utf8::NextCodePoint(trimmed, i))
    {
    case 'p':
    case 'P':
        return number * 1.0e-12;
    case 'n':
    case 'N':
        return number * 1.0e-9;
    case 'u':
    case 'U':
    case 0x00B5: // micro sign
    case 0x03BC: // Greek mu
        return number * 1.0e-6;
    case 'm':
    case 'M':
        return farads ? number * 1.0e-3 : number * 1.0e6;
    case 'k':
    case 'K':
        return number * 1.0e3;
    default:
        // A unit letter with no prefix ("470 F" is no value a stack holds, "220 Ohm" is): base units.
        return number;
    }
}
} // namespace reckless::core
