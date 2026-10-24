#pragma once

#include "Color.h"
#include "LookMetrics.h"
#include "LookPalette.h"
#include "Text.h"
#include "Tooltip.h"

namespace reckless::app
{
/**
    An app's colours, the few every window of a desktop app on the toolkit draws its frames, text, controls and states
    in: the window's colour, the rules and frames, the near-black of its plates and screens and the plate under the
    pointer, the ink and its two fainter shades, the accent and the accent at rest, and the colours of a good and a bad
    state. A product gives its own (IR Capture the Reckless Cab's dark green, Amphitheater its charcoal); everything in
    AppWidgets.h and AppRoot.h is drawn in it, and Palette() makes the series' palette the toolkit's own views - its
    plates, buttons, menus, lists and text views (panel/PanelStyle.h, panel/ListStyles.h) - are dressed in.
*/
struct Theme
{
    gfx::Color panel;      ///< the window's colour, behind everything
    gfx::Color track;      ///< the rules, the frames round a section, a slider's groove
    gfx::Color plate;      ///< the near-black of the legend plates, the screens and the plots
    gfx::Color plate_lit;  ///< a plate under the pointer
    gfx::Color ink;        ///< the text and the controls' marks
    gfx::Color ink_dim;    ///< secondary text: a caption, an axis's figures
    gfx::Color ink_faint;  ///< a hint, a placeholder, a disabled control
    gfx::Color accent;     ///< a lit edge, a section's title, a slider's handle
    gfx::Color accent_dim; ///< the accent at rest: a slider's filled groove
    gfx::Color good;       ///< a good state: a level in its window, a result that passed
    gfx::Color bad;        ///< a bad state: a clip, a failure

    /** The series' palette in the theme's colours: the window's colour as the panel, the ink, the accent, the plate,
        and the accent at half strength round the plates, as look::Palette::Series puts it.
        @return  the palette the toolkit's views are dressed in */
    look::Palette Palette() const noexcept
    {
        return {panel, ink, accent, plate, accent.WithAlpha(0.5f)};
    }

    /** The series' tooltip in the theme: the window's colour a shade lighter, the accent at half strength round it,
        the ink.
        @return  the tooltip's style */
    view::TooltipStyle Tooltip() const
    {
        view::TooltipStyle style;
        style.background = panel.Brighter(0.08f);
        style.outline    = accent.WithAlpha(0.5f);
        style.text       = ink;
        return style;
    }
};

/** An app's fonts: the series' serif at the series' sizes, through look::kTextScale as a panel's are. */
namespace fonts
{
/** @return  a section's title: the caption size, bold */
inline gfx::Font Title()
{
    return gfx::Font(look::kCaptionFontSize * look::kTextScale).Bold();
}

/** @return  a control's caption beside it */
inline gfx::Font Caption()
{
    return gfx::Font(12.0f * look::kTextScale);
}

/** @return  the small print: hints and scales */
inline gfx::Font Small()
{
    return gfx::Font(look::kSmallPrintFontSize * look::kTextScale);
}

/** @return  text to be read at a glance, a status or a log: the small print half again as large */
inline gfx::Font Reading()
{
    return gfx::Font(1.5f * look::kSmallPrintFontSize * look::kTextScale);
}
} // namespace fonts
} // namespace reckless::app
