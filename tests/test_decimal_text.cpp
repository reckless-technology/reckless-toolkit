// The suite's decimal reading (core/DecimalText.h): the one grammar every platform holds a number's text to,
// the nearest double read back from it bit for bit - every shortest text std::to_chars writes reads back to the double
// it was written from - and the same reading under a decimal-comma locale, which a host may set and which plain strtod
// would follow, reading "0.5" as 0.
#include <bit>
#include <charconv>
#include <clocale>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <random>
#include <string>
#include <string_view>

#include "DecimalText.h"
#include "Json.h"
#include "ParameterText.h"

#include <gtest/gtest.h>

namespace
{
using reckless::core::DecimalLength;
using reckless::core::ReadDecimal;

/** `value` as the shortest text that reads back to it, whatever the locale: what the tables print.
    @param value  the number
    @return       its text */
std::string Shortest(double value)
{
    char       text[64];
    const auto written = std::to_chars(text, text + sizeof(text), value);
    return std::string(text, written.ptr);
}

/** A locale whose decimal point is a comma, set as the process's numeric locale for the scope and put back after it;
    none where the machine has none of the names tried. */
class CommaLocale
{
  public:
    CommaLocale()
    {
        const char *current = std::setlocale(LC_NUMERIC, nullptr);
        saved_              = current != nullptr ? current : "C";
        for (const char *name : {"de_DE.UTF-8", "de_DE.utf8", "de_DE", "fr_FR.UTF-8", "fr_FR.utf8", "de-DE", "German"})
        {
            if (std::setlocale(LC_NUMERIC, name) != nullptr && std::localeconv()->decimal_point[0] == ',')
            {
                name_ = name;
                return;
            }
        }
        std::setlocale(LC_NUMERIC, saved_.c_str());
    }

    ~CommaLocale()
    {
        std::setlocale(LC_NUMERIC, saved_.c_str());
    }

    CommaLocale(const CommaLocale &)            = delete;
    CommaLocale &operator=(const CommaLocale &) = delete;

    bool IsSet() const
    {
        return !name_.empty();
    }

    std::string Name() const
    {
        return name_;
    }

  private:
    std::string saved_;
    std::string name_;
};
} // namespace

TEST(DecimalText, TheGrammarIsTheSuitesOnEveryPlatform)
{
    struct Case
    {
        std::string_view text;
        std::size_t      length;
    };

    const Case cases[] = {
        {"0", 1},    {"-0", 2},    {"12.5", 4}, {"5.", 2},    {".5", 2},   {"-.5", 3},  {"1e5", 3},
        {"1E-5", 4}, {"1e+05", 5}, {"1e", 1},   {"1e+", 1},   {"2.5k", 3}, {"35 %", 2}, {"6.5e3Hz", 5},
        {"0x10", 1}, {"", 0},      {"-", 0},    {".", 0},     {"-.", 0},   {"+1", 0},   {" 1", 0},
        {"inf", 0},  {"nan", 0},   {"e5", 0},   {"1.2.3", 3}, {"--5", 0},
    };
    for (const Case &c : cases)
    {
        EXPECT_EQ(DecimalLength(c.text), c.length) << "\"" << c.text << "\"";
    }
}

TEST(DecimalText, ReadsOneNumberWholeAndNothingElse)
{
    EXPECT_EQ(ReadDecimal("0.1"), 0.1);
    EXPECT_EQ(ReadDecimal("-2.5e-3"), -0.0025);
    EXPECT_EQ(ReadDecimal("5."), 5.0);
    EXPECT_EQ(ReadDecimal(".5"), 0.5);
    EXPECT_EQ(ReadDecimal("1e308"), 1e308);
    EXPECT_EQ(ReadDecimal("4.9406564584124654e-324"), std::numeric_limits<double>::denorm_min());
    const std::optional<double> negative_zero = ReadDecimal("-0");
    ASSERT_TRUE(negative_zero.has_value());
    EXPECT_TRUE(*negative_zero == 0.0 && std::signbit(*negative_zero)) << "-0 keeps its sign";

    for (const std::string_view refused :
         {"", " 1", "1 ", "+1", "1e", "0x10", "inf", "nan", "1,5", "abc", "1e309", "-1e309", "1e-400"})
    {
        EXPECT_FALSE(ReadDecimal(refused).has_value()) << "\"" << refused << "\" is not one number in range";
    }
    EXPECT_FALSE(ReadDecimal(std::string(reckless::core::kLongestDecimal + 1, '1')).has_value())
        << "past the longest text read";
    const std::string longest = "0." + std::string(reckless::core::kLongestDecimal - 2, '1');
    EXPECT_EQ(ReadDecimal(longest), 1.0 / 9.0) << "the longest text is read";
}

TEST(DecimalText, EveryShortestTextReadsBackToItsDouble)
{
    // Doubles across the whole normal range, each written as the shortest text that names it and read back: the
    // reading must be the nearest double, so every one comes back bit for bit.
    std::mt19937_64                        engine(20261013);
    std::uniform_int_distribution<int>     exponent(-1021, 1023);
    std::uniform_real_distribution<double> mantissa(1.0, 2.0);
    constexpr int                          kCount = 200000;
    int                                    exact  = 0;
    for (int n = 0; n < kCount; ++n)
    {
        const double value = std::ldexp(mantissa(engine), exponent(engine)) * ((n & 1) != 0 ? -1.0 : 1.0);
        char         text[64];
        const auto   written = std::to_chars(text, text + sizeof(text), value);
        const auto   read    = ReadDecimal(std::string_view(text, static_cast<std::size_t>(written.ptr - text)));
        if (read.has_value() && std::bit_cast<std::uint64_t>(*read) == std::bit_cast<std::uint64_t>(value))
        {
            ++exact;
        }
        else if (exact + 5 > n) // name the first few that miss, not every one
        {
            ADD_FAILURE() << std::string(text, written.ptr) << " did not read back to itself";
        }
    }
    std::printf("shortest texts read back bit for bit: %d of %d\n", exact, kCount);
    EXPECT_EQ(exact, kCount);
}

TEST(DecimalText, ACommaLocaleChangesNothing)
{
    const CommaLocale comma;
    if (!comma.IsSet())
    {
        GTEST_SKIP() << "this machine has no decimal-comma locale to try";
    }
    const double plain = std::strtod("0.5", nullptr);
    std::printf("under %s, plain strtod reads \"0.5\" as %s; the suite reads it as %s\n", comma.Name().c_str(),
                Shortest(plain).c_str(), Shortest(ReadDecimal("0.5").value_or(-1.0)).c_str());
    EXPECT_EQ(ReadDecimal("0.5"), 0.5);
    EXPECT_EQ(ReadDecimal("1,5"), std::nullopt) << "a comma is never a decimal point";
    EXPECT_EQ(reckless::core::NumberAtFront("6.5k"), 6.5) << "a knob's typed text";
    const auto parsed = reckless::core::json::Parse("[0.25, -1.5e2]");
    ASSERT_TRUE(parsed.IsOk()) << parsed.error;
    EXPECT_EQ(reckless::core::json::Write(parsed.value),
              reckless::core::json::Write(reckless::core::json::Parse("[0.25, -150.0]").value))
        << "a session's numbers";
}
