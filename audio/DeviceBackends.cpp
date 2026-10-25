// The back ends this build carries, assembled by hand: DeviceBackends.h says why by hand and not by self-registration.
//
// While the native back ends are written, RtAudio still serves whatever they do not yet: it is one back end among
// them, offering the API names the native ones have not taken. A native back end lands ahead of it in the list and
// takes its name - two back ends may offer the same name, and the first in the list wins - so each API moves over on
// its own, with the device suite run against both to compare them. RtAudio's descriptor goes when the last name it
// serves has moved, and with it the three patches under cmake/.

#include "DeviceBackends.h"

#include <algorithm>
#include <string>
#include <vector>

#include "RtAudioDevice.h"

#if defined(_WIN32)
#include "win/WasapiDevice.h"
#endif

namespace reckless::audio
{
namespace
{
/** Every audio back end this build carries, most preferred first; the order AudioApis() returns and the order a name
    offered by two of them is resolved in. Built on first use, so no translation unit's statics have to be ready
    before another's.
    @return  the back ends, in preference order */
const std::vector<const AudioBackend *> &AudioBackends()
{
    static const std::vector<const AudioBackend *> backends = {
#if defined(_WIN32)
        // The suite's own WASAPI, in its three modes, ahead of ASIO: only a person with an ASIO driver wants ASIO.
        &WasapiBackend(),
#endif
        &RtAudioBackend(),
    };
    return backends;
}

/** The platform's MIDI back end.
    @return  the back end */
const MidiBackend &TheMidiBackend()
{
    return RtMidiBackend();
}
} // namespace

std::vector<std::string> AudioApis()
{
    std::vector<std::string> names;
    for (const AudioBackend *backend : AudioBackends())
    {
        for (std::string &name : backend->Apis())
        {
            // A name two back ends offer belongs to the first of them, so one already here is not added again.
            if (std::find(names.begin(), names.end(), name) == names.end())
            {
                names.push_back(std::move(name));
            }
        }
    }
    return names;
}

std::string DefaultAudioApi()
{
    const std::vector<std::string> apis = AudioApis();
    for (const std::string &name : apis)
    {
        // A name AudioApis() offered whose back end will not make a system is passed over rather than dereferenced:
        // Apis() answers for the machine as it stood a moment ago, and a device can go between the two calls.
        const std::unique_ptr<AudioSystem> system = MakeAudioSystem(name);
        if (!system)
        {
            continue;
        }
        for (const DeviceInfo &device : system->Devices())
        {
            if (device.output_channels > 0)
            {
                return name;
            }
        }
    }
    return apis.empty() ? std::string() : apis.front();
}

std::unique_ptr<AudioSystem> MakeAudioSystem(const std::string &name)
{
    for (const AudioBackend *backend : AudioBackends())
    {
        if (std::unique_ptr<AudioSystem> system = backend->Make(name))
        {
            return system;
        }
    }
    return nullptr;
}

std::unique_ptr<MidiSystem> MakeMidiSystem(const std::string &client_name)
{
    return TheMidiBackend().Make(client_name);
}

std::string AudioLibraryVersions()
{
    std::string text;
    for (const AudioBackend *backend : AudioBackends())
    {
        if (!text.empty())
        {
            text += ", ";
        }
        text += backend->Library();
    }
    if (!text.empty())
    {
        text += ", ";
    }
    text += TheMidiBackend().Library();
    return text;
}
} // namespace reckless::audio
