#pragma once

#include <atomic>

namespace reckless::core
{
/**
    A meter reading held between the audio thread and the display: the largest value published since the reader
    last consumed it. A block is 1.3 ms and a display frame 7 to 33, so a meter that showed the last block's value
    would miss a transient that came and went between two frames; the audio thread raises this at the end of every
    block (Publish, a lock-free maximum against the reader's consuming store), the editor's frame clock reads it and
    then consumes it (Consume) - lowering it to a fallback by compare-exchange, so a block that raised it between the
    read and the consume keeps its value and the next frame sees it.

    Two fallbacks: a peak meter that only rises to a reading consumes to zero, so a frame with no block in it moves
    nothing; a gain-reduction meter that follows its reading down as well as up consumes to the last block's own
    value (`last()`), which Publish keeps beside the maximum - with a zero there, a frame landing between two blocks
    would read no reduction at all and the needle sawtooth toward the stop. The Comp's four meters, the
    Limiter's three and the Tape's two peaks.

    Consume exchanges against a fresh load, so the guarantee is against a block landing between its own load and
    its exchange; a block that lands between the frame's Read and its Consume - the frame's paint, microseconds -
    is lowered with what the frame showed. A consume that exchanged against the value the frame read would keep
    that block for the next frame, and would also leave a reader that consumes long after it read holding
    everything that landed in between (the Pedalboard's suite consumes so); the microseconds are the smaller
    cost.

    Relaxed atomics throughout: a meter is a picture, not a synchronisation.
*/
class HeldMaximum
{
  public:
    /** Audio thread, once a block: record the block's value and raise the held maximum to it if it is higher.
        @param block_value  the block's reading, in the meter's own unit */
    void Publish(double block_value) noexcept
    {
        last_.store(block_value, std::memory_order_relaxed);
        Raise(block_value);
    }

    /** Audio thread: raise the held maximum to `value` if it is higher, leaving last() alone.
        @param value  the reading to raise the held maximum to, in the meter's own unit */
    void Raise(double value) noexcept
    {
        double current = held_.load(std::memory_order_relaxed);
        while (current < value && !held_.compare_exchange_weak(current, value, std::memory_order_relaxed))
        {
        }
    }

    /** Both back to `value` outright: prepareToPlay, reset, a bypassed block.
        @param value  the reading both the held maximum and last() are set to, in the meter's own unit */
    void Clear(double value = 0.0) noexcept
    {
        held_.store(value, std::memory_order_relaxed);
        last_.store(value, std::memory_order_relaxed);
    }

    /** The held maximum.
        @return  the largest reading published since the last Consume(), in the meter's own unit */
    double Read() const noexcept
    {
        return held_.load(std::memory_order_relaxed);
    }

    /** What the last block published.
        @return  the value of the last Publish(), in the meter's own unit */
    double last() const noexcept
    {
        return last_.load(std::memory_order_relaxed);
    }

    /** The reader has taken the maximum: lower it to `fallback` unless a block raises it between this load and the
        exchange (the exchange then fails and the next frame reads the new value).
        @param fallback  the value the held maximum is lowered to, in the meter's own unit
        @return          the maximum that was held when the consume began */
    double Consume(double fallback) noexcept
    {
        double seen = held_.load(std::memory_order_relaxed);
        held_.compare_exchange_strong(seen, fallback, std::memory_order_relaxed);
        return seen;
    }

    /** Consume to the last block's value: a meter that follows its reading down.
        @return  the maximum that was held when the consume began */
    double ConsumeToLast() noexcept
    {
        return Consume(last());
    }

  private:
    std::atomic<double> held_{0.0}; ///< The largest value published since the last consume.
    std::atomic<double> last_{0.0}; ///< The value of the last Publish().
};
} // namespace reckless::core
