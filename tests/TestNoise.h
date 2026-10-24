#pragma once

#include <cstdint>

// Test-only: the one deterministic noise source every suite in the series drives its engine with. Not part of any
// plugin.
namespace reckless::test
{
/** Iterated LCG (glibc constants), full-scale broadband noise in [-1, 1). Its values are multiples of 2^-14, so a
    float cast of Next() is exact and a suite that works in float hears the same samples as one in double. Seed it
    by setting `state`; the default sequence starts from 1. */
struct NoiseLcg
{
    std::uint32_t state{1u}; ///< the generator's state: the seed before the first Next(), then the last word

    /** The next sample of the sequence.
        @return  a sample in [-1, 1), a multiple of 2^-14 */
    double Next() noexcept
    {
        state = state * 1103515245u + 12345u;
        return 2.0 * static_cast<double>((state >> 16) & 0x7fffu) / 32768.0 - 1.0;
    }
};
} // namespace reckless::test
