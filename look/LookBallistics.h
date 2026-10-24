#pragma once

#include <cmath>
#include <optional>

#include "LookMetrics.h"

namespace reckless::look
{
/**
    How the series' indicators move, written in seconds and stepped once a display frame with the time the frame took (a
    panel's Frame, view::FrameSteps), so a needle or a lamp is in the same place after the same milliseconds at any
    frame rate: the needle's second-order mechanism, the lamp's filament and the glow a header lamp shows on it, and the
    peak programme reading. No toolkit; the meters and lamps on the views (panel/) step them.
*/

/** A second-order needle: a damped spring in seconds, the movement of a VU meter's mechanism - a rise that runs a
    little past its mark and settles, a fall that is slower and does not - integrated from the elapsed time the frame
    hands it. The stiffness switches with the direction the needle has to travel (`rise_hz` toward a larger target,
    `fall_hz` toward a smaller one); the damping ratio is shared (0.7 overshoots by 4.6 %, 1 by nothing).
    Semi-implicit Euler in substeps of at most kSubstepSeconds, so a clamped frame of a tenth of a second is still
    stable at any stiffness a meter would use. */
class NeedleBallistics
{
  public:
    static constexpr double kSubstepSeconds = 0.002; ///< the longest integration substep, in seconds.

    /** Move toward `target` over `seconds`; returns the new position.
        @param target   the position the needle is pulled toward
        @param seconds  the time the frame took, in seconds; zero or less moves nothing
        @param rise_hz  the spring's natural frequency while the target is above the needle, in hertz
        @param fall_hz  the spring's natural frequency while the target is at or below the needle, in hertz
        @param damping  the damping ratio, 0.7 overshooting by 4.6 % and 1 by nothing
        @return         the needle's new position */
    double Step(double target, double seconds, double rise_hz, double fall_hz, double damping) noexcept
    {
        constexpr double kTwoPi = 6.283185307179586;
        while (seconds > 0.0)
        {
            const double dt    = seconds < kSubstepSeconds ? seconds : kSubstepSeconds;
            const double omega = kTwoPi * (target > position_ ? rise_hz : fall_hz);
            velocity_ += dt * (omega * omega * (target - position_) - 2.0 * damping * omega * velocity_);
            position_ += dt * velocity_;
            seconds -= dt;
        }
        return position_;
    }

    /** Put the needle at `position` at rest.
        @param position  the position to put it at; the velocity goes to zero */
    void Snap(double position) noexcept
    {
        position_ = position;
        velocity_ = 0.0;
    }

    /** @return the needle's position */
    double position() const noexcept
    {
        return position_;
    }

    /** @return the needle's velocity, in position units per second */
    double velocity() const noexcept
    {
        return velocity_;
    }

  private:
    double position_{0.0}; ///< where the needle is.
    double velocity_{0.0}; ///< how fast it is moving, in position units per second.
};

/** A lamp's filament: a first-order lag in seconds toward a brightness, quicker to light than to cool
    (`rise_seconds` and `fall_seconds` are the two time constants, the series' kLampRiseSeconds and kLampFallSeconds),
    for a lamp fed a raw per-block peak - the Amp's tubes, the Distortion's tubes, the Drive's lamp, the Limiter's
    CLIP lamp, the Pedalboard's output lamp - and for a lamp that only turns on and off, warming and cooling as a
    filament does: the Pedalboard's pedals' LEDs. Read at the display's rate a per-block peak is a
    flicker; through a filament it is a glow. A lamp whose feed is already the engine's own lagged brightness (the
    Vibe's photocell lamp, the Delay's held wet level) needs none of this. */
class LampGlow
{
  public:
    /** Lag toward `target` for `seconds`, exactly: the glow keeps e^(-seconds / tau) of its distance.
        @param target        the brightness the filament is driven toward, 0 to 1
        @param seconds       the time the frame took, in seconds
        @param rise_seconds  the time constant while the target is above the glow, in seconds
        @param fall_seconds  the time constant while the target is at or below the glow, in seconds
        @return              the new glow */
    double Step(double target, double seconds, double rise_seconds, double fall_seconds) noexcept
    {
        const double tau = target > glow_ ? rise_seconds : fall_seconds;
        glow_ += (target - glow_) * (1.0 - std::exp(-seconds / tau));
        return glow_;
    }

    /** Put the glow at `glow` outright.
        @param glow  the brightness to put it at, 0 to 1 */
    void Snap(double glow) noexcept
    {
        glow_ = glow;
    }

    /** @return the glow, 0 to 1 */
    double glow() const noexcept
    {
        return glow_;
    }

  private:
    double glow_{0.0}; ///< the filament's brightness.
};

/** The filament behind a lamp fed a raw peak: its rise and fall time constants, the series' (kLampRiseSeconds,
    kLampFallSeconds) unless a lamp says otherwise - `LampFilament{}` is the series' filament. */
struct LampFilament
{
    double rise_seconds = kLampRiseSeconds; ///< the time constant toward a brighter target, in seconds.
    double fall_seconds = kLampFallSeconds; ///< the time constant toward a dimmer target, in seconds.
};

/** A header lamp's glow as the lamp shows it: what it is fed, or that through a filament, moved only when the change is
    worth a repaint (`threshold`: 0.002 for a level, 0.004 to 0.005 behind a filament), so a lamp repaints only on the
    frames its glow moves. The views' LampView steps one. */
class LampLight
{
  public:
    /** @param threshold     the change in glow that is worth a repaint: 0.002 for a level, 0.004 to 0.005 behind a
                             filament
        @param initial_glow  the glow the lamp starts at, 0 to 1
        @param filament      the filament the target goes through, or an empty optional for a lamp that shows its feed
     */
    LampLight(double threshold, double initial_glow, std::optional<LampFilament> filament) noexcept
        : threshold_(threshold), glow_(initial_glow), filament_(filament)
    {
    }

    /** One display frame, `seconds` long: the target through the filament if there is one; true when the glow moved by
        more than the threshold, the frame a repaint is worth.
        @param target   the glow the lamp is fed, 0 to 1
        @param seconds  the time the frame took, in seconds
        @return         true when the glow moved by more than the threshold */
    bool Step(double target, double seconds) noexcept
    {
        const double next =
            filament_ ? filament_glow_.Step(target, seconds, filament_->rise_seconds, filament_->fall_seconds) : target;
        if (std::abs(next - glow_) <= threshold_)
        {
            return false;
        }
        glow_ = next;
        return true;
    }

    /** @return the glow as last shown, 0 to 1 */
    double glow() const noexcept
    {
        return glow_;
    }

  private:
    double                      threshold_;     ///< the change in glow worth a repaint.
    double                      glow_;          ///< the glow as last shown.
    std::optional<LampFilament> filament_;      ///< the filament, if the lamp has one.
    LampGlow                    filament_glow_; ///< the filament's state.
};

/** A peak programme meter's reading with a hold: it rises to a new peak at once, holds it for hold_seconds, then lets
    go of it as an RC discharge in seconds - each frame the reading first spends the frame on what is left of the hold
    and discharges for the remainder, keeping e^(-remainder / fall_seconds) of itself, then takes the frame's peak if
    that matches or tops it and starts the hold again - the law of the series' peak followers (reckless::PeakFollower,
    per sample at a fixed rate) read at the display's rate, with a peak-hold in front of it. So a peak is on the needle
    in the frame it arrives and stays there for the hold, a held tone stands still (every frame's peak renews the
    hold), a lower peak during the hold changes nothing, and once the hold has run out the reading is 8.7 dB down one
    time constant later and 20 dB down after 2.3 of them, the same at any frame rate: the frame that crosses the hold's
    end discharges for its remainder alone, so the hold is exact in seconds and not a count of frames; with a hold of
    zero it is the plain peak follower. The level bargraph's bar (no hold) and its peak line (a second's), and the
    needle level meter's reading (no hold), each fed the block peak the processor holds for the frame. Snapped to
    exactly zero under kFloor, so a long silence does not discharge it into denormals. */
class PeakDecay
{
  public:
    static constexpr double kFloor = 1.0e-12; ///< -240 dB: under anything a meter shows.

    /** `seconds` of the frame: the hold first, the discharge at the time constant `fall_seconds` for what the hold
        leaves; then `peak` if it matches or tops the reading, which restarts the hold of `hold_seconds`; returns the
        reading.
        @param peak          the frame's peak, zero or more
        @param seconds       the time the frame took, in seconds
        @param hold_seconds  how long a new peak is held, in seconds; zero for the plain peak follower
        @param fall_seconds  the discharge's time constant, in seconds
        @return              the reading */
    double Step(double peak, double seconds, double hold_seconds, double fall_seconds) noexcept
    {
        if (hold_remaining_ >= seconds)
        {
            hold_remaining_ -= seconds;
        }
        else
        {
            const double discharging = seconds - hold_remaining_;
            hold_remaining_          = 0.0;
            level_ *= std::exp(-discharging / fall_seconds);
        }
        if (peak >= level_)
        {
            level_          = peak;
            hold_remaining_ = hold_seconds;
        }
        if (level_ < kFloor)
        {
            level_ = 0.0;
        }
        return level_;
    }

    /** The reading put to `level` outright, with no hold on it.
        @param level  the reading to put it at, zero or more */
    void Snap(double level) noexcept
    {
        level_          = level;
        hold_remaining_ = 0.0;
    }

    /** @return the reading */
    double level() const noexcept
    {
        return level_;
    }

    /** How much of the hold is left, in seconds; zero once the reading is discharging.
        @return the hold left, in seconds */
    double hold_remaining() const noexcept
    {
        return hold_remaining_;
    }

  private:
    double level_{0.0};          ///< the reading.
    double hold_remaining_{0.0}; ///< the hold left, in seconds.
};
} // namespace reckless::look
