#pragma once

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

#include "ArtworkMeterView.h"
#include "LookBallistics.h"
#include "LookMeters.h"
#include "PanelView.h"

namespace reckless::panel
{
/**
    The small peak level meter on the series' backlit artwork, as a view: an ArtworkMeterView on its own face and glass
    (assets/meters/level_meter_face.svg and level_meter_glass.svg, heavier ticks for the size it is shown at),
    reading from kMinDb at the left to kMaxDb at the right - the face's -30 to +6 - on a VU face's half-power law
    (look::LevelMeterScale), the last six decibels in red. The face's decibels are the digital level six decibels up
    (kZeroDbfs): a -6 dBFS peak stands on the 0, full scale on the +6 at the top, and the face's -30 is -36 dBFS.

    Advance() is fed the block peak a core holds for the display frame and gives a peak programme reading with no hold
    (look::PeakDecay): the reading takes a new peak in the frame it arrives and from the next frame lets go of it as an
    RC discharge with a time constant of kFallSeconds, in the linear domain, the series' peak detectors' release. The
    needle follows the reading through a mechanism (look::NeedleBallistics at kNeedleRiseHz up, kNeedleFallHz down,
    kNeedleDamping): a new peak takes about 70 ms to arrive on it, and the slower fall lets the pointer glide down
    behind the discharge. A level over full scale is read as it is and holds the needle on the right stop; a frame that
    moved the tip less than a tenth of a pixel repaints nothing. The Pedalboard's IN and OUT meters.
*/
class LevelMeterView final : public ArtworkMeterView
{
  public:
    /** The scale's bottom, at the left, in the face's decibels. */
    static constexpr double kMinDb = look::LevelMeterScale::kMinDb;
    /** The scale's top, at the right, in the face's decibels. */
    static constexpr double kMaxDb = look::LevelMeterScale::kMaxDb;
    /** The needle's sweep from kMinDb to kMaxDb, in degrees. */
    static constexpr double kSweepDegrees = look::LevelMeterScale::kSweepDegrees;

    /** The digital level, in dBFS, that reads 0 on the face: the six decibels over the 0 are the headroom. */
    static constexpr double kZeroDbfs = -6.0;

    /** The reading's discharge time constant, in seconds: 8.7 dB down after one, 20 dB after 2.3. */
    static constexpr double kFallSeconds = 0.75;

    /** The needle's mechanism, following the reading in the linear domain: 90 % of a rise at 67 ms, 2.5 % over at
        its furthest; three times slower down. */
    static constexpr double kNeedleRiseHz = 8.0;
    /** The needle's stiffness falling, in hertz. */
    static constexpr double kNeedleFallHz = 3.0;
    /** The needle's damping ratio, both ways. */
    static constexpr double kNeedleDamping = 0.8;

    /** The width the series shows a level meter at - 0.45 of the 420 px gain-reduction meter - and the height its
        artwork's aspect gives it. */
    static constexpr int kWidth = 189;
    /** The height its artwork's aspect gives that width, in design pixels. */
    static constexpr int kHeight = static_cast<int>(kWidth / look::meter_art::kAspect + 0.5f);

    /** `legend` names the meter on its dial - "IN" or "OUT" - with "PEAK" under it.
        @param context  the panel's context, which carries the artwork's drawer and files
        @param legend   the meter's name on its dial, moved from */
    LevelMeterView(const view::PanelContext &context, std::string legend)
        : ArtworkMeterView(
              context,
              MeterArt{"level_meter_face.svg", "level_meter_glass.svg", 10.0f,
                       [legend = std::move(legend)](gfx::Canvas &c) { look::LevelMeterScale::Paint(c, legend); }},
              AngleDegrees(FaceDb(0.0)))
    {
    }

    /** One display frame, `seconds` long, with the frame's held peak (linear, 1.0 = 0 dBFS): the reading on the peak
        at once when it matches or tops it, the needle toward the reading through its mechanism; repainted, and true,
        when the tip has moved a tenth of a pixel from where it was last painted.
        @param peak     the frame's held peak, linear, 1.0 = 0 dBFS
        @param seconds  the frame's length, in seconds; a negative one counts as zero for the needle
        @return         true when the tip moved and the meter was repainted */
    bool Advance(double peak, double seconds)
    {
        const double reading = reading_.Step(peak, seconds, 0.0, kFallSeconds);
        needle_.Step(reading, std::max(seconds, 0.0), kNeedleRiseHz, kNeedleFallHz, kNeedleDamping);
        return PointNeedle(AngleDegrees(needle_db()));
    }

    /** What a digital level reads on the face: dBFS - kZeroDbfs.
        @param dbfs  the level, in dBFS
        @return      the reading, in the face's decibels */
    static constexpr double ReadingDb(double dbfs) noexcept
    {
        return dbfs - kZeroDbfs;
    }

    /** Where on the arc a reading sits, 0 (kMinDb, the left end) to 1 (kMaxDb, the right end).
        @param reading_db  the reading, in the face's decibels
        @return            the position along the arc, 0 to 1 */
    static double ScalePosition(double reading_db) noexcept
    {
        return look::LevelMeterScale::Position(reading_db);
    }

    /** The needle's angle for a reading, in degrees clockwise from straight up.
        @param reading_db  the reading, in the face's decibels
        @return            the angle, in degrees */
    static double AngleDegrees(double reading_db) noexcept
    {
        return look::LevelMeterScale::AngleDegrees(reading_db);
    }

    /** The reading in the face's decibels, the discharging level; silence reads under the scale.
        @return  the reading, in the face's decibels */
    double reading_db() const noexcept
    {
        return FaceDb(reading_.level());
    }

    /** Where the needle is, in the face's decibels.
        @return  the needle's position, in the face's decibels */
    double needle_db() const noexcept
    {
        return FaceDb(needle_.position());
    }

    /** A linear amplitude on the face, six decibels up; nothing - a discharge run down, or the needle's rest - is
        silence, well under the scale's floor: the needle's reading, and the level bargraph's.
        @param linear  the amplitude, linear, 1.0 = 0 dBFS
        @return        the reading, in the face's decibels; kMinDb less 100 for silence */
    static double FaceDb(double linear) noexcept
    {
        return linear > 1.0e-9 ? ReadingDb(20.0 * std::log10(linear)) : kMinDb - 100.0;
    }

  private:
    look::PeakDecay        reading_; ///< in linear amplitude, 1.0 = 0 dBFS: the detector
    look::NeedleBallistics needle_;  ///< the pointer, following the reading in the linear domain
};
} // namespace reckless::panel
