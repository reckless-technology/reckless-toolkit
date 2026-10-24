#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>

namespace reckless::view
{
/**
    The seconds a panel's frame is handed (Panel::Frame): the time since the frame before, so a lamp's filament or a
    needle's ballistics, written in seconds, read the same at 60 Hz and at 165. The first frame is a nominal thirtieth
    of a second, and no frame is longer than kMaxStepSeconds: after a stall, a hidden window or the first frame, an
    animation resumes from where it was rather than leaping. A window with only a timer (window/'s) steps it
    from the steady clock.
*/
class FrameSteps
{
  public:
    static constexpr double kFirstStepSeconds = 1.0 / 30.0; ///< the first frame's step, in seconds
    static constexpr double kMaxStepSeconds   = 0.1;        ///< the longest step a frame is handed, in seconds

    /** The step to the frame at `now_seconds`, on a clock in seconds that only counts up.
        @param now_seconds  the time of this frame, in seconds on a monotonic clock of the caller's
        @return             the seconds since the frame before, in [0, kMaxStepSeconds]; kFirstStepSeconds for the first
     */
    double Next(double now_seconds) noexcept
    {
        const double elapsed = last_seconds_ < 0.0 ? kFirstStepSeconds : now_seconds - last_seconds_;
        last_seconds_        = now_seconds;
        ++frames_;
        return std::clamp(elapsed, 0.0, kMaxStepSeconds);
    }

    /** The step to now, on the steady clock.
        @return  the seconds since the frame before, in [0, kMaxStepSeconds]; kFirstStepSeconds for the first */
    double Next() noexcept
    {
        const auto since = std::chrono::steady_clock::now().time_since_epoch();
        return Next(std::chrono::duration<double>(since).count());
    }

    /** @return  how many frames have been stepped */
    std::int64_t frames() const noexcept
    {
        return frames_;
    }

  private:
    double       last_seconds_{-1.0}; ///< the clock's reading at the last frame, in seconds; negative before the first
    std::int64_t frames_{0};          ///< frames stepped so far
};
} // namespace reckless::view
