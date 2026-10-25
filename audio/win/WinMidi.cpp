// MIDI input on WinMM (WinMidi.h says what it does and why): the one translation unit that meets Windows' MIDI API.
#include "WinMidi.h"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <mmsystem.h>

#include "WinMidiFormats.h"

namespace reckless::audio
{
namespace
{
/** UTF-16 as UTF-8.
    @param wide  the text, null-terminated
    @return      the UTF-8 text */
std::string Utf8(const wchar_t *wide)
{
    if (wide == nullptr || *wide == L'\0')
    {
        return {};
    }
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, nullptr, 0, nullptr, nullptr);
    if (bytes <= 1)
    {
        return {};
    }
    std::string text(static_cast<std::size_t>(bytes - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, -1, text.data(), bytes, nullptr, nullptr);
    return text;
}

/** WinMM's inputs now, each under the name RtMidi gave it (winmm::ListedName).
    @return  the names, in WinMM's order, an input's index its place */
std::vector<std::string> Listed()
{
    std::vector<std::string> names;
    const UINT               count = midiInGetNumDevs();
    for (UINT i = 0; i < count; ++i)
    {
        MIDIINCAPSW caps{};
        const bool  read = midiInGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR;
        names.push_back(winmm::ListedName(read ? Utf8(caps.szPname) : std::string(), static_cast<unsigned int>(i)));
    }
    return names;
}

/** WinMM's inputs as a MidiSystem: an input of WinMM's for each one opened, its messages handed by WinMM's callback to
    the sink. Message thread throughout but for Message, WinMM's callback. */
class WinMidiSystem final : public MidiSystem
{
  public:
    WinMidiSystem() = default;

    /** Every open input closed (CloseAll). */
    ~WinMidiSystem() override
    {
        CloseAll();
    }

    WinMidiSystem(const WinMidiSystem &)            = delete;
    WinMidiSystem &operator=(const WinMidiSystem &) = delete;

    std::vector<std::string> Inputs() override
    {
        return Listed();
    }

    bool Open(const std::string &name, int port, MidiSink &sink) override
    {
        Close(port);
        const std::vector<std::string> listed = Listed();
        const int                      index  = winmm::Find(listed, name);
        if (index < 0)
        {
            Keep("no MIDI input named '" + name + "'");
            return false;
        }
        auto input   = std::make_unique<Input>();
        input->index = port;
        input->sink  = &sink;
        MMRESULT result =
            midiInOpen(&input->handle, static_cast<UINT>(index), reinterpret_cast<DWORD_PTR>(&WinMidiSystem::Message),
                       reinterpret_cast<DWORD_PTR>(input.get()), CALLBACK_FUNCTION);
        if (result != MMSYSERR_NOERROR)
        {
            input->handle = nullptr;
            Keep("the MIDI input '" + listed[static_cast<std::size_t>(index)] +
                 "' would not open: " + Describe(result));
            return false;
        }
        result = midiInStart(input->handle);
        if (result != MMSYSERR_NOERROR)
        {
            Shut(*input);
            Keep("the MIDI input '" + listed[static_cast<std::size_t>(index)] +
                 "' would not start: " + Describe(result));
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
    /** One opened input: the index its messages carry, its sink, and WinMM's handle. Its address is what WinMM's
        callback is given, so it stays where it was made until it is shut. */
    struct Input
    {
        int       index{0};        ///< the index its messages carry to the sink
        MidiSink *sink{nullptr};   ///< where they land; not owned
        HMIDIIN   handle{nullptr}; ///< WinMM's input; null once shut
    };

    /** WinMM's words for a failure.
        @param result  what a WinMM call returned
        @return        the words, or the code where WinMM has none */
    static std::string Describe(MMRESULT result)
    {
        wchar_t text[MAXERRORLENGTH] = {};
        if (midiInGetErrorTextW(result, text, MAXERRORLENGTH) == MMSYSERR_NOERROR && text[0] != L'\0')
        {
            return Utf8(text);
        }
        return "WinMM error " + std::to_string(result);
    }

    /** An input stopped, reset and closed: once this returns, WinMM calls its callback no more.
        @param input  the input */
    static void Shut(Input &input) noexcept
    {
        if (input.handle != nullptr)
        {
            midiInStop(input.handle);
            midiInReset(input.handle);
            midiInClose(input.handle);
            input.handle = nullptr;
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

    /** WinMM's callback, on a thread of its own: a short message handed to the input's sink as its bytes, stamped as it
        arrives - but what the cores do not read (winmm::ShortMessage). It calls no WinMM function, which a callback
        may not.
        @param message   what WinMM reports: MIM_DATA for a short message, the rest unread
        @param instance  the Input, as midiInOpen was given it
        @param packed    for MIM_DATA, the message packed in one word
        @return          nothing: a WinMM callback returns no value (CALLBACK is its calling convention) */
    static void CALLBACK Message(HMIDIIN, UINT message, DWORD_PTR instance, DWORD_PTR packed, DWORD_PTR) noexcept
    {
        if (message != MIM_DATA || instance == 0)
        {
            return;
        }
        const auto  *input = reinterpret_cast<const Input *>(instance);
        std::uint8_t bytes[3];
        const int    size = winmm::ShortMessage(static_cast<std::uint32_t>(packed), bytes);
        if (size > 0)
        {
            input->sink->Arrived(input->index, NowNanoseconds(), bytes, size);
        }
    }

    /** `text` kept for LastError.
        @param text  the error's text */
    void Keep(const std::string &text)
    {
        const std::lock_guard lock(error_mutex_);
        error_ = text;
    }

    std::vector<std::unique_ptr<Input>> inputs_;      ///< the open inputs
    mutable std::mutex                  error_mutex_; ///< guards `error_`
    std::string                         error_;       ///< the last error, for a person to read
};

/** The machine's MIDI inputs on WinMM. WinMM's inputs carry no name of the program's, so the client's name is unused.
    @return  the system, owned by the caller */
std::unique_ptr<MidiSystem> MakeMidi(const std::string &)
{
    return std::make_unique<WinMidiSystem>();
}

/** What this back end is.
    @return  "Windows MIDI (WinMM)" */
std::string Library()
{
    return "Windows MIDI (WinMM)";
}
} // namespace

const MidiBackend &WinMidiBackend()
{
    static const MidiBackend backend{&Library, &MakeMidi};
    return backend;
}
} // namespace reckless::audio
