#include "LookMeters.h"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <string>

namespace reckless::look
{
namespace
{
/** An angle in degrees as the radians a rotation takes.
    @param degrees  the angle, in degrees
    @return         the angle, in radians */
float Radians(double degrees) noexcept
{
    return static_cast<float>(degrees * (std::numbers::pi / 180.0));
}

// The gain-reduction meter's figures and legend, in the artwork's units.
constexpr float kGainFigureRadius  = 646.0f; ///< the figures' centres (the SVG set their baseline at 630).
constexpr float kGainFigureSize    = 46.0f;  ///< the figures' font size, in artwork units.
constexpr float kGainLegendSize    = 40.0f;  ///< the legend's font size, in artwork units.
constexpr float kGainUnitSize      = 30.0f;  ///< the unit's font size, in artwork units.
constexpr float kGainLegendY       = 546.0f; ///< "GAIN REDUCTION" centre (the SVG's baseline at 560).
constexpr float kGainUnitY         = 600.0f; ///< "dB" centre (baseline 610).
constexpr float kGainLegendKerning = 0.25f;  ///< letter-spacing 10 on 40, as a fraction of the height.
/** The unit's letter-spacing, half the artwork's 6 on 30: Noto Serif is wider than its Helvetica, and at 0.2 the two
    letters fell apart. */
constexpr float kGainUnitKerning = 0.1f;

// The level meter's figures and legend (make_level_meter.py's FIGURE_* and LEGEND_*), in the artwork's units:
// heavier than the gain-reduction meter's, since this meter is shown at 0.45 of its size.
constexpr float kLevelFigureRadius = 648.0f; ///< the figures' centres, in artwork units.
constexpr float kLevelFigureSize   = 58.0f;  ///< the figures' font size, in artwork units.
constexpr float kLevelLegendSize   = 80.0f;  ///< the legend's font size, in artwork units.
constexpr float kLevelLegendY      = 594.0f; ///< the legend's centre, in artwork units.
constexpr float kLevelUnitSize     = 44.0f;  ///< "PEAK", under the legend: its font size, in artwork units.
constexpr float kLevelUnitY        = 664.0f; ///< "PEAK" centre, in artwork units.
constexpr float kLevelUnitKerning  = 0.1f;   ///< "PEAK"'s letter-spacing, as a fraction of the height.
} // namespace

// ---- The gain-reduction meter ---------------------------------------------------------------------------------

double GainReductionScale::Position(double gr_db) noexcept
{
    const double clamped = std::clamp(gr_db, 0.0, kFullScaleDb);
    return std::log(1.0 + clamped / kScaleCurve) / std::log(1.0 + kFullScaleDb / kScaleCurve);
}

double GainReductionScale::AngleDegrees(double gr_db) noexcept
{
    return kRestDegrees - 2.0 * kRestDegrees * Position(gr_db);
}

void GainReductionScale::Paint(gfx::Canvas &c)
{
    // Each figure on its tick's ray, turned with it, at the artwork's size; the caption letter-spaced as the
    // artwork's was.
    using meter_art::kPivot;
    using meter_art::kWindow;
    c.SetColor(meter_art::kFaceInk);
    c.SetFont(gfx::Font(kGainFigureSize).Bold());
    for (double db : {0.0, 1.0, 2.0, 3.0, 5.0, 7.0, 10.0, 15.0, 20.0, 30.0})
    {
        gfx::ScopedState turned(c);
        c.AddTransform(gfx::Affine::Rotation(Radians(AngleDegrees(db)), kPivot.x, kPivot.y));
        c.DrawText(std::to_string(gfx::RoundToInt(db)),
                   gfx::RectF(120.0f, kGainFigureSize).WithCentre({kPivot.x, kPivot.y - kGainFigureRadius}),
                   gfx::Justify::kCentred);
    }
    c.SetFont(gfx::Font(kGainLegendSize).Bold().WithKerning(kGainLegendKerning));
    c.DrawText("GAIN REDUCTION", kWindow.WithHeight(kGainLegendSize).WithCentre({kPivot.x, kGainLegendY}),
               gfx::Justify::kCentred);
    c.SetFont(gfx::Font(kGainUnitSize).WithKerning(kGainUnitKerning));
    c.DrawText("dB", kWindow.WithHeight(kGainUnitSize).WithCentre({kPivot.x, kGainUnitY}), gfx::Justify::kCentred);
}

// ---- The level meter ------------------------------------------------------------------------------------------

double LevelMeterScale::Position(double reading_db) noexcept
{
    // The half-power law: the position is linear in sqrt(amplitude), 10^(dB / 40), between the ends of the scale.
    const double clamped = std::clamp(reading_db, kMinDb, kMaxDb);
    const double lo      = std::pow(10.0, kMinDb / 40.0);
    const double hi      = std::pow(10.0, kMaxDb / 40.0);
    return (std::pow(10.0, clamped / 40.0) - lo) / (hi - lo);
}

double LevelMeterScale::AngleDegrees(double reading_db) noexcept
{
    return -kSweepDegrees + 2.0 * kSweepDegrees * Position(reading_db);
}

void LevelMeterScale::Paint(gfx::Canvas &c, std::string_view legend, std::string_view reads)
{
    // The figures upright on each tick's ray (as the artwork set them), the ones over 0 in the red zone's red; the
    // meter's name where the artwork's "VU" sat and what the needle reads under it, smaller.
    using meter_art::kPivot;
    using meter_art::kWindow;
    c.SetFont(gfx::Font(kLevelFigureSize).Bold());
    for (double db : {-30.0, -20.0, -10.0, -3.0, 0.0, 3.0, 6.0})
    {
        const auto radians = static_cast<float>(AngleDegrees(db) * (std::numbers::pi / 180.0));
        const auto centre  = gfx::PointF(kPivot.x + kLevelFigureRadius * std::sin(radians),
                                         kPivot.y - kLevelFigureRadius * std::cos(radians));
        const auto figure  = gfx::RoundToInt(db);
        c.SetColor(db > 0.0 ? meter_art::kRed : meter_art::kFaceInk);
        c.DrawText((figure > 0 ? "+" : "") + std::to_string(figure),
                   gfx::RectF(160.0f, kLevelFigureSize).WithCentre(centre), gfx::Justify::kCentred);
    }
    c.SetColor(meter_art::kFaceInk);
    c.SetFont(gfx::Font(kLevelLegendSize).Bold());
    c.DrawText(legend, kWindow.WithHeight(kLevelLegendSize).WithCentre({kPivot.x, kLevelLegendY}),
               gfx::Justify::kCentred);
    c.SetFont(gfx::Font(kLevelUnitSize).WithKerning(kLevelUnitKerning));
    c.DrawText(reads, kWindow.WithHeight(kLevelUnitSize).WithCentre({kPivot.x, kLevelUnitY}), gfx::Justify::kCentred);
}
} // namespace reckless::look
