#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <initializer_list>
#include <string>
#include <vector>

#include "Canvas.h"
#include "Color.h"
#include "Geometry.h"
#include "LevelMeterView.h"
#include "LookBallistics.h"
#include "LookMeters.h"
#include "LookMetrics.h"
#include "View.h"

namespace reckless::panel
{
/**
    The series' level bargraph as a view: vertical peak bars side by side on one dark screen let into the plate - the
    Comp's IN and OUT, right of its gain-reduction meter, and the Tape's, right of its deck - each a bar with a
    peak-hold line over it, a dB scale with ticks between the first two bars, the bars' captions under them.

    The face is the needle level meter's (LevelMeterView: kMinDb -30 to kMaxDb +6, read six decibels over the digital
    level, kZeroDbfs, so a -6 dBFS peak stands on the 0 and full scale on the +6 at the top), but linear in decibels
    from bottom to top, so every 2 dB segment of the bar is the same height. From 0 to +6 the bar, its ticks and its
    figures are red; the rest cream.

    Advance() is fed each bar's block peak the core holds for the display frame and gives two readings per bar, both
    look::PeakDecay in the linear domain: the bar takes a new peak in the frame it arrives and discharges from it with
    no hold, an RC of kBarFallSeconds; and the peak line takes the same peak and holds it kPeakHoldSeconds before it
    discharges at the same rate, riding down on top of the bar. A held tone stands still; a level over full scale is
    read as it is, the bar and the line clamped at the top. A frame that moved neither a bar's top nor a line by half
    a pixel repaints nothing.
*/
class LevelBarsView final : public view::View
{
  public:
    /** The scale and the calibration: the needle level meter's, so the two meters agree figure for figure. */
    static constexpr double kMinDb = LevelMeterView::kMinDb;
    /** The scale's top, in the face's decibels. */
    static constexpr double kMaxDb = LevelMeterView::kMaxDb;
    /** The digital level, in dBFS, that reads 0 on the face. */
    static constexpr double kZeroDbfs = LevelMeterView::kZeroDbfs;

    /** The red zone's bottom, in the face's decibels: 0 (-6 dBFS) to the top. */
    static constexpr double kRedFromDb = 0.0;

    /** The bar's discharge time constant from the frame a peak arrives (no hold), and the peak line's hold and then
        its discharge, in seconds. */
    static constexpr double kBarFallSeconds = 0.25;
    /** How long the peak line holds a peak before it discharges, in seconds. */
    static constexpr double kPeakHoldSeconds = 1.0;
    /** The peak line's discharge time constant after its hold, in seconds: the bar's. */
    static constexpr double kPeakFallSeconds = kBarFallSeconds;

    /** The drawing's fixed sizes, in pixels: a bar's width, the scale column between the first two bars, the gap
        between further bars, the screen's padding, the caption row, and half a figure's row. */
    static constexpr int kBarWidth = 30;
    /** The scale column between the first two bars, in design pixels. */
    static constexpr int kScaleWidth = 44;
    /** The gap between bars past the first two, in design pixels. */
    static constexpr int kBarGap = 12;
    /** The screen's padding on every side, in design pixels. */
    static constexpr int kPadding = 10;
    /** The caption row under the bars, in design pixels. */
    static constexpr int kCaptionRow = 18;
    /** Half a figure's row, in design pixels: how far a figure reaches over and under the bars' top and bottom. */
    static constexpr int kFigureReach = 7;

    /** One bar per caption ("IN", "OUT"), on a screen of `screen` (the plugin's plate colour).
        @param captions  the text under each bar, in order; the strings must be valid for the call only
        @param screen    the screen's colour */
    LevelBarsView(std::initializer_list<const char *> captions, gfx::Color screen) : screen_(screen)
    {
        bars_.reserve(captions.size());
        for (const char *caption : captions)
        {
            bars_.push_back(Bar{std::string(caption), {}, {}, -1.0f, -1.0f});
        }
    }

    /** One display frame, `seconds` long, with the frame's held peaks (linear, 1.0 = 0 dBFS; one per bar, in order):
        each bar's level discharges by the frame and takes its peak if that tops it; each line spends the frame on its
        hold or its discharge and takes the peak if that tops it. Repainted, and true, when a bar's top or a line moved
        half a pixel from where it was last painted.
        @param peaks    each bar's held peak, linear, 1.0 = 0 dBFS; peaks past the last bar are ignored
        @param seconds  the frame's length, in seconds
        @return         true when a bar or a line moved and the view was repainted */
    bool Advance(std::initializer_list<double> peaks, double seconds)
    {
        bool        moved = false;
        std::size_t i     = 0;
        for (const double peak : peaks)
        {
            if (i >= bars_.size())
            {
                break;
            }
            auto &bar = bars_[i++];
            bar.level.Step(peak, seconds, 0.0, kBarFallSeconds);
            bar.peak.Step(peak, seconds, kPeakHoldSeconds, kPeakFallSeconds);
            const float level_px = HeightPixels(LevelMeterView::FaceDb(bar.level.level()));
            const float peak_px  = HeightPixels(LevelMeterView::FaceDb(bar.peak.level()));
            if (std::fabs(level_px - bar.painted_level_px) >= 0.5f || std::fabs(peak_px - bar.painted_peak_px) >= 0.5f)
            {
                bar.painted_level_px = level_px;
                bar.painted_peak_px  = peak_px;
                moved                = true;
            }
        }
        if (moved)
        {
            Repaint();
        }
        return moved;
    }

    /** How many bars the view holds.
        @return  the count of captions given */
    int bar_count() const noexcept
    {
        return static_cast<int>(bars_.size());
    }

    /** What a digital level reads on the face: dBFS - kZeroDbfs.
        @param dbfs  the level, in dBFS
        @return      the reading, in the face's decibels */
    static constexpr double ReadingDb(double dbfs) noexcept
    {
        return dbfs - kZeroDbfs;
    }

    /** Where on the bar a reading sits, 0 (kMinDb, the bottom) to 1 (kMaxDb, the top), linear in decibels and
        clamped at both ends.
        @param reading_db  the reading, in the face's decibels
        @return            the position up the bar, 0 to 1 */
    static double Position(double reading_db) noexcept
    {
        return (std::clamp(reading_db, kMinDb, kMaxDb) - kMinDb) / (kMaxDb - kMinDb);
    }

    /** A bar's level in the face's decibels; silence reads under the scale, a level over full scale over it.
        @param bar  the bar's index, less than bar_count()
        @return     the bar's level, in the face's decibels */
    double bar_db(int bar) const noexcept
    {
        return LevelMeterView::FaceDb(bars_[static_cast<std::size_t>(bar)].level.level());
    }

    /** A bar's peak line in the face's decibels.
        @param bar  the bar's index, less than bar_count()
        @return     the line's level, in the face's decibels */
    double peak_db(int bar) const noexcept
    {
        return LevelMeterView::FaceDb(bars_[static_cast<std::size_t>(bar)].peak.level());
    }

    /** The rectangle the bars stand in, from kMinDb at its bottom to kMaxDb at its top: the screen's padding off every
        side, the caption row off the bottom, half a figure's row off the top and the bottom.
        @return  the rectangle, in the view's local design pixels */
    gfx::RectF BarsArea() const noexcept
    {
        auto inner = LocalBounds().ToFloat().Reduced(static_cast<float>(kPadding));
        inner.RemoveFromBottom(static_cast<float>(kCaptionRow));
        return inner.Reduced(0.0f, static_cast<float>(kFigureReach));
    }

    /** Bar `bar`'s column: the bars and the scale one group centred on the screen - bar 0, the scale, bar 1, then any
        more at kBarGap.
        @param bar  the bar's index, less than bar_count()
        @return     the column, in the view's local design pixels, as tall as BarsArea() */
    gfx::RectF BarColumn(int bar) const noexcept
    {
        const int   count = bar_count();
        const float total = static_cast<float>(count * kBarWidth + kScaleWidth + std::max(0, count - 2) * kBarGap);
        const auto  area  = BarsArea();
        float       x     = area.CentreX() - total / 2.0f;
        for (int b = 0; b < bar; ++b)
        {
            x += static_cast<float>(kBarWidth + (b == 0 ? kScaleWidth : kBarGap));
        }
        return {x, area.y(), static_cast<float>(kBarWidth), area.height()};
    }

    /** The screen, each bar with its segments, peak line and caption, and the scale between the first two.
        @param c  the canvas, in the view's local pixels */
    void Paint(gfx::Canvas &c) override
    {
        const auto bounds = LocalBounds().ToFloat();
        const auto ink    = look::colors::kInk;

        // A dark screen let into the plate, the frames' weight round it.
        c.SetColor(screen_);
        c.FillRoundedRect(bounds, kCorner);
        c.SetColor(ink.WithAlpha(0.6f));
        c.DrawRoundedRect(bounds.Reduced(0.6f), kCorner, 1.2f);

        const auto  area   = BarsArea();
        const float bottom = area.bottom();
        const float red_y  = bottom - HeightPixels(kRedFromDb);
        c.SetFont(gfx::Font(kFigureSize));

        for (int b = 0; b < bar_count(); ++b)
        {
            const auto &bar    = bars_[static_cast<std::size_t>(b)];
            const auto  column = BarColumn(b);

            // The track, dark on the screen; the bar cream to the 0 and red over it; a segment line every 2 dB across
            // both, so the bar reads as eighteen segments, three of them red.
            c.SetColor(gfx::colors::kBlack.WithAlpha(0.35f));
            c.FillRoundedRect(column, 1.5f);
            const float level_y = bottom - HeightPixels(bar_db(b));
            if (level_y < bottom)
            {
                const float cream_top = std::max(level_y, red_y);
                c.SetColor(ink.WithAlpha(0.92f));
                c.FillRect(gfx::RectF{column.x(), cream_top, column.width(), bottom - cream_top});
                if (level_y < red_y)
                {
                    c.SetColor(look::meter_art::kRed);
                    c.FillRect(gfx::RectF{column.x(), level_y, column.width(), red_y - level_y});
                }
            }
            c.SetColor(screen_);
            for (double db = kMinDb + kSegmentDb; db < kMaxDb - 1.0e-9; db += kSegmentDb)
            {
                c.FillRect(gfx::RectF{column.x(), std::round(bottom - HeightPixels(db)) - 0.5f, column.width(), 1.0f});
            }

            // The peak line, in its zone's colour, on the bar's top when nothing is held over it.
            const double peak = peak_db(b);
            if (peak > kMinDb)
            {
                const float peak_y = std::max(area.y(), bottom - HeightPixels(peak) - kPeakLine / 2.0f);
                c.SetColor(ZoneColour(peak, 1.0f));
                c.FillRect(gfx::RectF{column.x(), peak_y, column.width(), kPeakLine});
            }

            // The caption under the bar, the series' caption face.
            c.SetColor(ink.WithAlpha(0.8f));
            c.SetFont(gfx::Font(look::kCaptionFontSize * look::kTextScale).Bold());
            c.DrawText(bar.caption,
                       gfx::RectF{column.x() - 10.0f, bottom + static_cast<float>(kFigureReach), column.width() + 20.0f,
                                  static_cast<float>(kCaptionRow)},
                       gfx::Justify::kCentred);
            c.SetFont(gfx::Font(kFigureSize));
        }

        // The scale between the first two bars (right of the first when it is alone): a minor tick every 2 dB and a
        // major one at each figure on both bars' facing edges, the figure between them; red from the 0 up.
        const auto  first   = BarColumn(0);
        const float left_x  = first.right();
        const float right_x = bar_count() > 1 ? BarColumn(1).x() : left_x + static_cast<float>(kScaleWidth);
        auto        tick    = [&](double db, float length, float alpha) {
            const float y = std::round(bottom - HeightPixels(db)) - 0.5f;
            c.SetColor(ZoneColour(db, alpha));
            c.FillRect(gfx::RectF{left_x + 1.0f, y, length, 1.0f});
            if (bar_count() > 1)
            {
                c.FillRect(gfx::RectF{right_x - 1.0f - length, y, length, 1.0f});
            }
        };
        for (double db = kMinDb; db <= kMaxDb + 1.0e-9; db += kSegmentDb)
        {
            tick(db, kMinorTick, 0.35f);
        }
        for (const double db : kFigures)
        {
            tick(db, kMajorTick, 0.7f);
            const float y      = bottom - HeightPixels(db);
            const auto  figure = gfx::RoundToInt(db);
            c.SetColor(ZoneColour(db, 0.75f));
            c.DrawText((figure > 0 ? "+" : "") + std::to_string(figure),
                       gfx::RectF{left_x + 1.0f + kMajorTick, y - static_cast<float>(kFigureReach),
                                  right_x - left_x - 2.0f - 2.0f * kMajorTick, 2.0f * static_cast<float>(kFigureReach)},
                       gfx::Justify::kCentred);
        }
    }

  private:
    /** The figures on the scale, in the face's decibels: the tens, the last six in threes. */
    static constexpr double kFigures[] = {-30.0, -20.0, -10.0, -6.0, -3.0, 0.0, 3.0, 6.0};

    /** A minor tick, and a segment line across the bar, every this many decibels. */
    static constexpr double kSegmentDb = 2.0;

    static constexpr float kMajorTick = 6.0f; ///< a figure's tick length, in design pixels
    static constexpr float kMinorTick = 3.0f; ///< a minor tick's length, in design pixels
    static constexpr float kPeakLine  = 2.0f; ///< the peak line's thickness, in design pixels
    static constexpr float kCorner    = 6.0f; ///< the screen's corner radius, in design pixels

    /** The figures' face: a size over the series' fine print, which read too small on a bar this tall. */
    static constexpr float kFigureSize = 11.5f;

    /** One bar: its caption, its two readings and where it was last painted. */
    struct Bar
    {
        std::string     caption;                 ///< the text under the bar
        look::PeakDecay level;                   ///< the bar, in linear amplitude: instant up, the RC down
        look::PeakDecay peak;                    ///< the line: instant up, held, the RC down
        float           painted_level_px{-1.0f}; ///< the bar's height when the meter last asked to repaint
        float           painted_peak_px{-1.0f};  ///< the line's height when the meter last asked to repaint
    };

    /** A mark's colour: red from the 0 up, the series' ink under it.
        @param reading_db  the mark's reading, in the face's decibels
        @param alpha       the opacity, 0 to 1
        @return            the colour */
    static gfx::Color ZoneColour(double reading_db, float alpha) noexcept
    {
        return (reading_db >= kRedFromDb ? look::meter_art::kRed : look::colors::kInk).WithAlpha(alpha);
    }

    /** A reading's height above the bars' bottom, in pixels, for the current bounds.
        @param reading_db  the reading, in the face's decibels, clamped to the scale
        @return            the height, in design pixels, 0 at kMinDb to the height of BarsArea() at kMaxDb */
    float HeightPixels(double reading_db) const noexcept
    {
        return static_cast<float>(Position(reading_db)) * BarsArea().height();
    }

    std::vector<Bar> bars_;   ///< the bars, in the order of the captions
    gfx::Color       screen_; ///< the screen's colour
};
} // namespace reckless::panel
