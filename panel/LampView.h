#pragma once

#include <optional>

#include "Canvas.h"
#include "Color.h"
#include "Geometry.h"
#include "LookBallistics.h"
#include "LookDrawing.h"
#include "LookMetrics.h"
#include "View.h"

namespace reckless::panel
{
/**
    The header lamp as a view: the jewel at the header's right end (look::DrawLamp), fed a target once a display frame
    and repainting only itself, and only when its glow moved by more than its threshold.

    Two kinds of lamp: one that shows what it is fed (the Vibe's photocell, the Mod's LFO, the Delay's repeats - already
    smooth quantities), and one behind a filament (the Drive's clipper drive, a per-block peak that would flicker read
    raw), rising and falling in the series' times - look::LampLight. `threshold` is the change in glow worth a repaint -
    0.002 for a level, 0.004 to 0.005 behind a filament. The lamp is paint, not a control: it takes no mouse, and the
    plate and the header under it are the panel's to paint, so a repaint of its strip paints them clipped to it
    (DrawHeader skips its glyph layout when the clip excludes them), then the lamp on top.

    PlaceIn(header) sits it on the header's lamp strip; the view is the strip, so DrawLamp reads it as a header whose
    strip is its whole width.
*/
class LampView final : public view::View
{
  public:
    /** The filament behind a lamp fed a raw peak: the series' rise and fall unless a lamp says otherwise -
        `Filament{}` is the series' filament. */
    using Filament = look::LampFilament;

    /** A lamp that shows what it is fed: `accent` lights it, `rim` is its hairline, `initial_glow` is what it shows
        before its first Step.
        @param accent        the colour that lights the jewel
        @param rim           the colour of its hairline
        @param threshold     the change in glow, 0 to 1, worth a repaint
        @param initial_glow  the glow shown before the first Step, 0 to 1 */
    LampView(gfx::Color accent, gfx::Color rim, double threshold, double initial_glow = 0.0)
        : LampView(accent, rim, threshold, initial_glow, std::nullopt)
    {
    }

    /** A lamp behind a filament: the target goes through a LampGlow with the filament's times; dark until its first
        Step.
        @param accent     the colour that lights the jewel
        @param rim        the colour of its hairline
        @param threshold  the change in glow, 0 to 1, worth a repaint
        @param filament   the rise and fall times of the glow */
    LampView(gfx::Color accent, gfx::Color rim, double threshold, Filament filament)
        : LampView(accent, rim, threshold, 0.0, filament)
    {
    }

    /** A flag is neither a threshold nor a glow: a filament is a Filament. (A bool would otherwise convert to the
        threshold and compile.) */
    LampView(gfx::Color, gfx::Color, bool, double) = delete;

    /** One display frame, `seconds` long: the target through the filament if there is one, then a repaint when the
        glow moved by more than the threshold.
        @param target   the glow to show, 0 to 1
        @param seconds  the frame's length, in seconds */
    void Step(double target, double seconds)
    {
        if (light_.Step(target, seconds))
        {
            Repaint();
        }
    }

    /** The lamp's strip at the header's right end.
        @param header  the header's bounds, in design pixels */
    void PlaceIn(gfx::RectI header)
    {
        SetBounds(look::LampStrip(header));
    }

    /** The glow shown now.
        @return  the glow, 0 to 1 */
    double glow() const noexcept
    {
        return light_.glow();
    }

    /** Draws the jewel over the whole view.
        @param c  the canvas, in the view's local pixels */
    void Paint(gfx::Canvas &c) override
    {
        look::DrawLamp(c, LocalBounds(), light_.glow(), accent_, rim_);
    }

  private:
    /** The one constructor the public two share.
        @param accent        the colour that lights the jewel
        @param rim           the colour of its hairline
        @param threshold     the change in glow, 0 to 1, worth a repaint
        @param initial_glow  the glow shown before the first Step, 0 to 1
        @param filament      the filament, or empty for a lamp that shows what it is fed */
    LampView(gfx::Color accent, gfx::Color rim, double threshold, double initial_glow, std::optional<Filament> filament)
        : accent_(accent), rim_(rim), light_(threshold, initial_glow, filament)
    {
        SetInterceptsMouse(false, false); // the header's, for the mouse: the lamp is paint, not a control
    }

    gfx::Color      accent_; ///< the colour that lights the jewel
    gfx::Color      rim_;    ///< the colour of its hairline
    look::LampLight light_;  ///< the glow, through the filament if there is one
};
} // namespace reckless::panel
