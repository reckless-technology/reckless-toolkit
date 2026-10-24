#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "Json.h"

namespace reckless::net
{
/** A SHA-256 digest: 32 bytes. */
using Sha256Digest = std::array<std::uint8_t, 32>;

/** SHA-256 of `bytes` (FIPS 180-4): the 64-byte blocks through the compression function, the message padded with a
    1 bit, zeros and its length in bits big-endian. What PKCE's S256 challenge hashes its verifier with; not for bulk
    data, though it runs at some hundreds of megabytes a second.
    @param bytes  the message
    @return       its digest */
inline Sha256Digest Sha256(std::span<const std::uint8_t> bytes)
{
    static constexpr std::uint32_t kRound[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                              0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto    rotate   = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };
    const auto    compress = [&](const std::uint8_t *block) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i)
        {
            w[i] = (static_cast<std::uint32_t>(block[4 * i]) << 24) |
                   (static_cast<std::uint32_t>(block[4 * i + 1]) << 16) |
                   (static_cast<std::uint32_t>(block[4 * i + 2]) << 8) | static_cast<std::uint32_t>(block[4 * i + 3]);
        }
        for (int i = 16; i < 64; ++i)
        {
            const std::uint32_t s0 = rotate(w[i - 15], 7) ^ rotate(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotate(w[i - 2], 17) ^ rotate(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i]                   = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        std::uint32_t e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i)
        {
            const std::uint32_t s1    = rotate(e, 6) ^ rotate(e, 11) ^ rotate(e, 25);
            const std::uint32_t ch    = (e & f) ^ (~e & g);
            const std::uint32_t temp1 = h + s1 + ch + kRound[i] + w[i];
            const std::uint32_t s0    = rotate(a, 2) ^ rotate(a, 13) ^ rotate(a, 22);
            const std::uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t temp2 = s0 + maj;
            h                         = g;
            g                         = f;
            f                         = e;
            e                         = d + temp1;
            d                         = c;
            c                         = b;
            b                         = a;
            a                         = temp1 + temp2;
        }
        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
        state[5] += f;
        state[6] += g;
        state[7] += h;
    };

    std::size_t whole = bytes.size() / 64 * 64;
    for (std::size_t i = 0; i < whole; i += 64)
    {
        compress(bytes.data() + i);
    }
    // The tail, the 1 bit, the zeros and the length: one block, or two when the tail leaves no room for the length.
    std::uint8_t      tail[128] = {};
    const std::size_t rest      = bytes.size() - whole;
    for (std::size_t i = 0; i < rest; ++i)
    {
        tail[i] = bytes[whole + i];
    }
    tail[rest]                 = 0x80;
    const std::size_t   padded = rest + 1 + 8 <= 64 ? 64 : 128;
    const std::uint64_t bits   = static_cast<std::uint64_t>(bytes.size()) * 8u;
    for (int i = 0; i < 8; ++i)
    {
        tail[padded - 1 - static_cast<std::size_t>(i)] = static_cast<std::uint8_t>(bits >> (8 * i));
    }
    compress(tail);
    if (padded == 128)
    {
        compress(tail + 64);
    }

    Sha256Digest digest{};
    for (int i = 0; i < 8; ++i)
    {
        for (int j = 0; j < 4; ++j)
        {
            digest[static_cast<std::size_t>(4 * i + j)] = static_cast<std::uint8_t>(state[i] >> (24 - 8 * j));
        }
    }
    return digest;
}

/** SHA-256 of text's bytes.
    @param text  the message
    @return      its digest */
inline Sha256Digest Sha256(std::string_view text)
{
    return Sha256(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(text.data()), text.size()));
}

/** `bytes` in base64url with no padding (RFC 4648 section 5, as RFC 7636 asks of a PKCE challenge and as a verifier is
    spelt): the core's standard base-64 with '+' as '-', '/' as '_' and the '=' dropped.
    @param bytes  the bytes
    @return       the text, every character unreserved in a URL */
inline std::string Base64Url(std::span<const std::uint8_t> bytes)
{
    std::string text = core::json::EncodeBase64(bytes);
    while (!text.empty() && text.back() == '=')
    {
        text.pop_back();
    }
    for (char &c : text)
    {
        c = c == '+' ? '-' : c == '/' ? '_' : c;
    }
    return text;
}

/** `bytes.size()` bytes from the operating system's cryptographic generator - arc4random on macOS, getrandom on
    Linux, BCryptGenRandom on Windows - for a PKCE verifier and an OAuth state, which must be unguessable. Defined per
    platform with the transport (net/mac, win, linux).
    @param bytes  filled whole */
void FillRandom(std::span<std::uint8_t> bytes);
} // namespace reckless::net
