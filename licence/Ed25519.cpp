// The one place the suite meets Monocypher: Ed25519's check (Ed25519.h). Its header is the fetched library's, a system
// header to this file, so the suite's warnings stay on the suite's code.
#include "Ed25519.h"

#include <monocypher-ed25519.h>

namespace reckless::licence
{
bool VerifyEd25519(const SignatureBytes &signature, const PublicKeyBytes &key, std::span<const std::uint8_t> message)
{
    return crypto_ed25519_check(signature.data(), key.data(), message.data(), message.size()) == 0;
}
} // namespace reckless::licence
