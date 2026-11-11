#pragma once
/// @file EarlyBackBuffer.h The back buffer a presenter takes **before** the product's frame runs, and the rules for
/// holding it until that frame is presented.
///
/// **Why it is taken early.** Acquiring a back buffer blocks where the display has none free, and on a saturated
/// queue that wait is most of a refresh. Taken at the present, as it was, the wait falls **between the product
/// sampling its input and the picture reaching the glass**, and is paid as input lag; taken before the frame runs,
/// the same wait falls before the input is sampled and the picture the player sees is that much fresher. Measured
/// on `jonny-macbook` with a 60 Hz panel, full screen with one frame drawn a refresh: tick to glass **49.26 ms
/// before and 33.27 after**, which is the unsaturated figure, with the interval's spread and the late frames
/// unmoved (docs/window/frame-pacing.md).
///
/// **The rules are here rather than in the presenter** because the holding is where this goes wrong: a frame that
/// is never presented must not leak the buffer nor take a second one, and a buffer of the wrong size must not be
/// presented after a resize. They need no GPU to state or to test, so they are a template over whatever a back end
/// calls a surface and are held by `tests/test_gpu_drawing.cpp` on every platform.

#include <utility>

#include "Geometry.h"

namespace reckless::window
{

/**
 * A back buffer taken before a frame was drawn, held until that frame is presented.
 * @tparam Surface What a back end hands out for a back buffer; empty-constructible, movable, and false in a
 *                 boolean test where it holds nothing.
 */
template <typename Surface> class EarlyBackBuffer
{
  public:
    /**
     * One taken now, if none of this size is already held. **A frame that was never presented keeps its buffer**:
     * taking another would hold two of the pool's three for one frame, and the one held is still good. One of
     * another size is let go first, since a resize has been and it can no longer be presented.
     * @param size    the back buffer wanted, in pixels
     * @param acquire what takes one from the back end; called at most once, and may answer nothing
     */
    template <typename Acquire> void TakeNow(gfx::PointI size, Acquire &&acquire)
    {
        if (held_ && size_ == size)
        {
            return; // the frame before this one never presented; its buffer is still the right one
        }
        Release();
        held_ = std::forward<Acquire>(acquire)();
        size_ = held_ ? size : gfx::PointI{};
    }

    /**
     * What to present, and the holder emptied either way: a resize between the taking and the present leaves a
     * buffer that cannot be shown, and the caller takes a fresh one rather than showing it.
     * @param size the back buffer the present wants, in pixels
     * @return     what was held where it is that size; nothing otherwise
     */
    Surface ForPresent(gfx::PointI size)
    {
        Surface held = (held_ && size_ == size) ? std::move(held_) : Surface{};
        Release();
        return held;
    }

    /** Let go of whatever is held, so the back end has it again: the window closing, the GPU lost, full screen
        left, or a size that makes it unpresentable. */
    void Release() noexcept
    {
        held_ = Surface{};
        size_ = gfx::PointI{};
    }

    /** @return whether one is held */
    bool holds() const noexcept
    {
        return static_cast<bool>(held_);
    }

    /** @return the size the one held was taken at; {0, 0} where none is */
    gfx::PointI size() const noexcept
    {
        return size_;
    }

  private:
    Surface     held_{}; ///< what was taken, or nothing
    gfx::PointI size_{}; ///< the size it was taken at
};

} // namespace reckless::window
