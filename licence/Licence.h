#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Ed25519.h"
#include "Json.h"

namespace reckless::licence
{
// A licence to the suite, as the person who bought it holds it: one line of text,
//
//     RTL1.<payload>.<signature>
//
// the payload a compact JSON object in base64url with no padding, the signature the Ed25519 signature of the ASCII
// bytes "RTL1." followed by the payload's text - the encoded text, so nothing needs putting in a canonical form before
// it is checked - in base64url with no padding. The payload's members: `v` (1), `kid` (the signing key's id),
// `serial`, `name`, `email`, `product` ("suite"), `major` (the newest major version of the suite it opens) and
// `issued` (the date, YYYY-MM-DD); a member it does not know is passed over, so a later issuer may add one. The
// licensing service writes the members in that order with `source` after them; the order matters to nothing that
// checks a licence, only to whoever reproduces one byte for byte (the test vectors).
//
// The plugins hold public keys alone (PublicKeys.h): a licence cannot be made from anything in a binary, only checked.
// A licence is per person and per suite, not per machine; it is checked offline, and nothing is sent anywhere. The
// format is docs/licence/licence-format.md; licence/tests/vectors.json the bytes both ends are held to.

/** The text every licence starts with, the format's version in it. */
inline constexpr std::string_view kPrefix = "RTL1.";

/** The product a licence to the whole suite names. */
inline constexpr std::string_view kSuiteProduct = "suite";

/** The longest licence text read, in characters before white space is taken out: twenty times a real one's length. */
inline constexpr std::size_t kMaxTextLength = 8192;

/** A key a licence may be signed by: its id, as a licence's `kid` names it, and its public half. */
struct PublicKey
{
    std::string_view id;    ///< the id a licence's `kid` names, such as "rt-2026-1"
    PublicKeyBytes   bytes; ///< the public key
};

/** What a licence says: the payload's members. */
struct Licence
{
    std::string kid;       ///< the id of the key that signed it
    std::string serial;    ///< the serial number, such as "RT-7K3M-QX9P-2HFD"
    std::string name;      ///< the licensee's name
    std::string email;     ///< the licensee's email address
    std::string product;   ///< what it opens: "suite"
    int         major = 0; ///< the newest major version of the suite it opens
    std::string issued;    ///< the date it was issued, YYYY-MM-DD
};

/** A licence's standing, best first. */
enum class Verdict
{
    kValid,            ///< a licence this build accepts
    kMalformed,        ///< not a licence: not RTL1 text, not base64url, not a JSON object of the members it needs
    kUnknownKey,       ///< signed by a key this build does not hold
    kBadSignature,     ///< its signature is not its key's over its payload: altered, or made up
    kWrongProduct,     ///< a licence to something other than the suite
    kRevoked,          ///< its serial is withdrawn: refunded, charged back or replaced
    kOlderMajorVersion ///< a licence to an earlier major version of the suite than this one
};

/** A licence's text checked: its standing and, where the payload could be read, what it says. */
struct Checked
{
    Verdict                verdict = Verdict::kMalformed; ///< its standing
    std::optional<Licence> licence;                       ///< what it says, wherever its payload could be read
};

/** What a licence is checked against: the keys the build holds, the serials withdrawn, the build's major version. */
struct Rules
{
    std::span<const PublicKey>        keys;            ///< the keys a licence may be signed by
    std::span<const std::string_view> revoked;         ///< the serials withdrawn
    int                               suite_major = 0; ///< the major version of the suite checking the licence
    std::string_view                  product     = kSuiteProduct; ///< the product the build is
};

/** The words a person reads for a verdict, in the licence dialog.
    @param verdict  the verdict
    @return         one short sentence */
inline std::string_view VerdictText(Verdict verdict) noexcept
{
    switch (verdict)
    {
    case Verdict::kValid:
        return "The licence is valid.";
    case Verdict::kMalformed:
        return "That is not a Reckless Technology licence.";
    case Verdict::kUnknownKey:
        return "The licence was not issued by Reckless Technology.";
    case Verdict::kBadSignature:
        return "The licence has been altered.";
    case Verdict::kWrongProduct:
        return "The licence is not for this product.";
    case Verdict::kRevoked:
        return "The licence has been withdrawn.";
    case Verdict::kOlderMajorVersion:
        return "The licence is for an earlier version: this one needs an upgrade.";
    }
    return "That is not a Reckless Technology licence.";
}

/** `bytes` in base64url with no padding (RFC 4648 section 5): the core's standard base-64 with '+' as '-', '/' as '_'
    and the '=' dropped.
    @param bytes  the bytes
    @return       the text */
inline std::string EncodeBase64Url(std::span<const std::uint8_t> bytes)
{
    std::string text = core::json::EncodeBase64(bytes);
    while (!text.empty() && text.back() == '=')
    {
        text.pop_back();
    }
    std::replace(text.begin(), text.end(), '+', '-');
    std::replace(text.begin(), text.end(), '/', '_');
    return text;
}

/** Base64url with no padding read back, or nothing for text that is not exactly that: '+', '/' and '=' are refused,
    and so is text whose last character carries bits past the last byte, so each run of bytes has one text alone.
    @param text  the text
    @return      the bytes, or nothing */
inline std::optional<core::json::Bytes> DecodeBase64Url(std::string_view text)
{
    if (text.size() % 4 == 1 || text.find_first_of("+/=") != std::string_view::npos)
    {
        return std::nullopt;
    }
    std::string standard(text);
    std::replace(standard.begin(), standard.end(), '-', '+');
    std::replace(standard.begin(), standard.end(), '_', '/');
    standard.append((4 - standard.size() % 4) % 4, '=');
    std::optional<core::json::Bytes> bytes = core::json::DecodeBase64(standard);
    if (!bytes || EncodeBase64Url(*bytes) != text)
    {
        return std::nullopt;
    }
    return bytes;
}

/** `text` with every ASCII space, tab and line break taken out: a licence wrapped by an email or pasted with its line
    ends reads as the one line it is.
    @param text  the text as it came
    @return      the text without white space */
inline std::string WithoutWhiteSpace(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (const char c : text)
    {
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n')
        {
            out.push_back(c);
        }
    }
    return out;
}

/** The payload's members read, or nothing when one it needs is missing or of the wrong kind: `v` must be 1, the text
    members strings, `major` a whole number from 0 to 255.
    @param payload  the payload's JSON text
    @return         what it says, or nothing */
inline std::optional<Licence> ReadPayload(std::string_view payload)
{
    core::json::ParseResult parsed = core::json::Parse(payload);
    if (!parsed.IsOk() || !parsed.value.IsObject())
    {
        return std::nullopt;
    }
    const core::json::Value &object  = parsed.value;
    const core::json::Value *version = object.Find("v");
    const core::json::Value *major   = object.Find("major");
    if (version == nullptr || !version->IsNumber() || version->AsDouble() != 1.0 || major == nullptr ||
        !major->IsNumber())
    {
        return std::nullopt;
    }
    const double major_value = major->AsDouble();
    if (!(major_value >= 0.0 && major_value <= 255.0) || std::floor(major_value) != major_value)
    {
        return std::nullopt;
    }
    Licence licence;
    licence.major = static_cast<int>(major_value);
    for (auto [member, field] : {std::pair{"kid", &licence.kid}, std::pair{"serial", &licence.serial},
                                 std::pair{"name", &licence.name}, std::pair{"email", &licence.email},
                                 std::pair{"product", &licence.product}, std::pair{"issued", &licence.issued}})
    {
        const core::json::Value *value = object.Find(member);
        if (value == nullptr || !value->IsString() || value->AsString().empty())
        {
            return std::nullopt;
        }
        *field = value->AsString();
    }
    return licence;
}

/** A licence's text checked against `rules`: its form, its key, its signature, then what it opens. White space in the
    text is passed over (WithoutWhiteSpace). The signature is checked before anything the payload says is believed, so
    a verdict past kBadSignature is the issuer's word.
    @param text   the licence as the person has it
    @param rules  the keys, the withdrawn serials and the build's version
    @return       the verdict and, wherever the payload could be read, what it says */
inline Checked Check(std::string_view text, const Rules &rules)
{
    Checked checked;
    if (text.size() > kMaxTextLength)
    {
        return checked;
    }
    const std::string line = WithoutWhiteSpace(text);
    if (!line.starts_with(kPrefix))
    {
        return checked;
    }
    const std::string_view rest = std::string_view(line).substr(kPrefix.size());
    const std::size_t      dot  = rest.find('.');
    if (dot == std::string_view::npos || rest.find('.', dot + 1) != std::string_view::npos)
    {
        return checked;
    }
    const std::string_view payload_text   = rest.substr(0, dot);
    const std::string_view signature_text = rest.substr(dot + 1);
    const auto             payload        = DecodeBase64Url(payload_text);
    const auto             signature      = DecodeBase64Url(signature_text);
    if (!payload || !signature || signature->size() != SignatureBytes().size())
    {
        return checked;
    }
    checked.licence = ReadPayload(std::string_view(reinterpret_cast<const char *>(payload->data()), payload->size()));
    if (!checked.licence)
    {
        return checked;
    }

    const auto key = std::find_if(rules.keys.begin(), rules.keys.end(),
                                  [&](const PublicKey &candidate) { return candidate.id == checked.licence->kid; });
    if (key == rules.keys.end())
    {
        checked.verdict = Verdict::kUnknownKey;
        return checked;
    }
    SignatureBytes signature_bytes{};
    std::copy(signature->begin(), signature->end(), signature_bytes.begin());
    const std::string_view signed_text = std::string_view(line).substr(0, kPrefix.size() + dot);
    if (!VerifyEd25519(signature_bytes, key->bytes,
                       std::span(reinterpret_cast<const std::uint8_t *>(signed_text.data()), signed_text.size())))
    {
        checked.verdict = Verdict::kBadSignature;
        return checked;
    }

    if (checked.licence->product != rules.product)
    {
        checked.verdict = Verdict::kWrongProduct;
    }
    else if (std::find(rules.revoked.begin(), rules.revoked.end(), checked.licence->serial) != rules.revoked.end())
    {
        checked.verdict = Verdict::kRevoked;
    }
    else if (checked.licence->major < rules.suite_major)
    {
        checked.verdict = Verdict::kOlderMajorVersion;
    }
    else
    {
        checked.verdict = Verdict::kValid;
    }
    return checked;
}
} // namespace reckless::licence
