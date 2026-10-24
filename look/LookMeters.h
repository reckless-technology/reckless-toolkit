#pragma once

#include <string_view>

#include "Canvas.h"
#include "Color.h"
#include "Geometry.h"

namespace reckless::look
{
/** The seventies backlit meters' artwork (assets/meters/vu_meter_*.svg and level_meter_*.svg), one set for every
    meter of the family so a plugin's meter is the series' meter: the print on the dials and the needles, and the
    geometry of the artwork's one 1000 x 1000 space. The housing - the bezel's outer edge, 920 x 720 - is what a meter
    fits to its bounds at one uniform scale, letterboxed and never stretched; the needle pivots below the dial, behind
    the bezel; the dial window is the needle's clip; the ticks stand on the arc's outside and the figures beyond them;
    the needle's tip reaches past the figures. panel/'s ArtworkMeterView draws the artwork, the needle and these
    scales. */
namespace meter_art
{
inline constexpr gfx::Color kFaceInk{0xff1a1005}; ///< the arc, the ticks and the figures.
inline constexpr gfx::Color kRed{0xffc8102e};     ///< a red zone on the dial, and every needle's tip.
inline constexpr gfx::Color kNeedle{0xff111111};  ///< the needle's blade.

inline constexpr gfx::RectF kHousing{40.0f, 140.0f, 920.0f, 720.0f}; ///< the bezel's outer edge, in artwork units.
/** The housing's width over its height, 1.278: a box of this shape is filled. */
inline constexpr float       kAspect = kHousing.width() / kHousing.height();
inline constexpr gfx::PointF kPivot{500.0f, 880.0f};                 ///< the needle's pivot, in artwork units.
inline constexpr gfx::RectF  kWindow{92.0f, 192.0f, 816.0f, 556.0f}; ///< the dial window, the needle's clip.
inline constexpr float       kWindowCorner  = 28.0f;                 ///< the dial window's corner radius.
inline constexpr float       kHousingCorner = 56.0f;                 ///< the housing's corner radius.
inline constexpr float       kNeedleTip     = 640.0f;                ///< the pivot to the tip, in artwork units.
} // namespace meter_art

/**
    The gain-reduction meter's scale: the needle at rest (0 dB) points kRestDegrees clockwise of straight up, at the
    right, and swings left as a dynamics plugin takes gain away, to the same angle anticlockwise at kFullScaleDb (30,
    the artwork's full scale); between them the arc is log(1 + GR / kScaleCurve) compressed, opening up the first few
    decibels the way a VU's scale does - the formula the artwork's ticks were generated with. Paint sets the figures
    (0 1 2 3 5 7 10 15 20 30) on their ticks' rays and the legend, in the series' typeface where the SVG placed its
    Helvetica, in the artwork's units. The Comp's and the Limiter's.
*/
struct GainReductionScale
{
    static constexpr double kFullScaleDb = 30.0; ///< the reduction at the left end of the arc, in decibels.
    static constexpr double kRestDegrees = 34.0; ///< the needle's angle at rest, in degrees clockwise from up.
    static constexpr double kScaleCurve  = 4.0;  ///< the log law's knee, in decibels.

    /** Where on the arc a reduction sits, 0 (no reduction, the right end) to 1 (full scale, the left end):
        log(1 + GR / kScaleCurve) / log(1 + kFullScaleDb / kScaleCurve), clamped.
        @param gr_db  the gain reduction, in decibels, zero or more
        @return       the position on the arc, 0 to 1 */
    static double Position(double gr_db) noexcept;

    /** The needle's angle for a reduction, in degrees clockwise from straight up: +34 at 0 dB, 16.2 at 3, 8.2 at 5,
        -5.8 at 10, -22.9 at 20, -34 at kFullScaleDb (the artwork's reference points).
        @param gr_db  the gain reduction, in decibels, zero or more
        @return       the angle, in degrees clockwise from straight up */
    static double AngleDegrees(double gr_db) noexcept;

    /** The figures, each on its tick's ray and turned with it, and the legend - "GAIN REDUCTION", letter-spaced as
        the artwork's was, and "dB" under it - on a canvas already in the artwork's space.
        @param c  the canvas drawn on, already transformed to the artwork's 1000 x 1000 space */
    static void Paint(gfx::Canvas &);
};

/**
    The small peak level meter's scale: the face's -30 (kMinDb) at the left to its +6 (kMaxDb) at the right, the needle
    kSweepDegrees either side of straight up, on a VU face's half-power law - sqrt of the linear amplitude, which puts
    the 0 at two thirds of the sweep and spreads the quiet end more evenly than a voltage law would - the last six
    decibels red. The decibels are the face's, not dBFS (panel/'s LevelMeterView reads them six decibels over the
    digital level). Paint sets the figures upright on their ticks' rays, the ones over 0 in the red zone's red, the
    meter's name where the artwork's "VU" sat and what it reads under it - "PEAK" on the Pedalboard's IN and OUT, "VU"
    on the Tape's two tracks.
*/
struct LevelMeterScale
{
    static constexpr double kMinDb        = -30.0; ///< the face's reading at the left end, in decibels.
    static constexpr double kMaxDb        = 6.0;   ///< the face's reading at the right end, in decibels.
    static constexpr double kSweepDegrees = 34.0;  ///< the needle's swing either side of straight up, in degrees.

    /** Where on the arc a reading sits, 0 (kMinDb, the left end) to 1 (kMaxDb, the right):
        (10^(dB/40) - 10^(min/40)) / (10^(max/40) - 10^(min/40)), the half-power law the artwork's ticks were
        generated with, clamped at both ends.
        @param reading_db  the reading, in the face's decibels, which sit six over the digital level's dBFS
        @return            the position on the arc, 0 to 1 */
    static double Position(double reading_db) noexcept;

    /** The needle's angle for a reading, in degrees clockwise from straight up: -34 at the face's -30, 11.3 at its 0,
        +34 at its +6 - the artwork's reference points.
        @param reading_db  the reading, in the face's decibels
        @return            the angle, in degrees clockwise from straight up */
    static double AngleDegrees(double reading_db) noexcept;

    /** The figures, the meter's `legend` ("IN", "OUT") and what it reads (`reads`, "PEAK" or "VU") under it, on a
        canvas already in the artwork's space.
        @param c       the canvas drawn on, already transformed to the artwork's 1000 x 1000 space
        @param legend  the meter's name, printed where the artwork's "VU" sat
        @param reads   what the needle reads, printed smaller under the name */
    static void Paint(gfx::Canvas &, std::string_view legend, std::string_view reads = "PEAK");
};
} // namespace reckless::look
