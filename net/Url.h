#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace reckless::net
{
/** A query's or a form's fields, in order: each name with its value, both decoded. */
using Fields = std::vector<std::pair<std::string, std::string>>;

/** ASCII lower case of `text`: a scheme's and a host's, which compare ignoring case.
    @param text  the text
    @return      the text with A-Z made a-z */
inline std::string AsciiLower(std::string_view text)
{
    std::string lower(text);
    for (char &c : lower)
    {
        if (c >= 'A' && c <= 'Z')
        {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return lower;
}

/**
    An absolute http or https URL in its parts, as far as the suite reads one: the scheme and the host in lower case,
    the port where one is written, and the target - the path with its query, never empty - with any fragment dropped.
    User information in the authority is refused, since nothing the suite talks to carries it.
*/
struct Url
{
    std::string scheme;  ///< "http" or "https"
    std::string host;    ///< the host, lower case; an IPv6 literal keeps its brackets
    int         port{0}; ///< the port written, or the scheme's default
    std::string target;  ///< the path and the query, "/" at the least

    /** `text` read as an absolute http or https URL.
        @param text  the URL
        @return      its parts, or nothing for text that is not one */
    static std::optional<Url> Parse(std::string_view text)
    {
        const std::size_t colon = text.find("://");
        if (colon == std::string_view::npos)
        {
            return std::nullopt;
        }
        Url url;
        url.scheme = AsciiLower(text.substr(0, colon));
        if (url.scheme != "http" && url.scheme != "https")
        {
            return std::nullopt;
        }
        std::string_view rest = text.substr(colon + 3);
        if (const std::size_t hash = rest.find('#'); hash != std::string_view::npos)
        {
            rest = rest.substr(0, hash);
        }
        const std::size_t      end       = rest.find_first_of("/?");
        const std::string_view authority = rest.substr(0, end);
        url.target = end == std::string_view::npos ? std::string("/") : std::string(rest.substr(end));
        if (url.target.front() == '?')
        {
            url.target.insert(url.target.begin(), '/');
        }
        if (authority.empty() || authority.find('@') != std::string_view::npos)
        {
            return std::nullopt;
        }
        std::string_view host = authority;
        std::string_view port;
        if (authority.front() == '[')
        {
            const std::size_t close = authority.find(']');
            if (close == std::string_view::npos)
            {
                return std::nullopt;
            }
            host = authority.substr(0, close + 1);
            if (close + 1 < authority.size())
            {
                if (authority[close + 1] != ':')
                {
                    return std::nullopt;
                }
                port = authority.substr(close + 2);
            }
        }
        else if (const std::size_t at = authority.rfind(':'); at != std::string_view::npos)
        {
            host = authority.substr(0, at);
            port = authority.substr(at + 1);
        }
        if (host.empty())
        {
            return std::nullopt;
        }
        url.host = AsciiLower(host);
        url.port = url.scheme == "https" ? 443 : 80;
        if (!port.empty())
        {
            int value = 0;
            for (const char c : port)
            {
                if (c < '0' || c > '9' || value > 65535)
                {
                    return std::nullopt;
                }
                value = value * 10 + (c - '0');
            }
            if (value < 1 || value > 65535)
            {
                return std::nullopt;
            }
            url.port = value;
        }
        return url;
    }

    /** The origin, as the web compares them: the scheme, the host and the port.
        @return  "scheme://host:port" */
    std::string Origin() const
    {
        return scheme + "://" + host + ":" + std::to_string(port);
    }

    /** The path alone, without the query.
        @return  the target up to its '?' */
    std::string Path() const
    {
        return target.substr(0, target.find('?'));
    }

    /** The query alone, without the '?'.
        @return  what follows the target's '?', or an empty string */
    std::string Query() const
    {
        const std::size_t at = target.find('?');
        return at == std::string::npos ? std::string() : target.substr(at + 1);
    }
};

/** A Location, or any reference, resolved against the URL it came from as RFC 3986 section 5.2 resolves one, as far as
    servers write them: an absolute URL as it is; a network-path reference ("//host/path") on the base's scheme; an
    absolute path ("/path") on the base's origin; anything else beside the base's last path segment.
    @param base       the absolute URL the reference came from
    @param reference  the reference
    @return           the absolute URL it names */
inline std::string ResolveReference(std::string_view base, std::string_view reference)
{
    if (reference.find("://") != std::string_view::npos)
    {
        return std::string(reference);
    }
    const auto parsed = Url::Parse(base);
    if (!parsed.has_value())
    {
        return std::string(reference);
    }
    if (reference.substr(0, 2) == "//")
    {
        return parsed->scheme + ":" + std::string(reference);
    }
    const std::size_t authority_end = base.find('/', base.find("://") + 3);
    const std::string root =
        std::string(base.substr(0, authority_end == std::string_view::npos ? base.size() : authority_end));
    if (!reference.empty() && reference.front() == '/')
    {
        return root + std::string(reference);
    }
    const std::string path = parsed->Path();
    return root + path.substr(0, path.rfind('/') + 1) + std::string(reference);
}

/** `text` percent-encoded for a query or a form: every byte but RFC 3986's unreserved characters (A-Z a-z 0-9 - . _ ~)
    as %XX in upper-case hex.
    @param text  the text, UTF-8
    @return      the encoded text */
inline std::string PercentEncode(std::string_view text)
{
    constexpr std::string_view kHex = "0123456789ABCDEF";
    std::string                out;
    out.reserve(text.size());
    for (const char c : text)
    {
        const auto byte = static_cast<unsigned char>(c);
        if ((byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || c == '-' ||
            c == '.' || c == '_' || c == '~')
        {
            out.push_back(c);
        }
        else
        {
            out.push_back('%');
            out.push_back(kHex[byte >> 4]);
            out.push_back(kHex[byte & 0x0f]);
        }
    }
    return out;
}

/** Percent-encoded text read back: each %XX its byte, a '+' a space where the text is a form's or a query's, and a '%'
    that is not followed by two hex digits kept as itself.
    @param text           the encoded text
    @param plus_is_space  true for a query or a form, where '+' stands for a space
    @return               the decoded bytes */
inline std::string PercentDecode(std::string_view text, bool plus_is_space = true)
{
    const auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9')
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
    };
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i)
    {
        if (text[i] == '%' && i + 2 < text.size() && hex(text[i + 1]) >= 0 && hex(text[i + 2]) >= 0)
        {
            out.push_back(static_cast<char>(hex(text[i + 1]) * 16 + hex(text[i + 2])));
            i += 2;
        }
        else if (text[i] == '+' && plus_is_space)
        {
            out.push_back(' ');
        }
        else
        {
            out.push_back(text[i]);
        }
    }
    return out;
}

/** Fields as a form body or a query string: each name and value percent-encoded, joined "name=value&...".
    @param fields  the fields, in order
    @return        the encoded text, with no leading '?' */
inline std::string FormEncode(const Fields &fields)
{
    std::string out;
    for (const auto &[name, value] : fields)
    {
        if (!out.empty())
        {
            out.push_back('&');
        }
        out += PercentEncode(name);
        out.push_back('=');
        out += PercentEncode(value);
    }
    return out;
}

/** A query string or a form body read into its fields, in order, each name and value decoded; a field with no '=' has
    an empty value, and empty fields between '&'s are skipped.
    @param query  the query, with or without its leading '?'
    @return       the fields */
inline Fields ParseQuery(std::string_view query)
{
    if (!query.empty() && query.front() == '?')
    {
        query.remove_prefix(1);
    }
    Fields fields;
    while (!query.empty())
    {
        const std::size_t      amp   = query.find('&');
        const std::string_view field = query.substr(0, amp);
        if (!field.empty())
        {
            const std::size_t eq = field.find('=');
            fields.emplace_back(PercentDecode(field.substr(0, eq)),
                                eq == std::string_view::npos ? std::string() : PercentDecode(field.substr(eq + 1)));
        }
        if (amp == std::string_view::npos)
        {
            break;
        }
        query.remove_prefix(amp + 1);
    }
    return fields;
}

/** The first field of `name` among `fields`.
    @param fields  the fields, as ParseQuery reads them
    @param name    the field's name
    @return        its value, or nothing when no field has the name */
inline std::optional<std::string> FieldOf(const Fields &fields, std::string_view name)
{
    for (const auto &[field, value] : fields)
    {
        if (field == name)
        {
            return value;
        }
    }
    return std::nullopt;
}
} // namespace reckless::net
