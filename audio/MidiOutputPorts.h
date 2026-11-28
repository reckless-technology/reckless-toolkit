#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <thread>

#include "AudioDevice.h"
#include "MidiOutputQueue.h"

namespace reckless::audio
{
/**
    A MidiSystem's open outputs by port number, as Send finds them: a fixed table of kMidiOutputPorts entries, each the
    queue of the output open as that port, or none. Send reads it from any thread, wait-free; the owner publishes a
    queue when an output opens and withdraws it when it closes, and Withdraw returns only once no Send can touch that
    queue any more, so the owner may then stop the sender and free it. Header only and platform-neutral, one copy for
    every back end and the offline system.

    **How a Send and a Withdraw meet.** Each entry has an epoch and two counters of the Sends under way, one for each
    parity of the epoch. A Send reads the epoch, adds one to that epoch's counter, reads the epoch again, and only if it
    is unchanged loads the queue and pushes; then it takes its one back. A Withdraw clears the queue, advances the
    epoch, and waits until the counter of the epoch it ended reads zero. A Send whose second read came before the
    advance was counted before it, so the Withdraw waits for it; a Send whose second read came after sees the epoch
    moved and touches nothing. Sends that begin after the advance count on the other counter, so a stream of them
    cannot hold a Withdraw up. Every operation is sequentially consistent, which is what puts each Send's reads on one
    side of the advance or the other. A Send is five atomic operations and a push: wait-free.
*/
class MidiOutputPorts
{
  public:
    MidiOutputPorts() = default;

    MidiOutputPorts(const MidiOutputPorts &)            = delete;
    MidiOutputPorts &operator=(const MidiOutputPorts &) = delete;

    /** Whether `port` is a port number an output can be opened as.
        @param port  the number
        @return      true for 0 to kMidiOutputPorts - 1 */
    static constexpr bool IsPortNumber(int port) noexcept
    {
        return port >= 0 && port < kMidiOutputPorts;
    }

    /** Any thread, wait-free: MidiSystem::Send's whole work - the message checked, and queued to the output open as
        `port`.
        @param port              the port
        @param time_nanoseconds  when it is to leave, on NowNanoseconds' clock
        @param bytes             its bytes
        @param size              its length
        @return                  true when it was queued; false for a port not open, a full queue, or a message Send
                                 does not carry */
    bool Send(int port, std::int64_t time_nanoseconds, const std::uint8_t *bytes, int size) noexcept
    {
        if (!IsPortNumber(port) || !IsSendableMidiMessage(bytes, size))
        {
            return false;
        }
        MidiOutputMessage message;
        message.time = time_nanoseconds;
        for (int i = 0; i < size; ++i)
        {
            message.bytes[static_cast<std::size_t>(i)] = bytes[i];
        }
        message.size                  = static_cast<std::uint8_t>(size);
        Entry              &entry     = entries_[static_cast<std::size_t>(port)];
        const std::uint64_t epoch     = entry.epoch.load(std::memory_order_seq_cst);
        std::atomic<int>   &under_way = entry.under_way[epoch & 1];
        under_way.fetch_add(1, std::memory_order_seq_cst);
        bool queued = false;
        if (entry.epoch.load(std::memory_order_seq_cst) == epoch)
        {
            MidiOutputQueue *queue = entry.queue.load(std::memory_order_seq_cst);
            queued                 = queue != nullptr && queue->TryPush(message);
        }
        under_way.fetch_sub(1, std::memory_order_seq_cst);
        return queued;
    }

    /** The owner's thread: `queue` made the one Send reaches as `port`. The port must have none (Withdraw it first).
        @param port   the port, 0 to kMidiOutputPorts - 1
        @param queue  the queue; it must outlive its Withdraw */
    void Publish(int port, MidiOutputQueue &queue) noexcept
    {
        entries_[static_cast<std::size_t>(port)].queue.store(&queue, std::memory_order_seq_cst);
    }

    /** The owner's thread: `port`'s queue withdrawn, if it has one - once this returns, no Send touches it again, and
        a Send to `port` returns false until another is published. Waits for the Sends under way, a few instructions
        each.
        @param port  the port, 0 to kMidiOutputPorts - 1 */
    void Withdraw(int port) noexcept
    {
        Entry &entry = entries_[static_cast<std::size_t>(port)];
        if (entry.queue.load(std::memory_order_seq_cst) == nullptr)
        {
            return;
        }
        entry.queue.store(nullptr, std::memory_order_seq_cst);
        const std::uint64_t ended = entry.epoch.fetch_add(1, std::memory_order_seq_cst);
        while (entry.under_way[ended & 1].load(std::memory_order_seq_cst) != 0)
        {
            std::this_thread::yield();
        }
    }

  private:
    /** One port: its queue, and what tells a Send from a Withdraw (the class's comment). */
    struct Entry
    {
        std::atomic<MidiOutputQueue *>  queue{nullptr}; ///< the open output's queue; null when none is open
        std::atomic<std::uint64_t>      epoch{0};       ///< advanced by each Withdraw
        std::array<std::atomic<int>, 2> under_way{};    ///< the Sends under way, by their epoch's parity
    };

    std::array<Entry, kMidiOutputPorts> entries_; ///< the ports, by number
};
} // namespace reckless::audio
