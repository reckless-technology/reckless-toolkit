// MIDI input on ALSA's sequencer (AlsaMidi.h says what it does and why): the one translation unit that meets the
// sequencer.
#include "AlsaMidi.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
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

namespace reckless::audio
{
namespace
{
/** The bytes one decoded message may take: every message that reaches a sink is short, system exclusive being left
    out. */
constexpr std::size_t kMessageBytes = 64;

/** One sequencer port another client offers for reading: where it is, and the name the series lists it under. */
struct Source
{
    snd_seq_addr_t address{}; ///< its client and port numbers
    std::string    name;      ///< "<client>:<port> <client number>:<port number>", RtMidi's
};

/** Every port `seq` can read from and subscribe to that is a MIDI, synth or application port, client by client and
    port by port, client 0 (the sequencer's own) left out - the ports RtMidi listed, in its order, under its names.
    @param seq  a sequencer handle
    @return     the ports */
std::vector<Source> Sources(snd_seq_t *seq)
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
        if (number == 0)
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
            constexpr unsigned int kReadable = SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ;
            if ((snd_seq_port_info_get_capability(port) & kReadable) != kReadable)
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
        for (snd_seq_t *seq : {listing_, input_})
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
        for (Source &source : Sources(listing_))
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
        const std::vector<Source> sources = Sources(listing_);
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

    void CloseAll() override
    {
        StopThread();
        for (Input &input : inputs_)
        {
            Release(input);
        }
        inputs_.clear();
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
    snd_seq_t         *listing_{nullptr}; ///< the client the inputs are listed through
    snd_seq_t         *input_{nullptr};   ///< the client whose ports the opened inputs reach
    int                wake_{-1};         ///< the eventfd StopThread wakes the input thread through
    std::vector<Input> inputs_;           ///< the opened inputs; changed only while the thread is stopped
    std::atomic<bool>  stop_{false};      ///< true tells the input thread to end
    std::thread        thread_;           ///< the input thread, while there is an input open
    mutable std::mutex error_mutex_;      ///< guards `error_`
    std::string        error_;            ///< the last error, for a person to read
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
