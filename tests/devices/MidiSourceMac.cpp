// CoreMIDI's source of the suite's own: a virtual source on MIDI 1.0's protocol, made under a name the test chooses
// (DeviceRig.h, PrivateMidiSource). In a translation unit of its own: CoreMIDI's headers bring the Carbon types into
// the global namespace, which the suite's own names would meet.
#include "DeviceRig.h"

#include <cstdint>
#include <string>

#include <CoreFoundation/CoreFoundation.h>
#include <CoreMIDI/CoreMIDI.h>

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

/// @endcond
} // namespace reckless::audio::test
