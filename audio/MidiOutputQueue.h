#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <vector>

namespace reckless::audio
{
/**
    What MIDI output is built from beneath every back end, header only and platform-neutral so the audio suite holds it
    on every platform (tests/test_midi_output.cpp): the message a Send carries (MidiOutputMessage) and which messages it
    carries (IsSendableMidiMessage), the queue between the threads that send and the thread that sends them on
    (MidiOutputQueue), and the order that thread hands them to the platform in (MidiOutputSchedule).
*/

/** How many messages one open MIDI output holds between MidiSystem::Send and its sender: a power of two, so a ticket
    finds its slot with a mask. At 4,096 three-byte messages it is some 98 KiB an output, more than a block of any
    sequence a consumer sends ahead. */
inline constexpr std::size_t kMidiOutputQueueCapacity = 4096;

static_assert((kMidiOutputQueueCapacity & (kMidiOutputQueueCapacity - 1)) == 0, "a power of two");

/** One message on its way out: when it is to leave, on NowNanoseconds' clock, and its bytes, the status first. */
struct MidiOutputMessage
{
    std::int64_t                time{0}; ///< when it leaves, in nanoseconds on NowNanoseconds' clock
    std::array<std::uint8_t, 3> bytes{}; ///< its bytes, the status first; those past `size` are zero
    std::uint8_t                size{0}; ///< how many of `bytes` it has, 1 to 3
};

/** The length of the message a status byte starts, among those MidiSystem::Send carries: 3 for a note off, note on,
    polyphonic pressure, control change or pitch bend; 2 for a program change or channel pressure; 1 for a system
    real-time message (timing clock, start, continue, stop, active sensing, reset); 0 for everything else - a data byte
    (Send takes no running status), system exclusive and its end, the system common messages, and the two undefined
    real-time statuses (F9 and FD).
    @param status  the message's first byte
    @return        the length its status asks, or 0 for a message Send does not carry */
constexpr int SendableMidiLength(std::uint8_t status) noexcept
{
    if (status < 0x80)
    {
        return 0;
    }
    if (status < 0xC0)
    {
        return 3;
    }
    if (status < 0xE0)
    {
        return 2;
    }
    if (status < 0xF0)
    {
        return 3;
    }
    switch (status)
    {
    case 0xF8:
    case 0xFA:
    case 0xFB:
    case 0xFC:
    case 0xFE:
    case 0xFF:
        return 1;
    default:
        return 0;
    }
}

/** Whether `bytes` is a message MidiSystem::Send carries: one whole channel message, or a system real-time message, of
    exactly the length its status asks (SendableMidiLength), every byte after the status a data byte.
    @param bytes  the message's bytes; may be null
    @param size   its length, in bytes
    @return       true when Send may queue it */
constexpr bool IsSendableMidiMessage(const std::uint8_t *bytes, int size) noexcept
{
    if (bytes == nullptr || size < 1 || size > 3 || SendableMidiLength(bytes[0]) != size)
    {
        return false;
    }
    for (int i = 1; i < size; ++i)
    {
        if (bytes[i] >= 0x80)
        {
            return false;
        }
    }
    return true;
}

/**
    A bounded queue of MidiOutputMessage from any number of threads to one: TryPush from any thread, an audio callback
    among them, and TryPop from the one thread that sends the messages on (the output's sender). Fixed in size at
    construction (kMidiOutputQueueCapacity), and nothing on either side allocates, locks or calls the system.

    **TryPush is wait-free**: it finishes in a fixed number of its own steps - two read-modify-writes, a load, a copy
    and a store, or two read-modify-writes when the queue is full - whatever every other thread is doing, with no loop
    at all, so no thread's delay or death can delay it. It does so by keeping two counters apart. `reserved_` counts the
    slots claimed and not yet given back by the consumer: a producer adds one, and if the value it added to was already
    the capacity, the queue is full, so it takes its one back and returns false, having taken no ticket and touched no
    slot - the consumer sees nothing of it. Otherwise it takes a ticket, `tail_`'s next value, and writes the slot that
    ticket names. A full queue can therefore be reported full by a producer that races another producer's failing
    attempt while one slot is left - a false return a few messages early, never a message lost after a true one.

    **Why the ticket's slot is always free when the producer reaches it**, so it never has to wait for it. Tickets are
    given back in order: the consumer takes ticket k only once ticket k-1 is taken, and gives k back (marks the slot
    free for ticket k + capacity, then subtracts one from `reserved_`) only after reading it. Every ticket up to t was
    taken by a producer that had already reserved, and `reserved_`, never above the capacity at a reservation that
    succeeded, bounds the reservations not given back. So when ticket t is taken, the producers of the tickets the
    consumer has not yet given back number at most the capacity, and t among them: tickets 0 to t - capacity have all
    been given back, the last of them marking t's slot free for t. Every operation on the three counters and on the
    slots' sequence numbers is sequentially consistent, so the producer's load of its slot's sequence number, which
    comes after its ticket in that single order, reads the consumer's mark.

    **Why the consumer never reads a half-written slot.** A slot's sequence number says what it holds: t when it is
    free for ticket t, t + 1 once ticket t's message is in it. The producer writes the message and only then stores
    t + 1 (a release); the consumer reads a slot only when it loads t + 1 for the ticket it is at (an acquire), so
    every byte the producer wrote is visible to it; and it stores the next free mark only after it has copied the
    message out, which the next producer's load of that mark acquires before it writes.

    The one wait in the design is the consumer's: a producer that has taken a ticket and not yet stored its message
    holds back the messages behind it until it does, which is the few instructions of a copy unless the system
    preempts that thread there.
*/
class MidiOutputQueue
{
  public:
    static constexpr std::size_t kCapacity = kMidiOutputQueueCapacity; ///< the messages it holds at most

    /** An empty queue, its slots made once: every slot free for its first ticket. */
    MidiOutputQueue() : slots_(std::make_unique<Slot[]>(kCapacity))
    {
        for (std::size_t i = 0; i < kCapacity; ++i)
        {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
    }

    MidiOutputQueue(const MidiOutputQueue &)            = delete;
    MidiOutputQueue &operator=(const MidiOutputQueue &) = delete;

    /** Any thread, wait-free: `message` queued, or false when the queue is full, with nothing changed that the consumer
        sees (the class says why it never waits).
        @param message  the message
        @return         true when it was queued */
    bool TryPush(const MidiOutputMessage &message) noexcept
    {
        if (reserved_.fetch_add(1, std::memory_order_seq_cst) >= kCapacity)
        {
            reserved_.fetch_sub(1, std::memory_order_seq_cst);
            return false;
        }
        const std::uint64_t ticket = tail_.fetch_add(1, std::memory_order_seq_cst);
        Slot               &slot   = slots_[static_cast<std::size_t>(ticket & kMask)];
        // Free for this ticket already (the class says why); the load is the acquire of the consumer's mark, which
        // orders its copy of the slot's last message before this write of the next.
        [[maybe_unused]] const std::uint64_t free_for = slot.sequence.load(std::memory_order_seq_cst);
        slot.message                                  = message;
        slot.sequence.store(ticket + 1, std::memory_order_seq_cst);
        return true;
    }

    /** The consumer's thread only: the oldest message queued, moved out; false when there is none yet - the queue is
        empty, or the producer of the next ticket has not finished writing it.
        @param message  set to the message
        @return         true when one was taken */
    bool TryPop(MidiOutputMessage &message) noexcept
    {
        Slot &slot = slots_[static_cast<std::size_t>(head_ & kMask)];
        if (slot.sequence.load(std::memory_order_seq_cst) != head_ + 1)
        {
            return false;
        }
        message = slot.message;
        slot.sequence.store(head_ + kCapacity, std::memory_order_seq_cst);
        ++head_;
        reserved_.fetch_sub(1, std::memory_order_seq_cst);
        return true;
    }

  private:
    static constexpr std::uint64_t kMask = kCapacity - 1; ///< a ticket's slot is its low bits

    /** One slot: what it holds, and the sequence number that says so (the class's comment). */
    struct Slot
    {
        std::atomic<std::uint64_t> sequence{0}; ///< t: free for ticket t; t + 1: holds ticket t's message
        MidiOutputMessage          message;     ///< written by its ticket's producer, read by the consumer
    };

    std::unique_ptr<Slot[]>    slots_;       ///< the slots, kCapacity of them
    std::atomic<std::uint64_t> reserved_{0}; ///< slots claimed and not given back, failing claims for a moment too
    std::atomic<std::uint64_t> tail_{0};     ///< the next ticket
    std::uint64_t              head_{0};     ///< the consumer's next ticket; the consumer's own
};

/**
    The messages an output's sender holds, earliest first: a binary heap on the sender's own memory, its room made once
    at construction (kMidiOutputQueueCapacity messages, as many as the queue in front of it), so adding and removing
    never allocates. Two messages of one time come out in the order they were added, which is the order of the queue's
    tickets, so two that one thread sent for the same time leave in the order it sent them. The sender's thread only.
*/
class MidiOutputSchedule
{
  public:
    /** An empty schedule with room for kMidiOutputQueueCapacity messages. */
    MidiOutputSchedule()
    {
        entries_.reserve(kMidiOutputQueueCapacity);
    }

    /** Whether there is no room for another message.
        @return  true when it holds kMidiOutputQueueCapacity */
    bool full() const noexcept
    {
        return entries_.size() >= kMidiOutputQueueCapacity;
    }

    /** Whether it holds nothing.
        @return  true when empty */
    bool empty() const noexcept
    {
        return entries_.empty();
    }

    /** How many messages it holds.
        @return  the count */
    std::size_t size() const noexcept
    {
        return entries_.size();
    }

    /** `message` added, after any of the same time already held; nothing when it is full.
        @param message  the message
        @return         false when it was full and the message was not added */
    bool Add(const MidiOutputMessage &message) noexcept
    {
        if (full())
        {
            return false;
        }
        entries_.push_back(Entry{message, next_order_++}); // within the room reserved: no allocation
        std::push_heap(entries_.begin(), entries_.end(), &Later);
        return true;
    }

    /** The earliest message: the least time, and of those the first added. Not to be called when empty.
        @return  the message */
    const MidiOutputMessage &Earliest() const noexcept
    {
        return entries_.front().message;
    }

    /** The time of the earliest message, or the largest time there is when it holds none.
        @return  the time, in nanoseconds on NowNanoseconds' clock */
    std::int64_t EarliestTime() const noexcept
    {
        return entries_.empty() ? std::numeric_limits<std::int64_t>::max() : entries_.front().message.time;
    }

    /** The earliest message removed. Not to be called when empty. */
    void RemoveEarliest() noexcept
    {
        std::pop_heap(entries_.begin(), entries_.end(), &Later);
        entries_.pop_back();
    }

    /** Every message removed. */
    void Clear() noexcept
    {
        entries_.clear();
    }

  private:
    /** A message and the order it was added in, which breaks a tie of times. */
    struct Entry
    {
        MidiOutputMessage message;  ///< the message
        std::uint64_t     order{0}; ///< how many were added before it
    };

    /** The heap's order, earliest on top: whether `a` leaves after `b`.
        @param a  one entry
        @param b  another
        @return   true when `a` is later, or of the same time and added later */
    static bool Later(const Entry &a, const Entry &b) noexcept
    {
        return a.message.time != b.message.time ? a.message.time > b.message.time : a.order > b.order;
    }

    std::vector<Entry> entries_;       ///< the heap, its room reserved once
    std::uint64_t      next_order_{0}; ///< the order the next message added takes
};
} // namespace reckless::audio
