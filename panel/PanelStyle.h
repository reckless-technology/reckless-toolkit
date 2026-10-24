#pragma once

#include <algorithm>
#include <string_view>

#include "Button.h"
#include "Canvas.h"
#include "Dialog.h"
#include "Knob.h"
#include "Label.h"
#include "LookDrawing.h"
#include "LookMetrics.h"
#include "LookPalette.h"
#include "Menu.h"
#include "Switch.h"
#include "TextField.h"

namespace reckless::panel
{
/**
    The series' look for the views a panel is built from, in one place: the caption and the value box as JUCE's labels
    draw them, the field a value box opens as JUCE's text editor draws it, the plate and its legend as JUCE's combo box
    draws them, the button's plate and text as JUCE's text button draws them, and the menu and the dialog in the
    plugin's palette. Each is the look's numbers (look) and the palette's colours, the numbers the series' look
    gave JUCE's components in the releases built on JUCE.
*/

/** A label as the JUCE label leaves its room - a pixel above and below, five at each side - centred, in the ink on
    nothing, in `font`.
    @param palette  the plugin's palette; its ink colours the text
    @param font     the label's font
    @return         the style */
inline view::Label::Style LabelStyle(const look::Palette &palette, gfx::Font font)
{
    view::Label::Style style;
    style.font    = font;
    style.justify = gfx::Justify::kCentred;
    style.text    = palette.ink;
    return style;
}

/** The field a knob's value box opens: the JUCE editor as the series' boxes have it (look::colors' editor colours)
    - white text over the box's transparent background made opaque, the box's transparent outline, the selection
    and the caret in the editor's blue and the focused outline in its grey.
    @param font  the field's font
    @return      the style */
inline view::TextField::Style ValueEditorStyle(gfx::Font font)
{
    view::TextField::Style style;
    style.font             = font;
    style.text             = look::colors::kEditorText;
    style.background       = look::colors::kEditorBackground;
    style.outline          = gfx::Color{};
    style.focused_outline  = look::colors::kEditorFocusedOutline;
    style.highlight        = look::colors::kEditorHighlight;
    style.highlighted_text = look::colors::kEditorHighlightedText;
    style.caret            = look::colors::kEditorCaret;
    style.justify          = gfx::Justify::kTopLeft;
    return style;
}

/** The series' knob: the caption in the caption font, bold; the value box in the label's own font; both under the
    text scale, centred, in the ink; the box `box_width` wide, opening the series' editor.
    @param palette    the plugin's palette
    @param box_width  the value box's width, in design pixels
    @return           the style */
inline view::Knob::Style KnobStyle(const look::Palette &palette, int box_width = look::kValueBoxWidth)
{
    view::Knob::Style style;
    style.caption      = LabelStyle(palette, gfx::Font(look::kCaptionFontSize * look::kTextScale).Bold());
    style.value        = LabelStyle(palette, gfx::Font(look::kValueBoxFontSize * look::kTextScale));
    style.value.editor = ValueEditorStyle(style.value.font);
    style.box_width    = box_width;
    style.box_height   = look::kValueBoxHeight;
    style.dimmed_alpha = look::kDimmedAlpha;
    return style;
}

/** The series' menu: the palette's panel behind its ink, the row under the pointer lit in the accent at 35 %, the
    serif at the popup size under the text scale, rows 1.3 times its height.
    @param palette  the plugin's palette
    @return         the style */
inline view::MenuStyle MenuStyle(const look::Palette &palette)
{
    view::MenuStyle style;
    style.background       = palette.panel;
    style.border           = palette.ink.WithAlpha(0.6f);
    style.text             = palette.ink;
    style.highlight        = palette.accent.WithAlpha(0.35f);
    style.highlighted_text = palette.ink;
    style.disabled_text    = palette.ink.WithMultipliedAlpha(0.3f);
    style.font             = gfx::Font(look::kPopupMenuFontSize * look::kTextScale);
    style.row_height       = gfx::RoundToInt(style.font.height * 1.3f);
    style.separator_height = 10;
    style.tick_column      = style.row_height;
    style.arrow_column     = style.row_height;
    style.padding          = 10;
    return style;
}

/** The series' legend plate (look::DrawSwitchPlate in the palette's colours), darker while a press holds it.
    @param palette  the plugin's palette
    @return         the painter, which keeps a copy of the palette */
inline view::Switch::PlatePainter PlatePainter(const look::Palette &palette)
{
    return [palette](gfx::Canvas &c, gfx::RectI bounds, bool held) {
        look::DrawSwitchPlate(c, palette, bounds.width(), bounds.height(), held);
    };
}

/** A plate's menu, its caption (a knob's), and the legend it shows its choice in, as JUCE's combo box's label: inset
    clear of the arrow, a pixel of border, centred, squeezed to look::kLegendMinimumSqueeze before it is cut, in the ink
    on nothing, in the plate font - 0.85 of the plate's height held to 16 px - under the text scale twice, since the
    combo box's font is scaled once as the plate's and again as every label's is, for the series' plate height.
    @param palette  the plugin's palette
    @return         the style */
inline view::Switch::Style SwitchStyle(const look::Palette &palette)
{
    const float         height = std::min(16.0f, static_cast<float>(look::kPlateHeight) * 0.85f);
    view::Switch::Style style;
    style.menu                        = MenuStyle(palette);
    style.legend                      = LabelStyle(palette, gfx::Font(height * look::kTextScale * look::kTextScale));
    style.legend.border_top           = 1;
    style.legend.border_left          = 1;
    style.legend.border_bottom        = 1;
    style.legend.border_right         = 1;
    style.legend.min_horizontal_scale = look::kLegendMinimumSqueeze;
    style.caption      = LabelStyle(palette, gfx::Font(look::kCaptionFontSize * look::kTextScale).Bold());
    style.legend_inset = look::kArrowInset + look::kArrowHalfWidth + look::kLegendClearance;
    style.dimmed_alpha = look::kDimmedAlpha;
    return style;
}

namespace detail
{
/** The series' button's text, as JUCE's text button drew it: kButtonFontSize bold under the text scale, or six tenths
    of a short button's height, in the ink, fitted on up to two lines inside indents of at most four pixels above and
    below (three tenths of the height) and, at each side, the font's six tenths or two pixels and a quarter of the
    plate's shorter side - or, on a side that carries an arrow or a lamp, the room a legend keeps from the arrow
    (kArrowInset + kArrowHalfWidth + kLegendClearance); at half strength while the button is dimmed.
    @param c        the canvas, in the button's local coordinates
    @param palette  the plugin's palette
    @param bounds   the button's bounds, origin zero
    @param text     the button's text
    @param dimmed   true while the button is dimmed
    @param marked_left   true when a lamp stands at the plate's left
    @param marked_right  true when an arrow stands at the plate's right */
inline void DrawButtonText(gfx::Canvas &c, const look::Palette &palette, gfx::RectI bounds, std::string_view text,
                           bool dimmed, bool marked_left, bool marked_right)
{
    const int  height = bounds.height();
    const int  width  = bounds.width();
    const auto font =
        gfx::Font(std::min(look::kButtonFontSize * look::kTextScale, static_cast<float>(height) * 0.6f)).Bold();
    c.SetFont(font);
    c.SetColor(palette.ink.WithMultipliedAlpha(dimmed ? 0.5f : 1.0f));
    const int y_indent    = std::min(4, gfx::RoundToInt(0.3f * static_cast<float>(height)));
    const int corner      = std::min(height, width) / 2;
    const int font_height = gfx::RoundToInt(font.height * 0.6f);
    const int indent      = std::min(font_height, 2 + corner / 2);
    const int marked      = look::kArrowInset + look::kArrowHalfWidth + look::kLegendClearance;
    const int left        = marked_left ? marked : indent;
    const int right       = marked_right ? marked : indent;
    const int text_width  = width - left - right;
    if (text_width > 0)
    {
        c.DrawFittedText(text, {left, y_indent, text_width, height - 2 * y_indent}, gfx::Justify::kCentred, 2);
    }
}
} // namespace detail

/** The series' button as JUCE's text button draws it: the button plate (look::DrawButtonPlate) over the bounds, and its
    text in the button font (detail::DrawButtonText); a toggle - a button whose look carries the lamp - shows its state
    by a lamp at the plate's left (look::DrawPlateLamp), lit while on, and its text keeps clear of it.
    @param palette  the plugin's palette
    @return         the painter, which keeps a copy of the palette */
inline view::Button::Painter ButtonPainter(const look::Palette &palette)
{
    return [palette](gfx::Canvas &c, gfx::RectI bounds, std::string_view text, view::Button::Look state) {
        look::DrawButtonPlate(c, palette, bounds, state.on, state.over, state.held);
        if (state.lamp)
        {
            look::DrawPlateLamp(c, bounds, state.on);
        }
        detail::DrawButtonText(c, palette, bounds, text, state.dimmed, state.lamp, false);
    };
}

/** A plate that opens a menu - the PRESETS plate, the Amp's view, the NAM's MODEL: the series' button with the legend
    plate's arrow at its right (look::DrawPlateArrow, in the ink as a switch's), so it reads as the drop-down it is and
    not as a button that acts, and its text keeping clear of the arrow.
    @param palette  the plugin's palette
    @return         the painter, which keeps a copy of the palette */
inline view::Button::Painter MenuButtonPainter(const look::Palette &palette)
{
    return [palette](gfx::Canvas &c, gfx::RectI bounds, std::string_view text, view::Button::Look state) {
        look::DrawButtonPlate(c, palette, bounds, state.on, state.over, state.held);
        look::DrawPlateArrow(c, bounds, palette.ink.WithAlpha(state.dimmed ? 0.4f : 0.8f));
        detail::DrawButtonText(c, palette, bounds, text, state.dimmed, false, true);
    };
}

/** A question over the panel in the palette: its panel colour under the ink, the accent round the card, the series'
    buttons, and a line to type into in the editor's look.
    @param palette  the plugin's palette
    @return         the style */
inline view::Dialog::Style DialogStyle(const look::Palette &palette)
{
    view::Dialog::Style style;
    style.card           = palette.panel;
    style.outline        = palette.accent;
    style.text           = palette.ink;
    style.font           = gfx::Font(look::kPopupMenuFontSize);
    style.field          = ValueEditorStyle(gfx::Font(look::kValueBoxFontSize * look::kTextScale));
    style.field.outline  = look::colors::kEditorFocusedOutline;
    style.button_painter = ButtonPainter(palette);
    style.button_height  = look::kPlateHeight;
    return style;
}
} // namespace reckless::panel
