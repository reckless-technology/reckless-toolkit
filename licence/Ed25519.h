#pragma once

#include <array>
#include <cstdint>
#include <span>

namespace reckless::licence
{
/** An Ed25519 public key: 32 bytes, the encoded point (RFC 8032 section 5.1.5). */
using PublicKeyBytes = std::array<std::uint8_t, 32>;

/** An Ed25519 signature: 64 bytes, R and S (RFC 8032 section 5.1.6). */
using SignatureBytes = std::array<std::uint8_t, 64>;

/** Whether `signature` is `key`'s signature of `message` under Ed25519 as RFC 8032 defines it - the curve25519
    signature with SHA-512 every other implementation of the name makes and checks, so a signature made by any of them
    verifies here. Checked by Monocypher's crypto_ed25519_check, compiled once in reckless_monocypher (Ed25519.cpp).
    @param signature  the signature
    @param key        the signer's public key
    @param message    the bytes signed
    @return           true when the signature is the key's, over exactly these bytes */
bool VerifyEd25519(const SignatureBytes &signature, const PublicKeyBytes &key, std::span<const std::uint8_t> message);
} // namespace reckless::licence
