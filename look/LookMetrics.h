#pragma once

#include "Color.h"

namespace reckless::look
{
/** The series' colours as the canvas takes them: the cream every plugin prints in, the amber that lights a legend
    plate's edge and a header lamp, and the chrome rim round a lens - an LED's (DrawLedCell), the Comp's clip jewel's,
    the Gate's state lamp's. One number each. */
namespace colors
{
inline constexpr gfx::Color kInk{0xffe8dcc8};     ///< the series' cream ink.
inline constexpr gfx::Color kAmber{0xffe0a038};   ///< the amber of a lit edge and a header lamp.
inline constexpr gfx::Color kLensRim{0xff7d7a75}; ///< the chrome rim round a lens.

/** A knob's value box being edited, as the series has always drawn it: the dark scheme of JUCE's fourth look, never
    recoloured there - white text, the box's transparent background made opaque, so black, the selection in the scheme's
    blue at 40 % (half that again while the window does not hold the keyboard) under white, the caret in the blue and
    the focused outline in the scheme's grey. */
inline constexpr gfx::Color kEditorText{0xffffffff};
inline constexpr gfx::Color kEditorBackground{0xff000000};      ///< the edited box's opaque black background.
inline constexpr gfx::Color kEditorHighlight{0x6642a2c8};       ///< the selection's blue, at 40 %.
inline constexpr gfx::Color kEditorHighlightedText{0xffffffff}; ///< the selected text, white.
inline constexpr gfx::Color kEditorCaret{0xff42a2c8};           ///< the caret's blue.
inline constexpr gfx::Color kEditorFocusedOutline{0xff8e989b};  ///< the focused box's grey outline.
} // namespace colors

/** The layout metrics every series panel is built on (gui.md's table): the knob and the slider block that holds
    it with its value box, the caption row that sits directly on it, the legend plate, the header, the strip of
    plates under the knobs, the margins inside the face plate and the lamp's strip at the header's right end. The
    Cab's Cabinet Maker drawer has its own where it differs. */
inline constexpr int kKnobSize     = 106;            ///< the chicken head's cell, in design pixels
inline constexpr int kKnobBlock    = kKnobSize + 16; ///< the slider: knob and value box, in pixels
inline constexpr int kKnobWidth    = kKnobSize + 14; ///< the slider's width, in pixels; under its cell less 8
inline constexpr int kLabelHeight  = 18;             ///< a caption over its knob or plate, in pixels
inline constexpr int kPlateHeight  = 28;             ///< a legend plate, in pixels
inline constexpr int kHeaderHeight = 58;             ///< the header, in pixels; the seam is at 2x this
inline constexpr int kSwitchStrip  = 46;             ///< under the knobs: captions and plates, in pixels

/** The cream frame round a knob with its caption and box (view::FrameAround): kFrameInset in from its cell's sides,
    kFrameOverhang out past the caption's top and the box's bottom. */
inline constexpr int kFrameInset    = 2;
inline constexpr int kFrameOverhang = 5; ///< out past the caption's top and the box's bottom
/** The small chicken head: one size wherever a cell is small - the Cab's two drawers, the Space Craft, the
    Pedalboard's tuner and its pedals' floor knobs - across its frame (the nose's sweep and the shadow's room), at 1x,
    whatever the cell's whole pixels come to (panel/'s SeriesPanel::SetKnobSide, or KnobStrip::Draw at this
    side). Sized by their cells they came out 56 to 59 px: a column 2 px short or a pixel narrower from an integer
    division moved a knob by a pixel or three. */
inline constexpr float kSmallKnobSide = 60.0f;
/** The screen a reverb's waterfall takes beside its knobs (the waterfall view is the plugins' own), the two knob
    rows' height by this: near square, so a second of slices climbs rather than crawls and the levels have room to
    stand; a low ribbon under the knobs reads as the wrong shape. */
inline constexpr int kWaterfallWidth = 340;
/** The gap between the knobs' cells and the waterfall's screen beside them, the Oldsverb's and the Space's. */
inline constexpr int kWaterfallGap = 24;
// The body's margins inside the plate. The face plates' corner screws sit 16 px in from each corner, stainless steel
// torx heads 32 px square at 1x with a radius of 12.48 (face_plate.py: the 64 px render torx/torx_bolt_64.png laid on
// the 2x plate as it is), so they reach 28.5 px in from the edges and their shadows 31 on the far side; a frame's edge
// (FrameAround: the cell less 2, less 0.6) at 32 + 2.6 clears them, where a margin of 24 would run a bank's bottom
// corners over the bottom screws, obscuring one or the other. The vertical margin is smaller: a frame's edge
// (FrameAround: 5 out, 0.6 back) reaches 4.4 px past its group, 5.6 px from the plate's edge - within the screws'
// 28.5, but clear of them once it is right of them. Nothing else may sit in a screw's corner either: a header without
// a lamp keeps its PRESETS plate kHeaderInset in from the right, as the mark is from the left, and a lamp's glow stays
// 5.5 px clear of the right-hand screw.
inline constexpr int kMarginX   = 32; ///< the body's side margin in the plate, in pixels
inline constexpr int kMarginY   = 10; ///< the body's top and bottom margin, in pixels
inline constexpr int kLampStrip = 94; ///< the header's right end for a lamp, in pixels

// The lamp filament's times (LampGlow and LampFilament, LookBallistics.h): how fast a lamp lights toward what it reads
// and how fast it dims, in seconds - the Amp's, the Drive's, the Distortion's, the Limiter's and the Pedalboard's. A
// per-block peak read at the display's rate is a flicker; through these it is a glow.
inline constexpr double kLampRiseSeconds = 0.02; ///< a lamp's rise toward its reading, in seconds
inline constexpr double kLampFallSeconds = 0.08; ///< a lamp's fall, in seconds

/** How long the pointer rests on a control before its tooltip shows, in milliseconds: every window's one tooltip is
    timed by it (window/'s ShellView, view::TooltipTracker). */
inline constexpr int kTooltipDelayMs = 500;
/** An indicator's cell in the header, one width each with its caption under it: a clip indicator drawn as its
    tube (DrawTubeCell) or an LED's lens (DrawLedCell). The Amp and the Distortion draw theirs in the wider cells of
    their tube bays under the header (their kTubeCell, 96); this is the header cell's width, the Amp's gate LED's. */
inline constexpr int kLedCell = 60;

/** The gap every header keeps between a plate at its right end - PRESETS, the Amp's view plate - and what is right
    of it (the lamp, another plate, the corner screw). */
inline constexpr int kResetGap = 12;

/** The PRESETS plate every plugin carries in its header (SeriesPanel.h's PresetPlateView): wide enough to read a
    preset's name, but no wider than the narrowest header allows - the Distortion's 772 px holds its title and the mark
    with room to spare at this width against the 400 the title needs (gui.md's header budget) - so a longer name is
    fitted with an ellipsis rather than the plate made wider. kResetGap in from what is right of it. */
inline constexpr int kPresetPlateWidth = 96;
inline constexpr int kPresetReserve    = kPresetPlateWidth + kResetGap; ///< the header's reserve for PRESETS, in pixels

/** The settings' gear left of PRESETS in every header (SeriesPanel.h's SettingsPlateView): drawn bare, as large as a
    plate is high, with kResetGap between it and PRESETS. */
inline constexpr int kSettingsPlateSide = kPlateHeight;
inline constexpr int kSettingsReserve   = kSettingsPlateSide + kResetGap; ///< the header's reserve for the gear

/** The value box under a knob: wide enough for "1.00 kHz" in the serif, tall enough for the scaled serif. */
inline constexpr int kValueBoxWidth  = 62;
inline constexpr int kValueBoxHeight = 17; ///< the value box's height, in pixels

/** A caption's font before kTextScale; and how far a control a mode has no use for, or a knob a sync division
    overrides, fades (it stops taking the mouse too). */
inline constexpr float kCaptionFontSize = 12.1f;
inline constexpr float kDimmedAlpha     = 0.4f; ///< a dimmed control's opacity, 0 to 1

/** The value box's figures before kTextScale - a JUCE label's own 15 px, which the series never changed on a knob's
    box - and the menus' font before it, JUCE's popup menu size. */
inline constexpr float kValueBoxFontSize  = 15.0f;
inline constexpr float kPopupMenuFontSize = 17.0f; ///< a menu's font size before kTextScale

/** The series' small print, before kTextScale: the figures on a graph's axes and a status line
    (kSmallPrintFontSize - the waterfall's, the Cab's and the EQ's graphs', the Pedalboard's small labels), and
    the caption under an indicator or beside a meter's lane (kFinePrintFontSize - the header cells' captions,
    the Comp's CLIP, the Gate's meter). */
inline constexpr float kSmallPrintFontSize = 9.5f;
inline constexpr float kFinePrintFontSize  = 9.0f; ///< an indicator caption's size before kTextScale

// ---- The look's own sizes: the type, the header, the plates and buttons, the indicators, the knob ---------------

/** One lever for overall text size: every font the series draws is scaled by it. */
inline constexpr float kTextScale = 1.1f;

/** A header's title and the version stamp beside it (DrawTitle), before kTextScale, and the gap between them: the
    title in the series' title font on one row, the stamp kVersionGap after it in kVersionFontSize at 35 % of the
    ink, its baseline on the title's. */
inline constexpr float kTitleFontSize   = 27.95f;
inline constexpr float kVersionFontSize = 11.4f; ///< the version stamp's size before kTextScale
inline constexpr int   kVersionGap      = 12;    ///< between the title and the stamp, in pixels

/** The header (DrawHeader): everything kHeaderInset in from the window's left edge, clear of the corner screw baked
    into the face plate; the company mark in a kLogoWidth cell, then kLogoGap, then the title. kLogoInset holds the
    mark off the header's top and bottom edges: the mark is a roundel on transparency, so it is drawn as large as the
    header sensibly allows - at 48 px in a 58 px header it clears the rule and the plate's seam, and the cell it sits
    in is the wider for it, the gap after it narrower by as much, so the title stands where it always did. A circle
    gives the title more air than its cell's edge suggests - the roundel curves away from it. */
inline constexpr int kHeaderInset = 40;
inline constexpr int kLogoWidth   = 52; ///< the company mark's cell, in pixels
inline constexpr int kLogoGap     = 5;  ///< between the mark's cell and the title, in pixels
inline constexpr int kLogoInset   = 5;  ///< the mark's inset from the header's edges, in pixels

/** The legend plate's arrow (DrawLegendPlate): its centre kArrowInset in from the plate's right edge (the plate is
    the box less a pixel), kArrowHalfWidth to either side - so it reaches kArrowInset + kArrowHalfWidth + 1 in from the
    box's edge. A legend's text stops kLegendClearance short of it, and a legend wider than that room is squeezed into
    it, to no less than kLegendMinimumSqueeze of its width before it is curtailed with an ellipsis: a half, the
    narrowest a fitted text is meant to go, so that the widest legend in the series on its narrowest plate
    ("Exponential" on the Vibe's 108 px SHAPE plate, "M / S" on the Pedalboard's 64 px splitter) is squeezed and still
    read whole. */
inline constexpr int   kArrowInset           = 12;
inline constexpr int   kArrowHalfWidth       = 4;    ///< the arrow's half-width, in pixels
inline constexpr int   kLegendClearance      = 2;    ///< a legend's room short of the arrow, in pixels
inline constexpr float kLegendMinimumSqueeze = 0.5f; ///< the least share a legend is squeezed to

/** A toggle's lamp on its button plate (DrawPlateLamp): a lens kPlateLampDiameter across with its centre kArrowInset in
    from the plate's left edge - the arrow's place mirrored - so a plate that opens a menu carries an arrow at its right
    and a plate that stays on carries a lamp at its left, and the button's text keeps the same room from either. */
inline constexpr float kPlateLampDiameter = 7.0f;

/** The series' button (DrawButtonPlate): its caption kButtonFontSize bold before kTextScale, or six tenths of a short
    button's height, and its plate a shade brighter under the mouse (kPlateLitLift, Color::Brighter's amount) and
    brighter again while held (kPlatePressedLift). */
inline constexpr float kButtonFontSize   = 11.0f;
inline constexpr float kPlateLitLift     = 0.04f; ///< a plate's lift under the mouse, Color::Brighter
inline constexpr float kPlatePressedLift = 0.15f; ///< a plate's lift while held, Color::Brighter

/** The indicators in a header cell or a tube bay (DrawLedCell, DrawTubeCell): an LED's lens kLensSize across; a tube
    standing on a floor kTubeHeight under the cell's top in a header cell and kBayTubeHeight in the tube bay under the
    Amp's header and the Distortion's - the EL34 the full height, the 12AX7, half its size in life, at kSmallTubeShare
    of it; and every caption row kIndicatorCaptionGap under the floor and kIndicatorCaptionHeight tall
    (DrawIndicatorCaption), so a header's LEDs and tubes caption on one line. */
inline constexpr float kLensSize               = 15.0f;
inline constexpr int   kTubeHeight             = 36;    ///< a tube in a header cell, in pixels
inline constexpr int   kBayTubeHeight          = 104;   ///< the EL34 in a tube bay, in pixels
inline constexpr float kSmallTubeShare         = 0.86f; ///< the 12AX7's height over the EL34's, 0 to 1
inline constexpr int   kIndicatorCaptionGap    = 3;     ///< floor to caption row, in pixels
inline constexpr int   kIndicatorCaptionHeight = 14;    ///< a caption row's height, in pixels

/** The chicken head's film strip, its geometry fixed by assets/knobs/render_knob.py: the frame is square and its
    half-width is kKnobMargin times the nose's reach - the room the shadow needs beyond the nose - so the nose sweeps a
    circle of 1 / kKnobMargin of the half-width; the frames follow at equal angles clockwise from twelve. */
inline constexpr float kKnobMargin = 1.4f;
} // namespace reckless::look
