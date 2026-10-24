#pragma once

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <variant>
#include <vector>

#include "DecimalText.h"
#include "Utf8.h"

namespace reckless::core::json
{
/**
    JSON (RFC 8259), the one text the suite saves in: a plugin's state - a session, a preset, a pedal's state inside a
    board - and every settings file. A value is null, a flag, a whole number, a real, text, an array or an object whose
    members keep the order they were set in.

    Write gives the text: two spaces of indent a level, each member and each element on a line of its own, LF line
    ends, an empty object "{}" and an empty array "[]", no line break after the last bracket. Text is UTF-8, with " and
    `\` escaped by a backslash, backspace, form feed, line feed, carriage return and tab as `\b` `\f` `\n` `\r` `\t` and
    every other control character under 0x20 as `\u` and four hex digits; anything else as it is, but a byte that is not
   part of well-formed UTF-8, which is written as U+FFFD, so the text is always JSON. A whole number is its digits; a
   real is the shortest text that reads back to the same double (std::to_chars), with ".0" added where that text is a
   whole number so it reads back a real ("0.3", "50.0", "1e+21"); infinity and NaN, which JSON cannot hold, are null.

    Parse reads exactly RFC 8259: any value, white space (space, tab, line feed, carriage return) around and between,
    nothing after it. A number with no fraction or exponent that fits 64 bits is a whole number and any other a real; a
    real past a double's range is refused. Text must be well-formed UTF-8 with no raw control character, its escapes
    the eight above and `\/`, a `\u` pair making a character above U+FFFF and a lone surrogate refused. A member named
    twice keeps its first place and its last value. A document nested deeper than kMaxDepth is refused, so a damaged
    file cannot exhaust a thread's stack.

    Bytes (a loaded response's samples) are carried as an object of the one member "base64" holding them in standard
    base-64 with '=' padding (FromBytes, BytesOf).
*/
class Value;

using Array  = std::vector<Value>;            ///< the elements of a JSON array, in order
using Member = std::pair<std::string, Value>; ///< one member of an object: its name and its value
using Object = std::vector<Member>;           ///< the members of a JSON object, in the order they were set
using Bytes  = std::vector<std::uint8_t>;     ///< bytes a document carries (FromBytes, BytesOf)

/** How deep a document may nest: arrays and objects inside one another. A board, the deepest state the suite writes,
    nests under a dozen deep (a split's lane's pedal's own state and the objects in it). */
inline constexpr int kMaxDepth = 256;

/** One JSON value: null, a flag, a whole number, a real, text, an array or an object (see the namespace's note). The
    constructors convert implicitly from each kind's natural C++ type, so a value is written as the thing it holds. */
class Value
{
  public:
    /** The kinds, in the order of the variant's alternatives. */
    enum class Kind
    {
        kNull,   ///< null
        kBool,   ///< true or false
        kInt,    ///< a whole number that fits 64 bits
        kDouble, ///< any other number
        kString, ///< UTF-8 text
        kArray,  ///< an ordered list of values
        kObject  ///< named members in the order they were set
    };

    /** A null value. */
    Value() = default;

    /** A null value, from the null pointer constant, which is ignored. */
    Value(std::nullptr_t) noexcept
    {
    }

    /** A flag.
        @param value  the flag */
    Value(bool value) : value_(value)
    {
    }

    /** A whole number.
        @param value  the number */
    Value(int value) : value_(static_cast<std::int64_t>(value))
    {
    }

    /** A whole number.
        @param value  the number */
    Value(std::int64_t value) : value_(value)
    {
    }

    /** A real.
        @param value  the number; infinity and NaN are written as null */
    Value(double value) : value_(value)
    {
    }

    /** Text.
        @param value  the UTF-8 text, taken */
    Value(std::string value) : value_(std::move(value))
    {
    }

    /** Text.
        @param value  the UTF-8 text, copied */
    Value(std::string_view value) : value_(std::string(value))
    {
    }

    /** Text.
        @param value  the null-terminated UTF-8 text, copied */
    Value(const char *value) : value_(std::string(value))
    {
    }

    /** An array.
        @param value  the elements, taken */
    Value(Array value) : value_(std::move(value))
    {
    }

    /** An object.
        @param value  the members, taken */
    Value(Object value) : value_(std::move(value))
    {
    }

    /** An empty array.
        @return  the value */
    static Value MakeArray()
    {
        return Value(Array{});
    }

    /** An empty object.
        @return  the value */
    static Value MakeObject()
    {
        return Value(Object{});
    }

    /** What kind of value this is.
        @return  the kind */
    Kind kind() const noexcept
    {
        return static_cast<Kind>(value_.index());
    }

    /** Whether this is null.
        @return  true for null */
    bool IsNull() const noexcept
    {
        return std::holds_alternative<std::monostate>(value_);
    }

    /** Whether this is a flag.
        @return  true for a flag */
    bool IsBool() const noexcept
    {
        return std::holds_alternative<bool>(value_);
    }

    /** Whether this is a whole number.
        @return  true for a whole number */
    bool IsInt() const noexcept
    {
        return std::holds_alternative<std::int64_t>(value_);
    }

    /** Whether this is a real.
        @return  true for a real */
    bool IsDouble() const noexcept
    {
        return std::holds_alternative<double>(value_);
    }

    /** Whether this is a whole number or a real.
        @return  true for a number */
    bool IsNumber() const noexcept
    {
        return IsInt() || IsDouble();
    }

    /** Whether this is text.
        @return  true for text */
    bool IsString() const noexcept
    {
        return std::holds_alternative<std::string>(value_);
    }

    /** Whether this is an array.
        @return  true for an array */
    bool IsArray() const noexcept
    {
        return std::holds_alternative<Array>(value_);
    }

    /** Whether this is an object.
        @return  true for an object */
    bool IsObject() const noexcept
    {
        return std::holds_alternative<Object>(value_);
    }

    /** The flag, or a number other than zero; false for any other kind.
        @return  the flag */
    bool AsBool() const noexcept
    {
        if (const auto *flag = std::get_if<bool>(&value_))
        {
            return *flag;
        }
        return IsNumber() && AsDouble() != 0.0;
    }

    /** The whole number, a real truncated toward zero (held to the range, NaN as 0), a flag as 0 or 1; 0 for any other
        kind.
        @return  the whole number */
    std::int64_t AsInt() const noexcept
    {
        if (const auto *whole = std::get_if<std::int64_t>(&value_))
        {
            return *whole;
        }
        if (const auto *real = std::get_if<double>(&value_))
        {
            if (std::isnan(*real))
            {
                return 0;
            }
            constexpr double kTop = 9223372036854775807.0; // 2^63, the first double past the range
            return *real >= kTop    ? std::numeric_limits<std::int64_t>::max()
                   : *real <= -kTop ? std::numeric_limits<std::int64_t>::min()
                                    : static_cast<std::int64_t>(*real);
        }
        if (const auto *flag = std::get_if<bool>(&value_))
        {
            return *flag ? 1 : 0;
        }
        return 0;
    }

    /** The real, the whole number as one, a flag as 0 or 1; 0 for any other kind.
        @return  the number */
    double AsDouble() const noexcept
    {
        if (const auto *real = std::get_if<double>(&value_))
        {
            return *real;
        }
        if (const auto *whole = std::get_if<std::int64_t>(&value_))
        {
            return static_cast<double>(*whole);
        }
        if (const auto *flag = std::get_if<bool>(&value_))
        {
            return *flag ? 1.0 : 0.0;
        }
        return 0.0;
    }

    /** The text; empty for any other kind.
        @return  the text, valid while this value lives and is not changed */
    const std::string &AsString() const noexcept
    {
        static const std::string kNone;
        const auto              *text = std::get_if<std::string>(&value_);
        return text != nullptr ? *text : kNone;
    }

    /** The elements.
        @return  the array, or null when this is not one */
    const Array *AsArray() const noexcept
    {
        return std::get_if<Array>(&value_);
    }

    /** The elements, to change.
        @return  the array, or null when this is not one */
    Array *AsArray() noexcept
    {
        return std::get_if<Array>(&value_);
    }

    /** The members.
        @return  the object, or null when this is not one */
    const Object *AsObject() const noexcept
    {
        return std::get_if<Object>(&value_);
    }

    /** The members, to change.
        @return  the object, or null when this is not one */
    Object *AsObject() noexcept
    {
        return std::get_if<Object>(&value_);
    }

    /** The member named `name`, or null when this is not an object or has none.
        @param name  the member's name
        @return      the member, valid until this value changes, or null */
    const Value *Find(std::string_view name) const noexcept
    {
        if (const auto *object = AsObject())
        {
            for (const auto &[key, value] : *object)
            {
                if (key == name)
                {
                    return &value;
                }
            }
        }
        return nullptr;
    }

    /** The member named `name`, to change, or null when this is not an object or has none.
        @param name  the member's name
        @return      the member, valid until this value changes, or null */
    Value *Find(std::string_view name) noexcept
    {
        return const_cast<Value *>(static_cast<const Value *>(this)->Find(name));
    }

    /** Whether there is a member named `name`.
        @param name  the member's name
        @return      true when this is an object with such a member */
    bool Has(std::string_view name) const noexcept
    {
        return Find(name) != nullptr;
    }

    /** The member named `name`, or a null value when there is none.
        @param name  the member's name
        @return      the member, or a shared null value */
    const Value &operator[](std::string_view name) const noexcept
    {
        static const Value kAbsent;
        const Value       *found = Find(name);
        return found != nullptr ? *found : kAbsent;
    }

    /** Member `name`'s number, or `fallback` where there is none or it is not a number.
        @param name      the member's name
        @param fallback  the value for a missing or non-numeric member
        @return          the number */
    double NumberOr(std::string_view name, double fallback) const noexcept
    {
        const Value *found = Find(name);
        return found != nullptr && found->IsNumber() ? found->AsDouble() : fallback;
    }

    /** Member `name`'s number as a whole number (AsInt), or `fallback` where there is none or it is not a number.
        @param name      the member's name
        @param fallback  the value for a missing or non-numeric member
        @return          the whole number */
    std::int64_t IntOr(std::string_view name, std::int64_t fallback) const noexcept
    {
        const Value *found = Find(name);
        return found != nullptr && found->IsNumber() ? found->AsInt() : fallback;
    }

    /** Member `name`'s flag - a flag, or a number other than zero - or `fallback` where there is none or it is
        neither.
        @param name      the member's name
        @param fallback  the value for a member that is missing or neither a flag nor a number
        @return          the flag */
    bool BoolOr(std::string_view name, bool fallback) const noexcept
    {
        const Value *found = Find(name);
        return found != nullptr && (found->IsBool() || found->IsNumber()) ? found->AsBool() : fallback;
    }

    /** Member `name`'s text, or `fallback` where there is none or it is not text.
        @param name      the member's name
        @param fallback  the text for a missing or non-text member
        @return          the text, a copy */
    std::string StringOr(std::string_view name, std::string_view fallback) const
    {
        const Value *found = Find(name);
        return found != nullptr && found->IsString() ? found->AsString() : std::string(fallback);
    }

    /** Sets the member named `name` - in its place when there is one, else after the rest - making this an object
        first if it is not one; the member, to write further.
        @param name   the member's name
        @param value  the value to hold, taken
        @return       the member, valid until this value changes */
    Value &Set(std::string_view name, Value value)
    {
        if (!IsObject())
        {
            value_ = Object{};
        }
        auto &object = std::get<Object>(value_);
        for (auto &[key, existing] : object)
        {
            if (key == name)
            {
                existing = std::move(value);
                return existing;
            }
        }
        object.emplace_back(std::string(name), std::move(value));
        return object.back().second;
    }

    /** No member named `name`: nothing when this is not an object or has none.
        @param name  the member's name */
    void Remove(std::string_view name)
    {
        if (auto *object = AsObject())
        {
            for (auto it = object->begin(); it != object->end(); ++it)
            {
                if (it->first == name)
                {
                    object->erase(it);
                    return;
                }
            }
        }
    }

    /** `value` after the elements, making this an array first if it is not one.
        @param value  the element to add, taken */
    void Append(Value value)
    {
        if (!IsArray())
        {
            value_ = Array{};
        }
        std::get<Array>(value_).push_back(std::move(value));
    }

    /** Whether two values are the same kind and hold the same: members in the same order for an object.
        @return  true when equal */
    friend bool operator==(const Value &, const Value &) = default;

  private:
    /** What is held, one alternative a kind. */
    std::variant<std::monostate, bool, std::int64_t, double, std::string, Array, Object> value_;
};

// ---- Bytes ------------------------------------------------------------------------------------------------------

/** The one member of the object that stands for bytes. */
inline constexpr std::string_view kBase64Member = "base64";

/** `bytes` in standard base-64: three bytes to four characters of A-Z a-z 0-9 + /, the last group padded with '='.
    @param bytes  the bytes to encode
    @return       the text, four characters a group */
inline std::string EncodeBase64(std::span<const std::uint8_t> bytes)
{
    constexpr std::string_view kAlphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string                out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3)
    {
        const std::size_t   remaining = bytes.size() - i;
        const std::uint32_t byte0     = bytes[i];
        const std::uint32_t byte1     = remaining > 1 ? bytes[i + 1] : 0u;
        const std::uint32_t byte2     = remaining > 2 ? bytes[i + 2] : 0u;
        out.push_back(kAlphabet[byte0 >> 2]);
        out.push_back(kAlphabet[((byte0 & 0x03u) << 4) | (byte1 >> 4)]);
        out.push_back(remaining > 1 ? kAlphabet[((byte1 & 0x0fu) << 2) | (byte2 >> 6)] : '=');
        out.push_back(remaining > 2 ? kAlphabet[byte2 & 0x3fu] : '=');
    }
    return out;
}

/** Standard base-64 read back, or nothing for text that is not it: a length that is not a whole number of groups of
    four, a character outside the alphabet, padding anywhere but the last one or two places.
    @param text  the base-64 text
    @return      the bytes, or nothing when the text is not standard base-64 */
inline std::optional<Bytes> DecodeBase64(std::string_view text)
{
    const auto sextet = [](char c) -> int {
        if (c >= 'A' && c <= 'Z')
        {
            return c - 'A';
        }
        if (c >= 'a' && c <= 'z')
        {
            return c - 'a' + 26;
        }
        if (c >= '0' && c <= '9')
        {
            return c - '0' + 52;
        }
        return c == '+' ? 62 : (c == '/' ? 63 : -1);
    };
    if (text.size() % 4 != 0)
    {
        return std::nullopt;
    }
    Bytes out;
    out.reserve(text.size() / 4 * 3);
    for (std::size_t i = 0; i < text.size(); i += 4)
    {
        const bool last     = i + 4 == text.size();
        const int  padding  = last ? (text[i + 3] == '=' ? (text[i + 2] == '=' ? 2 : 1) : 0) : 0;
        int        group[4] = {};
        for (int k = 0; k < 4 - padding; ++k)
        {
            group[k] = sextet(text[i + static_cast<std::size_t>(k)]);
            if (group[k] < 0)
            {
                return std::nullopt;
            }
        }
        out.push_back(static_cast<std::uint8_t>((group[0] << 2) | (group[1] >> 4)));
        if (padding < 2)
        {
            out.push_back(static_cast<std::uint8_t>(((group[1] & 0x0f) << 4) | (group[2] >> 2)));
        }
        if (padding < 1)
        {
            out.push_back(static_cast<std::uint8_t>(((group[2] & 0x03) << 6) | group[3]));
        }
    }
    return out;
}

/** `bytes` as a document carries them: an object of the one member "base64" holding their text.
    @param bytes  the bytes to carry
    @return       the object */
inline Value FromBytes(std::span<const std::uint8_t> bytes)
{
    Value value = Value::MakeObject();
    value.Set(kBase64Member, EncodeBase64(bytes));
    return value;
}

/** The bytes `value` carries (FromBytes), or nothing where it is not such an object or its text does not decode.
    @param value  the value to read
    @return       the bytes, or nothing */
inline std::optional<Bytes> BytesOf(const Value &value)
{
    const Object *object = value.AsObject();
    if (object == nullptr || object->size() != 1 || object->front().first != kBase64Member ||
        !object->front().second.IsString())
    {
        return std::nullopt;
    }
    return DecodeBase64(object->front().second.AsString());
}

// ---- Writing ------------------------------------------------------------------------------------------------------

/** A real as Write writes it: the shortest text that reads back to the same double, ".0" added to a whole number;
    "null" for infinity and NaN.
    @param number  the real
    @return        the text */
inline std::string NumberText(double number)
{
    if (!std::isfinite(number))
    {
        return "null";
    }
    char        buffer[32];
    const auto  result = std::to_chars(buffer, buffer + sizeof(buffer), number);
    std::string text(buffer, result.ptr);
    if (text.find_first_of(".e") == std::string::npos)
    {
        text += ".0";
    }
    return text;
}

namespace detail
{
/** Text as a JSON string literal, quotes included, escaped as the namespace's note says.
    @param out   the text to append to
    @param text  the UTF-8 text; a byte that is not well-formed UTF-8 is written as U+FFFD */
inline void WriteQuoted(std::string &out, std::string_view text)
{
    constexpr std::string_view kHex = "0123456789abcdef";
    out.push_back('"');
    for (std::size_t i = 0; i < text.size();)
    {
        const char c = text[i];
        switch (c)
        {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(c) < 0x20)
            {
                out += "\\u00";
                out.push_back(kHex[static_cast<unsigned char>(c) >> 4]);
                out.push_back(kHex[static_cast<unsigned char>(c) & 0xf]);
                break;
            }
            if (const std::size_t length = utf8::WellFormedLength(text, i); length > 0)
            {
                out.append(text.substr(i, length));
                i += length;
                continue;
            }
            out += "\xEF\xBF\xBD"; // U+FFFD for a byte that is not well-formed UTF-8
            break;
        }
        ++i;
    }
    out.push_back('"');
}

/** A value as JSON text, indented two spaces a level.
    @param out     the text to append to
    @param value   the value to write
    @param indent  the nesting depth of the value, 0 at the top */
inline void WriteValue(std::string &out, const Value &value, int indent)
{
    const auto line = [&out](int depth) {
        out.push_back('\n');
        out.append(static_cast<std::size_t>(depth) * 2, ' ');
    };
    switch (value.kind())
    {
    case Value::Kind::kNull:
        out += "null";
        return;
    case Value::Kind::kBool:
        out += value.AsBool() ? "true" : "false";
        return;
    case Value::Kind::kInt:
        out += std::to_string(value.AsInt());
        return;
    case Value::Kind::kDouble:
        out += NumberText(value.AsDouble());
        return;
    case Value::Kind::kString:
        WriteQuoted(out, value.AsString());
        return;
    case Value::Kind::kArray: {
        const Array &array = *value.AsArray();
        if (array.empty())
        {
            out += "[]";
            return;
        }
        out.push_back('[');
        for (std::size_t i = 0; i < array.size(); ++i)
        {
            line(indent + 1);
            WriteValue(out, array[i], indent + 1);
            if (i + 1 < array.size())
            {
                out.push_back(',');
            }
        }
        line(indent);
        out.push_back(']');
        return;
    }
    case Value::Kind::kObject: {
        const Object &object = *value.AsObject();
        if (object.empty())
        {
            out += "{}";
            return;
        }
        out.push_back('{');
        for (std::size_t i = 0; i < object.size(); ++i)
        {
            line(indent + 1);
            WriteQuoted(out, object[i].first);
            out += ": ";
            WriteValue(out, object[i].second, indent + 1);
            if (i + 1 < object.size())
            {
                out.push_back(',');
            }
        }
        line(indent);
        out.push_back('}');
        return;
    }
    }
}
} // namespace detail

/** `value` as JSON text (see the namespace's note).
    @param value  the value to write
    @return       the text, with no line break after the last bracket */
inline std::string Write(const Value &value)
{
    std::string out;
    detail::WriteValue(out, value, 0);
    return out;
}

// ---- Reading ------------------------------------------------------------------------------------------------------

/** What Parse made of a text. */
struct ParseResult
{
    /** The document; null when the text was refused. */
    Value value;

    /** Why the text was refused - "line:column: what" - or empty when it was read. */
    std::string error;

    /** Whether the text was read.
        @return  true when `error` is empty */
    bool IsOk() const noexcept
    {
        return error.empty();
    }
};

namespace detail
{
/** RFC 8259's grammar, recursive descent over the text's bytes; the first refusal recorded with where it happened, and
    everything unwinding from it. */
class Reader
{
  public:
    /** A reader over `text`.
        @param text  the JSON text, which must outlive the reader */
    explicit Reader(std::string_view text) : text_(text)
    {
    }

    /** Read the whole text as one document, white space around it and nothing after.
        @return  the document, or why the text was refused */
    ParseResult Document()
    {
        ParseResult result;
        SkipSpace();
        Value value = ParseValue(0);
        if (!failed_)
        {
            SkipSpace();
            if (position_ < text_.size())
            {
                Fail("unexpected text after the document");
            }
        }
        if (failed_)
        {
            result.error = Where() + ": " + message_;
            return result;
        }
        result.value = std::move(value);
        return result;
    }

  private:
    /** Record the first refusal, and where it happened; later ones are ignored.
        @param message  what was wrong, in a few words */
    void Fail(std::string_view message)
    {
        if (!failed_)
        {
            failed_   = true;
            message_  = std::string(message);
            location_ = position_;
        }
    }

    /** "line:column" of the refusal, both counted from 1, the column in bytes.
        @return  the place, as text */
    std::string Where() const
    {
        int         line   = 1;
        std::size_t column = 1;
        for (std::size_t i = 0; i < location_ && i < text_.size(); ++i)
        {
            if (text_[i] == '\n')
            {
                ++line;
                column = 1;
            }
            else
            {
                ++column;
            }
        }
        return std::to_string(line) + ":" + std::to_string(column);
    }

    /** Step over white space: space, tab, line feed and carriage return. */
    void SkipSpace() noexcept
    {
        while (position_ < text_.size() && (text_[position_] == ' ' || text_[position_] == '\t' ||
                                            text_[position_] == '\n' || text_[position_] == '\r'))
        {
            ++position_;
        }
    }

    /** Step over `c` if it is next.
        @param c  the character to expect
        @return   true when it was there and is consumed */
    bool Match(char c) noexcept
    {
        if (position_ < text_.size() && text_[position_] == c)
        {
            ++position_;
            return true;
        }
        return false;
    }

    /** Step over `word` if it is next.
        @param word  the characters to expect
        @return      true when they were there and are consumed */
    bool MatchWord(std::string_view word) noexcept
    {
        if (text_.substr(position_, word.size()) == word)
        {
            position_ += word.size();
            return true;
        }
        return false;
    }

    /** Read one value at the position.
        @param depth  how many arrays and objects enclose it; at kMaxDepth the nesting is refused
        @return       the value, or null with the refusal recorded (Fail) */
    Value ParseValue(int depth)
    {
        if (position_ >= text_.size())
        {
            Fail("expected a value");
            return {};
        }
        const char c = text_[position_];
        if (c == '{' || c == '[')
        {
            if (depth >= kMaxDepth)
            {
                Fail("nested too deep");
                return {};
            }
            ++position_;
            return c == '{' ? ParseObject(depth + 1) : ParseArray(depth + 1);
        }
        if (c == '"')
        {
            return Value(ParseString());
        }
        if (c == '-' || (c >= '0' && c <= '9'))
        {
            return ParseNumber();
        }
        if (MatchWord("true"))
        {
            return Value(true);
        }
        if (MatchWord("false"))
        {
            return Value(false);
        }
        if (MatchWord("null"))
        {
            return {};
        }
        Fail("expected a value");
        return {};
    }

    /** Read an object's members, after its opening brace.
        @param depth  the nesting depth of the object's members
        @return       the object, or null with the refusal recorded */
    Value ParseObject(int depth)
    {
        Value object = Value::MakeObject();
        SkipSpace();
        if (Match('}'))
        {
            return object;
        }
        for (;;)
        {
            SkipSpace();
            if (position_ >= text_.size() || text_[position_] != '"')
            {
                Fail("expected a member's name in double quotes");
                return {};
            }
            std::string name = ParseString();
            if (failed_)
            {
                return {};
            }
            SkipSpace();
            if (!Match(':'))
            {
                Fail("expected ':'");
                return {};
            }
            SkipSpace();
            Value value = ParseValue(depth);
            if (failed_)
            {
                return {};
            }
            object.Set(name, std::move(value));
            SkipSpace();
            if (Match(','))
            {
                continue;
            }
            if (Match('}'))
            {
                return object;
            }
            Fail("expected ',' or '}'");
            return {};
        }
    }

    /** Read an array's elements, after its opening bracket.
        @param depth  the nesting depth of the array's elements
        @return       the array, or null with the refusal recorded */
    Value ParseArray(int depth)
    {
        Value array = Value::MakeArray();
        SkipSpace();
        if (Match(']'))
        {
            return array;
        }
        for (;;)
        {
            SkipSpace();
            Value value = ParseValue(depth);
            if (failed_)
            {
                return {};
            }
            array.Append(std::move(value));
            SkipSpace();
            if (Match(','))
            {
                continue;
            }
            if (Match(']'))
            {
                return array;
            }
            Fail("expected ',' or ']'");
            return {};
        }
    }

    /** Four hex digits after a `\u`, or -1.
        @return  the number 0 to 0xffff, or -1 when four hex digits do not follow */
    int Hex4()
    {
        if (position_ + 4 > text_.size())
        {
            return -1;
        }
        int value = 0;
        for (int k = 0; k < 4; ++k)
        {
            const char c = text_[position_++];
            const int  d = (c >= '0' && c <= '9')   ? c - '0'
                           : (c >= 'a' && c <= 'f') ? c - 'a' + 10
                           : (c >= 'A' && c <= 'F') ? c - 'A' + 10
                                                    : -1;
            if (d < 0)
            {
                return -1;
            }
            value = (value << 4) | d;
        }
        return value;
    }

    /** Read a string literal at its opening quote, escapes resolved.
        @return  the UTF-8 text, or empty with the refusal recorded */
    std::string ParseString()
    {
        ++position_; // the opening quote
        std::string out;
        for (;;)
        {
            if (position_ >= text_.size())
            {
                Fail("unterminated text");
                return {};
            }
            const auto c = static_cast<unsigned char>(text_[position_]);
            if (c == '"')
            {
                ++position_;
                return out;
            }
            if (c < 0x20)
            {
                Fail("a control character in text");
                return {};
            }
            if (c == '\\')
            {
                ++position_;
                if (position_ >= text_.size())
                {
                    Fail("unterminated text");
                    return {};
                }
                const char escape = text_[position_++];
                switch (escape)
                {
                case '"':
                case '\\':
                case '/':
                    out.push_back(escape);
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u': {
                    int unit = Hex4();
                    if (unit < 0)
                    {
                        Fail("a bad \\u escape");
                        return {};
                    }
                    std::uint32_t code = static_cast<std::uint32_t>(unit);
                    if (unit >= 0xdc00 && unit <= 0xdfff)
                    {
                        Fail("a lone surrogate");
                        return {};
                    }
                    if (unit >= 0xd800 && unit <= 0xdbff)
                    {
                        const int low = MatchWord("\\u") ? Hex4() : -1;
                        if (low < 0xdc00 || low > 0xdfff)
                        {
                            Fail("a lone surrogate");
                            return {};
                        }
                        code = 0x10000u + ((code - 0xd800u) << 10) + (static_cast<std::uint32_t>(low) - 0xdc00u);
                    }
                    utf8::AppendUtf8(out, code);
                    break;
                }
                default:
                    Fail("an unknown escape");
                    return {};
                }
                continue;
            }
            const std::size_t length = utf8::WellFormedLength(text_, position_);
            if (length == 0)
            {
                Fail("text that is not UTF-8");
                return {};
            }
            out.append(text_.substr(position_, length));
            position_ += length;
        }
    }

    /** Read a number at the position: a whole number when it has no fraction or exponent and fits 64 bits.
        @return  the number, or null with the refusal recorded */
    Value ParseNumber()
    {
        const std::size_t start  = position_;
        const auto        digits = [this]() {
            const std::size_t from = position_;
            while (position_ < text_.size() && text_[position_] >= '0' && text_[position_] <= '9')
            {
                ++position_;
            }
            return position_ - from;
        };
        Match('-');
        const std::size_t integer_start = position_;
        const std::size_t integer       = digits();
        if (integer == 0 || (integer > 1 && text_[integer_start] == '0'))
        {
            position_ = start;
            Fail("a malformed number");
            return {};
        }
        bool whole = true;
        if (Match('.'))
        {
            whole = false;
            if (digits() == 0)
            {
                Fail("a malformed number");
                return {};
            }
        }
        if (Match('e') || Match('E'))
        {
            whole = false;
            if (!Match('+'))
            {
                Match('-');
            }
            if (digits() == 0)
            {
                Fail("a malformed number");
                return {};
            }
        }
        const char *first = text_.data() + start;
        const char *last  = text_.data() + position_;
        if (whole)
        {
            std::int64_t number = 0;
            if (const auto result = std::from_chars(first, last, number);
                result.ec == std::errc() && result.ptr == last)
            {
                return Value(number);
            }
        }
        const auto number = ReadDecimal(std::string_view(first, static_cast<std::size_t>(last - first)));
        if (!number || !std::isfinite(*number))
        {
            position_ = start;
            Fail("a number out of range");
            return {};
        }
        return Value(*number);
    }

    std::string_view text_;          ///< the text being read
    std::size_t      position_{0};   ///< the next byte to read
    bool             failed_{false}; ///< whether a refusal has been recorded
    std::string      message_;       ///< the first refusal's message
    std::size_t      location_{0};   ///< the byte offset of the first refusal
};
} // namespace detail

/** `text` read as a JSON document (see the namespace's note).
    @param text  the JSON text
    @return      the document, or why the text was refused */
inline ParseResult Parse(std::string_view text)
{
    return detail::Reader(text).Document();
}
} // namespace reckless::core::json
