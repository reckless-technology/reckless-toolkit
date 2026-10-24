#pragma once

#include "Canvas.h"
#include "ListView.h"
#include "LookPalette.h"
#include "Text.h"
#include "TextView.h"

namespace reckless::panel
{
/**
    The series' look for the views that show many lines - a list of rows (view::ListView) and lines of text
    (view::TextView) - in a palette: on the plate's colour as the series' screens are, printed in the ink; a list's
    header on the panel's colour in the ink's bold, every other row striped and the columns parted by the ink at a
    fraction; the selection in the accent at the strength a menu lights its row, and the edge lit in the accent while
    the view has the keyboard. The font is the caller's - the serif, or a fixed-pitch face for figures that line up -
    and the rows' and the lines' heights follow it.
*/

/** A list in the palette, in `font`: rows one and a half times the font's height, the header two pixels taller.
    @param palette  the product's palette
    @param font     the cells' font; the header's is its bold
    @return         the style */
inline view::ListView::Style ListViewStyle(const look::Palette &palette, gfx::Font font)
{
    view::ListView::Style style;
    style.font              = font;
    style.header_font       = font.Bold();
    style.row_height        = gfx::RoundToInt(font.height * 1.5f);
    style.header_height     = style.row_height + 2;
    style.padding           = gfx::RoundToInt(font.height * 0.4f);
    style.background        = palette.plate;
    style.stripe            = palette.ink.WithAlpha(0.04f);
    style.text              = palette.ink;
    style.selection         = palette.accent.WithAlpha(0.35f);
    style.selected_text     = palette.ink;
    style.header_background = palette.panel;
    style.header_text       = palette.ink;
    style.grid              = palette.ink.WithAlpha(0.15f);
    style.focused_outline   = palette.accent.WithAlpha(0.6f);
    return style;
}

/** Lines of text in the palette, in `font`: on the plate's colour in the ink, lines 1.35 times the font's height, an
    outline in the ink at a fraction that the accent lights while the view has the keyboard, the selection in the
    accent as a list's, and the caret in the accent.
    @param palette  the product's palette
    @param font     the text's font
    @return         the style */
inline view::TextView::Style TextViewStyle(const look::Palette &palette, gfx::Font font)
{
    view::TextView::Style style;
    style.font             = font;
    style.line_height      = gfx::RoundToInt(font.height * 1.35f);
    style.padding          = gfx::RoundToInt(font.height * 0.5f);
    style.background       = palette.plate;
    style.text             = palette.ink;
    style.outline          = palette.ink.WithAlpha(0.15f);
    style.focused_outline  = palette.accent.WithAlpha(0.6f);
    style.highlight        = palette.accent.WithAlpha(0.35f);
    style.highlighted_text = palette.ink;
    style.caret            = palette.accent;
    return style;
}
} // namespace reckless::panel
