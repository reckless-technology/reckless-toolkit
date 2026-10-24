#pragma once

#include <cstdlib>

#include <gtest/gtest.h>

/**
    Test-only: what a suite does differently when it is built with the sanitizers (`-fsanitize=address,undefined,vptr`,
    the Mac's check before CI - the skill's testing.md, "Threads, sanitizers and images in a suite").

    Under the sanitizers every sample costs some twenty-five times what it costs in a release build, and most of a
    suite's time is a few long measurements - a response, a level or a table taken at every speed, rate or setting -
    which run the very paths the quick tests run, only longer and at more settings, and so find no memory error those
    do not. Such a test opens with RECKLESS_SKIP_MEASUREMENT_UNDER_SANITIZERS(): under the sanitizers it is reported
    skipped, everywhere else it runs as it always has. A test that drives a path at an extreme, reads damaged input,
    crosses threads or indexes buffers in a way of its own is the sanitizers' business and is never marked, however
    long it runs. Setting RECKLESS_RUN_MEASUREMENTS in the environment runs the measurements under the sanitizers too.
*/
namespace reckless::test
{
#if defined(__SANITIZE_ADDRESS__)
inline constexpr bool kBuiltWithSanitizers = true; ///< gcc's and MSVC's word for an AddressSanitizer build
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
inline constexpr bool kBuiltWithSanitizers = true; ///< clang's
#else
inline constexpr bool kBuiltWithSanitizers = false; ///< a build with no sanitizer
#endif
#else
inline constexpr bool kBuiltWithSanitizers = false; ///< a compiler that says neither
#endif

/** Whether a long measurement is skipped in this run: built with the sanitizers, and RECKLESS_RUN_MEASUREMENTS not
    set.
    @return  true to skip it */
inline bool SkipsMeasurements()
{
    // MSVC deprecates getenv for _dupenv_s, whose copy a test has no use for.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4996)
#endif
    return kBuiltWithSanitizers && std::getenv("RECKLESS_RUN_MEASUREMENTS") == nullptr;
#if defined(_MSC_VER)
#pragma warning(pop)
#endif
}
} // namespace reckless::test

/** The first line of a long measurement's test body: reported skipped under the sanitizers (reckless::test::
    SkipsMeasurements), run everywhere else. A macro because GTEST_SKIP returns from the test body it is written in. */
#define RECKLESS_SKIP_MEASUREMENT_UNDER_SANITIZERS()                                                 \
    if (::reckless::test::SkipsMeasurements())                                                       \
    GTEST_SKIP() << "a long measurement of paths the quick tests run, skipped under the sanitizers " \
                    "(RECKLESS_RUN_MEASUREMENTS runs it)"
