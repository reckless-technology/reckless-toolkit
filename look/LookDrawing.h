#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "Color.h"
#include "Geometry.h"
#include "LookImages.h"
#include "LookMetrics.h"
#include "LookPalette.h"
#include "LookTubes.h"

namespace reckless::look
{
/**
    The series' look drawn on a canvas, with no toolkit under it: the chicken-head knob, the legend plate every switched
    setting and button is drawn as, the header with the company mark, the title and the version stamp, the header lamp,
    the LED and tube cells with their captions, and the frames round a bank of knobs. Every coordinate is a panel's
    design pixels; the two pictures the look caches - the knob's frames and the mark - are made at the physical pixels
    the canvas says they land on, through a toolkit's ImageBackend. The panels on the views (panel/) draw every
    piece of the look through here, so a panel drawn through any canvas is the same drawing.
*/

/**
    The chicken-head knob: a film strip rendered from a CAD model of the real thing (assets/knobs/render_knob.py
    - 128 frames, the count read from the strip, one per pointer angle clockwise from twelve, each lit by the same light
    from the panel's upper left and casting its own shadow), the frame nearest the knob's angle drawn. The light stays
    with the panel while the knob turns, which no single picture turned in code can do. The knob is every slider's in
    the series, drawn small where a cell is small, at the series' one small size (kSmallKnobSide). A drawn teardrop
    does not look like a chicken head, and neither does a photograph turned in code; the render does.
*/
class KnobStrip
{
  public:
    /** The film strip `strip` - one frame wide and as many frames tall as it holds - whose frames are made through
        `images`, which outlives this: a strip of its own, its picture and its frames shared with no other.
        @param images  the toolkit's image operations, which must outlive this strip
        @param strip   the film strip's picture; an invalid image gives a strip with no picture */
    KnobStrip(const ImageBackend &images, gfx::Image strip);

    /** The film strip a module carries as the encoded file `asset`, an embedded file whose bytes outlive every panel,
        decoded through `images`, which outlives this, and shared: every strip made from the same bytes while one of
        them lives draws from one decoded picture, and from one set of frames at a size more than one of them draws at,
        so a host's dozen panels or a board of pedals hold the series' strip (256 x 32768, 33.5 MB decoded) once, not
        once each. A module's panels decode through one toolkit, so the first strip's decode is every strip's. Safe on
        any thread; nothing is kept once the last strip made from the bytes goes. No bytes: a strip of its own with no
        picture.
        @param images  the toolkit's image operations, which must outlive every strip made from the bytes
        @param asset   the encoded strip file, whose bytes outlive every panel; empty for a strip with no picture
        @return        a strip sharing its decoded picture and frames with the others made from the same bytes */
    static KnobStrip Shared(const ImageBackend &images, std::span<const std::uint8_t> asset);

    /** The side a knob's frame is drawn at in a knob area `width` by `height` that gives it no size of its own: the
        nose's sweep plus the shadow's room, 2 kKnobMargin times half the area's shorter side less 4 px a side.
        @param width   the knob area's width, in design pixels
        @param height  the knob area's height, in design pixels
        @return        the frame's side, in design pixels */
    static float SideFor(int width, int height) noexcept;

    /** The frame nearest `angle` (radians clockwise from twelve), `side` design pixels across, centred in `area` less
        4 px a side - the knob area a slider hands its look.
        @param c      the canvas drawn on
        @param area   the knob area, in the canvas's design pixels
        @param angle  the pointer's angle, in radians clockwise from twelve
        @param side   the side the frame is drawn at, in design pixels */
    void Draw(gfx::Canvas &, gfx::RectI area, float angle, float side);

    /** The strip's frames rescaled to a side of `pixels` physical pixels, made once per size: a bilinear resample is
        a clean box filter at 2:1, so each frame halves while it is still at least 4x the size and takes the last step
        from there, between 2:1 and 4:1, where one resample straight from the strip's size would alias into a shimmer
        as the knob turns. A size is kept while this strip draws at the physical scale it was asked at, beside every
        other size drawn there (a panel's knobs and its small ones), and let go once Draw finds another scale: a zoom's
        sizes are made for it and dropped at the next, so a window dragged through a hundred sizes holds the frames of
        the last alone, not a hundred sets of 128 frames (154 MB a set at 549 px). The frames stay valid until then.
        Each strip is painted on one thread, so what it holds needs no lock; what strips share is locked.
        @param pixels  the frame's side, in physical pixels
        @return        every frame of the strip at that size, in angle order */
    const std::vector<gfx::Image> &FramesAtSize(int pixels);

    /** @return the decoded film strip's picture, which is invalid for a strip with none */
    const gfx::Image &strip() const noexcept;

  private:
    using Frames = std::vector<gfx::Image>; ///< the frames of the strip at one size, in angle order.

    /** The decoded strip and its frames at each size a strip holds, shared by the strips made from one asset. */
    struct Film;

    /** A strip on a film it shares with the others made from the same bytes.
        @param images  the toolkit's image operations, which must outlive this strip
        @param film    the decoded strip and its frames at each size */
    KnobStrip(const ImageBackend &images, std::shared_ptr<Film> film);

    const ImageBackend                                        &images_;      ///< the toolkit's image operations.
    std::shared_ptr<Film>                                      film_;        ///< the decoded strip, shared.
    float                                                      scale_{0.0f}; ///< the scale the sizes held are drawn at
    std::vector<std::pair<int, std::shared_ptr<const Frames>>> held_;        ///< each size drawn there, by its side
};

/**
    The company mark: the red roundel with the R knocked out in white, on transparency, drawn as it comes and never
    recoloured - the one thing on a panel that is not in the plugin's palette. Drawn straight from its 512 px asset, a
    header's 48 px mark is a tenth of it, and a software renderer's bilinear read steps the roundel's soft edge into a
    jagged rim; so the mark is shrunk by halving (ShrunkImage) to the physical pixels it is drawn at, and copied there
    on whole pixels.
*/
class CompanyMark
{
  public:
    /** The mark's picture `mark`, shrunk through `images`, which outlives this.
        @param images  the toolkit's image operations, which must outlive this mark
        @param mark    the mark's picture, the 512 px asset; an invalid image gives no mark */
    CompanyMark(const ImageBackend &images, gfx::Image mark);

    /** Whether there is a mark to draw: a header without one starts its title where the mark would have been.
        @return true when the mark's picture is valid */
    bool IsValid() const noexcept
    {
        return mark_.IsValid();
    }

    /** @return the mark's picture as given, unshrunk */
    const gfx::Image &image() const noexcept
    {
        return mark_;
    }

    /** The mark at `width` x `height` physical pixels, shrunk from the asset by halving. The last size asked for is
        kept - a header draws one mark, at one size while its zoom stands - and the one before it let go, so a window
        dragged through its sizes holds one mark, not one a size. The header is drawn from a const look, so what is kept
        is mutable; painting is one thread's, so it needs no lock.
        @param width   the mark's width, in physical pixels
        @param height  the mark's height, in physical pixels
        @return        the mark at that size; valid until the next call asks for another size */
    const gfx::Image &AtSize(int width, int height) const;

    /** The mark fitted inside `cell` and centred (CentredFit), at the physical pixels it covers there, on whole ones
        (OnPhysicalPixels), so the draw is a copy.
        @param c     the canvas drawn on
        @param cell  the cell the mark is fitted in, in the canvas's design pixels */
    void Draw(gfx::Canvas &, gfx::RectF cell) const;

  private:
    const ImageBackend                                               &images_;  ///< the toolkit's image operations.
    gfx::Image                                                        mark_;    ///< the mark's picture as given.
    mutable std::optional<std::pair<std::pair<int, int>, gfx::Image>> at_size_; ///< the last size, and the mark at it
};

/** A rectangle's drop shadow, the one every legend plate stands on: the shadow's colour solid over `target` shrunk by
    half the radius and moved by `offset`, and round it, a radius and a half wide, eight pieces filled with one ramp -
    the four corners radial from the solid's corners, the four edges straight out from its sides - that keeps i^2 of
    the colour's alpha a share 1 - i of the way out, for i from 0.05 to 0.95 in tenths.
    @param target  the rectangle the plate stands on, in the canvas's pixels
    @param colour  the shadow's colour, solid under the plate
    @param radius  the blur radius, in pixels
    @param offset  where the shadow sits against the plate, in pixels */
void FillDropShadow(gfx::Canvas &, gfx::RectI target, gfx::Color colour, int radius, gfx::PointI offset);

/** A dark legend plate screwed to the panel, like the model plate on an amp head, `width` by `height` from the
    canvas's origin: a drop shadow, the plate (`pressed_plate` while pressed), an edge, a small arrow at the right for
    the choice printed over it (kArrowInset, kArrowHalfWidth). The drawing is the series'; the four colours are the
    caller's - DrawSwitchPlate's are the palette's.
    @param width           the plate's width, in design pixels
    @param height          the plate's height, in design pixels
    @param is_button_down  true while the plate is pressed
    @param plate           the plate's colour
    @param pressed_plate   the plate's colour while pressed
    @param edge            the edge's colour
    @param arrow           the arrow's colour */
void DrawLegendPlate(gfx::Canvas &, int width, int height, bool is_button_down, gfx::Color plate,
                     gfx::Color pressed_plate, gfx::Color edge, gfx::Color arrow);

/** Every switched setting in the series is a legend plate in its palette's colours: the plate, darker while pressed,
    its edge, the arrow in the ink. One drawing, a colouring per plugin: the same drop-downs across the suite, a
    two-way setting included - a toggle reads as a lever, a three-way choice never does.
    @param width           the plate's width, in design pixels
    @param height          the plate's height, in design pixels
    @param is_button_down  true while the plate is pressed */
void DrawSwitchPlate(gfx::Canvas &, const Palette &, int width, int height, bool is_button_down);

/** A button is a legend plate too, without the switch's shadow - the Cab's drawer buttons, the Comp's LISTEN, the
    PRESETS plate: the plate `bounds` less a pixel, a shade lighter under the mouse (kPlateLitLift) and lighter again
    while held (kPlatePressedLift), its edge coming up when lit and further while the button is `on` (toggled).
    @param bounds          the button's bounds, in the canvas's design pixels
    @param on              true while the button is toggled on
    @param is_mouse_over   true while the pointer is over the button
    @param is_button_down  true while the button is pressed */
void DrawButtonPlate(gfx::Canvas &, const Palette &, gfx::RectI bounds, bool on, bool is_mouse_over,
                     bool is_button_down);

/** A menu's arrow on a button plate - a plate that opens a menu, the PRESETS plate among them - the legend plate's
    own arrow (DrawLegendPlate), so every plate that opens a menu reads as one: its centre kArrowInset in from the
    right of `bounds` less a pixel, kArrowHalfWidth to either side, on the plate's centre line.
    @param bounds  the button's bounds, in the canvas's design pixels
    @param arrow   the arrow's colour */
void DrawPlateArrow(gfx::Canvas &, gfx::RectI bounds, gfx::Color arrow);

/** A toggle's lamp on a button plate - the Cab's CABINET MAKER, the EQ's analyser, the board's AUTO MUTE: an
    indicator's lens (DrawLedCell's dark glass in its chrome rim) kPlateLampDiameter across, its centre kArrowInset in
    from the left of `bounds` less a pixel and on the plate's centre line; lit, the lens amber with its glow on the
    plate round it, so a toggle that is on is seen to be on across the panel and not by its edge alone.
    @param bounds  the button's bounds, in the canvas's design pixels
    @param lit     true while the toggle is on */
void DrawPlateLamp(gfx::Canvas &, gfx::RectI bounds, bool lit);

/** A settings gear, the one icon the series draws: eight square-shouldered teeth round a ring with a bore at its
    centre, the bore wound against the teeth so the non-zero rule leaves it open and the plate under it shows, filled
    in `colour` and centred in `area` at the size of its shorter side.
    @param area    the square the gear fills, in the canvas's design pixels
    @param colour  the gear's colour */
void DrawGear(gfx::Canvas &, gfx::RectF area, gfx::Color colour);

/** A header's title in one placement for the series: the title in the series' title font (kTitleFontSize bold) on
    one row, centred on the header's height and flush with the left of `header_content`, in `ink`. The title is the
    plugin's name and nothing else - there is no subtitle under it: the panel says what the plugin is.

    `version` is the suite's version stamp ("v0.1.3"), printed on the title's row just to the right of the title,
    kVersionGap after it, in kVersionFontSize at 35 % of the ink, its baseline on the title's: one size, one place,
    and part of the header on every plugin, where no plate can cover it. Empty draws no stamp.
    @param title           the plugin's name
    @param header_content  the header's content area, in design pixels
    @param ink             the title's colour
    @param version         the version stamp, empty for none
    @param badge           a word after the stamp in `badge_colour`, bold - DEMO on an unlicensed plugin; empty for
                           none, and nothing else on the header moves for it
    @param badge_colour    the badge's colour: the panel's ink, at full strength where the stamp is faint */
void DrawTitle(gfx::Canvas &, std::string_view title, gfx::RectI header_content, gfx::Color ink,
               std::string_view version = {}, std::string_view badge = {}, gfx::Color badge_colour = {});

/** The series' header in one call, for `header` - the window's top kHeaderHeight rows: `mark` at x = kHeaderInset,
    clear of the corner screw baked into the face plate, in a kLogoWidth cell inset kLogoInset from the header's top
    and bottom; a kLogoGap; the title with the version stamp beside it (DrawTitle) in `ink` (the palette's), stopping
    `right_reserve` short of the right edge for whatever the header's right end holds (a lamp - DrawLamp - or the
    Amp's indicator cells, drawn by the caller after this); and the rule under the header in `rule`, where the plate's
    seam is. A lamp or a meter repainting its own strip clips the paint to it, and when none of the header's content
    lies inside the clip only the rule is drawn: the mark and the glyph layouts are not laid out again on every lamp
    frame.
    @param mark           the company mark
    @param header         the header's bounds, the window's top kHeaderHeight rows, in design pixels
    @param title          the plugin's name
    @param version        the version stamp, empty for none
    @param ink            the title's colour, the palette's
    @param rule           the rule's colour
    @param right_reserve  the width left clear at the header's right end, in design pixels
    @param badge          a word after the version stamp (DrawTitle); empty for none
    @param badge_colour   the badge's colour */
void DrawHeader(gfx::Canvas &, const CompanyMark &mark, gfx::RectI header, std::string_view title,
                std::string_view version, gfx::Color ink, gfx::Color rule, int right_reserve = 0,
                std::string_view badge = {}, gfx::Color badge_colour = {});

/** The header lamp's strip, the kLampStrip at `header`'s right end: what a lamp repaints when its glow moves, and
    what a header passes DrawHeader as its right_reserve.
    @param header  the header's bounds, in design pixels
    @return        the strip, in design pixels */
gfx::RectI LampStrip(gfx::RectI header) noexcept;

/** The header lamp - the Vibe's photocell lamp, the Mod's LFO, the Delay's repeats, the Drive's drive, the Oldsverb's
    shimmer return, the Limiter's CLIP and the Pedalboard's output (the Gate's meter paints a jewel of its own; the
    Amp has none - its tubes are its indicators): a jewel in `accent` (the series' amber; the Limiter's red) in the
    lamp strip, trimmed clear of the corner screw, its glow and its halo growing with `glow` (0 to 1, clamped) inside
    a hairline `rim`.
    @param header  the header's bounds, in design pixels
    @param glow    the lamp's brightness, 0 to 1, clamped
    @param accent  the jewel's colour
    @param rim     the hairline's colour */
void DrawLamp(gfx::Canvas &, gfx::RectI header, double glow, gfx::Color accent, gfx::Color rim);

/** The indicators' floor in a header cell, `tube_height` under a 3 px gap at the cell's top: a tube stands on it and
    a lens sits on it, and every caption row starts kIndicatorCaptionGap under it - one line of captions across a
    header's cells whatever each holds.
    @param cell         the indicator's cell, in design pixels
    @param tube_height  the tube's height, in design pixels
    @return             the floor's y, in design pixels */
float IndicatorFloor(gfx::RectI cell, int tube_height = kTubeHeight) noexcept;

/** An LED in its header cell (the Amp's gate's, the NAM's): a kLensSize lens with a shaded highlight, its halo
    growing with `glow` (0 to 1, clamped) and its colour fading from the dark glass to `lit` as it does, sitting on
    the indicators' floor (IndicatorFloor: where a tube in the next cell stands), and `caption` under the floor in the
    series' fine print - the row a tube's caption takes, so LEDs and tubes in one header caption on one line. `cell` is
    one kLedCell wide; a plugin lays its cells out left of its lamp strip or, with no lamp, kHeaderInset clear of the
    corner screw.
    @param cell         the LED's cell, one kLedCell wide, in design pixels
    @param caption      the caption under the floor
    @param lit          the colour the lens fades to as it lights
    @param glow         the LED's brightness, 0 to 1, clamped
    @param tube_height  the height of the tubes in the neighbouring cells, in design pixels, for the floor */
void DrawLedCell(gfx::Canvas &, gfx::RectI cell, std::string_view caption, gfx::Color lit, double glow,
                 int tube_height = kTubeHeight);

/** A clip indicator drawn as its tube in a cell - a gain stage's a 12AX7, a power stage's an EL34 (TubeGlyph) -
    glowing as the stage is driven hard, with clear glass the glow shines through: the tube standing on the floor
    `tube_height` under the cell's top - the EL34 the full height, the 12AX7, half its size in life, at
    kSmallTubeShare of it, so a preamp tube reads as the smaller beside a power tube and both stay legible at 36 px -
    with `caption` under the floor, its heater glowing with `glow` (0 to 1, clamped): an amber halo growing behind
    the envelope, and inside the glass what TubeGlyph::Draw paints - the warm light from the heater through the clear
    glass, the plates reddening, a yellow-white core as the stage nears its limit - the way a tube lights up in a dark
    room when it is worked. Cold it is the tube as it sits in a cold amp. The Amp's three nonlinear stages and the
    Distortion's two draw this; a gate's LED stays a lens (DrawLedCell). `tube_height` is the EL34's height in
    pixels: kTubeHeight in a header cell (no panel draws one today), and kBayTubeHeight in the tube bay under the
    Amp's header and the Distortion's, where the glass and what is inside it can be read; the halo and the caption
    row follow it.
    @param cell         the indicator's cell, in design pixels
    @param caption      the caption under the floor
    @param tube         which tube is drawn
    @param lit          the colour of the halo and the heater's glow
    @param glow         the stage's drive, 0 to 1, clamped
    @param tube_height  the EL34's height, in design pixels */
void DrawTubeCell(gfx::Canvas &, gfx::RectI cell, std::string_view caption, TubeGlyph::Kind tube, gfx::Color lit,
                  double glow, int tube_height = kTubeHeight);

/** An indicator's caption - "GATE", "PRE", "CLIP" - in the series' fine print (kFinePrintFontSize, the cream at
    45 %), centred on a kIndicatorCaptionHeight row of `cell` from `top`: under the indicators' floor or a jewel.
    Every indicator in the series captions itself with this, so the rows read alike.
    @param cell  the indicator's cell, in design pixels
    @param top   the caption row's top, in design pixels
    @param text  the caption */
void DrawIndicatorCaption(gfx::Canvas &, gfx::RectI cell, int top, std::string_view text);

/** The cream frames round a bank of knobs: `ink` at 60 %, 6 px corners, 1.2 px - the weight of the Cab's graph
    frame and of the plates. Every bank of knobs in the series is framed.
    @param frames  the frames' rectangles, in design pixels
    @param ink     the palette's ink, drawn at 60 % */
void DrawFrames(gfx::Canvas &, const std::vector<gfx::RectF> &frames, gfx::Color ink);
} // namespace reckless::look
