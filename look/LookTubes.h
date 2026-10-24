#pragma once

#include "Canvas.h"
#include "Color.h"
#include "Geometry.h"
#include "Path.h"

namespace reckless::look
{
/**
    A vacuum tube drawn as geometry, for the series' clip indicators: the envelope, what is inside it and what it
    stands on, as gfx::Paths in millimetres of the real part, so the shape is right at any size and on any
    display, and the glass between the parts is left clear - the panel shows through it, and so does the heater's
    glow, drawn inside the envelope behind the plates: a 2D icon that is the right shape, with transparency the
    gain glow can shine through, and none of the provenance question a cut-out product photograph would carry.

    Two kinds. The 12AX7 is the noval miniature twin triode of every preamp: 20 mm of glass 46 tall on nine pins
    straight out of its button, the exhaust tip on the dome, the silver getter flash inside the dome with the
    getter ring on its post under it, and two plate structures side by side between two mica spacers - the
    heaters show between them. The EL34 is the octal power pentode: 32 mm of glass 70 tall on a brown phenolic
    base with the keyed spigot and eight pins, one big grey plate with its cooling fins between two micas, a window
    low on the plate the cathode's glow comes through, the same flash and ring in the dome.

    Draw() paints one to fit an area on a canvas: the glow inside the glass first (a warm gradient from the heater,
    a yellow-white core with the square of the glow), then the glass's tint and the flash, the plates (which redden
    with the square of the glow, as an over-driven plate does), the micas, the metal, the pins, the base, and last
    the outline in the panel's ink. Cold it is a tube on a shelf; lit it is a tube at work in a dark room.
*/
class TubeGlyph
{
  public:
    /** Which tube is drawn. */
    enum class Kind
    {
        k12AX7, ///< the noval miniature twin triode of a preamp stage
        kEL34,  ///< the octal power pentode of an output stage
    };

    /** The tube of that kind, built once.
        @param kind  which tube
        @return      the glyph, which lives for the process */
    static const TubeGlyph &Of(Kind kind);

    /** The whole part - glass, base, pins - in millimetres: x centred on the tube's axis, y down from the top of
        the glass (the tip reaches a little above zero).
        @return the part's bounds, in millimetres */
    gfx::RectF bounds() const noexcept
    {
        return bounds_;
    }

    /** Where the light comes from - the heater inside the plates - and how far it reaches, in millimetres.
        @return the heater's position, in millimetres in the bounds' coordinates */
    gfx::PointF heater() const noexcept
    {
        return heater_;
    }

    /** @return how far the heater's light reaches, in millimetres */
    float glow_radius() const noexcept
    {
        return glow_radius_;
    }

    /** A point inside the glass that is clear glass - between the 12AX7's plates, in the EL34's window - and one
        on a plate, in millimetres: the tests look through the one and at the other.
        @return a point of clear glass, in millimetres in the bounds' coordinates */
    gfx::PointF glass_sample() const noexcept
    {
        return glass_sample_;
    }

    /** @return a point on a plate, in millimetres in the bounds' coordinates */
    gfx::PointF plate_sample() const noexcept
    {
        return plate_sample_;
    }

    /** The tube placed in an area: the area, millimetres to its pixels (the tube scaled to fit it and centred in
        it), that scale in pixels per millimetre, and where the heater lands. Taken once by FitTo and handed to
        Draw, so a caller that lights a halo from the heater before the tube (DrawTubeCell) and the Draw that
        follows share one fit. */
    struct Fit
    {
        gfx::RectF  area;      ///< the area the tube is fitted to, in pixels
        gfx::Affine to_pixels; ///< the transform from millimetres to the area's pixels
        float       scale;     ///< the scale, in pixels per millimetre
        gfx::PointF heater;    ///< where the heater lands, in pixels
    };

    /** The tube fitted to `area`: scaled to fit it and centred in it.
        @param area  the area, in pixels
        @return      the fit, to hand to Draw */
    Fit FitTo(gfx::RectF area) const noexcept;

    /** The tube in `fit`'s area, lit by `glow` (0 to 1, clamped) in the accent `lit` - the heater's light is the
        accent pulled toward orange-red - and outlined in `ink`.
        @param c     the canvas drawn on
        @param fit   the fit FitTo made for the area
        @param ink   the outline's colour, the panel's ink
        @param lit   the accent the glow is drawn in
        @param glow  the heater's glow, 0 to 1, clamped */
    void Draw(gfx::Canvas &, const Fit &fit, gfx::Color ink, gfx::Color lit, double glow) const;

    /** The same, fitted to `area` here.
        @param canvas  the canvas drawn on
        @param area    the area, in pixels
        @param ink     the outline's colour, the panel's ink
        @param lit     the accent the glow is drawn in
        @param glow    the heater's glow, 0 to 1, clamped */
    void Draw(gfx::Canvas &canvas, gfx::RectF area, gfx::Color ink, gfx::Color lit, double glow) const
    {
        Draw(canvas, FitTo(area), ink, lit, glow);
    }

  private:
    TubeGlyph() = default;
    /** @return the 12AX7's geometry */
    static TubeGlyph Make12AX7();
    /** @return the EL34's geometry */
    static TubeGlyph MakeEL34();

    gfx::Path envelope_;  ///< the glass, closed
    gfx::Path flash_;     ///< the getter flash, filled silver inside the dome
    gfx::Path mica_;      ///< the spacers
    gfx::Path plates_;    ///< the plate structure(s), filled
    gfx::Path fins_;      ///< the EL34's cooling fins, a shade lighter; empty on the 12AX7
    gfx::Path seams_;     ///< the folds and pressings in the plates, stroked dark
    gfx::Path metal_;     ///< rods, the getter ring and its post, stroked
    gfx::Path pins_;      ///< stroked
    gfx::Path base_;      ///< the EL34's phenolic base; empty on the 12AX7
    gfx::Path base_rim_;  ///< its lit top edge
    gfx::Path spigot_;    ///< the octal key
    gfx::Path key_;       ///< the ridge on it
    gfx::Path highlight_; ///< the glass's reflection, stroked

    gfx::Color  plate_colour_;        ///< the plates' fill.
    gfx::Color  plate_edge_;          ///< the plates' edge and seams.
    gfx::Color  pin_colour_;          ///< the pins' stroke.
    float       pin_width_ = 0.9f;    ///< the pins' stroke width, in millimetres.
    gfx::RectF  bounds_;              ///< the whole part, in millimetres.
    gfx::PointF heater_;              ///< where the light comes from, in millimetres.
    float       glow_radius_ = 10.0f; ///< how far the light reaches, in millimetres.
    gfx::PointF glass_sample_;        ///< a point of clear glass, in millimetres.
    gfx::PointF plate_sample_;        ///< a point on a plate, in millimetres.
};
} // namespace reckless::look
