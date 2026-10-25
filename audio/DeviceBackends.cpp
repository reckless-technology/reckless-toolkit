// The back ends this build carries, assembled by hand: DeviceBackends.h says why by hand and not by self-registration.
//
// Every back end is the toolkit's own, on each platform's API; RtAudio and RtMidi, which the layer was first written
// on, served each API until its own back end landed ahead of them and took its name (docs/audio/native-backends.md).
// Two back ends may still offer one name, the first in the list serving it, which is how the next one would move.
//
// So the list's order says which back end serves a name, and the order a Standalone offers the names in is the
// series' own (kPreference), apart from it.

#include "DeviceBackends.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(RECKLESS_AUDIO_COREAUDIO)
#include "mac/CoreAudioDevice.h"
#endif
#if defined(RECKLESS_AUDIO_COREMIDI)
#include "mac/CoreMidi.h"
#endif

#if defined(_WIN32)
#include "win/WasapiDevice.h"
#endif
#if defined(RECKLESS_AUDIO_ASIO)
#include "win/AsioDevice.h"
#endif
#if defined(RECKLESS_AUDIO_WINMM)
#include "win/WinMidi.h"
#endif
#if defined(RECKLESS_AUDIO_JACK)
#include "linux/JackDevice.h"
#endif
#if defined(RECKLESS_AUDIO_ALSA)
#include "linux/AlsaDevice.h"
#include "linux/AlsaMidi.h"
#endif

namespace reckless::audio
{
namespace
{
/** The series' APIs in the order a Standalone offers them (DeviceBackends.h's AudioApis says why); a name not here
    comes after them, in the order the back ends offer it. */
constexpr std::string_view kPreference[] = {"CoreAudio", "Windows Audio", "Windows Audio (Exclusive Mode)",
                                            "ASIO",      "JACK",          "ALSA"};

/** Where a name comes in kPreference.
    @param name  an API's name
    @return      its index, or the count past the last for a name not there */
std::size_t RankOf(const std::string &name)
{
    const auto found = std::find(std::begin(kPreference), std::end(kPreference), name);
    return static_cast<std::size_t>(found - std::begin(kPreference));
}

/** Every audio back end this build carries, the order a name offered by two of them is resolved in: the first serves
    it. Built on first use, so no translation unit's statics have to be ready
    before another's.
    @return  the back ends, in preference order */
const std::vector<const AudioBackend *> &AudioBackends()
{
    static const std::vector<const AudioBackend *> backends = {
#if defined(_WIN32)
        // The suite's own WASAPI, in its two modes, ahead of ASIO: only a person with an ASIO driver wants ASIO.
        &WasapiBackend(),
#endif
#if defined(RECKLESS_AUDIO_ASIO)
        &AsioBackend(),
#endif
#if defined(RECKLESS_AUDIO_JACK)
        &JackBackend(),
#endif
#if defined(RECKLESS_AUDIO_ALSA)
        &AlsaBackend(),
#endif
#if defined(RECKLESS_AUDIO_COREAUDIO)
        &CoreAudioBackend(),
#endif
    };
    return backends;
}

#if !defined(RECKLESS_AUDIO_ALSA) && !defined(RECKLESS_AUDIO_WINMM) && !defined(RECKLESS_AUDIO_COREMIDI)
/** A build with no MIDI back end - Linux with no ALSA headers - has no MIDI to offer: no inputs, and an open that says
    why.
    @return  "no MIDI (built without ALSA)" */
std::string NoMidiLibrary()
{
    return "no MIDI (built without ALSA)";
}

/** No MIDI inputs at all. */
class NoMidiSystem final : public MidiSystem
{
  public:
    std::vector<std::string> Inputs() override
    {
        return {};
    }

    bool Open(const std::string &name, int, MidiSink &) override
    {
        error_ = "no MIDI input named '" + name + "': this build has no MIDI back end (it was built without ALSA)";
        return false;
    }

    void CloseAll() override
    {
    }

    std::string LastError() const override
    {
        return error_;
    }

  private:
    std::string error_; ///< why the last open failed
};

/** A system with no inputs.
    @return  the system, owned by the caller */
std::unique_ptr<MidiSystem> MakeNoMidi(const std::string &)
{
    return std::make_unique<NoMidiSystem>();
}
#endif

/** The platform's MIDI back end: ALSA's sequencer on Linux, WinMM on Windows, CoreMIDI on macOS.
    @return  the back end */
const MidiBackend &TheMidiBackend()
{
#if defined(RECKLESS_AUDIO_ALSA)
    return AlsaMidiBackend();
#elif defined(RECKLESS_AUDIO_WINMM)
    return WinMidiBackend();
#elif defined(RECKLESS_AUDIO_COREMIDI)
    return CoreMidiBackend();
#else
    static const MidiBackend none{&NoMidiLibrary, &MakeNoMidi};
    return none;
#endif
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
    std::stable_sort(names.begin(), names.end(),
                     [](const std::string &a, const std::string &b) { return RankOf(a) < RankOf(b); });
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
