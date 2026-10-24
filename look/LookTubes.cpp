#include "LookTubes.h"

#include <algorithm>

#include "LookGeometry.h"

namespace reckless::look
{
namespace
{
constexpr gfx::Color kMica{0xffd8d3c4};     ///< the spacers.
constexpr gfx::Color kMetal{0xffb4b6b0};    ///< rods, the getter ring.
constexpr gfx::Color kFlash{0xffc0c6d0};    ///< the getter's silvering.
constexpr gfx::Color kHeater{0xffff5a1a};   ///< the accent is pulled toward this.
constexpr gfx::Color kCore{0xffffe2a0};     ///< the heater itself, at the limit.
constexpr gfx::Color kRedPlate{0xfff0421c}; ///< an over-driven plate, cherry rather than neon.
constexpr gfx::Color kPhenolic{0xff3d2316}; ///< the octal base.
constexpr gfx::Color kRim{0xff7a5138};      ///< its lit edge.
constexpr gfx::Color kSpigot{0xff23150e};   ///< the octal key's spigot.
constexpr gfx::Color kKey{0xff4a3222};      ///< the ridge on the spigot.

/** A line as a sub-path.
    @param path  the path appended to
    @param x1    the start's x, in millimetres
    @param y1    the start's y, in millimetres
    @param x2    the end's x, in millimetres
    @param y2    the end's y, in millimetres */
void AddLine(gfx::Path &path, float x1, float y1, float x2, float y2)
{
    path.MoveTo(x1, y1);
    path.LineTo(x2, y2);
}

/** A rectangle `w` by `h` at (x, y) with corners of radii `csx` and `csy` (each held to half the side), each
    corner rounded only where its flag says - the EL34's plate halves, base and spigot, square where they meet
    something - traced clockwise from the top-left corner's foot as one closed sub-path.
    @param path                the path appended to
    @param x                   the left edge, in millimetres
    @param y                   the top edge, in millimetres
    @param w                   the width, in millimetres
    @param h                   the height, in millimetres
    @param csx                 the corners' horizontal radius, in millimetres
    @param csy                 the corners' vertical radius, in millimetres
    @param curve_top_left      true to round the top-left corner
    @param curve_top_right     true to round the top-right corner
    @param curve_bottom_left   true to round the bottom-left corner
    @param curve_bottom_right  true to round the bottom-right corner */
void AddRoundedRect(gfx::Path &path, float x, float y, float w, float h, float csx, float csy, bool curve_top_left,
                    bool curve_top_right, bool curve_bottom_left, bool curve_bottom_right)
{
    csx              = std::min(csx, w * 0.5f);
    csy              = std::min(csy, h * 0.5f);
    const auto cs45x = csx * 0.45f;
    const auto cs45y = csy * 0.45f;
    const auto x2    = x + w;
    const auto y2    = y + h;

    if (curve_top_left)
    {
        path.MoveTo(x, y + csy);
        path.CubicTo(x, y + cs45y, x + cs45x, y, x + csx, y);
    }
    else
    {
        path.MoveTo(x, y);
    }

    if (curve_top_right)
    {
        path.LineTo(x2 - csx, y);
        path.CubicTo(x2 - cs45x, y, x2, y + cs45y, x2, y + csy);
    }
    else
    {
        path.LineTo(x2, y);
    }

    if (curve_bottom_right)
    {
        path.LineTo(x2, y2 - csy);
        path.CubicTo(x2, y2 - cs45y, x2 - cs45x, y2, x2 - csx, y2);
    }
    else
    {
        path.LineTo(x2, y2);
    }

    if (curve_bottom_left)
    {
        path.LineTo(x + csx, y2);
        path.CubicTo(x + cs45x, y2, x, y2 - cs45y, x, y2 - csy);
    }
    else
    {
        path.LineTo(x, y2);
    }

    path.Close();
}

/** A stroke for one of a tube's lines: `width` canvas pixels - StrokePath strokes the path after `t` has taken it from
    millimetres to pixels, so a width is never scaled with the tube - or `floor / scale` where that is wider, `scale`
    the pixels to the millimetre. The widths are the ones the bay's tubes (kBayTubeHeight) are drawn at: the 12AX7
    there at 1.65 px to the millimetre, its outline 0.7 px and its plates' edges 0.36; the EL34 at 1.05, its outline
    0.86 and its plates' edges 0.57. The floor widens a smaller tube's lines, not a larger one's - a 36 px header tube,
    at 0.36 px to the millimetre, would take a 2.5 px outline - so a tube drawn at another size wants its widths
    looked at again.
    @param width  the stroke's width, in canvas pixels
    @param scale  the tube's scale, in pixels per millimetre
    @param floor  the narrowest stroke, in canvas pixels
    @return       the stroke, with curved joins and rounded caps */
gfx::Stroke TubeStroke(float width, float scale, float floor = 0.9f)
{
    return gfx::Stroke(std::max(width, floor / scale), gfx::Stroke::Join::kCurved, gfx::Stroke::Cap::kRounded);
}
} // namespace

const TubeGlyph &TubeGlyph::Of(Kind kind)
{
    static const TubeGlyph triode  = Make12AX7();
    static const TubeGlyph pentode = MakeEL34();
    return kind == Kind::kEL34 ? pentode : triode;
}

TubeGlyph TubeGlyph::Make12AX7()
{
    TubeGlyph t;

    // The glass: a T-6 1/2 bulb, 20 mm across and 46 tall on its button, a round-shouldered dome with the exhaust
    // tip on it.
    auto &e = t.envelope_;
    e.MoveTo(-10.0f, 42.0f);
    e.LineTo(-10.0f, 9.0f);
    e.CubicTo(-10.0f, 3.4f, -6.4f, 0.4f, -1.6f, 0.4f);
    e.CubicTo(-1.4f, -0.9f, -0.9f, -1.7f, 0.0f, -1.7f);
    e.CubicTo(0.9f, -1.7f, 1.4f, -0.9f, 1.6f, 0.4f);
    e.CubicTo(6.4f, 0.4f, 10.0f, 3.4f, 10.0f, 9.0f);
    e.LineTo(10.0f, 42.0f);
    e.CubicTo(10.0f, 44.6f, 9.1f, 46.0f, 7.5f, 46.0f);
    e.LineTo(-7.5f, 46.0f);
    e.CubicTo(-9.1f, 46.0f, -10.0f, 44.6f, -10.0f, 42.0f);
    e.Close();

    // The getter flash silvers the dome (filled inside the glass); the ring sits on its post over the top mica.
    t.flash_.AddRect({-10.0f, -1.7f, 20.0f, 8.4f});
    t.metal_.AddEllipse({-3.0f, 9.6f, 6.0f, 2.4f});
    AddLine(t.metal_, 0.0f, 12.0f, 0.0f, 15.0f);

    // Two triodes side by side between their micas, clear glass either side of them; the heaters show in the gap
    // between the plates.
    t.mica_.AddRoundedRect({-8.4f, 15.0f, 16.8f, 1.5f}, 0.4f);
    t.mica_.AddRoundedRect({-8.4f, 33.5f, 16.8f, 1.5f}, 0.4f);
    t.plates_.AddRoundedRect({-7.4f, 16.8f, 5.4f, 16.7f}, 0.7f);
    t.plates_.AddRoundedRect({2.0f, 16.8f, 5.4f, 16.7f}, 0.7f);
    AddLine(t.seams_, -4.7f, 17.6f, -4.7f, 32.7f); // the seam where each plate's box is folded
    AddLine(t.seams_, 4.7f, 17.6f, 4.7f, 32.7f);

    // The support rods down to the button, and six of the nine pins facing us.
    AddLine(t.metal_, -4.6f, 35.0f, -4.6f, 45.6f);
    AddLine(t.metal_, 4.6f, 35.0f, 4.6f, 45.6f);
    for (const float x : {-6.8f, -4.2f, -1.5f, 1.5f, 4.2f, 6.8f})
    {
        AddLine(t.pins_, x, 46.0f, x, 52.6f);
    }
    AddLine(t.highlight_, -8.4f, 11.0f, -8.4f, 40.5f);

    t.plate_colour_ = gfx::Color(0xff3b3940); // a black plate
    t.plate_edge_   = gfx::Color(0xff1c1b1f);
    t.pin_colour_   = gfx::Color(0xffc9cbc6);
    t.pin_width_    = 0.9f;
    t.bounds_       = {-10.0f, -1.7f, 20.0f, 54.3f};
    t.heater_       = {0.0f, 25.2f};
    t.glow_radius_  = 16.0f;
    t.glass_sample_ = {0.0f, 25.2f}; // between the plates
    t.plate_sample_ = {4.7f, 25.2f}; // the right-hand plate
    return t;
}

TubeGlyph TubeGlyph::MakeEL34()
{
    TubeGlyph t;

    // The glass: 32 mm across and 70 tall to the base, the tip on the dome.
    auto &e = t.envelope_;
    e.MoveTo(-16.0f, 66.0f);
    e.LineTo(-16.0f, 9.0f);
    e.CubicTo(-16.0f, 3.2f, -9.0f, 0.4f, -2.2f, 0.4f);
    e.CubicTo(-1.9f, -0.9f, -1.1f, -1.9f, 0.0f, -1.9f);
    e.CubicTo(1.1f, -1.9f, 1.9f, -0.9f, 2.2f, 0.4f);
    e.CubicTo(9.0f, 0.4f, 16.0f, 3.2f, 16.0f, 9.0f);
    e.LineTo(16.0f, 66.0f);
    e.CubicTo(16.0f, 68.4f, 15.2f, 70.0f, 13.5f, 70.0f);
    e.LineTo(-13.5f, 70.0f);
    e.CubicTo(-15.2f, 70.0f, -16.0f, 68.4f, -16.0f, 66.0f);
    e.Close();

    t.flash_.AddRect({-16.0f, -1.9f, 32.0f, 11.0f});
    t.metal_.AddEllipse({-4.0f, 10.5f, 8.0f, 3.4f});
    AddLine(t.metal_, 0.0f, 13.9f, 0.0f, 17.0f);

    // One big grey plate with its cooling fins between two micas, and a window low on it that the cathode's glow
    // comes through - four pieces round the gap, since a path has no subtraction.
    t.mica_.AddRoundedRect({-14.6f, 17.0f, 29.2f, 1.8f}, 0.5f);
    t.mica_.AddRoundedRect({-14.6f, 57.2f, 29.2f, 1.8f}, 0.5f);
    AddRoundedRect(t.plates_, -11.0f, 19.0f, 22.0f, 28.0f, 0.8f, 0.8f, true, true, false, false);
    t.plates_.AddRect({-11.0f, 47.0f, 6.5f, 4.5f});
    t.plates_.AddRect({4.5f, 47.0f, 6.5f, 4.5f});
    AddRoundedRect(t.plates_, -11.0f, 51.5f, 22.0f, 5.5f, 0.8f, 0.8f, false, false, true, true);
    t.fins_.AddRoundedRect({-13.8f, 26.0f, 3.0f, 24.0f}, 0.6f);
    t.fins_.AddRoundedRect({10.8f, 26.0f, 3.0f, 24.0f}, 0.6f);
    AddLine(t.seams_, -11.0f, 38.0f, 11.0f, 38.0f); // the pressed seam across the plate

    // The rods down to the stem; the octal's pins stand on a 17.5 mm circle, so two face us and the rest hide
    // behind the spigot; the phenolic base with its lit top edge, the spigot and the key ridge on it.
    AddLine(t.metal_, -9.2f, 59.0f, -9.2f, 69.6f);
    AddLine(t.metal_, 9.2f, 59.0f, 9.2f, 69.6f);
    for (const float x : {-8.0f, 8.0f})
    {
        AddLine(t.pins_, x, 88.0f, x, 96.5f);
    }
    AddRoundedRect(t.base_, -17.0f, 70.0f, 34.0f, 18.0f, 2.0f, 2.0f, false, false, true, true);
    t.base_rim_.AddRect({-17.0f, 70.0f, 34.0f, 1.6f});
    AddRoundedRect(t.spigot_, -4.0f, 88.0f, 8.0f, 9.0f, 1.5f, 1.5f, false, false, true, true);
    t.key_.AddRect({-1.0f, 88.0f, 2.0f, 9.0f});
    AddLine(t.highlight_, -13.8f, 12.0f, -13.8f, 63.0f);

    t.plate_colour_ = gfx::Color(0xff77756f); // a grey plate
    t.plate_edge_   = gfx::Color(0xff45433f);
    t.pin_colour_   = gfx::Color(0xff9a9a96);
    t.pin_width_    = 1.3f;
    t.bounds_       = {-17.0f, -1.9f, 34.0f, 98.9f};
    t.heater_       = {0.0f, 39.0f};
    t.glow_radius_  = 24.0f;
    t.glass_sample_ = {0.0f, 49.2f}; // the window
    t.plate_sample_ = {0.0f, 30.0f};
    return t;
}

TubeGlyph::Fit TubeGlyph::FitTo(gfx::RectF area) const noexcept
{
    const float scale = FitScale(bounds_, area); // pixels per millimetre
    const auto  t     = FitTransform(bounds_, area, scale);
    return {area, t, scale, t.Apply(heater_)};
}

void TubeGlyph::Draw(gfx::Canvas &c, const Fit &fit, gfx::Color ink, gfx::Color lit, double glow) const
{
    const auto  area       = fit.area;
    const auto &t          = fit.to_pixels;
    const float scale      = fit.scale;
    const float lit_amount = static_cast<float>(std::clamp(glow, 0.0, 1.0));
    const auto  heater     = fit.heater;

    // The heater's light inside the glass, clipped to the envelope so it is the tube's and not the panel's: warm
    // from the heater out to the glow radius, and a yellow-white core that comes up with the square of the glow -
    // the knee lights the tube, the limit whites out its middle.
    if (lit_amount > 0.0f)
    {
        gfx::ScopedState state(c);
        c.ClipToPath(envelope_, t);
        const gfx::Color heat  = lit.InterpolatedWith(kHeater, 0.55f);
        const float      reach = glow_radius_ * scale;
        c.SetGradient(gfx::Gradient{heat.WithAlpha(0.85f * lit_amount),
                                    heater,
                                    heat.WithAlpha(0.0f),
                                    {heater.x, heater.y + reach},
                                    true,
                                    {{0.5f, heat.WithAlpha(0.4f * lit_amount)}}});
        c.FillRect(area.Expanded(1.0f));

        c.SetGradient(gfx::Gradient{kCore.WithAlpha(0.9f * lit_amount * lit_amount),
                                    heater,
                                    kCore.WithAlpha(0.0f),
                                    {heater.x, heater.y + 0.32f * reach},
                                    true,
                                    {}});
        c.FillRect(area.Expanded(1.0f));
    }

    // The glass: a breath of tint so the body reads on a dark panel, and the getter's silvering of the dome.
    c.SetColor(gfx::colors::kWhite.WithAlpha(0.05f));
    c.FillPath(envelope_, t);
    {
        gfx::ScopedState state(c);
        c.ClipToPath(envelope_, t);
        c.SetColor(kFlash.WithAlpha(0.38f));
        c.FillPath(flash_, t);
    }

    // Inside: the plates and their fins, reddening with the square of the glow; their edges and seams; the micas;
    // the rods and the ring.
    c.SetColor(plate_colour_);
    c.FillPath(plates_, t);
    c.SetColor(plate_colour_.Brighter(0.3f));
    c.FillPath(fins_, t);
    if (lit_amount > 0.0f)
    {
        c.SetColor(kRedPlate.WithAlpha(0.45f * lit_amount * lit_amount));
        c.FillPath(plates_, t);
        c.FillPath(fins_, t);
    }
    c.SetColor(plate_edge_.WithAlpha(0.8f));
    c.StrokePath(plates_, TubeStroke(0.35f, scale, 0.6f), t);
    c.StrokePath(fins_, TubeStroke(0.35f, scale, 0.6f), t);
    c.SetColor(plate_edge_.WithAlpha(0.6f));
    c.StrokePath(seams_, TubeStroke(0.4f, scale, 0.6f), t);
    c.SetColor(kMica.WithAlpha(0.85f));
    c.FillPath(mica_, t);
    c.SetColor(kMetal.WithAlpha(0.9f));
    c.StrokePath(metal_, TubeStroke(0.7f, scale), t);

    // What it stands on: the pins, and the octal base with its spigot and key.
    c.SetColor(pin_colour_);
    c.StrokePath(pins_, TubeStroke(pin_width_, scale), t);
    if (!base_.IsEmpty())
    {
        c.SetColor(kPhenolic);
        c.FillPath(base_, t);
        c.SetColor(kRim);
        c.FillPath(base_rim_, t);
        c.SetColor(kSpigot);
        c.FillPath(spigot_, t);
        c.SetColor(kKey);
        c.FillPath(key_, t);
    }

    // The glass's outline in the panel's ink, and a reflection down one side.
    c.SetColor(ink.WithAlpha(0.62f));
    c.StrokePath(envelope_, TubeStroke(0.7f, scale), t);
    c.SetColor(gfx::colors::kWhite.WithAlpha(0.16f));
    c.StrokePath(highlight_, TubeStroke(1.2f, scale), t);
}
} // namespace reckless::look
