// MIDI input and output on ALSA's sequencer (AlsaMidi.h says what it does and why): the one translation unit that
// meets the sequencer.
#include "AlsaMidi.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <alsa/asoundlib.h>
#include <poll.h>
#include <sys/eventfd.h>
#include <unistd.h>

#include "MidiOutputPorts.h"
#include "MidiOutputSender.h"

namespace reckless::audio
{
namespace
{
/** The bytes one decoded message may take: every message that reaches a sink is short, system exclusive being left
    out. */
constexpr std::size_t kMessageBytes = 64;

/** One sequencer port another client offers: where it is, and the name the series lists it under. */
struct Source
{
    snd_seq_addr_t address{}; ///< its client and port numbers
    std::string    name;      ///< "<client>:<port> <client number>:<port number>", RtMidi's
};

/** What a port offers to be read from (an input of ours) or written to (an output of ours), and subscribed to. */
constexpr unsigned int kReadable = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
constexpr unsigned int kWritable = SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE; ///< see kReadable

/** Every port `seq` can reach with `capabilities` that is a MIDI, synth or application port, client by client and port
    by port, client 0 (the sequencer's own) and the system's own clients left out - for inputs, the ports RtMidi listed,
    in its order, under its names; for outputs, the same of the ports that can be written to.
    @param seq           a sequencer handle
    @param capabilities  kReadable for the inputs, kWritable for the outputs
    @param own           the system's own clients, left out; -1 for none
    @param own_too       another of them; -1 for none
    @return              the ports */
std::vector<Source> Sources(snd_seq_t *seq, unsigned int capabilities, int own, int own_too)
{
    std::vector<Source>    sources;
    snd_seq_client_info_t *client = nullptr;
    snd_seq_port_info_t   *port   = nullptr;
    snd_seq_client_info_alloca(&client);
    snd_seq_port_info_alloca(&port);
    snd_seq_client_info_set_client(client, -1);
    while (snd_seq_query_next_client(seq, client) >= 0)
    {
        const int number = snd_seq_client_info_get_client(client);
        if (number == 0 || number == own || number == own_too)
        {
            continue;
        }
        snd_seq_port_info_set_client(port, number);
        snd_seq_port_info_set_port(port, -1);
        while (snd_seq_query_next_port(seq, port) >= 0)
        {
            const unsigned int type = snd_seq_port_info_get_type(port);
            if ((type & (SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_SYNTH | SND_SEQ_PORT_TYPE_APPLICATION)) ==
                0)
            {
                continue;
            }
            if ((snd_seq_port_info_get_capability(port) & capabilities) != capabilities)
            {
                continue;
            }
            Source source;
            source.address = *snd_seq_port_info_get_addr(port);
            source.name = std::string(snd_seq_client_info_get_name(client)) + ":" + snd_seq_port_info_get_name(port) +
                          " " + std::to_string(source.address.client) + ":" + std::to_string(source.address.port);
            sources.push_back(std::move(source));
        }
    }
    return sources;
}

/** A listed name with its numbers set aside: "UMC1820:UMC1820 MIDI 1 24:0" as "UMC1820:UMC1820 MIDI 1".
    @param name  a port's listed name
    @return      the name without its trailing " <client>:<port>"; the name itself where it has none */
std::string WithoutNumbers(const std::string &name)
{
    const std::size_t space = name.rfind(' ');
    if (space == std::string::npos)
    {
        return name;
    }
    const std::string tail   = name.substr(space + 1);
    const std::size_t colon  = tail.find(':');
    const auto        digits = [](const std::string &text) {
        return !text.empty() &&
               std::all_of(text.begin(), text.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
    };
    if (colon == std::string::npos || !digits(tail.substr(0, colon)) || !digits(tail.substr(colon + 1)))
    {
        return name;
    }
    return name.substr(0, space);
}

/** The port a settings file's `name` means: that name exactly; else the first whose name is the same with the numbers
    set aside, a device plugged in again under new ones; else the first whose name holds it, as RtMidi found a shorter
    name.
    @param sources  the ports listed
    @param name     the name kept
    @return         the port, or null for none */
const Source *Find(const std::vector<Source> &sources, const std::string &name)
{
    for (const Source &source : sources)
    {
        if (source.name == name)
        {
            return &source;
        }
    }
    const std::string bare = WithoutNumbers(name);
    for (const Source &source : sources)
    {
        if (WithoutNumbers(source.name) == bare)
        {
            return &source;
        }
    }
    for (const Source &source : sources)
    {
        if (!name.empty() && source.name.find(name) != std::string::npos)
        {
            return &source;
        }
    }
    return nullptr;
}

/** How far ahead of its time the sender hands a message to the sequencer, which holds it on the output's queue and
    sends it at its time on the kernel's timer: far enough that an ordinary thread's late wake does not make a message
    late, near enough that little is in the kernel when an output closes, which drops it. */
constexpr std::int64_t kSequencerLeadNanoseconds = 20'000'000;

/** A sequencer handle's client number.
    @param seq  the handle; may be null
    @return     the client, or -1 for none */
int ClientOf(snd_seq_t *seq)
{
    return seq != nullptr ? snd_seq_client_id(seq) : -1;
}

/** How often a queue's timer ticks, in hertz: the kernel sends an event at the first tick at or after its time, so this
    is how late, at most, the kernel's own part makes it. The sequencer's default is 1,000, which puts a message up to
    a millisecond late; 4,000 puts it up to a quarter, at four thousand of the timer's wakes a second for each output
    open (the kernel takes at most 6,250). */
constexpr unsigned int kQueueTicksPerSecond = 4000;

/** A queue's timer set to tick kQueueTicksPerSecond times a second, on the timer the sequencer chose for it. Before the
    queue starts.
    @param seq    the handle that owns the queue
    @param queue  the queue
    @return       0, or the sequencer's negative error */
int TickFinely(snd_seq_t *seq, int queue)
{
    snd_seq_queue_timer_t *timer = nullptr;
    snd_seq_queue_timer_alloca(&timer);
    const int result = snd_seq_get_queue_timer(seq, queue, timer);
    if (result < 0)
    {
        return result;
    }
    snd_seq_queue_timer_set_resolution(timer, kQueueTicksPerSecond);
    return snd_seq_set_queue_timer(seq, queue, timer);
}

/** Where a queue's real-time clock stands on NowNanoseconds' clock, as the steady clock's reading at the queue's zero,
    measured once when an output opens. The queue's time is read between two readings of the steady clock, eight times
    a fifth of a millisecond apart, and the least of the zeros they give is kept: a read now and then finds the queue's
    time some 50 microseconds behind (an x86-64 desktop, the high-resolution timer, 2026-11-28), which would put the
    zero late. Measured so against the steady clock over a minute, the two clocks moved apart by 2.6 microseconds,
    which is why it is measured once and not again while the output is open.
    @param seq    the handle that owns the queue
    @param queue  the queue, started
    @return       the steady clock at the queue's zero, in nanoseconds; 0 when the queue would not say */
std::int64_t QueueZero(snd_seq_t *seq, int queue)
{
    snd_seq_queue_status_t *status = nullptr;
    snd_seq_queue_status_alloca(&status);
    std::int64_t zero = std::numeric_limits<std::int64_t>::max();
    for (int i = 0; i < 8; ++i)
    {
        if (i > 0)
        {
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        const std::int64_t before = NowNanoseconds();
        if (snd_seq_get_queue_status(seq, queue, status) < 0)
        {
            continue;
        }
        const std::int64_t         after      = NowNanoseconds();
        const snd_seq_real_time_t *real       = snd_seq_queue_status_get_real_time(status);
        const std::int64_t         queue_time = static_cast<std::int64_t>(real->tv_sec) * 1'000'000'000 + real->tv_nsec;
        zero                                  = std::min(zero, before + (after - before) / 2 - queue_time);
    }
    return zero == std::numeric_limits<std::int64_t>::max() ? 0 : zero;
}

/** One output open on the sequencer: a port of ours subscribed to by the output, a queue of its own whose real-time
    clock the kernel sends the port's events on, and the encoder that turns a message's bytes into an event. Its sender
    hands it each message a lead ahead of its time, and Deliver schedules it on the queue at that time, so the kernel's
    timer - not the sender's wake - decides when it leaves. */
class SequencerOutput final : public MidiOutputStream
{
  public:
    int               index{0};            ///< the port number Send names it by
    snd_seq_t        *seq{nullptr};        ///< the output client; not owned
    int               port{-1};            ///< our port, subscribed to by the output; -1 for none
    int               sequencer_queue{-1}; ///< the sequencer queue its events are scheduled on; -1 for none
    snd_midi_event_t *coder{nullptr};      ///< the encoder from bytes to events
    std::int64_t      zero{0};             ///< the steady clock at the queue's zero, in nanoseconds (QueueZero)

  protected:
    bool Deliver(const MidiOutputMessage &message) noexcept override
    {
        snd_seq_event_t event;
        snd_seq_ev_clear(&event);
        snd_midi_event_reset_encode(coder);
        const long used = snd_midi_event_encode(coder, message.bytes.data(), message.size, &event);
        if (used != message.size || event.type == SND_SEQ_EVENT_NONE)
        {
            return true; // a message the encoder will not make an event of; none Send takes
        }
        // On the queue's clock; a time already past leaves at once, after any of the queue's still to go.
        const std::int64_t  stamp = std::max(message.time - zero, std::int64_t{0});
        snd_seq_real_time_t real;
        real.tv_sec  = static_cast<unsigned int>(stamp / 1'000'000'000);
        real.tv_nsec = static_cast<unsigned int>(stamp % 1'000'000'000);
        snd_seq_ev_set_source(&event, static_cast<unsigned char>(port));
        snd_seq_ev_set_subs(&event);
        snd_seq_ev_schedule_real(&event, sequencer_queue, 0, &real);
        const int result = snd_seq_event_output_direct(seq, &event);
        if (result == -EAGAIN)
        {
            return false; // the client's pool in the kernel is full: offered again on the next round
        }
        return true;
    }
};

/** ALSA's sequencer as a MidiSystem: a listing client, and an input client whose ports each carry one opened input to
    its sink on the input thread. Message thread throughout but for Run, the input thread. */
class AlsaMidiSystem final : public MidiSystem
{
  public:
    /** A system whose sequencer clients and ports are named after `client_name`.
        @param client_name  the name its client and ports carry */
    explicit AlsaMidiSystem(std::string client_name) : client_name_(std::move(client_name))
    {
    }

    /** Every open input closed (CloseAll), and the clients with them. */
    ~AlsaMidiSystem() override
    {
        CloseAll();
        for (snd_seq_t *seq : {listing_, input_, output_})
        {
            if (seq != nullptr)
            {
                snd_seq_close(seq);
            }
        }
        if (wake_ >= 0)
        {
            close(wake_);
        }
    }

    AlsaMidiSystem(const AlsaMidiSystem &)            = delete;
    AlsaMidiSystem &operator=(const AlsaMidiSystem &) = delete;

    std::vector<std::string> Inputs() override
    {
        std::vector<std::string> names;
        if (!Connect())
        {
            return names;
        }
        for (Source &source : Sources(listing_, kReadable, ClientOf(output_), -1))
        {
            names.push_back(std::move(source.name));
        }
        return names;
    }

    bool Open(const std::string &name, int port, MidiSink &sink) override
    {
        if (!Connect())
        {
            return false;
        }
        const std::vector<Source> sources = Sources(listing_, kReadable, ClientOf(output_), -1);
        const Source             *source  = Find(sources, name);
        if (source == nullptr)
        {
            Keep("no MIDI input named '" + name + "'");
            return false;
        }
        StopThread();
        Close(port);
        Input input;
        input.index                 = port;
        input.sink                  = &sink;
        const std::string port_name = client_name_ + " MIDI in";
        input.port =
            snd_seq_create_simple_port(input_, port_name.c_str(), SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                       SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        int result = input.port;
        if (result >= 0)
        {
            result = snd_seq_connect_from(input_, input.port, source->address.client, source->address.port);
        }
        if (result >= 0)
        {
            result = snd_midi_event_new(kMessageBytes, &input.coder);
        }
        if (result < 0)
        {
            Release(input);
            StartThread();
            Keep("the MIDI input '" + name + "' would not open: " + snd_strerror(result));
            return false;
        }
        snd_midi_event_no_status(input.coder, 1); // every message whole, its status byte with it
        inputs_.push_back(input);
        StartThread();
        return true;
    }

    std::vector<std::string> Outputs() override
    {
        std::vector<std::string> names;
        if (!Connect())
        {
            return names;
        }
        for (Source &destination : Sources(listing_, kWritable, ClientOf(input_), ClientOf(output_)))
        {
            names.push_back(std::move(destination.name));
        }
        return names;
    }

    bool OpenOutput(const std::string &name, int port) override
    {
        if (!MidiOutputPorts::IsPortNumber(port))
        {
            Keep("the MIDI output '" + name + "' cannot be port " + std::to_string(port) + ": ports are 0 to " +
                 std::to_string(kMidiOutputPorts - 1));
            return false;
        }
        if (!Connect() || !ConnectOutput())
        {
            return false;
        }
        const std::vector<Source> destinations = Sources(listing_, kWritable, ClientOf(input_), ClientOf(output_));
        const Source             *destination  = Find(destinations, name);
        if (destination == nullptr)
        {
            Keep("no MIDI output named '" + name + "'");
            return false;
        }
        ports_.Withdraw(port);
        sender_.Stop();
        CloseOutput(port);
        auto output                 = std::make_unique<SequencerOutput>();
        output->index               = port;
        output->seq                 = output_;
        const std::string port_name = client_name_ + " MIDI out";
        output->port = snd_seq_create_simple_port(output_, port_name.c_str(), kReadable,
                                                  SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        int result   = output->port;
        if (result >= 0)
        {
            result = snd_seq_connect_to(output_, output->port, destination->address.client, destination->address.port);
        }
        if (result >= 0)
        {
            result                  = snd_seq_alloc_queue(output_);
            output->sequencer_queue = result;
        }
        if (result >= 0)
        {
            result = TickFinely(output_, output->sequencer_queue);
        }
        if (result >= 0)
        {
            result = snd_seq_start_queue(output_, output->sequencer_queue, nullptr);
        }
        if (result >= 0)
        {
            result = snd_seq_drain_output(output_);
        }
        if (result >= 0)
        {
            result = snd_midi_event_new(kMessageBytes, &output->coder);
        }
        if (result < 0)
        {
            ReleaseOutput(*output);
            StartSender();
            Keep("the MIDI output '" + name + "' would not open: " + snd_strerror(result));
            return false;
        }
        output->zero = QueueZero(output_, output->sequencer_queue);
        ports_.Publish(port, output->queue());
        outputs_.push_back(std::move(output));
        StartSender();
        return true;
    }

    bool Send(int port, std::int64_t time_nanoseconds, const std::uint8_t *bytes, int size) noexcept override
    {
        return ports_.Send(port, time_nanoseconds, bytes, size);
    }

    void CloseAll() override
    {
        StopThread();
        for (Input &input : inputs_)
        {
            Release(input);
        }
        inputs_.clear();
        for (const std::unique_ptr<SequencerOutput> &output : outputs_)
        {
            ports_.Withdraw(output->index);
        }
        sender_.Stop();
        for (const std::unique_ptr<SequencerOutput> &output : outputs_)
        {
            ReleaseOutput(*output);
        }
        outputs_.clear();
    }

    std::string LastError() const override
    {
        const std::lock_guard lock(error_mutex_);
        return error_;
    }

  private:
    /** One opened input: the index its messages carry, its sink, the port of ours subscribed to it, and the decoder
       that turns its events into bytes. */
    struct Input
    {
        int               index{0};       ///< the index its messages carry to the sink
        MidiSink         *sink{nullptr};  ///< where they land; not owned
        int               port{-1};       ///< our port subscribed to the input; -1 for none
        snd_midi_event_t *coder{nullptr}; ///< its decoder, made at the open
    };

    /** The two clients and the input thread's wake, made on first use.
        @return  true when the sequencer answered */
    bool Connect()
    {
        if (listing_ != nullptr)
        {
            return true;
        }
        snd_seq_t *listing = nullptr;
        snd_seq_t *input   = nullptr;
        int        result  = snd_seq_open(&listing, "default", SND_SEQ_OPEN_DUPLEX, SND_SEQ_NONBLOCK);
        if (result >= 0)
        {
            result = snd_seq_open(&input, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK);
        }
        const int wake = result >= 0 ? eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC) : -1;
        if (result < 0 || wake < 0)
        {
            for (snd_seq_t *seq : {listing, input})
            {
                if (seq != nullptr)
                {
                    snd_seq_close(seq);
                }
            }
            Keep(result < 0 ? std::string("ALSA's sequencer would not open: ") + snd_strerror(result)
                            : std::string("the MIDI thread could not be given a way to be woken"));
            return false;
        }
        snd_seq_set_client_name(listing, (client_name_ + " probe").c_str());
        snd_seq_set_client_name(input, client_name_.c_str());
        listing_ = listing;
        input_   = input;
        wake_    = wake;
        return true;
    }

    /** The input opened as `port` closed, if there is one; the thread stopped already.
        @param port  the input's index */
    void Close(int port)
    {
        const auto at =
            std::find_if(inputs_.begin(), inputs_.end(), [port](const Input &input) { return input.index == port; });
        if (at != inputs_.end())
        {
            Release(*at);
            inputs_.erase(at);
        }
    }

    /** An input's port deleted - its subscription with it - and its decoder freed.
        @param input  the input */
    void Release(Input &input)
    {
        if (input.port >= 0)
        {
            snd_seq_delete_simple_port(input_, input.port);
            input.port = -1;
        }
        if (input.coder != nullptr)
        {
            snd_midi_event_free(input.coder);
            input.coder = nullptr;
        }
    }

    /** The output client, made on first use: apart from the input client, since its handle is the sender's.
        @return  true when the sequencer answered */
    bool ConnectOutput()
    {
        if (output_ != nullptr)
        {
            return true;
        }
        snd_seq_t *output = nullptr;
        const int  result = snd_seq_open(&output, "default", SND_SEQ_OPEN_OUTPUT, SND_SEQ_NONBLOCK);
        if (result < 0)
        {
            Keep(std::string("ALSA's sequencer would not open for output: ") + snd_strerror(result));
            return false;
        }
        snd_seq_set_client_name(output, (client_name_ + " out").c_str());
        output_ = output;
        return true;
    }

    /** The output open as `port` closed, if there is one; withdrawn from Send and the sender stopped already.
        @param port  the port */
    void CloseOutput(int port)
    {
        const auto at =
            std::find_if(outputs_.begin(), outputs_.end(),
                         [port](const std::unique_ptr<SequencerOutput> &output) { return output->index == port; });
        if (at != outputs_.end())
        {
            ReleaseOutput(**at);
            outputs_.erase(at);
        }
    }

    /** An output's queue freed - every event it held and had not yet sent with it - and its port deleted, its
        subscription with it, and its encoder freed. The sender stopped already.
        @param output  the output */
    void ReleaseOutput(SequencerOutput &output)
    {
        if (output.sequencer_queue >= 0)
        {
            snd_seq_free_queue(output_, output.sequencer_queue);
            output.sequencer_queue = -1;
        }
        snd_seq_drop_output(output_);
        if (output.port >= 0)
        {
            snd_seq_delete_simple_port(output_, output.port);
            output.port = -1;
        }
        if (output.coder != nullptr)
        {
            snd_midi_event_free(output.coder);
            output.coder = nullptr;
        }
        output.Drop();
    }

    /** The sender started on every open output, where there is one. */
    void StartSender()
    {
        std::vector<MidiOutputStream *> streams;
        for (const std::unique_ptr<SequencerOutput> &output : outputs_)
        {
            streams.push_back(output.get());
        }
        sender_.Start(std::move(streams));
    }

    /** The input thread started, where there is an input for it to carry. */
    void StartThread()
    {
        if (inputs_.empty() || thread_.joinable())
        {
            return;
        }
        std::uint64_t drained = 0;
        while (read(wake_, &drained, sizeof drained) > 0)
        {
        }
        stop_.store(false, std::memory_order_relaxed);
        thread_ = std::thread([this] { Run(); });
    }

    /** The input thread woken and joined: once this returns, no sink is called. */
    void StopThread()
    {
        if (!thread_.joinable())
        {
            return;
        }
        stop_.store(true, std::memory_order_release);
        const std::uint64_t            one   = 1;
        [[maybe_unused]] const ssize_t woken = write(wake_, &one, sizeof one); // an eventfd that cannot be full
        thread_.join();
    }

    /** The input thread: poll on the input client and the wake, and each event that reaches one of our ports handed
        to that input's sink as bytes, stamped as it arrives - but system exclusive, timing clock and active sensing,
        which the cores do not read. */
    void Run()
    {
        const int           count = snd_seq_poll_descriptors_count(input_, POLLIN);
        std::vector<pollfd> descriptors(static_cast<std::size_t>(1 + std::max(count, 0)));
        descriptors[0].fd     = wake_;
        descriptors[0].events = POLLIN;
        snd_seq_poll_descriptors(input_, descriptors.data() + 1, static_cast<unsigned int>(std::max(count, 0)), POLLIN);
        std::uint8_t bytes[kMessageBytes];
        while (!stop_.load(std::memory_order_acquire))
        {
            if (snd_seq_event_input_pending(input_, 1) <= 0)
            {
                if (poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), -1) < 0 && errno != EINTR)
                {
                    Keep(std::string("the MIDI thread's wait failed: ") + std::strerror(errno), false);
                    return;
                }
                continue;
            }
            snd_seq_event_t *event  = nullptr;
            const int        result = snd_seq_event_input(input_, &event);
            if (result < 0 || event == nullptr)
            {
                continue; // nothing after all (-EAGAIN), or the kernel's queue overran (-ENOSPC) and dropped some
            }
            const std::int64_t now = NowNanoseconds();
            switch (event->type)
            {
            case SND_SEQ_EVENT_SYSEX:
            case SND_SEQ_EVENT_CLOCK:
            case SND_SEQ_EVENT_TICK:
            case SND_SEQ_EVENT_QFRAME:
            case SND_SEQ_EVENT_SENSING:
                continue;
            default:
                break;
            }
            for (Input &input : inputs_)
            {
                if (input.port != event->dest.port)
                {
                    continue;
                }
                const long size = snd_midi_event_decode(input.coder, bytes, sizeof bytes, event);
                if (size > 0)
                {
                    input.sink->Arrived(input.index, now, bytes, static_cast<int>(size));
                }
                break;
            }
        }
    }

    /** `text` kept for LastError; a caller that may not wait (`waits` false) keeps nothing while a reader holds it.
        @param text   the error's text
        @param waits  true to wait for a reader to finish, false to drop the text rather than wait */
    void Keep(const std::string &text, bool waits = true)
    {
        std::unique_lock lock(error_mutex_, std::defer_lock);
        if (waits)
        {
            lock.lock();
        }
        else if (!lock.try_lock())
        {
            return;
        }
        try
        {
            error_ = text;
        }
        catch (const std::exception &)
        {
        }
    }

    std::string        client_name_;      ///< the name its clients and ports carry
    snd_seq_t         *listing_{nullptr}; ///< the client the inputs and outputs are listed through
    snd_seq_t         *input_{nullptr};   ///< the client whose ports the opened inputs reach
    snd_seq_t         *output_{nullptr};  ///< the client whose ports reach the opened outputs; the sender's
    int                wake_{-1};         ///< the eventfd StopThread wakes the input thread through
    std::vector<Input> inputs_;           ///< the opened inputs; changed only while the thread is stopped
    std::atomic<bool>  stop_{false};      ///< true tells the input thread to end
    std::thread        thread_;           ///< the input thread, while there is an input open
    mutable std::mutex error_mutex_;      ///< guards `error_`
    std::string        error_;            ///< the last error, for a person to read
    std::vector<std::unique_ptr<SequencerOutput>>
                     outputs_;                           ///< the opened outputs; changed only with the sender stopped
    MidiOutputPorts  ports_;                             ///< the opened outputs' queues, as Send finds them
    MidiOutputSender sender_{kSequencerLeadNanoseconds}; ///< hands the outputs' messages to the sequencer
};

/** The machine's MIDI inputs on ALSA's sequencer.
    @param client_name  the name its client and ports carry, a Standalone's plugin name
    @return             the system, owned by the caller */
std::unique_ptr<MidiSystem> MakeMidi(const std::string &client_name)
{
    return std::make_unique<AlsaMidiSystem>(client_name);
}

/** What this back end is.
    @return  "ALSA sequencer" */
std::string Library()
{
    return "ALSA sequencer";
}
} // namespace

const MidiBackend &AlsaMidiBackend()
{
    static const MidiBackend backend{&Library, &MakeMidi};
    return backend;
}
} // namespace reckless::audio
