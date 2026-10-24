#pragma once

#include <cstddef>
#include <string_view>

#include "View.h"

#include <gtest/gtest.h>

/**
    Test-only: the series' tooltip rule (the skill's gui.md) on a panel of the suite's views - every control's tip at
    most kTooltipWordLimit words, plain language, what the control is and what its extremes do; the mechanism and the
    numbers are the user guide's. The words are counted as runs of anything but a space, a tab, a carriage return or a
    line feed ("100 Hz" is two words, a hyphenated word one). Every plugin's panel suite holds its panel to it through
    CheckViewTooltips.
*/
namespace reckless::test
{
inline constexpr int kTooltipWordLimit = 20; ///< the most words a tooltip may hold

/** The words of `tip`, as the rule counts them.
    @param tip  the tooltip's text
    @return     the number of runs of anything but a space, a tab, a carriage return or a line feed */
inline int TooltipWords(std::string_view tip)
{
    int  words   = 0;
    bool in_word = false;
    for (const char c : tip)
    {
        const bool space = c == ' ' || c == '\t' || c == '\r' || c == '\n';
        if (!space && !in_word)
        {
            ++words;
        }
        in_word = !space;
    }
    return words;
}

/** The rule on every view's tip under `view`.
    @param view  the root of the tree to check; every descendant's tooltip is held to kTooltipWordLimit */
inline void CheckViewTooltips(const reckless::view::View &view)
{
    if (!view.tooltip.empty())
    {
        EXPECT_LE(TooltipWords(view.tooltip), kTooltipWordLimit)
            << "a tooltip over " << kTooltipWordLimit << " words: \"" << view.tooltip << "\"";
    }
    for (const auto *child : view.children())
    {
        CheckViewTooltips(*child);
    }
}
} // namespace reckless::test
