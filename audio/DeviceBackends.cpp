// The back ends this build carries, assembled by hand: DeviceBackends.h says why by hand and not by self-registration.
//
// While the native back ends are written, RtAudio still serves whatever they do not yet: it is one back end among
// them, offering the API names the native ones have not taken. A native back end lands ahead of it in the list and
// takes its name - two back ends may offer the same name, and the first in the list wins - so each API moves over on
// its own, with the device suite run against both to compare them. On Linux and Windows every name has moved - ALSA
// and JACK, Windows Audio and ASIO are the toolkit's own - so RtAudio is not listed there. Its descriptor goes when
// CoreAudio moves too.
//
// So the list's order says which back end serves a name, and the order a Standalone offers the names in is the
// series' own (kPreference), apart from it: a native back end listed ahead of RtAudio must not put its name ahead of
// RtAudio's others.

#include "DeviceBackends.h"

#include <algorithm>
#include <cstddef>
#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(__APPLE__)
#include "RtAudioDevice.h"
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
#if defined(__APPLE__)
        // RtAudio, for what is not yet the toolkit's own: CoreAudio. On Linux and Windows it serves nothing.
        &RtAudioBackend(),
#endif
    };
    return backends;
}

#if !defined(__APPLE__) && !defined(RECKLESS_AUDIO_ALSA) && !defined(RECKLESS_AUDIO_WINMM)
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

/** The platform's MIDI back end: ALSA's sequencer on Linux, WinMM on Windows, RtMidi on macOS until CoreMIDI is the
    toolkit's own.
    @return  the back end */
const MidiBackend &TheMidiBackend()
{
#if defined(RECKLESS_AUDIO_ALSA)
    return AlsaMidiBackend();
#elif defined(RECKLESS_AUDIO_WINMM)
    return WinMidiBackend();
#elif !defined(__APPLE__)
    static const MidiBackend none{&NoMidiLibrary, &MakeNoMidi};
    return none;
#else
    return RtMidiBackend();
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
