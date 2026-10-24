#pragma once

#include "Color.h"
#include "LookMetrics.h"

namespace reckless::look
{
/** The colours a plugin's face is drawn in: the plate, the ink printed on it (the menus, the tooltips and the legend
    plates' text print in it too), the accent that lights a highlight or an edge, and the legend plate every switched
    setting and button is drawn as - a shade darker than the panel - with the edge drawn round it. */
struct Palette
{
    gfx::Color panel;      ///< the panel's colour, under the face plate.
    gfx::Color ink;        ///< the colour printed on the plate: captions, menus, tooltips, legend text.
    gfx::Color accent;     ///< the colour of a lit highlight or edge.
    gfx::Color plate;      ///< the legend plates' colour.
    gfx::Color plate_edge; ///< the edge drawn round a legend plate.

    /** The series' palette - the cream ink and the amber accent every plugin wears (colors::kInk, colors::kAmber),
        the amber at half strength round the plates - on this panel colour with these legend plates. `ink` is the
        series' cream unless the plugin gives its own: a plugin anodised light (the Delay's cream) prints in a dark
        ink instead, since cream on cream reads as nothing.
        @param panel  the plugin's panel colour
        @param plate  the plugin's legend plate colour
        @param ink    the colour printed on the plate; the series' cream by default
        @return       the palette, with the amber accent and the plate edge at half its strength */
    static Palette Series(gfx::Color panel, gfx::Color plate, gfx::Color ink = colors::kInk) noexcept
    {
        return {panel, ink, colors::kAmber, plate, colors::kAmber.WithAlpha(0.5f)};
    }
};
} // namespace reckless::look
