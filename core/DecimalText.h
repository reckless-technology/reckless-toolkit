#pragma once

#include <cstddef>
#include <optional>
#include <string_view>

#if defined(__APPLE__)
#include <cerrno>
#include <cmath>
#include <xlocale.h>
#else
#include <charconv>
#include <system_error>
#endif

namespace reckless::core
{
/**
    Decimal text read as a double whatever the process's locale: the one reading every number the suite takes from
    text goes through - a saved session's and a preset's JSON (Json.h), a knob's typed text (ParameterText.h) and the
    meters' artwork (gfx's SvgArt.h). A host that sets a decimal-comma locale must not read "0.5" as 0, so
    nothing here asks the locale.

    The exact, locale-free reading is the floating-point std::from_chars, which glibc's libstdc++ and Microsoft's
    library carry and which Apple's libc++ carries only from macOS 26 - below the suite's macOS floor - so on Apple the
    text is read by strtod_l in the C locale (LC_C_LOCALE), which every macOS carries and which rounds to the nearest
    double as from_chars does. Either way the text is first held to one grammar, DecimalLength's, so what counts as a
    number is the suite's on every platform and never a library's: no hexadecimal, infinity, NaN, leading white space
    or plus sign, which strtod would read and from_chars would not.
*/

/** The longest decimal text ReadDecimal reads: far past any number the suite writes (seventeen significant digits
    and an exponent) or a person types, and the size of the buffer Apple's reading copies the text into. */
inline constexpr std::size_t kLongestDecimal = 512;

/** The length of the decimal number at the front of `text`: an optional minus sign, digits with an optional point and
    fraction - at least one digit between them, so "5.", ".5" and "5.5" are numbers and "." is not - and an exponent
    where one with digits follows ("1e5", "1E-5"; in "1e" and "1e+" the number is the 1 alone). 0 where the text does
    not begin with one: "", "-", "+1", " 1", "inf", "abc".
    @param text  the text, read from its first character
    @return      how many characters the number at its front takes, or 0 for none */
constexpr std::size_t DecimalLength(std::string_view text) noexcept
{
    const auto  is_digit = [](char c) { return c >= '0' && c <= '9'; };
    std::size_t i        = 0;
    if (i < text.size() && text[i] == '-')
    {
        ++i;
    }
    std::size_t digits = 0;
    while (i < text.size() && is_digit(text[i]))
    {
        ++i;
        ++digits;
    }
    if (i < text.size() && text[i] == '.')
    {
        ++i;
        while (i < text.size() && is_digit(text[i]))
        {
            ++i;
            ++digits;
        }
    }
    if (digits == 0)
    {
        return 0;
    }
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E'))
    {
        std::size_t j = i + 1;
        if (j < text.size() && (text[j] == '+' || text[j] == '-'))
        {
            ++j;
        }
        if (j < text.size() && is_digit(text[j]))
        {
            while (j < text.size() && is_digit(text[j]))
            {
                ++j;
            }
            i = j;
        }
    }
    return i;
}

/** `text`, which must be one decimal number whole (DecimalLength's, nothing before or after it), as the nearest
    double, whatever the process's locale.
    @param text  the number's text
    @return      the number; nothing for text that is not one number whole, one longer than kLongestDecimal, one
                 past the largest double, or a nonzero one so small it reads as zero */
inline std::optional<double> ReadDecimal(std::string_view text) noexcept
{
    if (text.empty() || text.size() > kLongestDecimal || DecimalLength(text) != text.size())
    {
        return std::nullopt;
    }
#if defined(__APPLE__)
    char buffer[kLongestDecimal + 1];
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        buffer[i] = text[i];
    }
    buffer[text.size()] = '\0';
    char *end           = nullptr;
    errno               = 0;
    const double value  = strtod_l(buffer, &end, LC_C_LOCALE);
    if (end != buffer + text.size() || (errno == ERANGE && (std::isinf(value) || value == 0.0)))
    {
        return std::nullopt;
    }
    return value;
#else
    double     value  = 0.0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value, std::chars_format::general);
    if (result.ec != std::errc() || result.ptr != text.data() + text.size())
    {
        return std::nullopt;
    }
    return value;
#endif
}
} // namespace reckless::core
