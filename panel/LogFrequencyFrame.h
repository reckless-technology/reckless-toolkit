#pragma once

#include <algorithm>
#include <cmath>
#include <string>

#include "Canvas.h"
#include "Color.h"
#include "Geometry.h"
#include "LookMetrics.h"

namespace reckless::panel
{
/**
    The frame of a response plotted against log frequency, shared by the EQ's graph and the Tone's Bode plot: the room
    kept round the plot for its scales, where a frequency lands across it, and the screen under it - a dark screen let
    into the plate with a rim, a line at every multiple of a decade's step with the 1, 2 and 5 multiples heavier, and
    the round numbers figured under the plot. A value a view holds; its level scales, its curves and its figures at the
    sides stay its own, drawn after DrawScreen in the small print it leaves set.
*/
struct LogFrequencyFrame
{
    double min_hz;        ///< the plot's left edge
    double max_hz;        ///< its right edge
    int    left_scale;    ///< the room for the figures at the left
    int    right_scale;   ///< and at the right
    int    bottom_scale;  ///< the frequency figures underneath
    int    top_inset;     ///< a little air above
    int    corner_radius; ///< the screen's corners

    /** The plot inside the scales, in a view of `local` bounds.
        @param local  the view's local bounds, in design pixels
        @return       the plot's rectangle: the bounds less the scales and the inset */
    gfx::RectF PlotBounds(gfx::RectI local) const noexcept
    {
        return local.ToFloat()
            .WithTrimmedLeft(static_cast<float>(left_scale))
            .WithTrimmedRight(static_cast<float>(right_scale))
            .WithTrimmedBottom(static_cast<float>(bottom_scale))
            .WithTrimmedTop(static_cast<float>(top_inset));
    }

    /** Where `hz` lands across `plot`, a decade the same width wherever it falls.
        @param plot  the plot's rectangle, from PlotBounds
        @param hz    the frequency, in hertz; floored a hair above zero
        @return      the horizontal position, in the plot's coordinates; off the plot outside min_hz to max_hz */
    float XOf(gfx::RectF plot, double hz) const noexcept
    {
        const double span = std::log(max_hz / min_hz);
        return plot.x() + plot.width() * static_cast<float>(std::log(std::max(hz, 1.0e-9) / min_hz) / span);
    }

    /** The screen over `bounds` in `screen` with its rim, the frequency grid across `plot` and its figures in `ink`;
        the canvas left on the series' small print for the view's own figures.
        @param c       the canvas, in the view's local pixels
        @param bounds  the screen's rectangle, the view's whole bounds
        @param plot    the plot's rectangle, from PlotBounds
        @param ink     the colour of the grid, the rim and the figures
        @param screen  the screen's colour */
    void DrawScreen(gfx::Canvas &c, gfx::RectF bounds, gfx::RectF plot, gfx::Color ink, gfx::Color screen) const
    {
        c.SetColor(screen.WithAlpha(0.9f));
        c.FillRoundedRect(bounds, static_cast<float>(corner_radius));
        c.SetColor(ink.WithAlpha(0.25f));
        c.DrawRoundedRect(bounds, static_cast<float>(corner_radius), 1.0f);

        c.SetFont(gfx::Font(look::kSmallPrintFontSize * look::kTextScale));

        for (double decade = 10.0; decade <= max_hz; decade *= 10.0)
        {
            for (int m = 1; m <= 9; ++m)
            {
                const double hz = decade * m;
                if (hz < min_hz || hz > max_hz)
                {
                    continue;
                }
                const bool major = m == 1 || m == 2 || m == 5;
                c.SetColor(ink.WithAlpha(major ? 0.16f : 0.07f));
                c.DrawVerticalLine(gfx::RoundToInt(XOf(plot, hz)), plot.y(), plot.bottom());
            }
        }
        constexpr double kHzLabels[] = {20.0, 50.0, 100.0, 200.0, 500.0, 1000.0, 2000.0, 5000.0, 10000.0, 20000.0};
        for (const double hz : kHzLabels)
        {
            if (hz < min_hz || hz > max_hz)
            {
                continue;
            }
            c.SetColor(ink.WithAlpha(0.55f));
            c.DrawText(HzLabel(hz), gfx::RectF(30.0f, 14.0f).WithCentre({XOf(plot, hz), bounds.bottom() - 8.0f}),
                       gfx::Justify::kCentred);
        }
    }

    /** A frequency figure: whole hertz, or whole kilohertz with a k from 1000 up.
        @param hz  the frequency, in hertz
        @return    the figure, such as "500" or "2k" */
    static std::string HzLabel(double hz)
    {
        return hz >= 1000.0 ? std::to_string(gfx::RoundToInt(hz / 1000.0)) + "k" : std::to_string(gfx::RoundToInt(hz));
    }

    /** A level figure in whole decibels, a plus on a boost.
        @param db  the level, in decibels
        @return    the figure, such as "+6", "0" or "-12" */
    static std::string DbLabel(double db)
    {
        return db > 0.0 ? "+" + std::to_string(gfx::RoundToInt(db)) : std::to_string(gfx::RoundToInt(db));
    }
};
} // namespace reckless::panel
