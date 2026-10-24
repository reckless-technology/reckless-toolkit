#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace reckless::gfx
{
/** Where text sits in its box: one horizontal placement and one vertical, OR-ed, or one of the common pairs. The
    values are the series' text placement as its panels have always laid text out. */
enum class Justify : int
{
    kLeft                = 1,  ///< against the left edge
    kRight               = 2,  ///< against the right edge
    kHorizontallyCentred = 4,  ///< centred across
    kTop                 = 8,  ///< against the top edge
    kBottom              = 16, ///< against the bottom edge
    kVerticallyCentred   = 32, ///< centred down
    kCentred             = 36, ///< both ways
    kCentredLeft         = 33, ///< at the left, centred vertically
    kCentredRight        = 34, ///< at the right, centred vertically
    kCentredTop          = 12, ///< centred across, at the top
    kCentredBottom       = 20, ///< centred across, at the bottom
    kTopLeft             = 9,  ///< at the top left
    kTopRight            = 10, ///< at the top right
    kBottomLeft          = 17, ///< at the bottom left
    kBottomRight         = 18, ///< at the bottom right
};

/** The two placements OR-ed.
    @param a  one placement
    @param b  another
    @return   both together */
constexpr Justify operator|(Justify a, Justify b) noexcept
{
    return static_cast<Justify>(static_cast<int>(a) | static_cast<int>(b));
}

/** Whether a placement includes a flag.
    @param j     the placement
    @param flag  the flag to look for
    @return      true when any of `flag`'s bits is set in `j` */
constexpr bool Has(Justify j, Justify flag) noexcept
{
    return (static_cast<int>(j) & static_cast<int>(flag)) != 0;
}

/** The families of faces text is drawn in: the series' serif, every font's unless it asks for another, and a
    monospaced face - every character the same advance - for text set in columns, a log or an engine's output. Which
    faces each family names is the backend's: gfx/skia's canvas draws the serif in Noto Serif and the monospaced family
    in Monaspace Xenon, each regular and bold, from the files a product installs (SetSerifFaces, SetMonospaceFaces). */
enum class FontFamily
{
    kSerif,     ///< the series' serif
    kMonospace, ///< the monospaced face
};

/**
    A font as the panels ask for one: a family - the series' serif unless it names another - at a height in pixels,
    bold or not, with the tracking a legend spreads its letters by (a fraction of the height added between letters)
    and a horizontal scale. Which face the family names is the backend's (the series' look installs the serif).
*/
struct Font
{
    float      height{14.0f};              ///< the font's height, in pixels
    bool       bold{false};                ///< the bold face
    bool       italic{false};              ///< the italic slant
    float      kerning{0.0f};              ///< extra space between letters, as a fraction of the height
    float      horizontal_scale{1.0f};     ///< the glyphs' width over their natural width
    FontFamily family{FontFamily::kSerif}; ///< the family of faces the text is drawn in

    /** A 14 px regular font. */
    constexpr Font() = default;

    /** A regular font of a height.
        @param height_px  the height, in pixels */
    constexpr explicit Font(float height_px) noexcept : height(height_px)
    {
    }

    /** This font at another height.
        @param h  the height, in pixels
        @return   the font */
    constexpr Font WithHeight(float h) const noexcept
    {
        Font f   = *this;
        f.height = h;
        return f;
    }

    /** This font bold or not.
        @param on  true for the bold face
        @return    the font */
    constexpr Font Bold(bool on = true) const noexcept
    {
        Font f = *this;
        f.bold = on;
        return f;
    }

    /** This font italic or not.
        @param on  true for the italic slant
        @return    the font */
    constexpr Font Italic(bool on = true) const noexcept
    {
        Font f   = *this;
        f.italic = on;
        return f;
    }

    /** This font with other tracking.
        @param k  the extra space between letters, as a fraction of the height
        @return   the font */
    constexpr Font WithKerning(float k) const noexcept
    {
        Font f    = *this;
        f.kerning = k;
        return f;
    }

    /** This font at another width.
        @param s  the glyphs' width over their natural width
        @return   the font */
    constexpr Font WithHorizontalScale(float s) const noexcept
    {
        Font f             = *this;
        f.horizontal_scale = s;
        return f;
    }

    /** This font in another family of faces.
        @param fam  the family
        @return     the font */
    constexpr Font WithFamily(FontFamily fam) const noexcept
    {
        Font f   = *this;
        f.family = fam;
        return f;
    }
};

namespace detail
{
/** The bytes of `text` from `at` that are one well-formed UTF-8 character (1 to 4, by the Unicode standard's table of
    well-formed byte sequences: no overlong form, no surrogate, nothing past U+10FFFF), or, negated, the bytes of the
    longest start of one that is cut short or broken - at least the one byte - which one U+FFFD stands for, the
    standard's practice for replacing what is ill-formed. `at` is inside `text`.
    @param text  the UTF-8 text
    @param at    the byte offset of the character's first byte, inside `text`
    @return      the character's length in bytes, 1 to 4; or the negated length of the broken run, at least -1 */
constexpr int Utf8Step(std::string_view text, std::size_t at) noexcept
{
    const auto          byte = [text](std::size_t i) { return static_cast<unsigned char>(text[i]); };
    const unsigned char lead = byte(at);
    if (lead < 0x80)
    {
        return 1;
    }
    int           length = 0;
    unsigned char low    = 0x80; // the second byte's range: narrower after E0, ED, F0 and F4
    unsigned char high   = 0xBF;
    if (lead >= 0xC2 && lead <= 0xDF)
    {
        length = 2;
    }
    else if (lead >= 0xE0 && lead <= 0xEF)
    {
        length = 3;
        low    = lead == 0xE0 ? 0xA0 : low;
        high   = lead == 0xED ? 0x9F : high;
    }
    else if (lead >= 0xF0 && lead <= 0xF4)
    {
        length = 4;
        low    = lead == 0xF0 ? 0x90 : low;
        high   = lead == 0xF4 ? 0x8F : high;
    }
    else
    {
        return -1; // a continuation byte, an overlong lead (C0, C1) or one past U+10FFFF (F5 to FF)
    }
    for (int i = 1; i < length; ++i)
    {
        const std::size_t next = at + static_cast<std::size_t>(i);
        if (next >= text.size())
        {
            return -i;
        }
        const unsigned char b = byte(next);
        if (b < (i == 1 ? low : 0x80) || b > (i == 1 ? high : 0xBF))
        {
            return -i;
        }
    }
    return length;
}
} // namespace detail

/** Whether `text` is well-formed UTF-8 throughout.
    @param text  the bytes to test
    @return      true when every byte is part of a well-formed character */
constexpr bool IsValidUtf8(std::string_view text) noexcept
{
    for (std::size_t at = 0; at < text.size();)
    {
        const int step = detail::Utf8Step(text, at);
        if (step < 0)
        {
            return false;
        }
        at += static_cast<std::size_t>(step);
    }
    return true;
}

/** `text` with each ill-formed part - the longest start of a character that is cut short or broken, or a byte that
    starts none - replaced by U+FFFD, as the Unicode standard recommends, and every well-formed character kept: text a
    shaper can draw whatever encoding it came in, a device's name in a code page or a system's message in another.
    @param text  the bytes, in any encoding
    @return      well-formed UTF-8 text */
inline std::string RepairedUtf8(std::string_view text)
{
    std::string repaired;
    repaired.reserve(text.size());
    for (std::size_t at = 0; at < text.size();)
    {
        const int step = detail::Utf8Step(text, at);
        if (step > 0)
        {
            repaired.append(text.substr(at, static_cast<std::size_t>(step)));
            at += static_cast<std::size_t>(step);
        }
        else
        {
            repaired.append("\xEF\xBF\xBD");
            at += static_cast<std::size_t>(-step);
        }
    }
    return repaired;
}

/** The character of UTF-8 `text` at `at` as a code point, `at` moved past it: a well-formed character's, or U+FFFD for
    the longest start of one that is cut short or broken - at least the one byte - as RepairedUtf8 replaces it, so the
    good bytes after a broken part are read as themselves. `at` is inside `text`.
    @param text  the UTF-8 text
    @param at    the byte offset of the character, inside `text`; moved past it
    @return      the code point, or U+FFFD for a broken part */
constexpr char32_t NextCodePoint(std::string_view text, std::size_t &at) noexcept
{
    const int step = detail::Utf8Step(text, at);
    if (step < 0)
    {
        at += static_cast<std::size_t>(-step);
        return U'\xFFFD';
    }
    const auto     lead = static_cast<unsigned char>(text[at]);
    const char32_t bits = step == 1 ? lead : step == 2 ? (lead & 0x1Fu) : step == 3 ? (lead & 0x0Fu) : (lead & 0x07u);
    char32_t       code = bits;
    for (int i = 1; i < step; ++i)
    {
        code = (code << 6) | (static_cast<unsigned char>(text[at + static_cast<std::size_t>(i)]) & 0x3Fu);
    }
    at += static_cast<std::size_t>(step);
    return code;
}
} // namespace reckless::gfx
