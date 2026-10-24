#pragma once

#include <cmath>
#include <cstdint>

namespace reckless::gfx
{
/**
    A colour as eight-bit alpha, red, green and blue, not premultiplied - the panels' every fill, stroke and text.
    Its arithmetic is the series' look's, to the byte: a brighter or darker shade, an alpha set from a fraction
    (rounded half to even), and a blend between two colours made on their premultiplied channels and unpremultiplied
    after, so a panel drawn through any backend lands on the same pixels.
*/
class Color
{
  public:
    constexpr Color() = default; ///< Transparent black.

    /** A colour from its packed word.
        @param argb  alpha in the top byte, then red, green and blue, not premultiplied */
    constexpr explicit Color(std::uint32_t argb) noexcept : argb_(argb)
    {
    }

    /** An opaque colour.
        @param r  red, 0 to 255
        @param g  green, 0 to 255
        @param b  blue, 0 to 255
        @return   the colour at alpha 255 */
    static constexpr Color FromRgb(std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
    {
        return FromArgb(0xff, r, g, b);
    }

    /** A colour from its four channels.
        @param a  alpha, 0 (clear) to 255 (opaque)
        @param r  red, 0 to 255
        @param g  green, 0 to 255
        @param b  blue, 0 to 255
        @return   the colour, not premultiplied */
    static constexpr Color FromArgb(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) noexcept
    {
        return Color((std::uint32_t{a} << 24) | (std::uint32_t{r} << 16) | (std::uint32_t{g} << 8) | std::uint32_t{b});
    }

    /** @return the packed word, alpha in the top byte, then red, green and blue */
    constexpr std::uint32_t argb() const noexcept
    {
        return argb_;
    }

    /** @return the alpha, 0 (clear) to 255 (opaque) */
    constexpr std::uint8_t alpha() const noexcept
    {
        return static_cast<std::uint8_t>(argb_ >> 24);
    }

    /** @return the red, 0 to 255 */
    constexpr std::uint8_t red() const noexcept
    {
        return static_cast<std::uint8_t>(argb_ >> 16);
    }

    /** @return the green, 0 to 255 */
    constexpr std::uint8_t green() const noexcept
    {
        return static_cast<std::uint8_t>(argb_ >> 8);
    }

    /** @return the blue, 0 to 255 */
    constexpr std::uint8_t blue() const noexcept
    {
        return static_cast<std::uint8_t>(argb_);
    }

    /** The alpha as a fraction of full.
        @return the alpha over 255, 0 to 1 */
    constexpr float AlphaFraction() const noexcept
    {
        return static_cast<float>(alpha()) / 255.0f;
    }

    /** The colour with its alpha replaced.
        @param alpha  the new alpha, 0 to 255
        @return       the colour with the same red, green and blue */
    constexpr Color WithAlpha(std::uint8_t alpha) const noexcept
    {
        return Color((argb_ & 0x00ffffffu) | (std::uint32_t{alpha} << 24));
    }

    /** The alpha set to `fraction` of full, rounded half to even and held to 0..255.
        @param fraction  the new alpha as a fraction of full, held to 0 to 1
        @return          the colour with the same red, green and blue */
    Color WithAlpha(float fraction) const noexcept
    {
        return WithAlpha(ByteOf(fraction));
    }

    /** The alpha multiplied by `multiplier` (at least 0), rounded half to even and held to 255.
        @param multiplier  the factor, zero or more
        @return            the colour with the same red, green and blue */
    Color WithMultipliedAlpha(float multiplier) const noexcept
    {
        const int scaled = RoundHalfEven(multiplier * static_cast<float>(alpha()));
        return WithAlpha(static_cast<std::uint8_t>(scaled < 0xff ? scaled : 0xff));
    }

    /** A lighter shade: each channel's distance from white scaled by 1 / (1 + amount), the alpha kept.
        @param amount  how much lighter, zero or more; 0 changes nothing
        @return        the shade */
    Color Brighter(float amount = 0.4f) const noexcept
    {
        const float scale = 1.0f / (1.0f + amount);
        const auto  lift  = [scale](std::uint8_t c) {
            return static_cast<std::uint8_t>(255.0f - scale * static_cast<float>(255 - c));
        };
        return FromArgb(alpha(), lift(red()), lift(green()), lift(blue()));
    }

    /** A darker shade: each channel scaled by 1 / (1 + amount), the alpha kept.
        @param amount  how much darker, zero or more; 0 changes nothing
        @return        the shade */
    Color Darker(float amount = 0.4f) const noexcept
    {
        const float scale = 1.0f / (1.0f + amount);
        const auto  drop = [scale](std::uint8_t c) { return static_cast<std::uint8_t>(scale * static_cast<float>(c)); };
        return FromArgb(alpha(), drop(red()), drop(green()), drop(blue()));
    }

    /** `proportion` of the way from this colour to `other`: their premultiplied channels blended in 256ths and the
        result unpremultiplied; the ends themselves at 0 and 1.
        @param other       the colour at proportion 1
        @param proportion  the way from this colour to `other`, held to 0 to 1
        @return            the blend */
    Color InterpolatedWith(Color other, float proportion) const noexcept
    {
        if (proportion <= 0.0f)
        {
            return *this;
        }
        if (proportion >= 1.0f)
        {
            return other;
        }
        const std::uint32_t amount = static_cast<std::uint32_t>(RoundHalfEven(proportion * 255.0f));
        const std::uint32_t a      = Premultiplied(argb_);
        const std::uint32_t b      = Premultiplied(other.argb_);

        std::uint32_t even = a & 0x00ff00ffu; // red and blue
        even += (((b & 0x00ff00ffu) - even) * amount) >> 8;
        even &= 0x00ff00ffu;
        std::uint32_t odd = (a >> 8) & 0x00ff00ffu; // alpha and green
        odd += ((((b >> 8) & 0x00ff00ffu) - odd) * amount) >> 8;
        odd &= 0x00ff00ffu;
        return Color(Unpremultiplied((odd << 8) | even));
    }

    /** Two colours are equal when their packed words are.
        @param a  the first colour
        @param b  the second colour
        @return   true when every channel agrees */
    friend constexpr bool operator==(Color a, Color b) noexcept
    {
        return a.argb_ == b.argb_;
    }

  private:
    /** Round half to even, as the default floating-point rounding mode does.
        @param value  the number to round
        @return       the nearest integer, a half going to the even one */
    static int RoundHalfEven(float value) noexcept
    {
        return static_cast<int>(std::lrint(value));
    }

    /** A fraction as a byte.
        @param fraction  0 to 1; held to it
        @return          the fraction of 255, rounded half to even */
    static std::uint8_t ByteOf(float fraction) noexcept
    {
        return fraction <= 0.0f
                   ? 0
                   : (fraction >= 1.0f ? 255 : static_cast<std::uint8_t>(RoundHalfEven(fraction * 255.0f)));
    }

    /** A colour's channels multiplied by its alpha.
        @param argb  the packed colour, not premultiplied
        @return      the packed colour, premultiplied */
    static std::uint32_t Premultiplied(std::uint32_t argb) noexcept
    {
        const std::uint32_t alpha = argb >> 24;
        if (alpha == 0xff)
        {
            return argb;
        }
        if (alpha == 0)
        {
            return 0;
        }
        const auto channel = [alpha](std::uint32_t c) { return ((c * alpha + 0x7f) >> 8) & 0xffu; };
        return (alpha << 24) | (channel((argb >> 16) & 0xffu) << 16) | (channel((argb >> 8) & 0xffu) << 8) |
               channel(argb & 0xffu);
    }

    /** The inverse of Premultiplied(): each channel divided by the alpha and held to 255.
        @param argb  the packed colour, premultiplied
        @return      the packed colour, not premultiplied */
    static std::uint32_t Unpremultiplied(std::uint32_t argb) noexcept
    {
        const std::uint32_t alpha = argb >> 24;
        if (alpha == 0xff)
        {
            return argb;
        }
        if (alpha == 0)
        {
            return 0;
        }
        const auto channel = [alpha](std::uint32_t c) {
            const std::uint32_t lifted = (c * 0xffu) / alpha;
            return lifted < 0xffu ? lifted : 0xffu;
        };
        return (alpha << 24) | (channel((argb >> 16) & 0xffu) << 16) | (channel((argb >> 8) & 0xffu) << 8) |
               channel(argb & 0xffu);
    }

    std::uint32_t argb_{0}; ///< alpha in the top byte, then red, green and blue, not premultiplied
};

namespace colors
{
inline constexpr Color kBlack{0xff000000u};       ///< opaque black
inline constexpr Color kWhite{0xffffffffu};       ///< opaque white
inline constexpr Color kTransparent{0x00000000u}; ///< fully transparent black
} // namespace colors
} // namespace reckless::gfx
