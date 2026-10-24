// CoreAudio's device of the suite's own: an aggregate of the default output alone, private to the process, made under
// a name the test chooses (DeviceRig.h, PrivateAggregate). In a translation unit of its own: CoreAudio's headers bring
// the Carbon types into the global namespace - MacTypes.h's Fixed among them - which the suite's own names would meet.
#include "DeviceRig.h"

#include <cstdint>
#include <string>

#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>

namespace reckless::audio::test
{
static_assert(sizeof(AudioObjectID) == sizeof(std::uint32_t), "the handle holds an AudioObjectID");
static_assert(kAudioObjectUnknown == 0, "none is 0");

/// @cond
PrivateAggregate::PrivateAggregate(const std::string &name)
{
    AudioObjectPropertyAddress address{kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal,
                                       kAudioObjectPropertyElementMain};
    AudioObjectID              output = kAudioObjectUnknown;
    UInt32                     size   = sizeof(output);
    if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &address, 0, nullptr, &size, &output) != noErr ||
        output == kAudioObjectUnknown)
    {
        why_ = "no default output to aggregate";
        return;
    }
    CFStringRef output_uid = nullptr;
    size                   = sizeof(output_uid);
    address.mSelector      = kAudioDevicePropertyDeviceUID;
    if (AudioObjectGetPropertyData(output, &address, 0, nullptr, &size, &output_uid) != noErr || output_uid == nullptr)
    {
        why_ = "the default output has no UID";
        return;
    }
    CFStringRef cf_name    = CFStringCreateWithBytes(nullptr, reinterpret_cast<const UInt8 *>(name.data()),
                                                     static_cast<CFIndex>(name.size()), kCFStringEncodingUTF8, false);
    const int   one        = 1;
    CFNumberRef is_private = CFNumberCreate(nullptr, kCFNumberIntType, &one);
    CFMutableDictionaryRef sub =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(sub, CFSTR(kAudioSubDeviceUIDKey), output_uid);
    const void            *subs[]   = {sub};
    CFArrayRef             sub_list = CFArrayCreate(nullptr, subs, 1, &kCFTypeArrayCallBacks);
    CFMutableDictionaryRef description =
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceNameKey), cf_name);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceUIDKey),
                         CFSTR("com.recklesstechnology.device-test.aggregate"));
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceIsPrivateKey), is_private);
    CFDictionarySetValue(description, CFSTR(kAudioAggregateDeviceSubDeviceListKey), sub_list);
    AudioObjectID  device = kAudioObjectUnknown;
    const OSStatus made   = AudioHardwareCreateAggregateDevice(description, &device);
    if (made == noErr)
    {
        device_ = device;
    }
    else
    {
        why_ = "AudioHardwareCreateAggregateDevice: " + std::to_string(made);
    }
    CFRelease(description);
    CFRelease(sub_list);
    CFRelease(sub);
    CFRelease(is_private);
    CFRelease(cf_name);
    CFRelease(output_uid);
}

PrivateAggregate::~PrivateAggregate()
{
    if (device_ != kAudioObjectUnknown)
    {
        AudioHardwareDestroyAggregateDevice(device_);
    }
}

/// @endcond
} // namespace reckless::audio::test
