// MIDI input and output on WinMM (WinMidi.h says what it does and why): the one translation unit that meets Windows'
// MIDI API.
#include "WinMidi.h"

#include <algorithm>
#include <atomic>
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

#include "MidiOutputPorts.h"
#include "MidiOutputSender.h"
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

/** WinMM's outputs now, each under the name RtMidi gave it (winmm::ListedName), as the inputs are listed.
    @return  the names, in WinMM's order, an output's index its place */
std::vector<std::string> ListedOutputs()
{
    std::vector<std::string> names;
    const UINT               count = midiOutGetNumDevs();
    for (UINT i = 0; i < count; ++i)
    {
        MIDIOUTCAPSW caps{};
        const bool   read = midiOutGetDevCapsW(i, &caps, sizeof caps) == MMSYSERR_NOERROR;
        names.push_back(winmm::ListedName(read ? Utf8(caps.szPname) : std::string(), static_cast<unsigned int>(i)));
    }
    return names;
}

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002 // Windows 10, version 1803; an older SDK's headers lack it
#endif

/** One output open on WinMM: its handle, and the port number Send names it by. WinMM's short messages carry no time,
    so its sender hands each one on when it is due (a lead of 0), and Deliver sends it then. */
class WinMidiOutput final : public MidiOutputStream
{
  public:
    int      index{0};        ///< the port number Send names it by
    HMIDIOUT handle{nullptr}; ///< WinMM's output; null once shut

  protected:
    bool Deliver(const MidiOutputMessage &message) noexcept override
    {
        // An error is a device gone, whose messages go nowhere.
        midiOutShortMsg(handle, static_cast<DWORD>(winmm::PackShortMessage(message.bytes.data(), message.size)));
        return true;
    }
};

/** WinMM's sender: MidiOutputSender's thread, asleep on a high-resolution waitable timer until each message is due, or
    on an ordinary one where Windows has none (before Windows 10, version 1803) - the system then holds the
    multimedia timer's period at a millisecond while an output is open (timeBeginPeriod), so the ordinary one wakes
    to the millisecond. Stop's word wakes it through an event. */
class WinMidiSender final : public MidiOutputSender
{
  public:
    /** A sender not yet started, its timer and its event made: a high-resolution timer where Windows has one. */
    WinMidiSender()
        : MidiOutputSender(0),
          timer_(CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS)),
          wake_(CreateEventW(nullptr, FALSE, FALSE, nullptr))
    {
        is_high_resolution_ = timer_ != nullptr;
        if (timer_ == nullptr)
        {
            timer_ = CreateWaitableTimerW(nullptr, FALSE, nullptr);
        }
    }

    /** Stopped, and its handles closed. */
    ~WinMidiSender() override
    {
        Stop();
        for (HANDLE handle : {timer_, wake_})
        {
            if (handle != nullptr)
            {
                CloseHandle(handle);
            }
        }
    }

    WinMidiSender(const WinMidiSender &)            = delete;
    WinMidiSender &operator=(const WinMidiSender &) = delete;

    /** Whether it sleeps on a high-resolution timer.
        @return  false where Windows has none, and the multimedia timer's period is to be raised */
    bool is_high_resolution() const noexcept
    {
        return is_high_resolution_;
    }

  protected:
    void WaitUntil(std::int64_t deadline) override
    {
        if (timer_ == nullptr || wake_ == nullptr)
        {
            MidiOutputSender::WaitUntil(deadline);
            return;
        }
        const std::int64_t ahead = deadline - NowNanoseconds();
        if (ahead <= 0 || stopping())
        {
            return;
        }
        LARGE_INTEGER due;
        due.QuadPart = -std::max<std::int64_t>(ahead / 100, 1); // relative, in 100-nanosecond units
        if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) == 0)
        {
            MidiOutputSender::WaitUntil(deadline);
            return;
        }
        const HANDLE handles[2] = {timer_, wake_};
        WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    }

    void Wake() override
    {
        if (wake_ != nullptr)
        {
            SetEvent(wake_);
        }
        MidiOutputSender::Wake();
    }

  private:
    HANDLE timer_{nullptr};            ///< what the thread sleeps on
    HANDLE wake_{nullptr};             ///< what Stop wakes it through; auto-reset
    bool   is_high_resolution_{false}; ///< whether `timer_` is a high-resolution timer
};

/** WinMM's inputs and outputs as a MidiSystem: an input of WinMM's for each one opened, its messages handed by WinMM's
    callback to the sink, and an output of WinMM's for each one opened, fed by the sender. Message thread throughout
    but for Message, WinMM's callback, the sender's Deliver, and Send. */
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

    std::vector<std::string> Outputs() override
    {
        return ListedOutputs();
    }

    bool OpenOutput(const std::string &name, int port) override
    {
        if (!MidiOutputPorts::IsPortNumber(port))
        {
            Keep("the MIDI output '" + name + "' cannot be port " + std::to_string(port) + ": ports are 0 to " +
                 std::to_string(kMidiOutputPorts - 1));
            return false;
        }
        const std::vector<std::string> listed = ListedOutputs();
        const int                      index  = winmm::Find(listed, name);
        if (index < 0)
        {
            Keep("no MIDI output named '" + name + "'");
            return false;
        }
        ports_.Withdraw(port);
        sender_.Stop();
        CloseOutput(port);
        auto output           = std::make_unique<WinMidiOutput>();
        output->index         = port;
        const MMRESULT result = midiOutOpen(&output->handle, static_cast<UINT>(index), 0, 0, CALLBACK_NULL);
        if (result != MMSYSERR_NOERROR)
        {
            output->handle = nullptr;
            StartSender();
            Keep("the MIDI output '" + listed[static_cast<std::size_t>(index)] +
                 "' would not open: " + DescribeOutput(result));
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
        for (const std::unique_ptr<WinMidiOutput> &output : outputs_)
        {
            ports_.Withdraw(output->index);
        }
        sender_.Stop();
        for (const std::unique_ptr<WinMidiOutput> &output : outputs_)
        {
            ShutOutput(*output);
        }
        outputs_.clear();
        HoldTimerPeriod();
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

    /** WinMM's words for an output's failure.
        @param result  what a WinMM output call returned
        @return        the words, or the code where WinMM has none */
    static std::string DescribeOutput(MMRESULT result)
    {
        wchar_t text[MAXERRORLENGTH] = {};
        if (midiOutGetErrorTextW(result, text, MAXERRORLENGTH) == MMSYSERR_NOERROR && text[0] != L'\0')
        {
            return Utf8(text);
        }
        return "WinMM error " + std::to_string(result);
    }

    /** An output closed, the sender stopped already: WinMM's handle closed - no note off sent for it, as nothing is
       sent that was not asked for - and what it still held dropped.
        @param output  the output */
    static void ShutOutput(WinMidiOutput &output) noexcept
    {
        if (output.handle != nullptr)
        {
            midiOutClose(output.handle);
            output.handle = nullptr;
        }
        output.Drop();
    }

    /** The output open as `port` closed, if there is one; withdrawn from Send and the sender stopped already.
        @param port  the port */
    void CloseOutput(int port)
    {
        const auto at =
            std::find_if(outputs_.begin(), outputs_.end(),
                         [port](const std::unique_ptr<WinMidiOutput> &output) { return output->index == port; });
        if (at != outputs_.end())
        {
            ShutOutput(**at);
            outputs_.erase(at);
        }
    }

    /** The sender started on every open output, where there is one, and the timer's period held to match. */
    void StartSender()
    {
        std::vector<MidiOutputStream *> streams;
        for (const std::unique_ptr<WinMidiOutput> &output : outputs_)
        {
            streams.push_back(output.get());
        }
        sender_.Start(std::move(streams));
        HoldTimerPeriod();
    }

    /** The multimedia timer's period held at a millisecond while an output is open and the sender has no
        high-resolution timer, and given back when none is. */
    void HoldTimerPeriod() noexcept
    {
        const bool wants = !outputs_.empty() && !sender_.is_high_resolution();
        if (wants && !is_period_raised_)
        {
            is_period_raised_ = timeBeginPeriod(1) == TIMERR_NOERROR;
        }
        else if (!wants && is_period_raised_)
        {
            timeEndPeriod(1);
            is_period_raised_ = false;
        }
    }

    /** `text` kept for LastError.
        @param text  the error's text */
    void Keep(const std::string &text)
    {
        const std::lock_guard lock(error_mutex_);
        error_ = text;
    }

    std::vector<std::unique_ptr<Input>>         inputs_;      ///< the open inputs
    mutable std::mutex                          error_mutex_; ///< guards `error_`
    std::string                                 error_;       ///< the last error, for a person to read
    std::vector<std::unique_ptr<WinMidiOutput>> outputs_; ///< the open outputs; changed only with the sender stopped
    MidiOutputPorts                             ports_;   ///< the open outputs' queues, as Send finds them
    WinMidiSender                               sender_;  ///< sends each output's messages when they are due
    bool is_period_raised_{false}; ///< whether this system holds the multimedia timer's period raised
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
