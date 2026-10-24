#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "Color.h"
#include "DecimalText.h"
#include "Geometry.h"

namespace reckless::gfx
{
/**
    What the toolkit's two SVG readers share - the meters' artwork (SvgArt.h) and the icons' drawings (SvgImage.h):
    the text read as a tree of elements, refusing what is not plain markup, and the numbers, colours and references
    their attributes hold, read whatever the process's locale. Each reader derives from it privately and reads the tree
    into what it draws by its own rules.
*/
class SvgSyntax
{
  protected:
    // ---- The text as a tree of elements ---------------------------------------------------------------------------

    /** One element of the document: its name, attributes, children and parent. */
    struct Node
    {
        std::string name; ///< the tag name
        std::vector<std::pair<std::string, std::string>>
                          attributes;      ///< the attributes, name and value, in document order
        std::vector<Node> children;        ///< the child elements, in document order
        const Node       *parent{nullptr}; ///< the enclosing element; null for the root, set by Reader::Link()

        /** @param key  the attribute's name
            @return     its value, valid as long as the node; nullptr when the element has none */
        const std::string *Find(std::string_view key) const
        {
            for (const auto &[k, v] : attributes)
            {
                if (k == key)
                {
                    return &v;
                }
            }
            return nullptr;
        }
    };

    /** Start and end tags with quoted attributes, empty-element tags, whitespace between them, comments and an XML
        declaration; anything else - text, an entity, a CDATA section, a doctype - refused. */
    class Xml
    {
      public:
        /** Begin reading a document, past a byte-order mark if it has one.
            @param text  the document, which outlives the reader */
        explicit Xml(std::string_view text) : text_(text)
        {
            if (text_.substr(0, 3) == "\xEF\xBB\xBF")
            {
                at_ = 3; // a byte-order mark
            }
        }

        /** Read the whole document.
            @param root   the root element, filled in
            @param error  the reason, set when the document is refused
            @return       true when the document is well formed and holds one element */
        bool Parse(Node &root, std::string &error)
        {
            SkipMisc();
            if (!Element(root, error))
            {
                return false;
            }
            SkipMisc();
            if (at_ != text_.size())
            {
                error = "something after the root element";
                return false;
            }
            return true;
        }

      private:
        /** @param s  the text to look for
            @return     true when the text at the read position starts with `s` */
        bool At(std::string_view s) const
        {
            return text_.substr(at_, s.size()) == s;
        }

        /** @param c  a character
            @return     true for a space, a tab or a line break */
        static bool Space(char c)
        {
            return c == ' ' || c == '\t' || c == '\n' || c == '\r';
        }

        /** Move the read position past any white space. */
        void SkipSpace()
        {
            while (at_ < text_.size() && Space(text_[at_]))
            {
                ++at_;
            }
        }

        /** Move the read position past white space, comments and processing instructions. */
        void SkipMisc()
        {
            for (;;)
            {
                SkipSpace();
                const bool comment = At("<!--");
                if (!comment && !At("<?"))
                {
                    return;
                }
                const std::string_view end_mark = comment ? "-->" : "?>";
                const auto             end      = text_.find(end_mark, at_ + (comment ? 4 : 2));
                at_                             = end == std::string_view::npos ? text_.size() : end + end_mark.size();
            }
        }

        /** @param c  a character
            @return     true when it may be in an element or attribute name */
        static bool NameChar(char c)
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' ||
                   c == ':' || c == '.';
        }

        /** Take a name at the read position.
            @return the name; empty when none starts there */
        std::string Name()
        {
            const std::size_t start = at_;
            while (at_ < text_.size() && NameChar(text_[at_]))
            {
                ++at_;
            }
            return std::string(text_.substr(start, at_ - start));
        }

        /** Read one element, its attributes and everything in it.
            @param node   the element, filled in
            @param error  the reason, set when the text is refused
            @return       true when the element was read to its end */
        bool Element(Node &node, std::string &error)
        {
            if (!At("<") || At("<!") || At("</"))
            {
                error = "something other than an element where one belongs";
                return false;
            }
            ++at_;
            node.name = Name();
            if (node.name.empty())
            {
                error = "an element with no name";
                return false;
            }
            for (;;)
            {
                SkipSpace();
                if (At("/>"))
                {
                    at_ += 2;
                    return true;
                }
                if (At(">"))
                {
                    ++at_;
                    break;
                }
                std::string key = Name();
                SkipSpace();
                if (key.empty() || !At("="))
                {
                    error = "a malformed attribute in <" + node.name + ">";
                    return false;
                }
                ++at_;
                SkipSpace();
                if (at_ >= text_.size() || (text_[at_] != '"' && text_[at_] != '\''))
                {
                    error = "an unquoted attribute in <" + node.name + ">";
                    return false;
                }
                const char        quote = text_[at_++];
                const std::size_t end   = text_.find(quote, at_);
                if (end == std::string_view::npos)
                {
                    error = "an unterminated attribute in <" + node.name + ">";
                    return false;
                }
                std::string value(text_.substr(at_, end - at_));
                at_ = end + 1;
                if (value.find_first_of("&<") != std::string::npos)
                {
                    error = "an entity or a '<' in <" + node.name + ">'s " + key;
                    return false;
                }
                if (node.Find(key) != nullptr)
                {
                    error = "<" + node.name + "> with " + key + " twice";
                    return false;
                }
                node.attributes.emplace_back(std::move(key), std::move(value));
            }
            for (;;)
            {
                SkipMisc();
                if (at_ >= text_.size())
                {
                    error = "<" + node.name + "> never closed";
                    return false;
                }
                if (At("</"))
                {
                    at_ += 2;
                    const std::string name = Name();
                    SkipSpace();
                    if (name != node.name || !At(">"))
                    {
                        error = "<" + node.name + "> closed by </" + name + ">";
                        return false;
                    }
                    ++at_;
                    return true;
                }
                if (!At("<"))
                {
                    error = "text in <" + node.name + ">";
                    return false;
                }
                node.children.emplace_back();
                if (!Element(node.children.back(), error))
                {
                    return false;
                }
            }
        }

        std::string_view text_;  ///< the document
        std::size_t      at_{0}; ///< the read position, a byte offset into the document
    };

    // ---- Numbers, colours and references -------------------------------------------------------------------------

    /** @param c  a character
        @return     true for 0 to 9 */
    static bool Digit(char c)
    {
        return c >= '0' && c <= '9';
    }

    /** A decimal number - a sign, digits, a point and an exponent - taken off the front of `s`, read
        whatever the process's locale by core/'s ReadDecimal (DecimalText.h), as the series' readouts read typed
        text: the nearest double. False, and `s` as it was, for none.
        @param s    the text, with the number removed from its front on success
        @param out  the number read, set on success
        @return     true when a number was taken */
    static bool TakeNumber(std::string_view &s, double &out)
    {
        std::size_t i = 0;
        if (i < s.size() && (s[i] == '-' || s[i] == '+'))
        {
            ++i;
        }
        const std::size_t integer = i;
        while (i < s.size() && Digit(s[i]))
        {
            ++i;
        }
        bool any = i > integer;
        if (i < s.size() && s[i] == '.')
        {
            const std::size_t fraction = ++i;
            while (i < s.size() && Digit(s[i]))
            {
                ++i;
            }
            any = any || i > fraction;
        }
        if (!any)
        {
            return false;
        }
        if (i < s.size() && (s[i] == 'e' || s[i] == 'E'))
        {
            std::size_t j = i + 1;
            if (j < s.size() && (s[j] == '-' || s[j] == '+'))
            {
                ++j;
            }
            if (j < s.size() && Digit(s[j]))
            {
                while (j < s.size() && Digit(s[j]))
                {
                    ++j;
                }
                i = j;
            }
        }
        const char *const first = s.data() + (s.front() == '+' ? 1 : 0); // the reading takes no plus sign
        const char *const last  = s.data() + i;
        const auto        value = core::ReadDecimal(std::string_view(first, static_cast<std::size_t>(last - first)));
        if (!value)
        {
            return false;
        }
        out = *value;
        s.remove_prefix(i);
        return true;
    }

    /** Remove the spaces, tabs, line breaks and commas at the front.
        @param s  the text */
    static void SkipSeparators(std::string_view &s)
    {
        while (!s.empty() &&
               (s.front() == ' ' || s.front() == ',' || s.front() == '\t' || s.front() == '\n' || s.front() == '\r'))
        {
            s.remove_prefix(1);
        }
    }

    /** The whole of `text` one plain number, in single precision as the parser reads an attribute; false for anything
        else - a unit, a second number, nothing.
        @param text  the attribute's text
        @param out   the number, set on success
        @return      true when the whole text is one number */
    static bool Number(std::string_view text, float &out)
    {
        SkipSeparators(text);
        double value = 0.0;
        if (!TakeNumber(text, value))
        {
            return false;
        }
        SkipSeparators(text);
        out = static_cast<float>(value);
        return text.empty();
    }

    /** @param c  a character
        @return     its value as a hexadecimal digit, 0 to 15; -1 for any other */
    static int Hex(char c)
    {
        if (Digit(c))
        {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f')
        {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F')
        {
            return c - 'A' + 10;
        }
        return -1;
    }

    /** `#rgb` or `#rrggbb`, opaque; none for anything else.
        @param text  the colour's text
        @return      the colour, or none */
    static std::optional<Color> HexColour(std::string_view text)
    {
        if ((text.size() != 4 && text.size() != 7) || text[0] != '#')
        {
            return std::nullopt;
        }
        const bool short_form = text.size() == 4;
        int        channels[3]{};
        for (std::size_t c = 0; c < 3; ++c)
        {
            const int hi = Hex(text[short_form ? 1 + c : 1 + 2 * c]);
            const int lo = short_form ? hi : Hex(text[2 + 2 * c]);
            if (hi < 0 || lo < 0)
            {
                return std::nullopt;
            }
            channels[c] = hi * 16 + lo;
        }
        return Color::FromRgb(static_cast<std::uint8_t>(channels[0]), static_cast<std::uint8_t>(channels[1]),
                              static_cast<std::uint8_t>(channels[2]));
    }

    /** The id of `url(#id)`, or empty.
        @param text  the attribute's text
        @return      the id; empty when the text is not of that form */
    static std::string UrlId(std::string_view text)
    {
        if (text.size() > 6 && text.substr(0, 5) == "url(#" && text.back() == ')')
        {
            return std::string(text.substr(5, text.size() - 6));
        }
        return {};
    }

    /** Whether two points are exactly the same.
        @param a  one point
        @param b  the other
        @return   true when both coordinates are equal */
    static bool Same(PointF a, PointF b) noexcept
    {
        return !(a.x < b.x) && !(a.x > b.x) && !(a.y < b.y) && !(a.y > b.y);
    }
};
} // namespace reckless::gfx
