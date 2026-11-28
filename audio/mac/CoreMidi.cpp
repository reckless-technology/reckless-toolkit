// MIDI input and output on CoreMIDI (CoreMidi.h says what it does and why): the one translation unit that meets
// CoreMIDI. Its headers bring the Carbon types into the global namespace, so nothing here names its own the same.
#include "CoreMidi.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include "CoreMidiFormats.h"
#include "MidiOutputPorts.h"
#include "MidiOutputSender.h"

namespace reckless::audio
{
namespace
{
/** A CFString as UTF-8.
    @param text  the string; may be null
    @return      the UTF-8 text */
std::string Utf8(CFStringRef text)
{
    if (text == nullptr)
    {
        return {};
    }
    const CFIndex     length = CFStringGetLength(text);
    const CFIndex     most   = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
    std::vector<char> bytes(static_cast<std::size_t>(std::max<CFIndex>(most, 1)), '\0');
    if (!CFStringGetCString(text, bytes.data(), most, kCFStringEncodingUTF8))
    {
        return {};
    }
    return std::string(bytes.data());
}

/** UTF-8 as a CFString, owned by the caller.
    @param text  the text
    @return      the string */
CFStringRef MakeString(const std::string &text)
{
    return CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(text.data()),
                                   static_cast<CFIndex>(text.size()), kCFStringEncodingUTF8, false);
}

/** An object's name, as UTF-8.
    @param object  the object
    @param named   set to whether it has a name at all
    @return        the name; empty where it has none */
std::string NameOf(MIDIObjectRef object, bool *named = nullptr)
{
    CFStringRef name = nullptr;
    const bool  has  = MIDIObjectGetStringProperty(object, kMIDIPropertyName, &name) == noErr && name != nullptr;
    if (named != nullptr)
    {
        *named = has;
    }
    if (!has)
    {
        return {};
    }
    std::string text = Utf8(name);
    CFRelease(name);
    return text;
}

/** An endpoint's name as RtMidi composed it from its entity and device (coremidi::EndpointName).
    @param endpoint     the endpoint
    @param is_external  whether it is an external device's
    @return             the name */
std::string EndpointName(MIDIEndpointRef endpoint, bool is_external)
{
    coremidi::EndpointParts parts;
    parts.endpoint       = NameOf(endpoint);
    MIDIEntityRef entity = 0;
    MIDIEndpointGetEntity(endpoint, &entity);
    parts.has_entity = entity != 0;
    if (parts.has_entity)
    {
        parts.entity         = NameOf(entity);
        MIDIDeviceRef device = 0;
        MIDIEntityGetDevice(entity, &device);
        parts.has_device = device != 0;
        if (parts.has_device)
        {
            parts.device          = NameOf(device, &parts.device_named);
            parts.device_entities = static_cast<int>(MIDIDeviceGetNumberOfEntities(device));
        }
    }
    return coremidi::EndpointName(parts, is_external);
}

/** An endpoint's name as RtMidi listed it, a source's or a destination's: the names of the external endpoints it is
    connected to, joined; else its own endpoint's (coremidi::JoinConnected).
    @param source  the source or destination
    @return        the name */
std::string ListedName(MIDIEndpointRef source)
{
    std::vector<std::string> connected;
    CFDataRef                connections = nullptr;
    if (MIDIObjectGetDataProperty(source, kMIDIPropertyConnectionUniqueID, &connections) == noErr &&
        connections != nullptr)
    {
        const CFIndex count = CFDataGetLength(connections) / static_cast<CFIndex>(sizeof(MIDIUniqueID));
        const auto   *ids   = reinterpret_cast<const SInt32 *>(CFDataGetBytePtr(connections));
        for (CFIndex i = 0; i < count; ++i)
        {
            const auto     id     = static_cast<MIDIUniqueID>(CFSwapInt32BigToHost(static_cast<uint32_t>(ids[i])));
            MIDIObjectRef  object = 0;
            MIDIObjectType type   = kMIDIObjectType_Other;
            if (MIDIObjectFindByUniqueID(id, &object, &type) != noErr)
            {
                continue;
            }
            const bool external = type == kMIDIObjectType_ExternalSource || type == kMIDIObjectType_ExternalDestination;
            connected.push_back(external ? EndpointName(static_cast<MIDIEndpointRef>(object), true) : NameOf(object));
        }
        CFRelease(connections);
    }
    const std::string joined = coremidi::JoinConnected(connected);
    return !joined.empty() ? joined : EndpointName(source, false);
}

/** CoreMIDI's sources now, as RtMidi listed them, CoreMIDI's notifications let in first so a source plugged in since
    is there.
    @return  each source and its name, in CoreMIDI's order */
std::vector<std::pair<MIDIEndpointRef, std::string>> Sources()
{
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);
    std::vector<std::pair<MIDIEndpointRef, std::string>> sources;
    const ItemCount                                      count = MIDIGetNumberOfSources();
    for (ItemCount i = 0; i < count; ++i)
    {
        const MIDIEndpointRef source = MIDIGetSource(i);
        if (source != 0)
        {
            sources.emplace_back(source, ListedName(source));
        }
    }
    return sources;
}

/** CoreMIDI's destinations now, as RtMidi listed them, CoreMIDI's notifications let in first so a destination plugged
    in since is there.
    @return  each destination and its name, in CoreMIDI's order */
std::vector<std::pair<MIDIEndpointRef, std::string>> Destinations()
{
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, false);
    std::vector<std::pair<MIDIEndpointRef, std::string>> destinations;
    const ItemCount                                      count = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < count; ++i)
    {
        const MIDIEndpointRef destination = MIDIGetDestination(i);
        if (destination != 0)
        {
            destinations.emplace_back(destination, ListedName(destination));
        }
    }
    return destinations;
}

/** How far ahead of its time the sender hands a message to CoreMIDI, stamped with that time, which CoreMIDI's own
    scheduler sends it at: far enough that an ordinary thread's late wake does not make a message late, near enough
    that little is scheduled when an output closes, which unschedules it. */
constexpr std::int64_t kCoreMidiLeadNanoseconds = 20'000'000;

/** One output open on CoreMIDI: an output port of ours and the destination it sends to. Its sender hands it each
    message a lead ahead of its time, and Deliver sends it as one Universal MIDI Packet stamped with that time in host
    ticks, so CoreMIDI's scheduler - not the sender's wake - decides when it leaves. */
class CoreMidiOutput final : public MidiOutputStream
{
  public:
    int                       index{0};       ///< the port number Send names it by
    MIDIPortRef               port{0};        ///< our output port; 0 once shut
    MIDIEndpointRef           destination{0}; ///< where it sends
    mach_timebase_info_data_t timebase{};     ///< host ticks to nanoseconds, read at the open

  protected:
    bool Deliver(const MidiOutputMessage &message) noexcept override
    {
        const std::int64_t  now      = NowNanoseconds();
        const std::uint64_t host_now = mach_absolute_time();
        // CoreMIDI's scheduler sends each packet at its stamp, so a message handed on after one of a later time still
        // leaves first; one sent for a time past is stamped now.
        std::uint64_t stamp = coremidi::HostTimeAt(message.time, now, host_now, timebase.numer, timebase.denom);
        // CoreMIDI orders packets of one stamp by a rule of its own (a note off before a note on), so two messages of
        // one time would not leave in the order they were sent. A message that is no earlier than the latest one
        // delivered is stamped at least a tick after it; one for an earlier time keeps its own stamp and leaves first.
        if (message.time >= latest_time_)
        {
            if (stamp <= latest_stamp_)
            {
                stamp = latest_stamp_ + 1;
            }
            latest_time_  = message.time;
            latest_stamp_ = stamp;
        }
        const std::uint32_t word = coremidi::Midi1Packet(message.bytes.data(), message.size);
        MIDIEventList       list{};
        MIDIEventPacket    *packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);
        packet                     = MIDIEventListAdd(&list, sizeof list, packet, stamp, 1, &word);
        if (packet != nullptr)
        {
            MIDISendEventList(port, destination, &list);
        }
        return true; // an error CoreMIDI reports is a destination gone, whose messages go nowhere
    }

  private:
    std::int64_t  latest_time_{std::numeric_limits<std::int64_t>::min()}; ///< the latest time delivered; sender thread
    std::uint64_t latest_stamp_{0};                                       ///< the stamp it was given; sender thread
};

/** CoreMIDI's sources and destinations as a MidiSystem: one client, an input port of its own for each source opened,
    each port's events handed by CoreMIDI's receive block to the sink, and an output port of its own for each
    destination opened, fed by the sender. Message thread throughout but for Received, which the receive blocks run,
    the sender's Deliver, and Send. */
class CoreMidiSystem final : public MidiSystem
{
  public:
    /** A system whose client and ports are named after `client_name`.
        @param client_name  the name its client and ports carry */
    explicit CoreMidiSystem(std::string client_name) : client_name_(std::move(client_name))
    {
    }

    /** Every open port closed (CloseAll), and the client with them. */
    ~CoreMidiSystem() override
    {
        CloseAll();
        if (client_ != 0)
        {
            MIDIClientDispose(client_);
        }
    }

    CoreMidiSystem(const CoreMidiSystem &)            = delete;
    CoreMidiSystem &operator=(const CoreMidiSystem &) = delete;

    std::vector<std::string> Inputs() override
    {
        std::vector<std::string> names;
        for (auto &source : Sources())
        {
            names.push_back(std::move(source.second));
        }
        return names;
    }

    bool Open(const std::string &name, int port, MidiSink &sink) override
    {
        Close(port);
        if (!Connect())
        {
            return false;
        }
        const auto            sources = Sources();
        const MIDIEndpointRef source  = Find(sources, name);
        if (source == 0)
        {
            Keep("no MIDI input named '" + name + "'");
            return false;
        }
        auto input         = std::make_unique<Input>();
        input->index       = port;
        input->sink        = &sink;
        Input      *target = input.get();
        CFStringRef label  = MakeString(client_name_ + " MIDI in");
        OSStatus    result = MIDIInputPortCreateWithProtocol(client_, label, kMIDIProtocol_1_0, &input->port,
                                                             ^(const MIDIEventList *events, void *) {
                                                            Received(*target, events);
                                                             });
        CFRelease(label);
        if (result == noErr)
        {
            result = MIDIPortConnectSource(input->port, source, nullptr);
        }
        if (result != noErr)
        {
            Shut(*input);
            Keep("the MIDI input '" + name + "' would not open: CoreMIDI error " + std::to_string(result));
            return false;
        }
        inputs_.push_back(std::move(input));
        return true;
    }

    std::vector<std::string> Outputs() override
    {
        std::vector<std::string> names;
        for (auto &destination : Destinations())
        {
            names.push_back(std::move(destination.second));
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
        if (!Connect())
        {
            return false;
        }
        const auto            destinations = Destinations();
        const MIDIEndpointRef destination  = Find(destinations, name);
        if (destination == 0)
        {
            Keep("no MIDI output named '" + name + "'");
            return false;
        }
        ports_.Withdraw(port);
        sender_.Stop();
        CloseOutput(port);
        auto output         = std::make_unique<CoreMidiOutput>();
        output->index       = port;
        output->destination = destination;
        mach_timebase_info(&output->timebase);
        CFStringRef    label  = MakeString(client_name_ + " MIDI out");
        const OSStatus result = MIDIOutputPortCreate(client_, label, &output->port);
        CFRelease(label);
        if (result != noErr || output->timebase.numer == 0 || output->timebase.denom == 0)
        {
            ShutOutput(*output);
            StartSender();
            Keep("the MIDI output '" + name + "' would not open: CoreMIDI error " + std::to_string(result));
            return false;
        }
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
        for (const std::unique_ptr<Input> &input : inputs_)
        {
            Shut(*input);
        }
        inputs_.clear();
        for (const std::unique_ptr<CoreMidiOutput> &output : outputs_)
        {
            ports_.Withdraw(output->index);
        }
        sender_.Stop();
        for (const std::unique_ptr<CoreMidiOutput> &output : outputs_)
        {
            ShutOutput(*output);
        }
        outputs_.clear();
    }

    std::string LastError() const override
    {
        const std::lock_guard lock(error_mutex_);
        return error_;
    }

  private:
    /** One opened source: the index its messages carry, its sink, and the port of ours connected to it. Its address is
        what the port's receive block holds, so it stays where it was made until it is shut. */
    struct Input
    {
        int         index{0};      ///< the index its messages carry to the sink
        MidiSink   *sink{nullptr}; ///< where they land; not owned
        MIDIPortRef port{0};       ///< our port; 0 once shut
    };

    /** The client made, on first use.
        @return  true when CoreMIDI answered */
    bool Connect()
    {
        if (client_ != 0)
        {
            return true;
        }
        CFStringRef    name   = MakeString(client_name_);
        const OSStatus result = MIDIClientCreate(name, nullptr, nullptr, &client_);
        CFRelease(name);
        if (result != noErr)
        {
            client_ = 0;
            Keep("CoreMIDI would not make a client: error " + std::to_string(result));
            return false;
        }
        return true;
    }

    /** The source a settings file's `name` means: that name exactly, else the first whose name holds it, as RtMidi
        found a shorter name.
        @param sources  the sources listed
        @param name     the name kept
        @return         the source; 0 for none */
    static MIDIEndpointRef Find(const std::vector<std::pair<MIDIEndpointRef, std::string>> &sources,
                                const std::string                                          &name)
    {
        for (const auto &[source, listed] : sources)
        {
            if (listed == name)
            {
                return source;
            }
        }
        for (const auto &[source, listed] : sources)
        {
            if (!name.empty() && listed.find(name) != std::string::npos)
            {
                return source;
            }
        }
        return 0;
    }

    /** A port disposed of - its connection with it - and any receive block still under way waited for: once this
        returns, the input's sink is called no more.
        @param input  the input */
    void Shut(Input &input) noexcept
    {
        if (input.port != 0)
        {
            MIDIPortDispose(input.port);
            input.port = 0;
        }
        while (in_flight_.load() != 0)
        {
            std::this_thread::yield();
        }
    }

    /** The input opened as `port` closed, if there is one.
        @param port  the input's index */
    void Close(int port)
    {
        const auto at = std::find_if(inputs_.begin(), inputs_.end(),
                                     [port](const std::unique_ptr<Input> &input) { return input->index == port; });
        if (at != inputs_.end())
        {
            Shut(**at);
            inputs_.erase(at);
        }
    }

    /** A port's receive block, on CoreMIDI's thread: each MIDI 1.0 message among the Universal MIDI Packets handed to
        the input's sink as its bytes, stamped as they arrive - but what the cores do not read (coremidi::Midi1Message).
        Counted in and out, so closing a port can wait for none to be under way.
        @param input   the input the port carries
        @param events  the events, as Universal MIDI Packets */
    void Received(const Input &input, const MIDIEventList *events) noexcept
    {
        in_flight_.fetch_add(1);
        const std::int64_t     now    = NowNanoseconds();
        const MIDIEventPacket *packet = events != nullptr ? &events->packet[0] : nullptr;
        for (UInt32 p = 0; packet != nullptr && p < events->numPackets; ++p)
        {
            for (UInt32 w = 0; w < packet->wordCount;)
            {
                const std::uint32_t first = packet->words[w];
                std::uint8_t        bytes[3];
                const int           size = coremidi::Midi1Message(first, bytes);
                if (size > 0)
                {
                    input.sink->Arrived(input.index, now, bytes, size);
                }
                w += static_cast<UInt32>(coremidi::UmpWords(first));
            }
            packet = MIDIEventPacketNext(packet);
        }
        in_flight_.fetch_sub(1);
    }

    /** An output shut, the sender stopped already: what it had handed CoreMIDI ahead of its time unscheduled, its port
        disposed of, and what it still held dropped.
        @param output  the output */
    static void ShutOutput(CoreMidiOutput &output) noexcept
    {
        if (output.port != 0)
        {
            MIDIFlushOutput(output.destination);
            MIDIPortDispose(output.port);
            output.port = 0;
        }
        output.Drop();
    }

    /** The output open as `port` closed, if there is one; withdrawn from Send and the sender stopped already.
        @param port  the port */
    void CloseOutput(int port)
    {
        const auto at =
            std::find_if(outputs_.begin(), outputs_.end(),
                         [port](const std::unique_ptr<CoreMidiOutput> &output) { return output->index == port; });
        if (at != outputs_.end())
        {
            ShutOutput(**at);
            outputs_.erase(at);
        }
    }

    /** The sender started on every open output, where there is one. */
    void StartSender()
    {
        std::vector<MidiOutputStream *> streams;
        for (const std::unique_ptr<CoreMidiOutput> &output : outputs_)
        {
            streams.push_back(output.get());
        }
        sender_.Start(std::move(streams));
    }

    /** `text` kept for LastError.
        @param text  the error's text */
    void Keep(const std::string &text)
    {
        const std::lock_guard lock(error_mutex_);
        error_ = text;
    }

    std::string                                  client_name_;  ///< the name its client and ports carry
    MIDIClientRef                                client_{0};    ///< the client, made on first use; 0 for none
    std::vector<std::unique_ptr<Input>>          inputs_;       ///< the open inputs
    std::atomic<int>                             in_flight_{0}; ///< receive blocks under way
    mutable std::mutex                           error_mutex_;  ///< guards `error_`
    std::string                                  error_;        ///< the last error, for a person to read
    std::vector<std::unique_ptr<CoreMidiOutput>> outputs_; ///< the open outputs; changed only with the sender stopped
    MidiOutputPorts                              ports_;   ///< the open outputs' queues, as Send finds them
    MidiOutputSender sender_{kCoreMidiLeadNanoseconds};    ///< hands the outputs' messages to CoreMIDI
};

/** The machine's MIDI inputs on CoreMIDI.
    @param client_name  the name its client and ports carry, a Standalone's plugin name
    @return             the system, owned by the caller */
std::unique_ptr<MidiSystem> MakeMidi(const std::string &client_name)
{
    return std::make_unique<CoreMidiSystem>(client_name);
}

/** What this back end is.
    @return  "CoreMIDI" */
std::string Library()
{
    return "CoreMIDI";
}
} // namespace

const MidiBackend &CoreMidiBackend()
{
    static const MidiBackend backend{&Library, &MakeMidi};
    return backend;
}
} // namespace reckless::audio
