// MIDI output beneath every back end (audio/MidiOutputQueue.h), held on every platform: which messages a Send carries,
// the queue from many sending threads to one sender - every message accepted comes out once, whole, each thread's in
// the order it sent them, and a full queue says so and keeps what it holds - and the schedule that hands them on
// earliest first.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <random>
#include <thread>
#include <vector>

#include "MidiOutputQueue.h"

#include <gtest/gtest.h>

namespace
{
using reckless::audio::IsSendableMidiMessage;
using reckless::audio::kMidiOutputQueueCapacity;
using reckless::audio::MidiOutputMessage;
using reckless::audio::MidiOutputQueue;
using reckless::audio::MidiOutputSchedule;

/** The message producer `producer` sends `sequence`th: its time says both, and its three bytes are made from them, so
    a message read whole can be told from one read half written. */
MidiOutputMessage Stamped(int producer, std::uint32_t sequence)
{
    MidiOutputMessage message;
    message.time     = (static_cast<std::int64_t>(producer) << 32) | static_cast<std::int64_t>(sequence);
    message.bytes[0] = static_cast<std::uint8_t>(0x90 | (sequence & 0x0F));
    message.bytes[1] = static_cast<std::uint8_t>((sequence >> 4) & 0x7F);
    message.bytes[2] = static_cast<std::uint8_t>((producer * 13 + static_cast<int>(sequence >> 11)) & 0x7F);
    message.size     = 3;
    return message;
}

/** Whether `message` is whole: its bytes are the ones its time makes. */
bool IsWhole(const MidiOutputMessage &message)
{
    const int  producer = static_cast<int>(message.time >> 32);
    const auto sequence = static_cast<std::uint32_t>(message.time & 0xFFFFFFFF);
    const auto made     = Stamped(producer, sequence);
    return message.bytes == made.bytes && message.size == made.size;
}
} // namespace

TEST(MidiOutputMessages, AreWholeChannelOrRealTimeMessagesOfTheirStatusesLength)
{
    const std::uint8_t note_on[]    = {0x90, 60, 100};
    const std::uint8_t controller[] = {0xB3, 7, 127};
    const std::uint8_t bend[]       = {0xEF, 0x00, 0x40};
    const std::uint8_t program[]    = {0xC0, 5};
    const std::uint8_t pressure[]   = {0xD9, 64};
    EXPECT_TRUE(IsSendableMidiMessage(note_on, 3));
    EXPECT_TRUE(IsSendableMidiMessage(controller, 3));
    EXPECT_TRUE(IsSendableMidiMessage(bend, 3));
    EXPECT_TRUE(IsSendableMidiMessage(program, 2));
    EXPECT_TRUE(IsSendableMidiMessage(pressure, 2));
    for (const std::uint8_t real_time : {0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF})
    {
        const std::uint8_t message[] = {real_time};
        EXPECT_TRUE(IsSendableMidiMessage(message, 1)) << int{real_time};
    }

    EXPECT_FALSE(IsSendableMidiMessage(note_on, 2)) << "a three-byte message with two bytes";
    EXPECT_FALSE(IsSendableMidiMessage(note_on, 1));
    EXPECT_FALSE(IsSendableMidiMessage(program, 3)) << "a two-byte message with three";
    EXPECT_FALSE(IsSendableMidiMessage(program, 1));
    const std::uint8_t clock_and_more[] = {0xF8, 0x00};
    EXPECT_FALSE(IsSendableMidiMessage(clock_and_more, 2));
    EXPECT_FALSE(IsSendableMidiMessage(note_on, 0));
    EXPECT_FALSE(IsSendableMidiMessage(note_on, -1));
    EXPECT_FALSE(IsSendableMidiMessage(nullptr, 3));
    EXPECT_FALSE(IsSendableMidiMessage(note_on, 4));

    const std::uint8_t running_status[] = {60, 100};
    EXPECT_FALSE(IsSendableMidiMessage(running_status, 2)) << "running status is not taken";
    const std::uint8_t data_with_status[] = {0x90, 0x80, 100};
    EXPECT_FALSE(IsSendableMidiMessage(data_with_status, 3)) << "a status where a data byte goes";
    const std::uint8_t sysex[] = {0xF0, 0x7D, 0xF7};
    EXPECT_FALSE(IsSendableMidiMessage(sysex, 3)) << "system exclusive is not sent";
    EXPECT_FALSE(IsSendableMidiMessage(sysex, 1));
    for (const std::uint8_t common : {0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFD})
    {
        for (int size = 1; size <= 3; ++size)
        {
            const std::uint8_t message[] = {common, 0, 0};
            EXPECT_FALSE(IsSendableMidiMessage(message, size)) << int{common} << " of " << size;
        }
    }
}

TEST(MidiOutputQueues, HandOnInOrderAndSayWhenFullKeepingWhatTheyHold)
{
    MidiOutputQueue   queue;
    MidiOutputMessage message;
    EXPECT_FALSE(queue.TryPop(message)) << "empty";
    for (std::uint32_t i = 0; i < kMidiOutputQueueCapacity; ++i)
    {
        ASSERT_TRUE(queue.TryPush(Stamped(0, i))) << i;
    }
    EXPECT_FALSE(queue.TryPush(Stamped(0, 99999))) << "full";
    EXPECT_FALSE(queue.TryPush(Stamped(0, 99998))) << "still full, and the refusals changed nothing";
    for (std::uint32_t i = 0; i < kMidiOutputQueueCapacity; ++i)
    {
        ASSERT_TRUE(queue.TryPop(message)) << i;
        EXPECT_EQ(message.time, Stamped(0, i).time) << "the oldest first: a full queue overwrote nothing";
        EXPECT_TRUE(IsWhole(message));
    }
    EXPECT_FALSE(queue.TryPop(message));

    // Round and round, so every slot is reused at tickets past its first lap.
    for (std::uint32_t lap = 0; lap < 5; ++lap)
    {
        for (std::uint32_t i = 0; i < kMidiOutputQueueCapacity - 1; ++i)
        {
            ASSERT_TRUE(queue.TryPush(Stamped(1, lap * 100000 + i)));
        }
        for (std::uint32_t i = 0; i < kMidiOutputQueueCapacity - 1; ++i)
        {
            ASSERT_TRUE(queue.TryPop(message));
            ASSERT_EQ(message.time, Stamped(1, lap * 100000 + i).time);
        }
    }
}

TEST(MidiOutputQueues, CarryEveryMessageOfManyThreadsOnceWholeAndInEachThreadsOrder)
{
    constexpr int            kProducers = 8;
    constexpr std::uint32_t  kEach      = 60000;
    MidiOutputQueue          queue;
    std::atomic<bool>        go{false};
    std::atomic<long>        refusals{0};
    std::vector<std::thread> producers;
    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back([&queue, &go, &refusals, p] {
            while (!go.load())
            {
            }
            for (std::uint32_t i = 0; i < kEach; ++i)
            {
                while (!queue.TryPush(Stamped(p, i)))
                {
                    refusals.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::yield();
                }
            }
        });
    }
    std::vector<std::uint32_t> next(kProducers, 0);
    long                       taken        = 0;
    long                       broken       = 0;
    long                       out_of_order = 0;
    go.store(true);
    MidiOutputMessage message;
    while (taken < static_cast<long>(kProducers) * kEach)
    {
        if (!queue.TryPop(message))
        {
            continue;
        }
        ++taken;
        if (!IsWhole(message))
        {
            ++broken;
            continue;
        }
        const auto producer = static_cast<std::size_t>(message.time >> 32);
        const auto sequence = static_cast<std::uint32_t>(message.time & 0xFFFFFFFF);
        if (producer >= next.size() || sequence != next[producer])
        {
            ++out_of_order;
            continue;
        }
        ++next[producer];
    }
    for (std::thread &producer : producers)
    {
        producer.join();
    }
    EXPECT_EQ(broken, 0) << "a message read half written";
    EXPECT_EQ(out_of_order, 0) << "a message lost, repeated or out of its thread's order";
    for (int p = 0; p < kProducers; ++p)
    {
        EXPECT_EQ(next[static_cast<std::size_t>(p)], kEach) << "producer " << p;
    }
    EXPECT_FALSE(queue.TryPop(message)) << "nothing more than was sent";
    std::printf("%d threads, %u messages each: %ld pushes found the queue full and were refused\n", kProducers, kEach,
                refusals.load());
}

TEST(MidiOutputQueues, FilledByManyThreadsAtOnceHoldExactlyTheirCapacity)
{
    constexpr int                           kProducers = 8;
    MidiOutputQueue                         queue;
    std::atomic<bool>                       go{false};
    std::vector<std::vector<std::uint32_t>> accepted(kProducers);
    std::vector<std::thread>                producers;
    for (int p = 0; p < kProducers; ++p)
    {
        producers.emplace_back([&queue, &go, &accepted, p] {
            while (!go.load())
            {
            }
            for (std::uint32_t i = 0; i < kMidiOutputQueueCapacity; ++i)
            {
                if (queue.TryPush(Stamped(p, i)))
                {
                    accepted[static_cast<std::size_t>(p)].push_back(i);
                }
            }
        });
    }
    go.store(true);
    for (std::thread &producer : producers)
    {
        producer.join();
    }
    std::size_t total = 0;
    for (const auto &each : accepted)
    {
        total += each.size();
    }
    EXPECT_LE(total, kMidiOutputQueueCapacity);
    // A refusal can come a message or two early while other threads' refusals are under way; alone, it fills to the
    // last slot.
    std::uint32_t topped = 0;
    while (queue.TryPush(Stamped(kProducers, topped)))
    {
        accepted.resize(kProducers + 1);
        accepted[kProducers].push_back(topped++);
        ++total;
    }
    EXPECT_EQ(total, kMidiOutputQueueCapacity) << "full at its capacity, not before";
    std::printf("%zu taken by %d threads at once, %u more by one alone\n", total - topped, kProducers, topped);

    std::vector<std::size_t> place(accepted.size(), 0);
    MidiOutputMessage        message;
    std::size_t              popped = 0;
    while (queue.TryPop(message))
    {
        ++popped;
        ASSERT_TRUE(IsWhole(message));
        const auto producer = static_cast<std::size_t>(message.time >> 32);
        ASSERT_LT(producer, accepted.size());
        ASSERT_LT(place[producer], accepted[producer].size()) << "more of a thread's than it was told were taken";
        EXPECT_EQ(static_cast<std::uint32_t>(message.time & 0xFFFFFFFF), accepted[producer][place[producer]])
            << "what was taken comes out, in order, and nothing in place of it";
        ++place[producer];
    }
    EXPECT_EQ(popped, kMidiOutputQueueCapacity);
}

TEST(MidiOutputSchedules, HandOnTheEarliestFirstAndTiesInTheOrderAdded)
{
    MidiOutputSchedule schedule;
    EXPECT_TRUE(schedule.empty());
    EXPECT_EQ(schedule.EarliestTime(), std::numeric_limits<std::int64_t>::max());
    std::mt19937_64                           random(102);
    std::vector<std::pair<std::int64_t, int>> added;
    for (int i = 0; i < 3000; ++i)
    {
        MidiOutputMessage message;
        message.time     = static_cast<std::int64_t>(random() % 200); // many ties
        message.bytes[1] = static_cast<std::uint8_t>(i & 0x7F);
        message.bytes[2] = static_cast<std::uint8_t>((i >> 7) & 0x7F);
        message.size     = 3;
        ASSERT_TRUE(schedule.Add(message));
        added.emplace_back(message.time, i);
    }
    std::stable_sort(added.begin(), added.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    for (const auto &[time, index] : added)
    {
        ASSERT_FALSE(schedule.empty());
        const MidiOutputMessage &earliest = schedule.Earliest();
        ASSERT_EQ(earliest.time, time);
        ASSERT_EQ(earliest.bytes[1] | (earliest.bytes[2] << 7), index) << "of one time, the first added first";
        schedule.RemoveEarliest();
    }
    EXPECT_TRUE(schedule.empty());

    MidiOutputMessage message;
    for (std::size_t i = 0; i < kMidiOutputQueueCapacity; ++i)
    {
        message.time = static_cast<std::int64_t>(kMidiOutputQueueCapacity - i);
        ASSERT_TRUE(schedule.Add(message));
    }
    EXPECT_TRUE(schedule.full());
    EXPECT_FALSE(schedule.Add(message)) << "no room past the queue's capacity";
    EXPECT_EQ(schedule.EarliestTime(), 1);
    schedule.Clear();
    EXPECT_TRUE(schedule.empty());
}
