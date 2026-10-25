// MIDI input on CoreMIDI (CoreMidi.h says what it does and why): the one translation unit that meets CoreMIDI. Its
// headers bring the Carbon types into the global namespace, so nothing here names its own the same.
#include "CoreMidi.h"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

#include "CoreMidiFormats.h"

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

/** A source's name as RtMidi listed it: the names of the external endpoints it is connected to, joined; else its own
    endpoint's (coremidi::JoinConnected).
    @param source  the source
    @return        the name */
std::string SourceName(MIDIEndpointRef source)
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
            sources.emplace_back(source, SourceName(source));
        }
    }
    return sources;
}

/** CoreMIDI's sources as a MidiSystem: one client, an input port of its own for each source opened, each port's
    events handed by CoreMIDI's receive block to the sink. Message thread throughout but for Received, which the receive
    blocks run. */
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

    void CloseAll() override
    {
        for (const std::unique_ptr<Input> &input : inputs_)
        {
            Shut(*input);
        }
        inputs_.clear();
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

    /** `text` kept for LastError.
        @param text  the error's text */
    void Keep(const std::string &text)
    {
        const std::lock_guard lock(error_mutex_);
        error_ = text;
    }

    std::string                         client_name_;  ///< the name its client and ports carry
    MIDIClientRef                       client_{0};    ///< the client, made on first use; 0 for none
    std::vector<std::unique_ptr<Input>> inputs_;       ///< the open inputs
    std::atomic<int>                    in_flight_{0}; ///< receive blocks under way
    mutable std::mutex                  error_mutex_;  ///< guards `error_`
    std::string                         error_;        ///< the last error, for a person to read
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
