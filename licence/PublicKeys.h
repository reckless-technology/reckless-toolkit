#pragma once

#include <array>
#include <string_view>

#include "Licence.h"

namespace reckless::licence
{
// The keys a shipped build accepts licences from: the public halves of the licensing service's signing keys, each by
// the id a licence's `kid` names. A key is added when it is made and never taken out while a licence it signed is
// honoured, so a key can be retired from signing (rotated) without a licence it signed going dark. The private halves
// are the service's alone and are in no repository.
//
// None is issued yet: until the first is, no licence checks as valid in any build.

/** The licensing service's signing keys, by id. */
inline constexpr std::array<PublicKey, 0> kProductionKeys{};

/** The serials withdrawn - a refund, a charge-back, a licence replaced by another - which no build from this one on
    accepts, written from the service's records (serials alone: nothing that names a person). */
inline constexpr std::array<std::string_view, 0> kRevokedSerials{};
} // namespace reckless::licence
