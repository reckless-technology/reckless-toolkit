#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "Canvas.h"
#include "Color.h"
#include "Geometry.h"
#include "LookBallistics.h"
#include "Path.h"

namespace reckless::look
{
/**
    The series' oscilloscope with no toolkit: the last 10 ms of a signal as a trace on a dark screen, the way a scope on
    the bench shows an amplifier's output - the Amp's, beside its tubes, and the NAM's, under its model screen - so a
    player sees the wave the clipping stages make: a clean sine, a rounded top, a square. A Scope is one screen's state
    and its drawing; the oscilloscope view that holds one, steps it once a display frame and
    paints it. Header only, and a template on the Source the audio thread writes (count(), Snapshot(),
    WindowSamples(), kLookBackWindows), so the look itself reads no signal processing and carries none.

    **The trace.** Once a display frame, Advance takes a snapshot of the source when its count has moved since the
    last, and draws its newest window (Source::WindowSamples) across the screen. A free-running sweep of a periodic
    signal would put the wave somewhere else every frame - a smear - so the sweep is triggered as a scope's is: the
    trace starts at a rising zero crossing that leaves a whole window after it, found by looking back through the two
    windows before the shown one (Source::kLookBackWindows; the snapshot's span is three windows for this, 30 ms, so a
    wave as slow as the low E's 12 ms always offers a crossing), and falls back to the newest window when there is
    none - silence, or a wave slower than the look-back.

    **Which crossing.** A guitar's wave crosses zero upward more than once a cycle - a strong second harmonic (the
    Rational and Triode laws add one), a chord's composite, the ripple on a clipped top - and a trigger that took the
    last crossing before the window would land on a different one of them every frame as the buffer advanced through
    the cycle, so the trace would flicker between two or three segments of the wave and the eye would see them all at
    once. Two things a bench scope does stop that. The trigger is armed: a rising crossing counts only once the wave
    has dipped below kArmFraction of the span's peak since the previous crossing, so the small crossings the
    harmonics and the ripple make are passed over and one crossing a cycle remains for most notes. And among the
    crossings that still qualify (a chord's), the one whose window best matches the trace shown last frame is taken -
    the normalised dot product of the two, a stable trigger's memory - so the picture stays locked to the phase it had
    and moves only when the wave itself does; with no last trace the latest qualifying crossing is taken, and with no
    qualifying crossing at all the latest plain one. A periodic wave then stands still on the screen and a change of
    shape reads as a change of shape.

    **The scale.** The trace is normalised to the recent peak - the shown window's peak through a filament that rises
    at once and falls over kScaleFallSeconds (LampGlow: a scope's auto-range) with a floor at kScaleFloor - so the
    wave's shape fills the screen at any level; the level is the tubes' or the lamp's business, and the screen says
    "shape". Silence, under kSilence, is a flat line at the centre. A centre line and two faint divisions above and
    below are the graticule; kInset keeps the trace off the screen's rounded edge.

    Everything drawn derives from the snapshot, the trigger and the scale; TriggerIndex is pure, so the tests hold it.
    Clear drops the trace for a screen nobody sees (the Pedalboard's miniature) until Advance runs again.
*/
class Scope
{
  public:
    static constexpr int    kInset            = 8;      ///< the room between the screen's edge and the trace, in pixels
    static constexpr int    kCornerRadius     = 5;      ///< the screen's corner radius, in pixels
    static constexpr double kScaleFallSeconds = 1.0;    ///< the auto-range's release: a decay lets the wave grow
    static constexpr double kScaleFloor       = 0.005;  ///< -46 dBFS: under this the range stops shrinking
    static constexpr double kSilence          = 1.0e-4; ///< -80 dBFS: the trace is a line
    static constexpr double kArmFraction =
        0.25; ///< the trigger arms once the wave dips this far below zero, of the peak

    /** Where the shown window starts in `span` (oldest first) for a window of `window` samples: a rising zero
        crossing at or before span.size() - window, within `look_back_windows` windows of it, armed by a dip below
        kArmFraction of the span's peak since the crossing before - the one whose window best matches `previous` (the
        trace shown last frame, `window` samples; empty for none), else the latest; the latest plain crossing when none
        is armed; span.size() - window when there is no crossing at all. Pure.
        @param span              the snapshot, oldest first
        @param window            the samples a shown window holds
        @param look_back_windows  how many windows before the newest whole one a crossing may start in
        @param previous          the window shown last frame, `window` samples, or empty for none
        @return                  the index in `span` where the shown window starts, 0 for a span shorter than a window
     */
    static int TriggerIndex(const std::vector<double> &span, int window, int look_back_windows,
                            const std::vector<double> &previous = {}) noexcept
    {
        const int n      = static_cast<int>(span.size());
        const int latest = n - window; // the newest whole window starts here
        if (latest <= 0)
        {
            return 0;
        }
        const auto at       = [&](int i) { return span[static_cast<std::size_t>(i)]; };
        const auto rising   = [&](int i) { return at(i) >= 0.0 && at(i - 1) < 0.0; };
        const int  earliest = std::max(1, latest - look_back_windows * window);

        // The arming level: a quarter of the span's peak. A crossing counts only once the wave has been that far
        // below zero since the crossing before it, so the small crossings a harmonic or a clipped top's ripple makes
        // about zero are passed over and a note keeps one trigger a cycle.
        double peak = 0.0;
        for (int i = 0; i < n; ++i)
        {
            peak = std::max(peak, std::fabs(at(i)));
        }
        const double arm = kArmFraction * peak;

        // The armed crossings in the look-back, oldest first, and the latest plain crossing as the fallback.
        int  candidates[64];
        int  num_candidates = 0;
        int  plain          = -1;
        bool armed          = at(0) <= -arm;
        for (int i = 1; i <= latest; ++i)
        {
            if (rising(i))
            {
                if (i >= earliest)
                {
                    plain = i;
                    if (armed && peak > 0.0)
                    {
                        if (num_candidates == 64) // more crossings than any wave the screen shows: keep the newest
                        {
                            std::copy(candidates + 1, candidates + 64, candidates);
                            --num_candidates;
                        }
                        candidates[num_candidates++] = i;
                    }
                }
                armed = false;
            }
            if (at(i) <= -arm)
            {
                armed = true;
            }
        }
        if (num_candidates == 0)
        {
            return plain >= 0 ? plain : latest;
        }
        if (num_candidates == 1 || static_cast<int>(previous.size()) != window)
        {
            return candidates[num_candidates - 1];
        }
        // Several armed crossings (a chord's): the one whose window most resembles the trace shown last frame, by the
        // normalised dot product, so the sweep keeps the phase it had; the latest on a tie.
        double previous_energy = 0.0;
        for (double v : previous)
        {
            previous_energy += v * v;
        }
        int    best       = candidates[num_candidates - 1];
        double best_score = -2.0;
        for (int c = 0; c < num_candidates; ++c)
        {
            const int start  = candidates[c];
            double    dot    = 0.0;
            double    energy = 0.0;
            for (int k = 0; k < window; ++k)
            {
                const double v = at(start + k);
                dot += v * previous[static_cast<std::size_t>(k)];
                energy += v * v;
            }
            const double score =
                energy > 0.0 && previous_energy > 0.0 ? dot / std::sqrt(energy * previous_energy) : 0.0;
            if (score >= best_score)
            {
                best_score = score;
                best       = start;
            }
        }
        return best;
    }

    /** The screen's inner plot in `bounds`: the trace's room, kInset clear of the rounded edge.
        @param bounds  the screen's bounds
        @return        the plot's bounds, `bounds` reduced by kInset on every side */
    static gfx::RectF PlotBounds(gfx::RectF bounds) noexcept
    {
        return bounds.Reduced(static_cast<float>(kInset));
    }

    /** A scope showing nothing, its range on the floor. */
    Scope()
    {
        range_.Snap(kScaleFloor);
    }

    /** Once a display frame: a fresh snapshot of `source` when it has published since the last, the trigger found and
        the range stepped by `seconds`; true when the trace moved, the frame a repaint is due.
        @tparam Source  the signal's ring: `count()`, `Snapshot(std::vector<double> &)`, `WindowSamples()` and
                        `kLookBackWindows`, as the WaveformHistory has them
        @param source   the ring the audio thread writes
        @param seconds  the time the frame took, in seconds
        @return         true when a new snapshot was taken and the trace moved */
    template <typename Source> bool Advance(const Source &source, double seconds)
    {
        const std::uint64_t count = source.count();
        if (copied_ && count == copied_count_)
        {
            // Nothing new has landed (most frames at 165 Hz, every frame while the host is stopped): the trace stands.
            return false;
        }
        if (!source.Snapshot(scratch_))
        {
            return false; // torn: keep the last trace, try again next frame
        }
        copied_       = true;
        copied_count_ = count;
        std::swap(span_, scratch_);

        const int window = source.WindowSamples();
        trigger_         = TriggerIndex(span_, window, Source::kLookBackWindows, previous_);

        // The range: the shown window's peak, up at once and down over kScaleFallSeconds, never under the floor; and
        // the shown window kept, whole, for the next frame's trigger to lock to (a partial span - the first frames
        // after a Prepare - leaves nothing to lock to).
        double    peak = 0.0;
        const int end  = std::min(static_cast<int>(span_.size()), trigger_ + window);
        for (int i = trigger_; i < end; ++i)
        {
            peak = std::max(peak, std::fabs(span_[static_cast<std::size_t>(i)]));
        }
        if (end - trigger_ == window)
        {
            previous_.assign(span_.begin() + trigger_, span_.begin() + end);
        }
        else
        {
            previous_.clear();
        }
        const double target = std::max(peak, kScaleFloor);
        if (target >= scale_)
        {
            range_.Snap(target); // up at once: a peak is never clipped by the screen
            scale_ = target;
        }
        else
        {
            scale_ = range_.Step(target, std::max(seconds, 0.0), kScaleFallSeconds, kScaleFallSeconds);
        }
        return true;
    }

    /** The trace dropped and the range back on its floor, standing still until Advance runs again - for a screen that
        has gone from view; true when there was a trace to drop.
        @return true when there was a trace to drop */
    bool Clear() noexcept
    {
        if (span_.empty() && !copied_)
        {
            return false;
        }
        span_.clear();
        previous_.clear();
        trigger_ = 0;
        copied_  = false;
        range_.Snap(kScaleFloor);
        scale_ = kScaleFloor;
        return true;
    }

    /** The screen filling `bounds`, its graticule, and the trace of a window of `window` samples from the trigger: the
        screen in `screen`, the graticule in `ink` at a fraction, the trace in `accent`.
        @param c       the canvas drawn on
        @param bounds  the screen's bounds, in design pixels
        @param window  the samples a shown window holds
        @param ink     the graticule's and the screen's hairline's colour, drawn at a fraction of its alpha
        @param accent  the trace's colour
        @param screen  the screen's colour */
    void Paint(gfx::Canvas &c, gfx::RectF bounds, int window, gfx::Color ink, gfx::Color accent, gfx::Color screen)
    {
        // The screen: a dark plate let into the panel, with a hairline of the ink round it.
        c.SetColor(screen);
        c.FillRoundedRect(bounds, static_cast<float>(kCornerRadius));
        c.SetColor(ink.WithAlpha(0.25f));
        c.DrawRoundedRect(bounds.Reduced(0.5f), static_cast<float>(kCornerRadius), 1.0f);

        // The graticule: the centre line, and a division at half the range above and below it.
        const auto  plot   = PlotBounds(bounds);
        const float mid_y  = plot.CentreY();
        const float half_y = plot.height() * 0.25f;
        c.SetColor(ink.WithAlpha(0.18f));
        c.DrawHorizontalLine(gfx::RoundToInt(mid_y), plot.x(), plot.right());
        c.SetColor(ink.WithAlpha(0.08f));
        c.DrawHorizontalLine(gfx::RoundToInt(mid_y - half_y), plot.x(), plot.right());
        c.DrawHorizontalLine(gfx::RoundToInt(mid_y + half_y), plot.x(), plot.right());
        for (int i = 1; i < 4; ++i)
        {
            const float x = plot.x() + plot.width() * static_cast<float>(i) / 4.0f;
            c.DrawVerticalLine(gfx::RoundToInt(x), plot.y(), plot.bottom());
        }

        // The trace: the shown window from the trigger, one point per pixel column at most (a window of thousands of
        // samples across a few hundred pixels: the nearest sample to each column, so the path stays light and the
        // wave's shape is what the eye reads), normalised to the range.
        const int end = std::min(static_cast<int>(span_.size()), trigger_ + window);
        const int n   = end - trigger_;
        c.SetColor(accent);
        if (n < 2)
        {
            c.DrawHorizontalLine(gfx::RoundToInt(mid_y), plot.x(), plot.right());
            return;
        }
        const int    columns = std::max(2, std::min(n, static_cast<int>(plot.width())));
        const double gain    = 0.5 * plot.height() / std::max(scale_, kScaleFloor);
        trace_.Clear();
        for (int col = 0; col < columns; ++col)
        {
            const double share = static_cast<double>(col) / static_cast<double>(columns - 1);
            const int    i     = trigger_ + static_cast<int>(std::lround(share * (n - 1)));
            const double v =
                std::clamp(span_[static_cast<std::size_t>(i)] * gain, -0.5 * plot.height(), 0.5 * plot.height());
            const float x = plot.x() + static_cast<float>(share) * plot.width();
            const float y = mid_y - static_cast<float>(v);
            if (col == 0)
            {
                trace_.MoveTo(x, y);
            }
            else
            {
                trace_.LineTo(x, y);
            }
        }
        c.StrokePath(trace_, gfx::Stroke(1.6f, gfx::Stroke::Join::kCurved, gfx::Stroke::Cap::kRounded));
    }

    /** For the tests: the samples as last taken (oldest first), where the trace starts in them, and the range.
        @return the snapshot as last taken, oldest first */
    const std::vector<double> &span() const noexcept
    {
        return span_;
    }

    /** @return where the trace starts in span() */
    int trigger() const noexcept
    {
        return trigger_;
    }

    /** @return the range the trace is normalised to, an absolute sample value of at least kScaleFloor */
    double scale() const noexcept
    {
        return scale_;
    }

  private:
    std::vector<double> span_;               ///< the snapshot as last taken, oldest first.
    std::vector<double> scratch_;            ///< the snapshot being taken, swapped with span_ once whole.
    std::vector<double> previous_;           ///< the window shown last frame: what the trigger locks to
    int                 trigger_{0};         ///< where the trace starts in span_.
    double              scale_{kScaleFloor}; ///< the range the trace is normalised to.
    LampGlow            range_;              ///< the range's filament: up at once, down over kScaleFallSeconds.
    std::uint64_t       copied_count_{0};    ///< the source's count at the last snapshot.
    bool                copied_{false};      ///< true once a snapshot has been taken since Clear().
    gfx::Path           trace_;              ///< the trace's path, kept between frames to reuse its storage.
};
} // namespace reckless::look
