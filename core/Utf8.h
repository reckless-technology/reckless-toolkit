#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace reckless::core::utf8
{
/** The next code point of UTF-8 `text` from `i`, `i` moved past it: a lead byte's sequence read whole, a malformed
    byte - a stray continuation byte, a sequence cut short by the end of the text - read as itself. What a parameter's
    host id is hashed over (HostParameterId) and a VST3 string is made from.
    @param text  the UTF-8 text
    @param i     the byte offset of the code point's first byte, less than `text.size()`; moved past the sequence read
    @return      the code point, or a malformed byte's own value */
inline std::uint32_t NextCodePoint(std::string_view text, std::size_t &i) noexcept
{
    const auto lead = static_cast<unsigned char>(text[i++]);
    if (lead < 0x80)
    {
        return lead;
    }
    const int extra = lead >= 0xf0 ? 3 : (lead >= 0xe0 ? 2 : (lead >= 0xc0 ? 1 : 0));
    if (extra == 0 || i + static_cast<std::size_t>(extra) > text.size())
    {
        return lead;
    }
    std::uint32_t c = lead & (0x3fu >> extra);
    for (int k = 0; k < extra; ++k)
    {
        c = (c << 6) | (static_cast<unsigned char>(text[i++]) & 0x3fu);
    }
    return c;
}

/** `c` onto `out` in UTF-8: one byte under 0x80, two under 0x800, three under 0x10000, else four.
    @param out  the text to append to
    @param c    the code point, up to U+10FFFF; not checked for a surrogate */
inline void AppendUtf8(std::string &out, std::uint32_t c)
{
    if (c < 0x80)
    {
        out.push_back(static_cast<char>(c));
    }
    else if (c < 0x800)
    {
        out.push_back(static_cast<char>(0xc0 | (c >> 6)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    }
    else if (c < 0x10000)
    {
        out.push_back(static_cast<char>(0xe0 | (c >> 12)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    }
    else
    {
        out.push_back(static_cast<char>(0xf0 | (c >> 18)));
        out.push_back(static_cast<char>(0x80 | ((c >> 12) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | ((c >> 6) & 0x3f)));
        out.push_back(static_cast<char>(0x80 | (c & 0x3f)));
    }
}

/** The length of the well-formed UTF-8 sequence starting at `text[i]` - one to four bytes, each continuation byte
    10xxxxxx, no overlong form, no surrogate, nothing past U+10FFFF - or 0 where none starts there.
    @param text  the text
    @param i     the byte offset to look at, less than `text.size()`
    @return      the sequence's length in bytes, 1 to 4, or 0 for none */
inline std::size_t WellFormedLength(std::string_view text, std::size_t i) noexcept
{
    const auto byte = [&text](std::size_t at) { return static_cast<unsigned char>(text[at]); };
    const auto lead = byte(i);
    if (lead < 0x80)
    {
        return 1;
    }
    std::size_t   length = 0;
    unsigned char low    = 0x80; // the second byte's range, which rules out the overlong forms and the surrogates
    unsigned char high   = 0xbf;
    if (lead >= 0xc2 && lead <= 0xdf)
    {
        length = 2;
    }
    else if (lead >= 0xe0 && lead <= 0xef)
    {
        length = 3;
        low    = lead == 0xe0 ? 0xa0 : 0x80;
        high   = lead == 0xed ? 0x9f : 0xbf;
    }
    else if (lead >= 0xf0 && lead <= 0xf4)
    {
        length = 4;
        low    = lead == 0xf0 ? 0x90 : 0x80;
        high   = lead == 0xf4 ? 0x8f : 0xbf;
    }
    else
    {
        return 0;
    }
    if (i + length > text.size() || byte(i + 1) < low || byte(i + 1) > high)
    {
        return 0;
    }
    for (std::size_t k = 2; k < length; ++k)
    {
        if ((byte(i + k) & 0xc0) != 0x80)
        {
            return 0;
        }
    }
    return length;
}

/** Whether `text` is well-formed UTF-8 throughout.
    @param text  the text
    @return      true when every byte belongs to a well-formed sequence */
inline bool IsWellFormed(std::string_view text) noexcept
{
    for (std::size_t i = 0; i < text.size();)
    {
        const std::size_t length = WellFormedLength(text, i);
        if (length == 0)
        {
            return false;
        }
        i += length;
    }
    return true;
}
} // namespace reckless::core::utf8
