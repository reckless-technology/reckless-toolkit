#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <optional>
#include <span>
#include <string_view>
#include <utility>

#include "Artwork.h"
#include "CachedPicture.h"
#include "Canvas.h"
#include "Geometry.h"
#include "LookBallistics.h"
#include "LookGeometry.h"
#include "LookMeters.h"
#include "PanelView.h"
#include "Path.h"
#include "View.h"

namespace reckless::panel
{
/** What a backlit meter is drawn from: its two layers' files by name, the needle blade's half width in the artwork's
    units (the gain-reduction meter's is 8, the small level meter's 10, so it reads at its size), and its scale's
    figures and legend, painted on a canvas already in the artwork's space and clipped to the dial window. */
struct MeterArt
{
    std::string_view                   face;             ///< the face layer's file, by the format's asset name
    std::string_view                   glass;            ///< the glass layer's file, by the format's asset name
    float                              blade_half_width; ///< the needle blade's half width, in the artwork's units
    std::function<void(gfx::Canvas &)> paint_scale;      ///< paints the figures and legend, in the artwork's space
};

/**
    A seventies backlit analog meter drawn from the series' SVG artwork, as a view: the Comp's and the Limiter's
    gain-reduction meter, and the small IN / OUT level meter. The artwork is one 1000 x 1000 space (look's
    meter_art): a face - the dark bezel, the backlit amber dial with its vignette, the arc and the ticks - and, over the
    needle, a glass highlight and the bezel's screws, each an SVG file the format carries by name
    (view::PanelContext::asset). The housing, 920 x 720 of the space, is fitted to the view's bounds at one uniform
    scale, centred and never stretched.

    The face and the glass are two pictures the view keeps (view::CachedPicture's kTwiceScale: twice the physical pixel
    scale the paint finds, drawn back at its size), each drawn in its own pixels (DrawInPixels), since the shadow is
    blurred a pixel at a time, by the format's toolkit (view::PanelContext::draw_artwork: gfx::DrawArtwork on Skia, the
    shadow blurred as juce::DropShadow blurs it): the housing's soft shadow and the face's document, then the scale's
    figures and legend set over them in the series' serif (the art's paint_scale, look's scales, since an SVG
    parser sets text in whatever falls back for Helvetica); and the glass's document alone. They are dropped on a resize
    and drawn again by the next paint at the scale it finds. A frame is the two pictures and the needle between them:
    the artwork's polygon as a path, a black blade with a red tip, turned about the pivot (500, 880) and clipped to the
    dial window so its base stays behind the bezel, its shadow the same blade filled first at an offset. What a meter is
    drawn from is a value (MeterArt), and where its needle points the meter's to say (PointNeedle), which repaints it
    only when the tip has moved a tenth of a pixel: the gain-reduction meter and the level meter are each this view and
    the ballistics that move their needle.
*/
class ArtworkMeterView : public view::View
{
  public:
    /** Drops both pictures: they are the bounds' and the scale's, drawn again by the next paint. */
    void Resized() override
    {
        // The pictures are the bounds' and the scale's: dropped here, drawn again by the next paint at the scale it
        // finds.
        face_.Drop();
        glass_.Drop();
    }

    /** The face's picture, the needle turned about the pivot with its shadow, then the glass's picture.
        @param c  the canvas, in the view's local pixels */
    void Paint(gfx::Canvas &c) override
    {
        const auto bounds = LocalBounds().ToFloat();
        c.SetResampling(gfx::Resampling::kHigh); // each picture stretched back over the bounds at the best filter
        face_.DrawInPixels(c, LocalBounds(), [this](gfx::Canvas &on, float scale) { PaintFace(on, scale); });

        // The needle turned about the artwork's pivot, clipped to the dial window, with its shadow: the one thing drawn
        // each frame.
        {
            gfx::ScopedState clip(c);
            const auto       art = look::FitTransform(look::meter_art::kHousing, bounds);
            c.ClipToPath(window_, art);

            const auto turned =
                gfx::Affine::Rotation(Radians(needle_degrees_), look::meter_art::kPivot.x, look::meter_art::kPivot.y)
                    .FollowedBy(art);
            c.SetColor(gfx::colors::kBlack.WithAlpha(0.28f));
            c.FillPath(blade_, gfx::Affine::Translation(6.0f, 8.0f).FollowedBy(turned));
            c.SetColor(look::meter_art::kNeedle);
            c.FillPath(blade_, turned);
            c.SetColor(look::meter_art::kRed);
            c.FillPath(tip_, turned);
        }
        glass_.DrawInPixels(c, LocalBounds(), [this](gfx::Canvas &on, float scale) { PaintGlass(on, scale); });
    }

  protected:
    /** The meter drawn from `art`, its layers from the format's assets, its needle resting at `rest_degrees`.
        @param context       the panel's context: its `draw_artwork` and `asset` are read once; either missing leaves
                             that layer undrawn
        @param art           the layers' files, the blade's half width and the scale's painter, moved from
        @param rest_degrees  the needle's first angle, in degrees clockwise from straight up */
    ArtworkMeterView(const view::PanelContext &context, MeterArt art, double rest_degrees)
        : draw_artwork_(context.draw_artwork),
          face_svg_(context.asset ? context.asset(art.face) : std::span<const std::uint8_t>()),
          glass_svg_(context.asset ? context.asset(art.glass) : std::span<const std::uint8_t>()),
          paint_scale_(std::move(art.paint_scale)), needle_degrees_(rest_degrees), painted_degrees_(rest_degrees)
    {
        // The needle - the artwork's polygon and its red tip, about the artwork's pivot - and the dial window that
        // clips it, in the artwork's units: every frame turns and draws these, none rebuilds them.
        using look::meter_art::kPivot;
        const float w = art.blade_half_width;
        blade_.MoveTo(kPivot.x - w, kPivot.y);
        blade_.LineTo(kPivot.x + w, kPivot.y);
        blade_.LineTo(kPivot.x + w / 4.0f, 260.0f);
        blade_.LineTo(kPivot.x, kPivot.y - look::meter_art::kNeedleTip);
        blade_.LineTo(kPivot.x - w / 4.0f, 260.0f);
        blade_.Close();
        tip_.MoveTo(499.0f, 300.0f);
        tip_.LineTo(501.0f, 300.0f);
        tip_.LineTo(500.6f, 265.0f);
        tip_.LineTo(500.0f, 255.0f);
        tip_.LineTo(499.4f, 265.0f);
        tip_.Close();
        window_.AddRoundedRect(look::meter_art::kWindow, look::meter_art::kWindowCorner);
    }

    /** The needle at `degrees`, clockwise from straight up, from now on; repainted, and true, when its tip has moved a
        tenth of a pixel from where it was last painted - against what was painted, not the last call, so a slow creep
        is not lost to a per-frame threshold at a high frame rate.
        @param degrees  the needle's angle, in degrees clockwise from straight up
        @return         true when the tip moved and the meter was repainted */
    bool PointNeedle(double degrees)
    {
        needle_degrees_ = degrees;
        if (TipTravelPixels(painted_degrees_, degrees) < 0.1)
        {
            return false;
        }
        painted_degrees_ = degrees;
        Repaint();
        return true;
    }

  private:
    /** How far the needle's tip moves, in the view's pixels, between two angles in degrees.
        @param from_degrees  the first angle, in degrees clockwise from straight up
        @param to_degrees    the second angle, in degrees clockwise from straight up
        @return              the distance the tip travels, in design pixels, never negative */
    double TipTravelPixels(double from_degrees, double to_degrees) const noexcept
    {
        return std::fabs(to_degrees - from_degrees) * (std::numbers::pi / 180.0) * look::meter_art::kNeedleTip *
               look::FitScale(look::meter_art::kHousing, LocalBounds().ToFloat());
    }

    /** An angle in radians for the canvas's rotation.
        @param degrees  the angle, in degrees
        @return         the angle, in radians */
    static float Radians(double degrees) noexcept
    {
        return static_cast<float>(degrees * (std::numbers::pi / 180.0));
    }

    /** The artwork's units onto `scale` pixels a design pixel of the current bounds: the housing fitted, then
        scaled - every number the layers are drawn with.
        @param scale  the pixels a design pixel lands on, as the cached picture draws at
        @return       the transform from the artwork's units to the picture's pixels */
    gfx::Affine ArtAt(float scale) const
    {
        return look::FitTransform(look::meter_art::kHousing, LocalBounds().ToFloat())
            .FollowedBy(gfx::Affine::Scale(scale));
    }

    /** The face's picture: the housing let into the plate with a drop shadow (the artwork's own bezel has none: on a
        panel it would float), then the artwork - bezel, backlit dial, the arc and the ticks - then the figures and the
        legend, the drawing in the artwork's units through the fit, so the figures' sizes and places are the SVG's
        numbers.
        @param on     the canvas of the face's picture, in its own pixels
        @param scale  the pixels a design pixel lands on */
    void PaintFace(gfx::Canvas &on, float scale) const
    {
        const auto art = ArtAt(scale);
        if (draw_artwork_)
        {
            gfx::Path housing;
            housing.AddRoundedRect(look::meter_art::kHousing, look::meter_art::kHousingCorner);
            housing.ApplyTransform(art);
            draw_artwork_(
                on, gfx::ArtworkLayer{face_svg_, art,
                                      gfx::Shadow{std::move(housing),
                                                  gfx::colors::kBlack.WithAlpha(0.5f),
                                                  gfx::RoundToInt(8.0f * scale),
                                                  {gfx::RoundToInt(2.0f * scale), gfx::RoundToInt(3.0f * scale)}}});
        }
        gfx::ScopedState clip(on);
        on.ClipToPath(window_, art);
        on.AddTransform(art);
        if (paint_scale_)
        {
            paint_scale_(on);
        }
    }

    /** The glass's picture: the highlight and the screws, over the needle.
        @param on     the canvas of the glass's picture, in its own pixels
        @param scale  the pixels a design pixel lands on */
    void PaintGlass(gfx::Canvas &on, float scale) const
    {
        if (draw_artwork_)
        {
            draw_artwork_(on, gfx::ArtworkLayer{glass_svg_, ArtAt(scale), std::nullopt});
        }
    }

    std::function<void(gfx::Canvas &, const gfx::ArtworkLayer &)> draw_artwork_; ///< the format's artwork drawer
    std::span<const std::uint8_t>                                 face_svg_;     ///< the housing and the backlit dial
    std::span<const std::uint8_t>                                 glass_svg_;    ///< the highlight and the screws
    std::function<void(gfx::Canvas &)> paint_scale_;     ///< the scale's figures and legend, in the artwork's space
    double                             needle_degrees_;  ///< where the needle points: PointNeedle's last
    double                             painted_degrees_; ///< where it pointed when the meter last asked to repaint

    /** The needle's geometry in the artwork's units, built once: the blade, the red tip and the dial window. */
    gfx::Path blade_;  ///< the needle's blade, about the pivot
    gfx::Path tip_;    ///< the blade's red tip
    gfx::Path window_; ///< the dial window that clips the needle

    view::CachedPicture face_{view::CachedPicture::Kind::kTwiceScale};  ///< everything under the needle
    view::CachedPicture glass_{view::CachedPicture::Kind::kTwiceScale}; ///< everything over it
};

/**
    The gain-reduction meter as a view: the needle rests at 0 on the right and swings left as a dynamics plugin takes
    gain away, 0 to kFullScaleDb (30, the artwork's scale) on look::GainReductionScale's arc, which opens up the first
    few decibels the way a VU's does. Advance() moves the needle with a mechanism's inertia of its own (a meter's
    ballistics, not the compressor's - the attack and release are already in the number): a second-order
    look::NeedleBallistics in seconds, stepped once a display frame, so the needle is in the same place after the same
    milliseconds at 60 Hz and at 165. Rising it runs a few percent past the reading and settles, as a VU's does; falling
    it is half as stiff and comes to rest on the 0 stop. The Comp's and the Limiter's.
*/
class GainReductionMeterView final : public ArtworkMeterView
{
  public:
    /** The artwork's full scale, in decibels of reduction, which a plugin's meter range is held to. */
    static constexpr double kFullScaleDb = look::GainReductionScale::kFullScaleDb;

    /** The needle's stiffness each way (the mechanism's natural frequency, Hz) and its damping ratio: 90 % of a rise
        in about 80 ms, 4 to 5 % past the mark, within 2 % from 300 ms; down to a tenth in about 170 ms. */
    static constexpr double kRiseHz = 6.0;
    /** The needle's stiffness falling, in hertz: half the rising stiffness. */
    static constexpr double kFallHz = 3.0;
    /** The needle's damping ratio, both ways. */
    static constexpr double kDamping = 0.75;

    /** The meter on the vu artwork, its needle at rest on 0 dB of reduction.
        @param context  the panel's context, which carries the artwork's drawer and files */
    explicit GainReductionMeterView(const view::PanelContext &context)
        : ArtworkMeterView(context,
                           MeterArt{"vu_meter_face.svg", "vu_meter_glass.svg", 8.0f,
                                    [](gfx::Canvas &c) { look::GainReductionScale::Paint(c); }},
                           AngleDegrees(0.0))
    {
    }

    /** One display frame, `seconds` long: the needle toward the current reduction in dB; repainted, and true, when
        the tip has moved a tenth of a pixel from where it was last painted - against what was painted, not the last
        step, so a slow creep is not lost to a per-frame threshold at a high frame rate.
        @param target_db  the reduction the needle moves toward, in decibels, 0 to kFullScaleDb
        @param seconds    the frame's length, in seconds
        @return           true when the tip moved and the meter was repainted */
    bool Advance(double target_db, double seconds)
    {
        needle_.Step(target_db, seconds, kRiseHz, kFallHz, kDamping);
        return PointNeedle(AngleDegrees(needle_.position()));
    }

    /** Where on the arc a reduction sits, 0 (none, the right end) to 1 (full scale, the left end).
        @param gr_db  the reduction, in decibels
        @return       the position along the arc, 0 to 1 */
    static double ScalePosition(double gr_db) noexcept
    {
        return look::GainReductionScale::Position(gr_db);
    }

    /** The needle's angle for a reduction, in degrees clockwise from straight up: +34 at 0 dB, -34 at full scale.
        @param gr_db  the reduction, in decibels
        @return       the angle, in degrees clockwise from straight up */
    static double AngleDegrees(double gr_db) noexcept
    {
        return look::GainReductionScale::AngleDegrees(gr_db);
    }

    /** Where the needle's mechanism stands now.
        @return  the needle's position, in decibels of reduction */
    double needle_db() const noexcept
    {
        return needle_.position();
    }

  private:
    look::NeedleBallistics needle_; ///< the needle's mechanism, in decibels of reduction
};
} // namespace reckless::panel
