// CoreMIDI's source and destination of the suite's own: a virtual source and a virtual destination on MIDI 1.0's
// protocol, made under a name the test chooses (DeviceRig.h, PrivateMidiSource and PrivateMidiDestination). In a
// translation unit of its own: CoreMIDI's headers bring the Carbon types into the global namespace, which the suite's
// own names would meet.
#include "DeviceRig.h"

#include <cstdint>
#include <string>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>
#include <mach/mach_time.h>

#include "CoreMidiFormats.h"

namespace reckless::audio::test
{
static_assert(sizeof(MIDIClientRef) == sizeof(std::uint32_t), "the handle holds a MIDIClientRef");
static_assert(sizeof(MIDIEndpointRef) == sizeof(std::uint32_t), "the handle holds a MIDIEndpointRef");

/// @cond
PrivateMidiSource::PrivateMidiSource(const std::string &name)
{
    CFStringRef   cf_name = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(name.data()),
                                                    static_cast<CFIndex>(name.size()), kCFStringEncodingUTF8, false);
    MIDIClientRef client  = 0;
    OSStatus      result  = MIDIClientCreate(cf_name, nullptr, nullptr, &client);
    if (result != noErr)
    {
        why_ = "MIDIClientCreate: " + std::to_string(result);
        CFRelease(cf_name);
        return;
    }
    client_                = client;
    MIDIEndpointRef source = 0;
    result                 = MIDISourceCreateWithProtocol(client, cf_name, kMIDIProtocol_1_0, &source);
    CFRelease(cf_name);
    if (result != noErr)
    {
        why_ = "MIDISourceCreateWithProtocol: " + std::to_string(result);
        return;
    }
    source_ = source;
}

PrivateMidiSource::~PrivateMidiSource()
{
    if (source_ != 0)
    {
        MIDIEndpointDispose(source_);
    }
    if (client_ != 0)
    {
        MIDIClientDispose(client_);
    }
}

void PrivateMidiSource::Send(const std::uint32_t *words, int count)
{
    MIDIEventList    list{};
    MIDIEventPacket *packet = MIDIEventListInit(&list, kMIDIProtocol_1_0);
    packet                  = MIDIEventListAdd(&list, sizeof(list), packet, 0, static_cast<ByteCount>(count), words);
    if (packet != nullptr)
    {
        MIDIReceivedEventList(source_, &list);
    }
}

PrivateMidiDestination::PrivateMidiDestination(const std::string &name, MidiSink &sink)
{
    CFStringRef   cf_name = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(name.data()),
                                                    static_cast<CFIndex>(name.size()), kCFStringEncodingUTF8, false);
    MIDIClientRef client  = 0;
    OSStatus      result  = MIDIClientCreate(cf_name, nullptr, nullptr, &client);
    if (result != noErr)
    {
        why_ = "MIDIClientCreate: " + std::to_string(result);
        CFRelease(cf_name);
        return;
    }
    client_ = client;
    mach_timebase_info_data_t timebase{};
    mach_timebase_info(&timebase);
    MidiSink       *target      = &sink;
    MIDIEndpointRef destination = 0;
    result                      = MIDIDestinationCreateWithProtocol(
        client, cf_name, kMIDIProtocol_1_0, &destination, ^(const MIDIEventList *events, void *) {
          const std::int64_t     now      = NowNanoseconds();
          const std::uint64_t    host_now = mach_absolute_time();
          const MIDIEventPacket *packet   = &events->packet[0];
          for (UInt32 p = 0; p < events->numPackets; ++p)
          {
              std::int64_t due = now;
              if (packet->timeStamp > host_now && timebase.denom != 0)
              {
                  due += static_cast<std::int64_t>((packet->timeStamp - host_now) * timebase.numer / timebase.denom);
              }
              for (UInt32 w = 0; w < packet->wordCount;)
              {
                  std::uint8_t bytes[3];
                  const int    size = coremidi::Midi1Message(packet->words[w], bytes);
                  if (size > 0)
                  {
                      target->Arrived(0, due, bytes, size);
                  }
                  w += static_cast<UInt32>(coremidi::UmpWords(packet->words[w]));
              }
              packet = MIDIEventPacketNext(packet);
          }
        });
    CFRelease(cf_name);
    if (result != noErr)
    {
        why_ = "MIDIDestinationCreateWithProtocol: " + std::to_string(result);
        return;
    }
    destination_ = destination;
}

PrivateMidiDestination::~PrivateMidiDestination()
{
    if (destination_ != 0)
    {
        MIDIEndpointDispose(destination_);
    }
    if (client_ != 0)
    {
        MIDIClientDispose(client_);
    }
}

/// @endcond
} // namespace reckless::audio::test
