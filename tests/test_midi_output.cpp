// MIDI output beneath every back end (audio/MidiOutputQueue.h, MidiOutputPorts.h, MidiOutputSender.h), held on every
// platform: which messages a Send carries; the queue from many sending threads to one sender - every message accepted
// comes out once, whole, each thread's in the order it sent them, and a full queue says so and keeps what it holds;
// the schedule that hands them on earliest first; the port table a Send finds its queue in, never touching one once
// it is withdrawn; the sender, in the order of the times and silent once stopped; and the offline system's record.
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <limits>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "MidiOutputPorts.h"
#include "MidiOutputQueue.h"
#include "MidiOutputSender.h"
#include "OfflineDevice.h"

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
    for (const std::uint8_t real_time : std::initializer_list<std::uint8_t>{0xF8, 0xFA, 0xFB, 0xFC, 0xFE, 0xFF})
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
    for (const std::uint8_t common :
         std::initializer_list<std::uint8_t>{0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF9, 0xFD})
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

// ---- The port table, the sender and the offline system ------------------------------------------------------------

namespace
{
using reckless::audio::kMidiOutputPorts;
using reckless::audio::MidiOutputPorts;
using reckless::audio::MidiOutputSender;
using reckless::audio::MidiOutputStream;
using reckless::audio::NowNanoseconds;
using reckless::audio::OfflineMidiSystem;

/** An output that keeps what its sender hands it, and when, into storage made up front - and counts any message handed
    to it once the test has said the sender is stopped. */
class KeptStream final : public MidiOutputStream
{
  public:
    static constexpr std::size_t kMost = 20000; ///< the deliveries kept

    KeptStream()
    {
        delivered_.reserve(kMost);
    }

    /** The test's word that the sender's Stop has returned. */
    void SayStopped() noexcept
    {
        is_stopped_.store(true);
    }

    long after_stop() const noexcept
    {
        return after_stop_.load();
    }

    long count() const noexcept
    {
        return count_.load();
    }

    /** What was delivered: each message and when; read once the sender has stopped. */
    const std::vector<std::pair<MidiOutputMessage, std::int64_t>> &delivered() const noexcept
    {
        return delivered_;
    }

  protected:
    bool Deliver(const MidiOutputMessage &message) noexcept override
    {
        if (is_stopped_.load())
        {
            after_stop_.fetch_add(1);
        }
        if (delivered_.size() < kMost)
        {
            delivered_.emplace_back(message, NowNanoseconds());
        }
        count_.fetch_add(1);
        return true;
    }

  private:
    std::vector<std::pair<MidiOutputMessage, std::int64_t>> delivered_;
    std::atomic<bool>                                       is_stopped_{false};
    std::atomic<long>                                       after_stop_{0};
    std::atomic<long>                                       count_{0};
};

/** A note on of `key` as bytes. */
std::array<std::uint8_t, 3> NoteOn(int key)
{
    return {0x90, static_cast<std::uint8_t>(key & 0x7F), 100};
}
} // namespace

TEST(MidiOutputPorts, RefuseAPortNotOpenOrOutOfRangeAndAMessageSendDoesNotCarry)
{
    MidiOutputPorts ports;
    MidiOutputQueue queue;
    const auto      note = NoteOn(60);
    EXPECT_FALSE(ports.Send(0, 0, note.data(), 3)) << "not open";
    ports.Publish(5, queue);
    EXPECT_FALSE(ports.Send(0, 0, note.data(), 3)) << "another port";
    EXPECT_FALSE(ports.Send(-1, 0, note.data(), 3));
    EXPECT_FALSE(ports.Send(kMidiOutputPorts, 0, note.data(), 3));
    EXPECT_FALSE(ports.Send(5, 0, note.data(), 2)) << "a three-byte message with two bytes";
    const std::uint8_t sysex[] = {0xF0, 0x7D, 0xF7};
    EXPECT_FALSE(ports.Send(5, 0, sysex, 3));
    EXPECT_TRUE(ports.Send(5, 1234, note.data(), 3));
    MidiOutputMessage message;
    ASSERT_TRUE(queue.TryPop(message));
    EXPECT_EQ(message.time, 1234);
    EXPECT_EQ(message.size, 3);
    EXPECT_EQ(message.bytes, note);
    EXPECT_FALSE(queue.TryPop(message)) << "only what was accepted";
    ports.Withdraw(5);
    EXPECT_FALSE(ports.Send(5, 0, note.data(), 3)) << "withdrawn";
    ports.Withdraw(5); // twice is nothing
}

TEST(MidiOutputPorts, NeverReachAQueueOnceItsWithdrawReturns)
{
    // Threads send to one port without pause while the owner opens and closes it again and again, freeing each queue
    // as soon as its Withdraw returns: nothing lands in a queue after that (the sanitizers would see a freed one
    // touched), and the senders see the port open and closed.
    // No more senders than the machine has processors to spare, so the owner's rounds are not starved on a small one,
    // and the counts are 64 bits wide: a long is 32 on Windows, and the senders count past that in a slow run.
    const int         sender_count = std::clamp(static_cast<int>(std::thread::hardware_concurrency()) - 1, 2, 6);
    MidiOutputPorts   ports;
    std::atomic<bool> done{false};
    std::atomic<std::int64_t> accepted{0};
    std::atomic<std::int64_t> refused{0};
    std::vector<std::thread>  senders;
    for (int s = 0; s < sender_count; ++s)
    {
        senders.emplace_back([&ports, &done, &accepted, &refused, s] {
            const auto note = NoteOn(s);
            while (!done.load())
            {
                (ports.Send(7, s, note.data(), 3) ? accepted : refused).fetch_add(1, std::memory_order_relaxed);
            }
        });
    }
    // Up to 3000 rounds, and no longer than five seconds once two hundred are done.
    std::int64_t landed_late = 0;
    const auto   began       = std::chrono::steady_clock::now();
    for (int round = 0; round < 3000; ++round)
    {
        if (round >= 200 && std::chrono::steady_clock::now() - began > std::chrono::seconds(5))
        {
            break;
        }
        auto queue = std::make_unique<MidiOutputQueue>();
        ports.Publish(7, *queue);
        std::this_thread::yield();
        ports.Withdraw(7);
        MidiOutputMessage message;
        while (queue->TryPop(message))
        {
        }
        std::this_thread::yield();
        if (queue->TryPop(message))
        {
            ++landed_late;
        }
    }
    done.store(true);
    for (std::thread &sender : senders)
    {
        sender.join();
    }
    EXPECT_EQ(landed_late, 0) << "a Send reached a queue after its Withdraw returned";
    EXPECT_GT(accepted.load(), 0);
    EXPECT_GT(refused.load(), 0);
}

TEST(MidiOutputSenders, HandOnInTheOrderOfTheTimesNotTheOrderSent)
{
    KeptStream       stream;
    MidiOutputSender sender(0);
    sender.Start({&stream});
    const std::int64_t start = NowNanoseconds() + 30'000'000;
    // Sent latest first, in two threads' worth of interleaving: 40 messages over 40 ms, and two of one time.
    std::vector<int> order(40);
    for (int i = 0; i < 40; ++i)
    {
        order[static_cast<std::size_t>(i)] = 39 - i;
    }
    for (const int i : order)
    {
        const auto        note = NoteOn(i);
        MidiOutputMessage message;
        message.time  = start + static_cast<std::int64_t>(i) * 1'000'000;
        message.bytes = note;
        message.size  = 3;
        ASSERT_TRUE(stream.queue().TryPush(message));
    }
    MidiOutputMessage tie;
    tie.time  = start + 20'000'000;
    tie.bytes = {0x80, 20, 0}; // a note off of key 20, sent after key 20's note on, for the same time
    tie.size  = 3;
    ASSERT_TRUE(stream.queue().TryPush(tie));
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (stream.count() < 41 && std::chrono::steady_clock::now() < until)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    sender.Stop();
    const auto &delivered = stream.delivered();
    ASSERT_EQ(delivered.size(), 41u);
    std::int64_t worst = 0;
    for (std::size_t i = 0; i < delivered.size(); ++i)
    {
        const auto &[message, at] = delivered[i];
        if (i > 0)
        {
            EXPECT_GE(message.time, delivered[i - 1].first.time) << "out of the order of the times at " << i;
        }
        EXPECT_GE(at, message.time) << "handed on before its time, with no lead";
        worst = std::max(worst, at - message.time);
    }
    EXPECT_EQ(delivered[20].first.bytes[0], 0x90);
    EXPECT_EQ(delivered[21].first.bytes[0], 0x80) << "of one time, in the order sent";
    std::printf("handed on at most %.3f ms after its time (an ordinary thread's sleep)\n",
                static_cast<double>(worst) * 1e-6);
}

TEST(MidiOutputSenders, HandOnNothingOnceStopReturns)
{
    // A thread keeps the queue fed with messages due at once, so the sender is always delivering; Stop, then the test
    // says stopped, and nothing more may reach Deliver.
    for (int round = 0; round < 50; ++round)
    {
        KeptStream        stream;
        MidiOutputSender  sender(0);
        std::atomic<bool> done{false};
        sender.Start({&stream});
        std::thread feeder([&stream, &done] {
            MidiOutputMessage message;
            message.bytes = NoteOn(1);
            message.size  = 3;
            while (!done.load())
            {
                message.time = NowNanoseconds();
                if (!stream.queue().TryPush(message))
                {
                    std::this_thread::yield();
                }
            }
        });
        while (stream.count() < 10)
        {
            std::this_thread::yield();
        }
        sender.Stop();
        stream.SayStopped();
        const long at_stop = stream.count();
        std::this_thread::sleep_for(std::chrono::milliseconds(3));
        done.store(true);
        feeder.join();
        ASSERT_EQ(stream.after_stop(), 0) << "round " << round;
        ASSERT_EQ(stream.count(), at_stop) << "round " << round;
        EXPECT_FALSE(sender.running());
    }
}

TEST(OfflineMidiOutputs, RecordWhatTheyAreSentInTheOrderOfTheTimes)
{
    OfflineMidiSystem midi({"Keys"}, {"Synth", "Drums"});
    EXPECT_EQ(midi.Outputs(), (std::vector<std::string>{"Synth", "Drums"}));
    EXPECT_FALSE(midi.OpenOutput("Nowhere", 0));
    EXPECT_NE(midi.LastError().find("Nowhere"), std::string::npos) << midi.LastError();
    EXPECT_FALSE(midi.OpenOutput("Synth", kMidiOutputPorts)) << "out of range";
    EXPECT_FALSE(midi.OpenOutput("Synth", -1));
    const auto note = NoteOn(60);
    EXPECT_FALSE(midi.Send(0, 0, note.data(), 3)) << "not open";

    ASSERT_TRUE(midi.OpenOutput("Synth", 0));
    ASSERT_TRUE(midi.OpenOutput("Drums", 9));
    EXPECT_EQ(midi.open_output_count(), 2);
    const std::uint8_t later[]   = {0x90, 62, 90};
    const std::uint8_t earlier[] = {0x99, 36, 127};
    const std::uint8_t off[]     = {0x80, 62, 0};
    const std::uint8_t clock[]   = {0xF8};
    EXPECT_TRUE(midi.Send(0, 3000, later, 3));
    EXPECT_TRUE(midi.Send(9, 1000, earlier, 3));
    EXPECT_TRUE(midi.Send(0, 3000, off, 3));
    EXPECT_TRUE(midi.Send(0, 2000, clock, 1));
    EXPECT_FALSE(midi.Send(0, 2500, later, 2)) << "refused, and not recorded";
    const std::uint8_t sysex[] = {0xF0, 0x7D, 0xF7};
    EXPECT_FALSE(midi.Send(0, 2500, sysex, 3));

    const auto &sent = midi.Sent();
    ASSERT_EQ(sent.size(), 4u);
    EXPECT_EQ(sent[0].port, 9);
    EXPECT_EQ(sent[0].time, 1000);
    EXPECT_EQ(sent[0].bytes, (std::vector<std::uint8_t>{0x99, 36, 127}));
    EXPECT_EQ(sent[1].bytes, (std::vector<std::uint8_t>{0xF8}));
    EXPECT_EQ(sent[2].bytes, (std::vector<std::uint8_t>{0x90, 62, 90}));
    EXPECT_EQ(sent[3].bytes, (std::vector<std::uint8_t>{0x80, 62, 0})) << "of one time, in the order sent";
    EXPECT_EQ(sent[3].port, 0);

    EXPECT_TRUE(midi.Send(0, 500, clock, 1));
    EXPECT_EQ(midi.Sent().size(), 5u) << "the record grows";
    EXPECT_EQ(midi.Sent()[0].time, 500);
    midi.ClearSent();
    EXPECT_TRUE(midi.Sent().empty());

    ASSERT_TRUE(midi.OpenOutput("Drums", 0)) << "a port reopened onto another output";
    EXPECT_TRUE(midi.Send(0, 10, clock, 1));
    midi.CloseAll();
    EXPECT_EQ(midi.open_output_count(), 0);
    EXPECT_FALSE(midi.Send(0, 20, clock, 1)) << "closed";
    EXPECT_FALSE(midi.Send(9, 20, clock, 1));
    ASSERT_EQ(midi.Sent().size(), 1u) << "what was accepted before the close is kept";
    EXPECT_EQ(midi.Sent()[0].time, 10);
}

TEST(OfflineMidiOutputs, TakeSendsFromManyThreadsRacingCloseAll)
{
    // Threads send to two ports without pause while the owner opens and closes everything again and again: no Send
    // crashes or lands after CloseAll returns, and every message recorded is one that was sent.
    constexpr int            kSenders = 4;
    OfflineMidiSystem        midi({}, {"A", "B"});
    std::atomic<bool>        done{false};
    std::vector<std::thread> senders;
    for (int s = 0; s < kSenders; ++s)
    {
        senders.emplace_back([&midi, &done, s] {
            const auto note = NoteOn(s);
            while (!done.load())
            {
                midi.Send(s % 2, NowNanoseconds(), note.data(), 3);
            }
        });
    }
    long recorded = 0;
    for (int round = 0; round < 2000; ++round)
    {
        ASSERT_TRUE(midi.OpenOutput("A", 0));
        ASSERT_TRUE(midi.OpenOutput("B", 1));
        std::this_thread::yield();
        midi.CloseAll();
        const std::size_t closed = midi.Sent().size();
        std::this_thread::yield();
        ASSERT_EQ(midi.Sent().size(), closed) << "a message landed after CloseAll returned";
        for (const auto &message : midi.Sent())
        {
            ASSERT_EQ(message.bytes.size(), 3u);
            ASSERT_LT(message.bytes[1], kSenders);
        }
        recorded += static_cast<long>(closed);
        midi.ClearSent();
    }
    done.store(true);
    for (std::thread &sender : senders)
    {
        sender.join();
    }
    std::printf("%ld messages recorded over 2000 opens and closes\n", recorded);
}
