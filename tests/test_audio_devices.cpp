// The audio layer on the real APIs (audio/DeviceBackends.h, whichever back end serves each API): for every API
// the build carries - CoreAudio, Windows Audio (WASAPI, in its shared, low-latency and exclusive modes), ASIO, JACK,
// ALSA - its device list, a stream run at a rate the
// device offers with its callbacks counted against what the rate and the block predict and the rate read by the clock,
// the samples a loop carries back (bit for bit through ALSA's loopback card and a JACK client wired to itself,
// correlated through WASAPI's loopback capture, the ASIO test driver's own sine and sawtooth exactly), an xrun the
// device reports, and a device that is not there; and on Linux, MIDI from a sequencer port of the test's own through
// ALSA's sequencer back end, and MIDI out to one, each message read back against the time it was sent for; on macOS,
// the same both ways through a CoreMIDI source and destination of the test's own. Each API's
// test device is deterministic and set up by the suite or the CI job (devices/DeviceRig.h says which); the tables each
// test judges are printed. The harness is devices/DeviceSuite.h, which a consumer's suite of an engine playing through
// the same devices shares.
//
// Covered means it cannot skip silently. A missing test device skips its tests with the reason printed - unless the API
// is named in RECKLESS_AUDIO_REQUIRE (a comma-separated list of the series' API names, which each CI job sets for its
// runner's APIs), when it fails, naming what is missing. And the build holds itself to its configure: AudioApis() must
// be exactly the APIs the configure promised (RECKLESS_AUDIO_PROMISED_APIS - ASIO wherever the SDK is, JACK and ALSA
// wherever their headers are), so a build that silently loses one fails here.
#include "devices/DeviceSuite.h"

#include "ChannelNames.h"
#include "MidiOutputSender.h"
#include "RealtimeThreadScope.h"

#if defined(RECKLESS_AUDIO_TEST_ALSA_SEQ)
#include <limits>

#include <alsa/asoundlib.h>
#include <poll.h>
#endif

using reckless::audio::RealtimeThreadMembership;
using reckless::audio::RealtimeThreadResult;
using reckless::audio::RealtimeThreadScope;
using namespace reckless::audio::test; // the rigs' own: Environment, Trim, the test device
using namespace reckless::audio::test::suite;

// ---- The build's APIs ----------------------------------------------------------------------------------------------

TEST(AudioApis, AreTheOnesTheConfigurePromised)
{
    const std::vector<std::string> apis = reckless::audio::AudioApis();
    Table("the build's audio APIs")
        .Row("the build carries", JoinList(apis))
        .Row("the configure promised", JoinList(PromisedApis()))
        .Row("the run requires", JoinList(RequiredApis()))
        .Row("libraries", reckless::audio::AudioLibraryVersions())
        .Print();

    // A build that loses an API its configure found the means for - the ASIO SDK, JACK's or ALSA's headers - fails.
    EXPECT_EQ(apis, PromisedApis());

    // Every API the run requires is one of the series', and one this build carries.
    const std::vector<std::string> series = {"CoreAudio", "Windows Audio", "Windows Audio (Exclusive Mode)",
                                             "ASIO",      "JACK",          "ALSA"};
    for (const std::string &api : RequiredApis())
    {
        EXPECT_NE(std::find(series.begin(), series.end(), api), series.end())
            << "RECKLESS_AUDIO_REQUIRE names '" << api << "', which is none of the series' APIs: " << JoinList(series);
        EXPECT_NE(std::find(apis.begin(), apis.end(), api), apis.end())
            << api << " is required (RECKLESS_AUDIO_REQUIRE) and this build does not carry it";
    }

    // Each is made by its name, under that name, and nothing else is.
    for (const std::string &api : apis)
    {
        const auto system = reckless::audio::MakeAudioSystem(api);
        ASSERT_NE(system, nullptr) << api;
        EXPECT_EQ(system->Name(), api);
    }
    EXPECT_EQ(reckless::audio::MakeAudioSystem("No Such API"), nullptr);
}

// ---- Each API: probing ---------------------------------------------------------------------------------------------

TEST_P(AudioDevices, ProbesTheDevicesEachUnderANameOfItsOwn)
{
    const std::vector<DeviceInfo> devices = system_->Devices();
    Table table(api_ + ": the devices (in / out / duplex, defaults, rates, current, preferred)");
    for (const DeviceInfo &d : devices)
    {
        table.Row(d.name, Str(d.input_channels) + " / " + Str(d.output_channels) + " / " + Str(d.duplex_channels) +
                              (d.is_default_input ? ", default in" : "") +
                              (d.is_default_output ? ", default out" : "") + ", rates " + Rates(d.sample_rates) +
                              ", current " + Str(d.current_sample_rate) + ", preferred " +
                              Str(d.preferred_sample_rate));
        table.Row("  channel names",
                  "in [" + JoinList(d.input_channel_names) + "] out [" + JoinList(d.output_channel_names) + "]");
    }
    if (devices.empty())
    {
        table.Row("(none)", "");
    }
    table.Row("the API's last message", system_->LastError().empty() ? std::string("none") : system_->LastError());
    table.Print();

    // What every device of every API keeps: a name no other device with channels the same way has - a name and a way
    // are what a settings file finds it by, and an input and an output may share one - channels one way or the
    // other, and rates that are rates.
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        EXPECT_FALSE(devices[i].name.empty());
        EXPECT_GT(devices[i].input_channels + devices[i].output_channels, 0) << devices[i].name;
        for (std::size_t j = 0; j < i; ++j)
        {
            const bool same_way = (devices[i].input_channels > 0 && devices[j].input_channels > 0) ||
                                  (devices[i].output_channels > 0 && devices[j].output_channels > 0);
            EXPECT_FALSE(same_way && devices[i].name == devices[j].name)
                << "two devices named '" << devices[i].name << "' with channels the same way on " << api_;
        }
        for (const int rate : devices[i].sample_rates)
        {
            EXPECT_GT(rate, 0) << devices[i].name;
        }
    }

    // Every name in UTF-8, the encoding a settings file keeps it in and a window draws.
    for (const DeviceInfo &d : devices)
    {
        EXPECT_TRUE(reckless::gfx::IsValidUtf8(d.name)) << "a name that is not UTF-8 on " << api_ << ": " << d.name;
    }

    // Channel names are the API's own or none: a list is empty or one name a channel, each UTF-8 and with no control
    // character, and never a name the layer made up. Nothing says what a back end must name - ALSA's are the driver's
    // speaker positions, WASAPI has none - so only the shape is held, and the table above is where to read them.
    for (const DeviceInfo &d : devices)
    {
        EXPECT_TRUE(d.input_channel_names.empty() ||
                    d.input_channel_names.size() == static_cast<std::size_t>(d.input_channels))
            << d.name << " on " << api_;
        EXPECT_TRUE(d.output_channel_names.empty() ||
                    d.output_channel_names.size() == static_cast<std::size_t>(d.output_channels))
            << d.name << " on " << api_;
        for (const auto *names : {&d.input_channel_names, &d.output_channel_names})
        {
            for (const std::string &name : *names)
            {
                EXPECT_TRUE(reckless::gfx::IsValidUtf8(name)) << d.name << " on " << api_;
                EXPECT_EQ(name, reckless::audio::CleanChannelName(name)) << d.name << " on " << api_;
            }
        }
    }

    // A second probe lists the same devices.
    const std::vector<DeviceInfo> again = system_->Devices();
    ASSERT_EQ(again.size(), devices.size());
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
        EXPECT_EQ(again[i].input_channel_names, devices[i].input_channel_names) << devices[i].name;
        EXPECT_EQ(again[i].output_channel_names, devices[i].output_channel_names) << devices[i].name;
        EXPECT_EQ(again[i].name, devices[i].name);
        EXPECT_EQ(again[i].input_channels, devices[i].input_channels) << devices[i].name;
        EXPECT_EQ(again[i].output_channels, devices[i].output_channels) << devices[i].name;
    }

    // And the test device is among them, with the channels and the rate its streams open.
    const TestDevice device = WithRunOverrides(rig_->Find(devices));
    RECKLESS_NEED_TEST_DEVICE(device);
    Table(api_ + ": the test device")
        .Row("what it is", device.about)
        .Row("output", device.output)
        .Row("input", device.input.empty() ? std::string("none") : device.input)
        .Row("channels each way",
             Str(device.channels) + " (from output " + Str(device.first_output + 1) +
                 (device.input.empty() ? std::string() : ", input " + Str(device.first_input + 1)) + ")")
        .Row("rate, block", Str(device.sample_rate) + " Hz, " + Str(device.block_size))
        .Print();
    const int out = reckless::audio::FindDevice(devices, device.output, reckless::audio::Direction::kOutput);
    ASSERT_GE(out, 0);
    EXPECT_GE(devices[static_cast<std::size_t>(out)].output_channels, device.first_output + device.channels);
    EXPECT_TRUE(devices[static_cast<std::size_t>(out)].Lists(device.sample_rate));
    if (!device.input.empty() && device.loop != Loop::kCorrelated)
    {
        const int in = reckless::audio::FindDevice(devices, device.input, reckless::audio::Direction::kInput);
        ASSERT_GE(in, 0);
        EXPECT_GE(devices[static_cast<std::size_t>(in)].input_channels, device.channels);
        EXPECT_TRUE(devices[static_cast<std::size_t>(in)].Lists(device.sample_rate));
    }
}

TEST_P(AudioDevices, ListsADeviceNamedOutsideAsciiInUtf8)
{
#if defined(__APPLE__)
    if (api_ != "CoreAudio")
    {
        GTEST_SKIP() << "a device named outside ASCII is made on CoreAudio alone, as a private aggregate";
    }
    // A device of the test's own named as macOS names a person's earphones - an apostrophe past ASCII - with a letter
    // past ASCII too. The back end reads CoreAudio's names in UTF-8 - RtAudio read them in the system's encoding unless
    // UNICODE was defined: Mac OS Roman's bytes, which a setting of the device kept in UTF-8 never matches, and which
    // the Standalone's settings would draw. A device is listed with its maker before its name, as RtAudio listed it,
    // so it is found by the name it ends in.
    const TestDevice output = Find();
    RECKLESS_NEED_TEST_DEVICE(output);
    const std::string                             name = "Reckless Test\xE2\x80\x99s Ger\xC3\xA4t";
    const reckless::audio::test::PrivateAggregate aggregate(name);
    ASSERT_TRUE(aggregate.made()) << aggregate.why();
    std::vector<DeviceInfo> devices;
    int                     found = -1;
    for (int i = 0; i < 100 && found < 0; ++i) // a device made is listed at once, but give the system its moment
    {
        devices = system_->Devices();
        for (std::size_t d = 0; d < devices.size() && found < 0; ++d)
        {
            const std::string &listed = devices[d].name;
            if (listed.size() >= name.size() && listed.compare(listed.size() - name.size(), name.size(), name) == 0 &&
                devices[d].output_channels > 0)
            {
                found = static_cast<int>(d);
            }
        }
        if (found < 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    Table table(api_ + ": a device named outside ASCII");
    for (const DeviceInfo &d : devices)
    {
        if (d.name.find("Reckless Test") != std::string::npos)
        {
            table.Row("listed", d.name + (reckless::gfx::IsValidUtf8(d.name) ? " (UTF-8)" : " (not UTF-8)"));
        }
    }
    table.Row("asked for", name).Print();
    ASSERT_GE(found, 0) << "the device by its name, in UTF-8";
    EXPECT_GT(devices[static_cast<std::size_t>(found)].output_channels, 0);
    for (const DeviceInfo &d : devices)
    {
        EXPECT_TRUE(reckless::gfx::IsValidUtf8(d.name)) << d.name;
    }
#else
    GTEST_SKIP() << "a device named outside ASCII is made on CoreAudio alone, as a private aggregate";
#endif
}

// ---- Each API: a stream --------------------------------------------------------------------------------------------

TEST_P(AudioDevices, RunsAStreamAtTheRateAndTheBlockItOpenedAt)
{
    const TestDevice device = Find();
    RECKLESS_NEED_TEST_DEVICE(device);
    const bool          with_input = !device.input.empty();
    const StreamRequest request    = RequestFor(device, with_input);
    Recorder            recorder(with_input ? device.channels : 0, device.channels, Recorder::Play::kSilence);
    {
        const Phase opening("system_->Open");
        ASSERT_TRUE(system_->Open(request, recorder)) << system_->LastError();
    }
    ASSERT_TRUE(system_->IsOpen());
    const StreamFormat format = system_->Format();

    const std::int64_t started = NowNanoseconds();
    {
        const Phase starting("system_->Start");
        ASSERT_TRUE(system_->Start()) << system_->LastError();
    }
    RunFor(1.2);
    const std::int64_t stopped = NowNanoseconds();
    {
        const Phase stopping("system_->Stop");
        system_->Stop();
    }
    const std::int64_t after_stop = recorder.callbacks();
    RunFor(0.1);
    const std::int64_t later = recorder.callbacks();
    {
        const Phase closing("system_->Close");
        system_->Close();
    }

    const Timing       timing    = Measure(recorder, started, stopped, format.sample_rate, format.block_size);
    const double       slack     = device.rate_tolerance * timing.predicted + 2.0;
    const std::int64_t allowance = XrunAllowance(timing.callbacks) + LoadAllowance(device, timing.callbacks);
    // Each input channel's level, printed and not judged - what a stream records depends on what is plugged in - so a
    // person can see the input is the device asked for: a measurement microphone's room, not an idle input's hiss.
    std::string levels = "none";
    if (with_input && recorder.recorded_frames() > 0)
    {
        levels.clear();
        for (int c = 0; c < device.channels; ++c)
        {
            double sum = 0.0;
            for (std::size_t i = 0; i < recorder.recorded_frames(); ++i)
            {
                const double sample = recorder.recorded(c)[i];
                sum += sample * sample;
            }
            const double mean = sum / static_cast<double>(recorder.recorded_frames());
            levels += (c > 0 ? " / " : "") + (mean > 0.0 ? Fixed(10.0 * std::log10(mean), 1) : std::string("-inf"));
        }
        levels += " dBFS RMS";
    }
    Table(api_ + ": a stream on " + device.output)
        .Row("rate asked / opened", Str(request.sample_rate) + " / " + Str(format.sample_rate) + " Hz")
        .Row("block asked / opened", Str(request.block_size) + " / " + Str(format.block_size))
        .Row("channels in / out", Str(format.input_channels) + " / " + Str(format.output_channels))
        .Row("input level, each channel", levels)
        .Row("latency the API reports", Str(format.reported_latency) + " samples")
        .Row("first callback after", Fixed(timing.to_first_ms, 1) + " ms")
        .Row("callbacks counted / predicted",
             Str(timing.callbacks) + " / " + Fixed(timing.predicted, 1) + " (within " + Fixed(slack, 1) + ")")
        .Row("frames a callback, fewest / most", Str(timing.fewest) + " / " + Str(timing.most))
        .Row("rate by the clock", Fixed(timing.rate_by_clock, 1) + " Hz over " + Fixed(timing.clock_seconds, 2) +
                                      " s (within " + Fixed(100.0 * device.rate_tolerance, 1) + " %)")
        .Row("longest wait between callbacks", Fixed(timing.worst_gap_blocks, 2) + " blocks")
        .Row("xruns", Str(recorder.xruns()) + " (at most " + Str(allowance) + ")")
        .Row("callback layout faults", Str(recorder.layout_faults()) + " " + recorder.first_layout_fault())
        .Row("callbacks after the Stop returned", Str(later - after_stop))
        .Print();

    EXPECT_EQ(format.sample_rate, request.sample_rate);
    EXPECT_GT(format.block_size, 0);
    EXPECT_EQ(format.input_channels, request.input_count);
    EXPECT_EQ(format.output_channels, request.output_count);
    EXPECT_GE(format.reported_latency, 0);
    // The two sides are the sum, on every API; an input the stream does not open reports none.
    EXPECT_GE(format.input_latency_frames, 0);
    EXPECT_GE(format.output_latency_frames, 0);
    EXPECT_EQ(format.input_latency_frames + format.output_latency_frames, format.reported_latency);
    if (format.input_channels == 0)
    {
        EXPECT_EQ(format.input_latency_frames, 0);
    }

    // The stream ran, for as long as the rate and the block say it should have, at the rate it opened at.
    ASSERT_GT(timing.callbacks, 0) << "the stream never called back";
    EXPECT_NEAR(static_cast<double>(timing.callbacks), timing.predicted, slack);
    EXPECT_NEAR(timing.rate_by_clock, format.sample_rate, device.rate_tolerance * format.sample_rate);
    EXPECT_EQ(timing.fewest, format.block_size);
    EXPECT_EQ(timing.most, format.block_size);

    // Every block carried the channels it opened, laid out as every back end promises, on the API's own thread.
    EXPECT_EQ(recorder.layout_faults(), 0) << recorder.first_layout_fault();
    EXPECT_EQ(recorder.on_test_thread(), 0);
    EXPECT_LE(recorder.xruns(), allowance);
    EXPECT_FALSE(recorder.failed());

    // Nothing calls back once the Stop has returned, and the Close closed it.
    EXPECT_EQ(later, after_stop);
    EXPECT_FALSE(system_->IsOpen());
}

// ---- Each API: a consumer's thread at the callback's standing ------------------------------------------------------

TEST_P(AudioDevices, GivesAConsumersThreadTheCallbacksStandingOrSaysWhyNot)
{
    // With no stream open, the join is refused with a reason, whatever the API.
    {
        RealtimeThreadMembership   membership;
        const RealtimeThreadResult closed = system_->JoinAudioThread(membership);
        EXPECT_FALSE(closed.is_joined);
        EXPECT_FALSE(membership.IsJoined());
        EXPECT_NE(closed.refused.find("no stream is open"), std::string::npos) << closed.refused;
    }
    const TestDevice device = Find();
    RECKLESS_NEED_TEST_DEVICE(device);
    Recorder recorder(0, device.channels, Recorder::Play::kSilence);
    ASSERT_TRUE(system_->Open(RequestFor(device, false), recorder)) << system_->LastError();
    ASSERT_TRUE(system_->Start()) << system_->LastError();

    // A thread of the test's own, as a consumer's worker would: joined as it starts, left as it ends.
    RealtimeThreadResult result;
    bool                 was_still_joined_at_the_end = false;
    std::thread          worker([&] {
        RealtimeThreadScope scope(*system_);
        result                      = scope.Result();
        was_still_joined_at_the_end = result.is_joined;
        scope.Leave();
    });
    worker.join();
    system_->Stop();
    system_->Close();
    Table(api_ + ": a consumer's thread joined to the callback's standing")
        .Row("joined", result.is_joined ? "yes" : "no")
        .Row("achieved", result.achieved.empty() ? "nothing" : result.achieved)
        .Row("refused", result.refused.empty() ? "nothing" : result.refused)
        .Print();

    // Always an answer: what was given, or why not - never both empty.
    EXPECT_FALSE(result.achieved.empty() && result.refused.empty());
    EXPECT_EQ(result.is_joined, !result.achieved.empty());
    if (!result.is_joined)
    {
        EXPECT_FALSE(result.refused.empty());
    }
    // CoreAudio and Windows Audio, ASIO included, have no limit to refuse by: a thread of ours joins their scheduler.
    // Linux's ALSA and JACK may refuse, as a user with no `rtprio` limit is refused.
    if (api_ == "CoreAudio" || api_ == "Windows Audio" || api_ == "Windows Audio (Exclusive Mode)" || api_ == "ASIO")
    {
        EXPECT_TRUE(result.is_joined) << result.Describe();
    }
    EXPECT_EQ(was_still_joined_at_the_end, result.is_joined);
}

// ---- Each API: the samples a loop carries back ---------------------------------------------------------------------

TEST_P(AudioDevices, CarriesTheSamplesALoopBringsBack)
{
    const TestDevice device = Find();
    RECKLESS_NEED_TEST_DEVICE(device);
    if (device.loop == Loop::kNone)
    {
        GTEST_SKIP() << api_ << "'s test device is output only: " << device.about;
    }
    const Recorder::Play play    = device.loop == Loop::kExact ? Recorder::Play::kIndices
                                   : device.loop == Loop::kCorrelated || device.loop == Loop::kCable
                                       ? Recorder::Play::kNoise
                                       : Recorder::Play::kSilence;
    const StreamRequest  request = RequestFor(device, true);
    Recorder             recorder(device.channels, device.channels, play);
    const StreamCloser   closer(*system_); // before the Recorder goes, whatever fails
    ASSERT_TRUE(system_->Open(request, recorder)) << system_->LastError();
    const StreamFormat format = system_->Format();
    ASSERT_TRUE(system_->Start()) << system_->LastError();
    std::string why;
    ASSERT_TRUE(rig_->Started(kClient, why)) << why;
    RunFor(device.settle_seconds + 1.2);
    system_->Stop();
    system_->Close();

    const std::size_t  recorded = recorder.recorded_frames();
    const std::size_t  settled  = static_cast<std::size_t>(device.settle_seconds * format.sample_rate);
    const auto        &in0      = recorder.recorded(0);
    const auto        &in1      = recorder.recorded(1);
    const std::int64_t xruns    = recorder.xruns();
    const double       rate     = format.sample_rate;
    ASSERT_GT(recorded, static_cast<std::size_t>(0.5 * rate)) << "the stream recorded too little to judge";

    if (device.loop == Loop::kExact)
    {
        // Each sample back names the frame and the channel it was played as: from the first frame the loop carries on
        // both channels, every later sample must name the next frame on both - bar a jump or a silent gap where the
        // device reported an xrun, which drops or pads a block - and none may name nothing.
        const LoopScan scan = ScanLoop(in0, in1, recorded);
        ASSERT_LT(scan.first, recorded) << "nothing came back: the loop carried silence";
        const std::size_t  first           = scan.first;
        const std::int64_t played_at_first = scan.played_at_first;
        const std::int64_t corrupt = scan.corrupt, jumps = scan.jumps, gaps = scan.gaps, compared = scan.compared;
        const std::size_t  first_corrupt = scan.first_corrupt;
        Table(api_ + ": the loop, sample for sample")
            .Row("played / recorded",
                 Str(recorder.played()) + " / " + Str(static_cast<std::int64_t>(recorded)) + " frames")
            .Row("loop latency",
                 played_at_first >= 0
                     ? Str(static_cast<std::int64_t>(first) - played_at_first) + " samples (" +
                           Fixed(1e3 * static_cast<double>(static_cast<std::int64_t>(first) - played_at_first) / rate,
                                 2) +
                           " ms); the API reports " + Str(format.reported_latency)
                     : std::string("-"))
            .Row("samples compared, bit for bit", Str(compared) + " on each of 2 channels")
            .Row("samples naming no frame, or channels apart",
                 Str(corrupt) + (corrupt > 0 ? " (first at " + Str(static_cast<std::int64_t>(first_corrupt)) + ")"
                                             : std::string()))
            .Row("jumps / silent samples", Str(jumps) + " / " + Str(gaps))
            .Row("xruns reported", Str(xruns))
            .Print();
        EXPECT_EQ(corrupt, 0) << "a sample came back other than it was played";
        EXPECT_GE(compared, static_cast<std::int64_t>(0.5 * rate));
        if (xruns == 0)
        {
            EXPECT_EQ(jumps, 0) << "the loop skipped frames with no xrun reported";
            EXPECT_EQ(gaps, 0) << "the loop carried silence with no xrun reported";
        }
        EXPECT_LE(jumps, xruns);
    }
    else if (device.loop == Loop::kCorrelated || device.loop == Loop::kCable)
    {
        // Through a mixer the signal comes back at some gain and some delay: the delay found from a window of the
        // noise played, then the correlation coefficient over everything after it - all of it once the device has
        // settled (TestDevice::settle_seconds).
        const auto        played = [](std::int64_t n) { return static_cast<double>(NoiseSample(n, 0)); };
        const std::size_t window = 4096, start = settled + static_cast<std::size_t>(0.25 * rate);
        const std::size_t max_lag = std::min(static_cast<std::size_t>(0.5 * rate), recorded - start - window);
        double            best    = 0.0;
        std::size_t       lag     = 0;
        for (std::size_t l = 0; l < max_lag; ++l)
        {
            double sum = 0.0;
            for (std::size_t i = 0; i < window; ++i)
            {
                sum += played(static_cast<std::int64_t>(start + i)) * in0[start + i + l];
            }
            if (std::abs(sum) > std::abs(best))
            {
                best = sum;
                lag  = l;
            }
        }
        double xy = 0.0, xx = 0.0, yy = 0.0;
        for (std::size_t i = start; i + lag < recorded; ++i)
        {
            const double x = played(static_cast<std::int64_t>(i));
            const double y = in0[i + lag];
            xy += x * y;
            xx += x * x;
            yy += y * y;
        }
        const double r = xx > 0.0 && yy > 0.0 ? xy / std::sqrt(xx * yy) : 0.0;
        // Through a cable the round trip is the API's: the block the output buffers, the block the input gathered,
        // and the latency it reports beyond them each way - the converters', the driver's and its safety offsets.
        const std::int64_t predicted = 2 * static_cast<std::int64_t>(format.block_size) + format.reported_latency;
        const bool         cable     = device.loop == Loop::kCable;
        Table              table(api_ + (cable ? ": the loop through the cable" : ": the loop through the mixer"));
        table.Row("loopback delay", Str(static_cast<std::int64_t>(lag)) + " samples (" +
                                        Fixed(1e3 * static_cast<double>(lag) / rate, 2) + " ms)");
        if (cable)
        {
            table.Row("the API's round trip", Str(predicted) + " samples: two blocks of " + Str(format.block_size) +
                                                  " and the " + Str(format.reported_latency) + " it reports");
            table.Row("measured less reported",
                      Str(static_cast<std::int64_t>(lag) - predicted) + " samples (" +
                          Fixed(1e3 * (static_cast<double>(lag) - static_cast<double>(predicted)) / rate, 2) + " ms)" +
                          (device.does_report_whole_round_trip
                               ? std::string()
                               : ", and the figure leaves out " + device.partial_latency));
        }
        // Through a cable, each channel's fit by a short filter (FilteredCorrelation): the converters' band and a
        // fraction of a sample's delay taken in, so only what the stream did wrong is left - and the second channel
        // judged too, against its own noise, which a pair played crossed over fails.
        const double fit0 = cable ? FilteredCorrelation(in0, recorded, 0, lag, start) : 0.0;
        const double fit1 = cable ? FilteredCorrelation(in1, recorded, 1, lag, start) : 0.0;
        table.Row("correlation", Fixed(r, 4) + (cable ? " sample for sample" : " (at least 0.9)"));
        if (cable)
        {
            table.Row("through a 64-tap filter", Fixed(fit0, 5) + " / " + Fixed(fit1, 5) + " (each at least 0.99)");
        }
        table.Row("gain", yy > 0.0 ? Fixed(10.0 * std::log10(yy / xx), 2) + " dB" : std::string("silence"))
            .Row("xruns reported", Str(xruns))
            .Print();
        if (cable && std::min(fit0, fit1) < 0.9995)
        {
            // Short of what a cable carries: which blocks came back wrong, and as what. Each block the output played
            // (at its delay) and each the input read, its correlation with what was played there; for one that came
            // back wrong, its level and the offset nearby where what it carries was played instead - silence for a
            // block dropped and padded, an earlier offset for one replayed or read again from a buffer's last lap.
            const auto block       = static_cast<std::size_t>(format.block_size);
            const auto correlation = [&](std::size_t from, std::int64_t offset) {
                double sxy = 0.0, sxx = 0.0, syy = 0.0;
                for (std::size_t i = from; i < from + block; ++i)
                {
                    const double x = played(static_cast<std::int64_t>(i) - offset), y = in0[i];
                    sxy += x * y;
                    sxx += x * x;
                    syy += y * y;
                }
                return sxx > 0.0 && syy > 0.0 ? sxy / std::sqrt(sxx * syy) : 0.0;
            };
            Table where(api_ + ": the blocks that came back wrong (the frame recorded, correlation at the delay " +
                        Str(static_cast<std::int64_t>(lag)) + ", level, where it was played instead)");
            int   shown = 0, wrong_in = 0, wrong_out = 0, blocks = 0;
            for (const bool by_output : {true, false})
            {
                // An output block played from frame k * block is recorded from k * block + lag; an input block is
                // read from k * block.
                std::size_t from = by_output ? (start / block + 1) * block + lag : (start / block + 1) * block;
                for (; from + 2 * block < recorded; from += block)
                {
                    ++blocks;
                    const double here = correlation(from, static_cast<std::int64_t>(lag));
                    if (here >= 0.5)
                    {
                        continue;
                    }
                    ++(by_output ? wrong_out : wrong_in);
                    double level = 0.0;
                    for (std::size_t i = from; i < from + block; ++i)
                    {
                        level += static_cast<double>(in0[i]) * in0[i];
                    }
                    double       best = 0.0;
                    std::int64_t at   = 0;
                    for (std::int64_t offset = 0; offset < 8192 && offset < static_cast<std::int64_t>(from); ++offset)
                    {
                        if (const double c = correlation(from, offset); c > best)
                        {
                            best = c;
                            at   = offset;
                        }
                    }
                    if (shown++ < 24)
                    {
                        where.Row(std::string(by_output ? "output" : "input") + " block at " +
                                      Str(static_cast<std::int64_t>(from)),
                                  Fixed(here, 3) + ", " +
                                      Fixed(10.0 * std::log10(level / static_cast<double>(block) + 1e-30), 1) +
                                      " dB, " +
                                      (best >= 0.5 ? "delay " + Str(at) + " (" + Fixed(best, 3) + ")"
                                                   : std::string("nowhere")));
                    }
                }
            }
            where
                .Row("blocks judged, wrong by the output's / the input's",
                     Str(blocks / 2) + ", " + Str(wrong_out) + " / " + Str(wrong_in))
                .Print();
        }
        if (cable)
        {
            EXPECT_GE(fit0, 0.99) << "the first channel back is not what was played, filtered - is the cable patched "
                                     "as this API's loop variable says, and the input's gain up but not clipping?";
            EXPECT_GE(fit1, 0.99) << "the second channel back is not what was played on it, filtered";
        }
        else
        {
            EXPECT_GE(r, 0.9) << "the loopback capture does not carry what the stream played - is the endpoint muted, "
                                 "or did another sound play over it?";
        }
        if (cable)
        {
            // A converter's filters take a few samples the driver may not count: the round trip within a 64th of a
            // second of what the API reports, and never sooner than its two blocks.
            EXPECT_GE(static_cast<std::int64_t>(lag), 2 * static_cast<std::int64_t>(format.block_size))
                << "the samples came back sooner than the two blocks they crossed";
            if (device.does_report_whole_round_trip)
            {
                EXPECT_LE(std::llabs(static_cast<std::int64_t>(lag) - predicted), static_cast<std::int64_t>(rate / 64))
                    << "the latency the API reports is not the round trip a cable measures";
            }
            else
            {
                // The API leaves part of the path out of the figure it reports (TestDevice::partial_latency), so the
                // round trip cannot be predicted from it - only that it is never sooner than what is reported.
                EXPECT_GE(static_cast<std::int64_t>(lag), predicted)
                    << "the samples came back sooner than the latency the API reports, which already leaves out "
                    << device.partial_latency;
            }
        }
    }
    else
    {
        // The ASIO test driver's inputs: its sine on the first, its sawtooth on the second, every block the same 256
        // 16-bit samples (AsioRig.cpp makes them as the driver does), each read as int16 / 32768 exactly. The driver
        // fills the half of its input buffers it is about to hand over, and the back end reads that half in the same
        // switch, so the first callback already carries the driver's samples: none of silence before them. (RtAudio
        // read the half before, a block late.)
        const std::vector<std::vector<float>> waves = reckless::audio::test::AsioTestDriverWaves();
        ASSERT_EQ(waves.size(), 2u);
        const std::size_t period = waves[0].size();
        ASSERT_GT(period, 0u);
        std::size_t lead = 0;
        while (lead < recorded && in0[lead] == 0.0f && in1[lead] == 0.0f)
        {
            ++lead;
        }
        std::int64_t mismatches = 0, compared = 0;
        std::size_t  first_mismatch = recorded;
        for (std::size_t i = lead; i < recorded; ++i)
        {
            for (int c = 0; c < 2; ++c)
            {
                const float want = waves[static_cast<std::size_t>(c)][(i - lead) % period];
                const float got  = recorder.recorded(c)[i];
                if (got != want)
                {
                    ++mismatches;
                    first_mismatch = std::min(first_mismatch, i);
                }
                ++compared;
            }
        }
        Table(api_ + ": the test driver's own inputs")
            .Row("silent before the driver's samples",
                 Str(static_cast<std::int64_t>(lead)) +
                     " frames (none; a block: " + Str(static_cast<std::int64_t>(format.block_size)) + ")")
            .Row("samples compared, exactly", Str(compared) + " over 2 channels")
            .Row("mismatches",
                 Str(mismatches) + (mismatches > 0
                                        ? " (first at frame " + Str(static_cast<std::int64_t>(first_mismatch)) + ")"
                                        : std::string()))
            .Row("xruns reported", Str(xruns))
            .Print();
        EXPECT_EQ(lead, 0u) << "the driver's samples did not start with the first callback";
        EXPECT_EQ(mismatches, 0) << "an input sample is not the driver's sine or sawtooth read as int16 / 32768";
        EXPECT_GE(compared, static_cast<std::int64_t>(rate)); // two channels of at least half a second
    }
}

// ---- Each API: the periods a stream asks for -----------------------------------------------------------------------

TEST_P(AudioDevices, KeepsTheCablesDelayThroughSilence)
{
    // A capture plays, falls silent and plays again in one stream - Reckless IR Capture's noise periods, its gap and
    // its sweep - and measures each against when it was played, so a stream that lost or gained a frame while it
    // carried silence moves every response after it. Through a cable, bursts of the known noise with silence between
    // them: each burst's delay found to a fraction of a frame by the peak of its matched filter, and every one the
    // first's.
    const TestDevice device = Find();
    RECKLESS_NEED_TEST_DEVICE(device);
    if (device.loop != Loop::kCable)
    {
        GTEST_SKIP() << "the delay through silence is measured through a cable, which " << api_
                     << "'s test device is not";
    }
    Recorder     recorder(device.channels, device.channels, Recorder::Play::kBursts);
    StreamFormat format;
    {
        const StreamCloser closer(*system_); // before the Recorder goes, whatever fails
        ASSERT_TRUE(system_->Open(RequestFor(device, true), recorder)) << system_->LastError();
        format = system_->Format();
        ASSERT_TRUE(system_->Start()) << system_->LastError();
        const double room = 0.9 * static_cast<double>(Recorder::kMaxFrames) / format.sample_rate;
        RunFor(std::min(8.0, room));
        system_->Stop();
        system_->Close();
    }
    const std::size_t recorded = recorder.recorded_frames();
    const auto       &in0      = recorder.recorded(0);
    const auto        max_lag  = static_cast<std::int64_t>(format.sample_rate / 2);

    // The matched filter of one burst at lag l: the burst played against what came back l frames later.
    const auto filtered = [&](std::int64_t burst, std::int64_t lag) {
        double sum = 0.0;
        for (std::int64_t i = 0; i < kBurstFrames; ++i)
        {
            const std::int64_t at = burst * kBurstPeriod + i + lag;
            sum += static_cast<double>(NoiseSample(burst * kBurstPeriod + i, 0)) * in0[static_cast<std::size_t>(at)];
        }
        return sum;
    };
    // The peak between `from` and `to`, interpolated: the delay to a fraction of a frame.
    const auto delay_of = [&](std::int64_t burst, std::int64_t from, std::int64_t to) {
        std::int64_t best_lag = from;
        double       best     = 0.0;
        for (std::int64_t lag = from; lag <= to; ++lag)
        {
            const double value = std::abs(filtered(burst, lag));
            if (value > best)
            {
                best     = value;
                best_lag = lag;
            }
        }
        const double before = std::abs(filtered(burst, best_lag - 1)), after = std::abs(filtered(burst, best_lag + 1));
        const double bend = before - 2.0 * best + after;
        return static_cast<double>(best_lag) + (bend != 0.0 ? 0.5 * (before - after) / bend : 0.0);
    };

    std::vector<double> delays;
    for (std::int64_t burst = 0; static_cast<std::size_t>((burst + 1) * kBurstPeriod + max_lag) <= recorded; ++burst)
    {
        if (delays.empty())
        {
            delays.push_back(delay_of(burst, 1, max_lag - 1));
        }
        else
        {
            const auto first_delay = static_cast<std::int64_t>(std::lround(delays.front()));
            delays.push_back(delay_of(burst, first_delay - 64, first_delay + 64));
        }
    }
    Table table(api_ + ": the cable's delay, burst by burst through silence");
    for (std::size_t b = 0; b < delays.size(); ++b)
    {
        table.Row("burst " + Str(static_cast<std::int64_t>(b + 1)) + ", from frame " +
                      Str(static_cast<std::int64_t>(b) * kBurstPeriod),
                  Fixed(delays[b], 2) + " frames");
    }
    table.Row("xruns reported", Str(recorder.xruns())).Print();
    ASSERT_GE(delays.size(), 3u) << "too few bursts came back to judge";
    if (recorder.xruns() == 0)
    {
        for (std::size_t b = 1; b < delays.size(); ++b)
        {
            EXPECT_NEAR(delays[b], delays.front(), 0.05)
                << "burst " << b + 1 << " came back at another delay: the stream lost or gained frames in the silence";
        }
    }
}

TEST_P(AudioDevices, BuffersThePeriodsTheRequestAsksFor)
{
    if (api_ != "ALSA")
    {
        GTEST_SKIP() << api_
                     << " buffers as it does: ALSA alone lets a stream choose its periods (StreamRequest::periods)";
    }
    const TestDevice device = Find();
    RECKLESS_NEED_TEST_DEVICE(device);
    if (device.loop != Loop::kExact)
    {
        GTEST_SKIP() << "the latency is read off an exact loop, which " << api_ << "'s test device is not";
    }
    // Each sample back names the frame it was played as, so the loop's latency is read to the frame: an output primed
    // with a buffer of the periods asked for brings each frame back that many blocks later.
    Table table(api_ + ": the periods a stream asks for (asked: opened, reported latency, loop latency, xruns)");
    for (const int periods : {2, 3, 4})
    {
        StreamRequest request = RequestFor(device, true);
        request.periods       = periods;
        Recorder     recorder(device.channels, device.channels, Recorder::Play::kIndices);
        StreamFormat format;
        {
            const StreamCloser closer(*system_); // before the Recorder goes, whatever fails
            ASSERT_TRUE(system_->Open(request, recorder)) << system_->LastError();
            format = system_->Format();
            ASSERT_TRUE(system_->Start()) << system_->LastError();
            std::string why;
            ASSERT_TRUE(rig_->Started(kClient, why)) << why;
            RunFor(0.6);
            system_->Stop();
            system_->Close();
        }
        const LoopScan scan = ScanLoop(recorder.recorded(0), recorder.recorded(1), recorder.recorded_frames());
        ASSERT_LT(scan.first, recorder.recorded_frames()) << "nothing came back: the loop carried silence";
        const std::int64_t loop = static_cast<std::int64_t>(scan.first) - scan.played_at_first;
        table.Row(Str(periods), Str(format.periods) + ", " + Str(format.reported_latency) + " samples, " + Str(loop) +
                                    " samples (" + Fixed(1e3 * static_cast<double>(loop) / format.sample_rate, 2) +
                                    " ms), " + Str(recorder.xruns()));
        EXPECT_EQ(format.periods, periods);
        // Beyond the block each way, as StreamFormat means it: the buffer less the two blocks a cable's round trip
        // counts beside it (CarriesTheSamplesALoopBringsBack).
        EXPECT_EQ(format.reported_latency, (periods - 2) * format.block_size) << periods << " periods";
        // ALSA's figure is the playback buffer's alone: the output side holds all of it, the input side is not told.
        EXPECT_EQ(format.output_latency_frames, format.reported_latency);
        EXPECT_EQ(format.input_latency_frames, 0);
        EXPECT_EQ(scan.corrupt, 0);
        if (recorder.xruns() == 0)
        {
            EXPECT_EQ(loop, static_cast<std::int64_t>(periods) * format.block_size) << periods << " periods";
        }
        EXPECT_LE(recorder.xruns(), XrunAllowance(static_cast<std::int64_t>(recorder.callbacks())));
    }
    table.Print();
}

// ---- Each API: a device that is not there --------------------------------------------------------------------------

TEST_P(AudioDevices, RefusesADeviceThatIsNotThereAndOpensTheNextOne)
{
    Recorder      recorder(0, 2, Recorder::Play::kSilence);
    StreamRequest request;
    request.output_device = "Reckless: no such device";
    request.output_count  = 2;
    request.sample_rate   = 48000;
    request.block_size    = 256;
    request.client_name   = kClient;
    EXPECT_FALSE(system_->Open(request, recorder));
    EXPECT_FALSE(system_->IsOpen());
    EXPECT_NE(system_->LastError().find(request.output_device), std::string::npos) << system_->LastError();
    const std::string no_output = system_->LastError();

    const TestDevice device = Find();
    RECKLESS_NEED_TEST_DEVICE(device);
    request              = RequestFor(device, false);
    request.input_device = "Reckless: no such input";
    request.input_count  = 2;
    Recorder with_input(2, device.channels, Recorder::Play::kSilence);
    EXPECT_FALSE(system_->Open(request, with_input));
    EXPECT_FALSE(system_->IsOpen());
    EXPECT_NE(system_->LastError().find(request.input_device), std::string::npos) << system_->LastError();
    const std::string no_input = system_->LastError();

    // What failed left nothing behind: the test device opens, runs and closes.
    request = RequestFor(device, false);
    Recorder after(0, device.channels, Recorder::Play::kSilence);
    ASSERT_TRUE(system_->Open(request, after)) << system_->LastError();
    ASSERT_TRUE(system_->Start()) << system_->LastError();
    RunFor(0.3);
    system_->Stop();
    system_->Close();
    Table(api_ + ": devices that are not there")
        .Row("no such output", no_output)
        .Row("no such input", no_input)
        .Row("then the test device", Str(after.callbacks()) + " callbacks")
        .Print();
    EXPECT_GT(after.callbacks(), 0);
}

// ---- Each API: an xrun ---------------------------------------------------------------------------------------------

TEST_P(AudioDevices, CountsAnXrunAndPlaysOnAfterIt)
{
    const TestDevice device = Find();
    RECKLESS_NEED_TEST_DEVICE(device);
    if (device.xrun_by == reckless::audio::test::XrunBy::kNothing)
    {
        GTEST_SKIP() << api_ << " cannot report an xrun: " << device.no_xrun;
    }
    const bool         exact      = device.loop == Loop::kExact;
    const bool         with_input = !device.input.empty();
    Recorder           recorder(with_input ? device.channels : 0, device.channels,
                                exact ? Recorder::Play::kIndices : Recorder::Play::kSilence);
    const StreamCloser closer(*system_); // before the Recorder goes, whatever fails
    ASSERT_TRUE(system_->Open(RequestFor(device, with_input), recorder)) << system_->LastError();
    const StreamFormat format = system_->Format();
    ASSERT_TRUE(system_->Start()) << system_->LastError();
    std::string why;
    ASSERT_TRUE(rig_->Started(kClient, why)) << why;
    RunFor(0.4);
    const std::int64_t before         = recorder.xruns();
    const std::int64_t before_counted = recorder.callbacks();
    // A stall of two blocks or 30 ms, whichever is longer: long enough past the block to run any device dry.
    const int stall_ms =
        std::max(30, static_cast<int>(std::ceil(2000.0 * static_cast<double>(format.block_size) / format.sample_rate)));
    if (device.xrun_by == reckless::audio::test::XrunBy::kStall)
    {
        recorder.StallOnce(stall_ms);
    }
    else
    {
        ASSERT_TRUE(rig_->Xrun(why)) << why;
    }
    RunFor(kRunAfterXrunSeconds);
    const std::int64_t stopped = NowNanoseconds();
    system_->Stop();
    system_->Close();
    const std::int64_t after = recorder.xruns();

    // The first callback told of the xrun, and from the time a device has to start again after it to the end, the
    // stream held as a stream from its start is: the callbacks as the rate and the block predict, at the rate by the
    // clock, no more xruns than a runner's scheduler is allowed, and the last of them where the stream was stopped.
    const std::size_t n    = recorder.stamped();
    std::size_t       told = n;
    for (std::size_t i = 0; i < n; ++i)
    {
        if (recorder.xrun_at(i))
        {
            told = i;
            break;
        }
    }
    ASSERT_GT(after, before) << "the device ran dry and the callback was not told";
    ASSERT_LT(told, n);
    std::size_t from = told;
    while (from + 1 < n && recorder.time_at(from) < recorder.time_at(told) + kStartAgainNs)
    {
        ++from;
    }
    std::int64_t frames = 0, later = 0;
    for (std::size_t i = from; i < n; ++i)
    {
        frames += i + 1 < n ? recorder.frames_at(i) : 0;
        later += (recorder.xrun_at(i) && i > told) ? 1 : 0;
    }
    const double       block_ns      = static_cast<double>(format.block_size) * 1e9 / format.sample_rate;
    const double       span_ns       = static_cast<double>(recorder.time_at(n - 1) - recorder.time_at(from));
    const double       counted       = static_cast<double>(n - 1 - from);
    const double       predicted     = span_ns / block_ns;
    const double       rate_after    = span_ns > 0.0 ? static_cast<double>(frames) / (span_ns * 1e-9) : 0.0;
    const double       last_ms       = static_cast<double>(stopped - recorder.time_at(n - 1)) * 1e-6;
    const auto         read          = static_cast<std::int64_t>(n - from);
    const std::int64_t later_allowed = XrunAllowance(read) + LoadAllowance(device, read);
    const std::string  from_label    = "from " + Str(kStartAgainNs / 1'000'000) + " ms after";

    // Through an exact loop, every sample back is one that was played, and once the device has started again the frames
    // come back one after another to the end of the run.
    LoopScan scan;
    if (exact)
    {
        scan = ScanLoop(recorder.recorded(0), recorder.recorded(1), recorder.recorded_frames());
    }
    const double tail_s =
        exact ? static_cast<double>(recorder.recorded_frames() - scan.settled) / format.sample_rate : 0.0;

    Table table(api_ + ": an xrun, and the stream after it");
    table
        .Row("made by", device.xrun_by == reckless::audio::test::XrunBy::kStall
                            ? "a callback running " + Str(stall_ms) + " ms"
                            : std::string("the device, asked by the rig"))
        .Row("xruns before / after", Str(before) + " / " + Str(after))
        .Row("first told at callback", Str(static_cast<std::int64_t>(told) + 1) + " of " + Str(recorder.callbacks()))
        .Row(from_label + ": callbacks / predicted", Fixed(counted, 0) + " / " + Fixed(predicted, 1) + " (within " +
                                                         Fixed(device.rate_tolerance * predicted + 2.0, 1) + ")")
        .Row(from_label + ": rate by the clock", Fixed(rate_after, 1) + " Hz over " + Fixed(span_ns * 1e-9, 2) +
                                                     " s (within " + Fixed(100.0 * device.rate_tolerance, 1) + " %)")
        .Row(from_label + ": xruns", Str(later) + " (at most " + Str(later_allowed) + ")")
        .Row("last callback before the stop", Fixed(last_ms, 1) + " ms");
    if (exact)
    {
        table.Row("loop: samples naming no frame, or channels apart", Str(scan.corrupt))
            .Row("loop: jumps / silent samples", Str(scan.jumps) + " / " + Str(scan.gaps))
            .Row("loop: frames one after another at the end", Fixed(tail_s, 2) + " s (at least 0.30)");
    }
    table.Print();

    EXPECT_LE(before, LoadAllowance(device, before_counted))
        << "the device reported an xrun before the test made one - at the start?";
    EXPECT_LE(later, later_allowed) << "xruns went on after the device had had time to start again";
    EXPECT_NEAR(rate_after, format.sample_rate, device.rate_tolerance * format.sample_rate)
        << "the stream did not play on at its rate";
    EXPECT_NEAR(counted, predicted, device.rate_tolerance * predicted + 2.0)
        << "the callbacks did not come on as the rate predicts";
    EXPECT_LT(last_ms, 100.0) << "the callbacks stopped before the stream was stopped";
    if (exact)
    {
        EXPECT_EQ(scan.corrupt, 0) << "a sample came back other than it was played";
        EXPECT_GE(tail_s, 0.30) << "the loop did not come back to one frame after another";
    }
}

// ---- MIDI -----------------------------------------------------------------------------------------------------------

namespace
{
/** What a MIDI input delivered: each message's port, time and bytes, into storage made up front, read once the inputs
    are closed. */
class Collected final : public reckless::audio::MidiSink
{
  public:
    static constexpr int kMost = 64; ///< the messages kept

    void Arrived(int port, std::int64_t time, const std::uint8_t *bytes, int size) noexcept override
    {
        const int n = count_.load(std::memory_order_relaxed);
        if (n < kMost)
        {
            ports_[static_cast<std::size_t>(n)] = port;
            times_[static_cast<std::size_t>(n)] = time;
            messages_[static_cast<std::size_t>(n)].assign(bytes, bytes + size);
        }
        count_.store(n + 1, std::memory_order_release);
    }

    int count() const noexcept
    {
        return count_.load(std::memory_order_acquire);
    }

    int port_at(int i) const noexcept
    {
        return ports_[static_cast<std::size_t>(i)];
    }

    std::int64_t time_at(int i) const noexcept
    {
        return times_[static_cast<std::size_t>(i)];
    }

    const std::vector<std::uint8_t> &message_at(int i) const noexcept
    {
        return messages_[static_cast<std::size_t>(i)];
    }

    /** Until `n` messages have arrived, for up to two seconds.
        @param n  the messages to wait for
        @return   true when they came */
    bool AwaitCount(int n) const
    {
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (count() < n && std::chrono::steady_clock::now() < until)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        return count() >= n;
    }

  private:
    std::atomic<int>                             count_{0};
    std::array<int, kMost>                       ports_{};
    std::array<std::int64_t, kMost>              times_{};
    std::array<std::vector<std::uint8_t>, kMost> messages_; // assigned on the MIDI thread: test code, not a core
};

#if defined(RECKLESS_AUDIO_TEST_ALSA_SEQ)
/** A sequencer client of the test's own with one readable port, which sends straight to whoever subscribes. */
class TestSource
{
  public:
    TestSource()
    {
        if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0)
        {
            seq_ = nullptr;
            return;
        }
        snd_seq_set_client_name(seq_, "Reckless test source");
        port_ = snd_seq_create_simple_port(seq_, "out", SND_SEQ_PORT_CAP_READ | SND_SEQ_PORT_CAP_SUBS_READ,
                                           SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
    }

    ~TestSource()
    {
        if (seq_ != nullptr)
        {
            snd_seq_close(seq_);
        }
    }

    TestSource(const TestSource &)            = delete;
    TestSource &operator=(const TestSource &) = delete;

    bool made() const noexcept
    {
        return seq_ != nullptr && port_ >= 0;
    }

    /** The name the layer lists the port under: RtMidi's "<client>:<port> <client number>:<port number>". */
    std::string Name() const
    {
        return "Reckless test source:out " + std::to_string(snd_seq_client_id(seq_)) + ":" + std::to_string(port_);
    }

    /** `event` sent to the port's subscribers at once. */
    void Send(snd_seq_event_t event)
    {
        snd_seq_ev_set_source(&event, port_);
        snd_seq_ev_set_subs(&event);
        snd_seq_ev_set_direct(&event);
        snd_seq_event_output_direct(seq_, &event);
    }

  private:
    snd_seq_t *seq_{nullptr};
    int        port_{-1};
};
#endif
} // namespace

#if defined(RECKLESS_AUDIO_TEST_ALSA_SEQ)
TEST(MidiInputs, CarryWhatASequencerPortSendsButWhatTheCoresIgnore)
{
    TestSource source;
    const bool required = std::find(RequiredApis().begin(), RequiredApis().end(), "ALSA") != RequiredApis().end();
    if (!source.made())
    {
        if (required)
        {
            FAIL() << "ALSA is required (RECKLESS_AUDIO_REQUIRE) and its sequencer would not open: is snd-seq loaded?";
        }
        GTEST_SKIP() << "ALSA's sequencer would not open: no snd-seq in this kernel";
    }
    const auto                     midi   = reckless::audio::MakeMidiSystem("Reckless MIDI test");
    const std::vector<std::string> inputs = midi->Inputs();
    Table                          table("ALSA sequencer: the inputs");
    for (const std::string &input : inputs)
    {
        table.Row(input, "");
    }
    table.Row("library", reckless::audio::AudioLibraryVersions()).Print();
    ASSERT_NE(std::find(inputs.begin(), inputs.end(), source.Name()), inputs.end())
        << "the test's port, listed as " << source.Name();

    // Refused by a name that is nowhere, the error naming it.
    Collected nothing;
    EXPECT_FALSE(midi->Open("Reckless: no such MIDI input", 0, nothing));
    EXPECT_NE(midi->LastError().find("Reckless: no such MIDI input"), std::string::npos) << midi->LastError();

    // Found by its name with other numbers after it, as a device plugged in again comes back.
    Collected sink;
    ASSERT_TRUE(midi->Open("Reckless test source:out 999:9", 3, sink)) << midi->LastError();

    const std::int64_t before = NowNanoseconds();
    snd_seq_event_t    event;
    snd_seq_ev_clear(&event);
    snd_seq_ev_set_noteon(&event, 1, 60, 100);
    source.Send(event);
    std::uint8_t sysex[] = {0xF0, 0x7D, 0x01, 0x02, 0xF7};
    snd_seq_ev_clear(&event);
    snd_seq_ev_set_sysex(&event, sizeof sysex, sysex);
    source.Send(event);
    snd_seq_ev_clear(&event);
    event.type = SND_SEQ_EVENT_CLOCK;
    source.Send(event);
    snd_seq_ev_clear(&event);
    snd_seq_ev_set_controller(&event, 0, 7, 64);
    source.Send(event);
    snd_seq_ev_clear(&event);
    event.type = SND_SEQ_EVENT_SENSING;
    source.Send(event);
    snd_seq_ev_clear(&event);
    snd_seq_ev_set_noteoff(&event, 1, 60, 0);
    source.Send(event);
    snd_seq_ev_clear(&event);
    snd_seq_ev_set_pitchbend(&event, 15, 0);
    source.Send(event);
    const bool         came  = sink.AwaitCount(4);
    const std::int64_t after = NowNanoseconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // anything left out would have come by now
    midi->CloseAll();
    const int counted = sink.count();

    // Closed: nothing more reaches the sink.
    snd_seq_ev_clear(&event);
    snd_seq_ev_set_noteon(&event, 1, 61, 100);
    source.Send(event);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const std::vector<std::vector<std::uint8_t>> want = {
        {0x91, 60, 100}, {0xB0, 7, 64}, {0x81, 60, 0}, {0xEF, 0x00, 0x40}};
    Table result("ALSA sequencer: what arrived");
    for (int i = 0; i < std::min(counted, Collected::kMost); ++i)
    {
        std::string bytes;
        for (const std::uint8_t b : sink.message_at(i))
        {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%02X ", b);
            bytes += hex;
        }
        result.Row("message " + Str(i + 1), bytes + "port " + Str(sink.port_at(i)) + ", " +
                                                Fixed(static_cast<double>(sink.time_at(i) - before) * 1e-6, 2) +
                                                " ms after the first was sent");
    }
    result.Row("after the inputs closed", Str(sink.count() - counted) + " messages").Print();

    ASSERT_TRUE(came) << "the messages did not all arrive";
    ASSERT_EQ(counted, static_cast<int>(want.size()))
        << "system exclusive, timing clock and active sensing are left out, and nothing else";
    for (int i = 0; i < counted; ++i)
    {
        EXPECT_EQ(sink.message_at(i), want[static_cast<std::size_t>(i)]) << "message " << i + 1;
        EXPECT_EQ(sink.port_at(i), 3) << "the index the input was opened under";
        EXPECT_GE(sink.time_at(i), before);
        EXPECT_LE(sink.time_at(i), after);
        if (i > 0)
        {
            EXPECT_GE(sink.time_at(i), sink.time_at(i - 1)) << "stamped in the order they came";
        }
    }
    EXPECT_EQ(sink.count(), counted) << "a message reached the sink after CloseAll";
    EXPECT_EQ(nothing.count(), 0);
}
#endif

#if defined(RECKLESS_AUDIO_TEST_ALSA_SEQ) || defined(__APPLE__)
namespace
{
/** What an output open as `port` on `midi` sends, read back by `arrived` as the test's own destination receives it:
    thirty messages sent latest first for times ahead, two for one time and one for a time already past, each judged
    against the time it was sent for; then messages for later and the output closed before their time, none of which
    may arrive after CloseAll returns.
    @param midi     the system, `port` open on it
    @param port     the port the test's destination was opened as
    @param arrived  what the destination receives, each message with the time it left
    @param api      the API's name, for the tables
    @param is_scheduler_timed  whether the machine is one whose timing is a hypervisor's
   (TestDevice::is_scheduler_timed, decided as the suite decides it for the API's audio): there the test's own clock
   reads and wakes are late by milliseconds, so the median lateness is held to the millisecond and every message to 50
   ms, and the close is judged only when the test closed in time and counts a message as after it only when it came 50
   ms after. Real hardware is held to the strict bars: every message within the millisecond, and nothing at all after
   the close. */
void SendAndReadBack(reckless::audio::MidiSystem &midi, int port, Collected &arrived, const std::string &api,
                     bool is_scheduler_timed)
{
    const std::uint8_t note[] = {0x90, 60, 100};
    // Thirty messages 7 ms apart from 60 ms ahead, sent latest first; a note off sent after a note on for one time;
    // one message for a time already past.
    constexpr int                                                   kAhead = 30;
    const std::int64_t                                              first  = NowNanoseconds() + 60'000'000;
    std::vector<std::pair<std::int64_t, std::vector<std::uint8_t>>> want;
    for (int i = kAhead - 1; i >= 0; --i)
    {
        const std::int64_t time    = first + static_cast<std::int64_t>(i) * 7'000'000;
        const std::uint8_t bytes[] = {static_cast<std::uint8_t>(0x90 | (i % 16)), static_cast<std::uint8_t>(30 + i),
                                      100};
        ASSERT_TRUE(midi.Send(port, time, bytes, 3)) << i;
        want.emplace_back(time, std::vector<std::uint8_t>(bytes, bytes + 3));
    }
    const std::int64_t tie       = first + 100'000'000;
    const std::uint8_t tie_on[]  = {0x92, 90, 64};
    const std::uint8_t tie_off[] = {0x82, 90, 0};
    ASSERT_TRUE(midi.Send(port, tie, tie_on, 3));
    ASSERT_TRUE(midi.Send(port, tie, tie_off, 3));
    want.emplace_back(tie, std::vector<std::uint8_t>(tie_on, tie_on + 3));
    want.emplace_back(tie, std::vector<std::uint8_t>(tie_off, tie_off + 3));
    const std::uint8_t sysex[] = {0xF0, 0x7D, 0xF7};
    EXPECT_FALSE(midi.Send(port, tie, sysex, 3)) << "system exclusive is not sent";
    const std::int64_t past_sent = NowNanoseconds();
    const std::uint8_t past[]    = {0xB0, 7, 99};
    ASSERT_TRUE(midi.Send(port, past_sent - 10'000'000, past, 3));
    want.emplace_back(past_sent, std::vector<std::uint8_t>(past, past + 3)); // due when it was sent
    std::stable_sort(want.begin(), want.end(), [](const auto &a, const auto &b) { return a.first < b.first; });

    const bool came = arrived.AwaitCount(static_cast<int>(want.size()));
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const int counted = arrived.count();

    // Messages for 80 to 89 ms ahead, and the output closed at 75 ms, once the sender has handed them to the platform
    // a lead ahead and before their time: none of them arrives once CloseAll has returned.
    const std::int64_t closing = NowNanoseconds();
    for (int i = 0; i < 10; ++i)
    {
        const std::uint8_t later[] = {0x80, static_cast<std::uint8_t>(i), 0};
        ASSERT_TRUE(midi.Send(port, closing + 80'000'000 + i * 1'000'000, later, 3));
    }
    std::this_thread::sleep_until(
        std::chrono::steady_clock::time_point(std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::nanoseconds(closing + 75'000'000))));
    const std::int64_t close_began = NowNanoseconds();
    midi.CloseAll();
    const std::int64_t closed   = NowNanoseconds();
    const int          at_close = arrived.count();
    EXPECT_FALSE(midi.Send(port, NowNanoseconds(), note, 3)) << "closed";
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    // On a scheduler-timed machine a message arriving just after CloseAll returned may have left before it, and been
    // read late: it counts only when it came 50 ms after. And the claim is judged only if the close began before the
    // first of the later messages was due, less a millisecond: a test thread woken late closed too late to say.
    constexpr std::int64_t kHypervisorSlack = 50'000'000;
    const std::int64_t     first_later      = closing + 80'000'000;
    const bool             is_close_judged  = !is_scheduler_timed || close_began < first_later - 1'000'000;
    int                    after_close      = 0;
    for (int i = at_close; i < std::min(arrived.count(), Collected::kMost); ++i)
    {
        if (!is_scheduler_timed || arrived.time_at(i) > closed + kHypervisorSlack)
        {
            ++after_close;
        }
    }

    Table               result(api + ": what left, against its time");
    std::vector<double> lateness;
    for (int i = 0; i < std::min(counted, Collected::kMost); ++i)
    {
        std::string bytes;
        for (const std::uint8_t b : arrived.message_at(i))
        {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%02X ", b);
            bytes += hex;
        }
        std::string late = "-";
        if (i < static_cast<int>(want.size()))
        {
            lateness.push_back(static_cast<double>(arrived.time_at(i) - want[static_cast<std::size_t>(i)].first) *
                               1e-3);
            late = Fixed(lateness.back(), 1) + " us after its time";
        }
        result.Row("message " + Str(i + 1), bytes + late);
    }
    std::vector<double> sorted = lateness;
    std::sort(sorted.begin(), sorted.end());
    if (!sorted.empty())
    {
        result.Row("lateness", "median " + Fixed(sorted[sorted.size() / 2], 1) + " us, earliest " +
                                   Fixed(sorted.front(), 1) + " us, latest " + Fixed(sorted.back(), 1) + " us");
    }
    result.Row("after the output closed",
               Str(after_close) + " messages" +
                   (is_scheduler_timed ? " (counted from 50 ms after CloseAll returned)" : ""));
    result.Row("the close began", Fixed(static_cast<double>(close_began - first_later) * 1e-3, 1) +
                                      " us from the first later message's time" +
                                      (is_close_judged ? "" : ": too late to judge what left after it"));
    result
        .Row("held to", is_scheduler_timed
                            ? "a scheduler-timed machine's bars: median within 1 ms, each within 50 ms, none 1 ms "
                              "early; the close judged only when it began in time"
                            : "real hardware's bars: each within 1 ms of its time (a poll period more for one "
                              "sent for a time past), none 1 ms early, nothing after the close")
        .Print();

    ASSERT_TRUE(came) << "the messages did not all arrive";
    ASSERT_EQ(counted, static_cast<int>(want.size())) << "every message sent, and nothing else";
    // The one sent for a time already past waits for the sender's next look at its queue, since a Send cannot wake it:
    // a poll period, then the platform's own scheduling.
    const double past_bound = static_cast<double>(reckless::audio::kMidiSenderPollNanoseconds) * 1e-3 + 1000.0;
    for (int i = 0; i < counted; ++i)
    {
        EXPECT_EQ(arrived.message_at(i), want[static_cast<std::size_t>(i)].second) << "message " << i + 1;
        const bool is_past = want[static_cast<std::size_t>(i)].first == past_sent;
        EXPECT_GE(lateness[static_cast<std::size_t>(i)], -1000.0) << "message " << i + 1 << " left early";
        if (is_scheduler_timed)
        {
            EXPECT_LE(lateness[static_cast<std::size_t>(i)], 50'000.0)
                << "message " << i + 1 << " left more than 50 ms after its time, past any hypervisor's delay";
        }
        else
        {
            EXPECT_LE(lateness[static_cast<std::size_t>(i)], is_past ? past_bound : 1000.0)
                << "message " << i + 1
                << (is_past ? ", sent for a time past, did not leave at once"
                            : " left more than a millisecond after its time");
        }
    }
    if (is_scheduler_timed && !sorted.empty())
    {
        EXPECT_LE(sorted[sorted.size() / 2], 1000.0) << "the median message left more than a millisecond late";
    }
    if (is_close_judged)
    {
        EXPECT_EQ(after_close, 0) << "a message left after CloseAll";
    }
    else
    {
        std::printf(
            "The close is not judged: it began %.1f us after the first later message's time less a millisecond, "
            "the test thread woken late.\n",
            static_cast<double>(close_began - (first_later - 1'000'000)) * 1e-3);
    }
}
} // namespace
#endif

#if defined(RECKLESS_AUDIO_TEST_ALSA_SEQ)
namespace
{
/** A sequencer client of the test's own with one port that can be written to and subscribed to, and a thread that
    reads what reaches it, each message decoded to bytes and stamped with NowNanoseconds as it arrives, into a sink. */
class TestDestination
{
  public:
    explicit TestDestination(reckless::audio::MidiSink &sink) : sink_(sink)
    {
        if (snd_seq_open(&seq_, "default", SND_SEQ_OPEN_INPUT, SND_SEQ_NONBLOCK) < 0)
        {
            seq_ = nullptr;
            return;
        }
        snd_seq_set_client_name(seq_, "Reckless test destination");
        port_ = snd_seq_create_simple_port(seq_, "in", SND_SEQ_PORT_CAP_WRITE | SND_SEQ_PORT_CAP_SUBS_WRITE,
                                           SND_SEQ_PORT_TYPE_MIDI_GENERIC | SND_SEQ_PORT_TYPE_APPLICATION);
        if (port_ < 0 || snd_midi_event_new(64, &coder_) < 0)
        {
            return;
        }
        snd_midi_event_no_status(coder_, 1);
        reader_ = std::thread([this] { Read(); });
    }

    ~TestDestination()
    {
        stop_.store(true);
        if (reader_.joinable())
        {
            reader_.join();
        }
        if (coder_ != nullptr)
        {
            snd_midi_event_free(coder_);
        }
        if (seq_ != nullptr)
        {
            snd_seq_close(seq_);
        }
    }

    TestDestination(const TestDestination &)            = delete;
    TestDestination &operator=(const TestDestination &) = delete;

    bool made() const noexcept
    {
        return reader_.joinable();
    }

    /** The name the layer lists the port under: "<client>:<port> <client number>:<port number>". */
    std::string Name() const
    {
        return "Reckless test destination:in " + std::to_string(snd_seq_client_id(seq_)) + ":" + std::to_string(port_);
    }

  private:
    /** Until told to stop: poll a millisecond at a time, and every event that has come handed on as bytes. */
    void Read()
    {
        const int           count = snd_seq_poll_descriptors_count(seq_, POLLIN);
        std::vector<pollfd> descriptors(static_cast<std::size_t>(std::max(count, 0)));
        snd_seq_poll_descriptors(seq_, descriptors.data(), static_cast<unsigned int>(descriptors.size()), POLLIN);
        std::uint8_t bytes[64];
        while (!stop_.load())
        {
            if (snd_seq_event_input_pending(seq_, 1) <= 0)
            {
                poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()), 1);
                continue;
            }
            snd_seq_event_t *event = nullptr;
            if (snd_seq_event_input(seq_, &event) < 0 || event == nullptr)
            {
                continue;
            }
            const std::int64_t now  = NowNanoseconds();
            const long         size = snd_midi_event_decode(coder_, bytes, sizeof bytes, event);
            if (size > 0)
            {
                sink_.Arrived(event->source.port, now, bytes, static_cast<int>(size));
            }
        }
    }

    reckless::audio::MidiSink &sink_;
    snd_seq_t                 *seq_{nullptr};
    int                        port_{-1};
    snd_midi_event_t          *coder_{nullptr};
    std::atomic<bool>          stop_{false};
    std::thread                reader_;
};

/** The sequencer's queue clock against NowNanoseconds' clock: the steady clock at the queue's zero, the least of a few
    reads. */
std::int64_t TestQueueZero(snd_seq_t *seq, int queue)
{
    snd_seq_queue_status_t *status = nullptr;
    snd_seq_queue_status_alloca(&status);
    std::int64_t zero = std::numeric_limits<std::int64_t>::max();
    for (int i = 0; i < 8; ++i)
    {
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        const std::int64_t before = NowNanoseconds();
        snd_seq_get_queue_status(seq, queue, status);
        const std::int64_t         after = NowNanoseconds();
        const snd_seq_real_time_t *real  = snd_seq_queue_status_get_real_time(status);
        zero = std::min(zero, before + (after - before) / 2 -
                                  (static_cast<std::int64_t>(real->tv_sec) * 1000000000 + real->tv_nsec));
    }
    return zero;
}
} // namespace

TEST(MidiOutputs, LeaveThroughTheSequencerWithinAMillisecondOfTheirTimesInTheirOrder)
{
    // The sequencer's own loop, no hardware: a client of the test's own with a port that can be written to, found by
    // its name and opened as an output; messages sent latest first for times ahead, two for one time, and one for a
    // time already past; each read back as it arrives. Then messages for later, the output closed before their time,
    // and nothing arrives. Only the test's own port is ever connected to.
    Collected       arrived;
    TestDestination destination(arrived);
    const bool      required = std::find(RequiredApis().begin(), RequiredApis().end(), "ALSA") != RequiredApis().end();
    if (!destination.made())
    {
        if (required)
        {
            FAIL() << "ALSA is required (RECKLESS_AUDIO_REQUIRE) and its sequencer would not open: is snd-seq loaded?";
        }
        GTEST_SKIP() << "ALSA's sequencer would not open: no snd-seq in this kernel";
    }
    const auto                     midi    = reckless::audio::MakeMidiSystem("Reckless MIDI test");
    const std::vector<std::string> outputs = midi->Outputs();
    Table                          table("ALSA sequencer: the outputs");
    for (const std::string &output : outputs)
    {
        table.Row(output, "");
    }
    table.Print();
    ASSERT_NE(std::find(outputs.begin(), outputs.end(), destination.Name()), outputs.end())
        << "the test's port, listed as " << destination.Name();

    const std::uint8_t note[] = {0x90, 60, 100};
    EXPECT_FALSE(midi->OpenOutput("Reckless: no such MIDI output", 0));
    EXPECT_NE(midi->LastError().find("Reckless: no such MIDI output"), std::string::npos) << midi->LastError();
    EXPECT_FALSE(midi->OpenOutput(destination.Name(), reckless::audio::kMidiOutputPorts)) << "no such port number";
    EXPECT_FALSE(midi->Send(5, NowNanoseconds(), note, 3)) << "not open";

    // Found by its name with other numbers after it, as a device plugged in again comes back.
    ASSERT_TRUE(midi->OpenOutput("Reckless test destination:in 999:9", 5)) << midi->LastError();
    const std::vector<std::string> outputs_open = midi->Outputs();
    for (const std::string &output : outputs_open)
    {
        EXPECT_EQ(output.find("Reckless MIDI test"), std::string::npos) << "the system's own port listed: " << output;
    }
    const std::vector<std::string> inputs = midi->Inputs();
    for (const std::string &input : inputs)
    {
        EXPECT_EQ(input.find("Reckless MIDI test"), std::string::npos) << "the system's own port listed: " << input;
    }

    SendAndReadBack(*midi, 5, arrived, "ALSA sequencer", false); // never a hosted runner: CI's Linux has no sequencer
}

TEST(MidiOutputs, TheSequencersQueueClockKeepsToTheSteadyClock)
{
    // What the ALSA output's one measurement of the clocks at its open rests on: a queue of the test's own, its zero on
    // the steady clock measured at the start and again RECKLESS_MIDI_CLOCK_SECONDS later (3 by default; 60 was
    // measured by hand).
    snd_seq_t *seq = nullptr;
    if (snd_seq_open(&seq, "default", SND_SEQ_OPEN_OUTPUT, 0) < 0)
    {
        if (std::find(RequiredApis().begin(), RequiredApis().end(), "ALSA") != RequiredApis().end())
        {
            FAIL() << "ALSA is required (RECKLESS_AUDIO_REQUIRE) and its sequencer would not open: is snd-seq loaded?";
        }
        GTEST_SKIP() << "ALSA's sequencer would not open: no snd-seq in this kernel";
    }
    snd_seq_set_client_name(seq, "Reckless test clock");
    const int queue = snd_seq_alloc_queue(seq);
    ASSERT_GE(queue, 0);
    snd_seq_start_queue(seq, queue, nullptr);
    snd_seq_drain_output(seq);
    const std::string  asked   = Environment("RECKLESS_MIDI_CLOCK_SECONDS");
    const int          seconds = asked.empty() ? 3 : std::max(1, std::atoi(asked.c_str()));
    const std::int64_t start   = TestQueueZero(seq, queue);
    std::this_thread::sleep_for(std::chrono::seconds(seconds));
    const std::int64_t end = TestQueueZero(seq, queue);
    snd_seq_free_queue(seq, queue);
    snd_seq_close(seq);
    const double moved = static_cast<double>(end - start) * 1e-3;
    Table("ALSA sequencer: its queue clock against the steady clock")
        .Row("over", Str(seconds) + " s")
        .Row("the queue's zero moved", Fixed(moved, 1) + " us")
        .Print();
    EXPECT_LE(std::abs(moved), 100.0) << "the queue's clock drifts from the steady clock";
}
#endif

#if defined(__APPLE__)
TEST(MidiInputs, CarryWhatACoreMidiSourceSendsButWhatTheCoresIgnore)
{
    // A virtual source of the test's own on MIDI 1.0's protocol: listed by its name (a source with no entity is named
    // by its endpoint alone), opened, and sent Universal MIDI Packets - a note on and off, a controller and a pitch
    // bend, and between them the kinds the cores do not read.
    const std::string                        name = "Reckless test source";
    reckless::audio::test::PrivateMidiSource source(name);
    if (!source.made())
    {
        if (std::find(RequiredApis().begin(), RequiredApis().end(), "CoreAudio") != RequiredApis().end())
        {
            FAIL() << "CoreAudio is required and CoreMIDI would not make a source: " << source.why();
        }
        GTEST_SKIP() << "CoreMIDI would not make a source: " << source.why();
    }
    const auto               midi = reckless::audio::MakeMidiSystem("Reckless MIDI test");
    std::vector<std::string> inputs;
    for (int i = 0; i < 100 && std::find(inputs.begin(), inputs.end(), name) == inputs.end(); ++i)
    {
        inputs = midi->Inputs(); // a source made is listed at once, but give CoreMIDI its moment
        if (std::find(inputs.begin(), inputs.end(), name) == inputs.end())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    Table table("CoreMIDI: the inputs");
    for (const std::string &input : inputs)
    {
        table.Row(input, "");
    }
    table.Row("library", reckless::audio::AudioLibraryVersions()).Print();
    ASSERT_NE(std::find(inputs.begin(), inputs.end(), name), inputs.end()) << "the test's source, listed as " << name;

    Collected nothing;
    EXPECT_FALSE(midi->Open("Reckless: no such MIDI input", 0, nothing));
    EXPECT_NE(midi->LastError().find("Reckless: no such MIDI input"), std::string::npos) << midi->LastError();

    Collected sink;
    ASSERT_TRUE(midi->Open(name, 3, sink)) << midi->LastError();
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // the connection made before anything is sent
    const std::int64_t                            before  = NowNanoseconds();
    const std::vector<std::vector<std::uint32_t>> packets = {
        {0x20913C64u},              // note on, channel 2
        {0x30037D01u, 0x02000000u}, // a system exclusive, left out
        {0x10F80000u},              // timing clock, left out
        {0x20B00740u},              // a controller
        {0x10FE0000u},              // active sensing, left out
        {0x20813C00u},              // note off
        {0x20EF0040u},              // a pitch bend, channel 16
    };
    for (const auto &packet : packets)
    {
        source.Send(packet.data(), static_cast<int>(packet.size()));
    }
    const bool         came  = sink.AwaitCount(4);
    const std::int64_t after = NowNanoseconds();
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // anything left out would have come by now
    midi->CloseAll();
    const int           counted = sink.count();
    const std::uint32_t late[]  = {0x20913D64u};
    source.Send(late, 1);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    const std::vector<std::vector<std::uint8_t>> want = {
        {0x91, 60, 100}, {0xB0, 7, 64}, {0x81, 60, 0}, {0xEF, 0x00, 0x40}};
    Table result("CoreMIDI: what arrived");
    for (int i = 0; i < std::min(counted, Collected::kMost); ++i)
    {
        std::string bytes;
        for (const std::uint8_t b : sink.message_at(i))
        {
            char hex[4];
            std::snprintf(hex, sizeof hex, "%02X ", b);
            bytes += hex;
        }
        result.Row("message " + Str(i + 1), bytes + "port " + Str(sink.port_at(i)));
    }
    result.Row("after the inputs closed", Str(sink.count() - counted) + " messages").Print();

    ASSERT_TRUE(came) << "the messages did not all arrive";
    ASSERT_EQ(counted, static_cast<int>(want.size()))
        << "system exclusive, timing clock and active sensing are left out, and nothing else";
    for (int i = 0; i < counted; ++i)
    {
        EXPECT_EQ(sink.message_at(i), want[static_cast<std::size_t>(i)]) << "message " << i + 1;
        EXPECT_EQ(sink.port_at(i), 3);
        EXPECT_GE(sink.time_at(i), before);
        EXPECT_LE(sink.time_at(i), after);
    }
    EXPECT_EQ(sink.count(), counted) << "a message reached the sink after CloseAll";
    EXPECT_EQ(nothing.count(), 0);
}

TEST(MidiOutputs, LeaveThroughCoreMidiWithinAMillisecondOfTheirTimesInTheirOrder)
{
    // A virtual destination of the test's own on MIDI 1.0's protocol: listed by its name, opened as an output, and sent
    // stamped messages, each judged by the time it is due at the destination (DeviceRig.h, PrivateMidiDestination).
    // Only the test's own destination is ever sent to.
    const std::string                             name = "Reckless test destination";
    Collected                                     arrived;
    reckless::audio::test::PrivateMidiDestination destination(name, arrived);
    if (!destination.made())
    {
        if (std::find(RequiredApis().begin(), RequiredApis().end(), "CoreAudio") != RequiredApis().end())
        {
            FAIL() << "CoreAudio is required and CoreMIDI would not make a destination: " << destination.why();
        }
        GTEST_SKIP() << "CoreMIDI would not make a destination: " << destination.why();
    }
    const auto               midi = reckless::audio::MakeMidiSystem("Reckless MIDI test");
    std::vector<std::string> outputs;
    for (int i = 0; i < 100 && std::find(outputs.begin(), outputs.end(), name) == outputs.end(); ++i)
    {
        outputs = midi->Outputs(); // a destination made is listed at once, but give CoreMIDI its moment
        if (std::find(outputs.begin(), outputs.end(), name) == outputs.end())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    Table table("CoreMIDI: the outputs");
    for (const std::string &output : outputs)
    {
        table.Row(output, "");
    }
    table.Print();
    ASSERT_NE(std::find(outputs.begin(), outputs.end(), name), outputs.end()) << "the test's destination, as " << name;
    EXPECT_FALSE(midi->OpenOutput("Reckless: no such MIDI output", 0));
    EXPECT_NE(midi->LastError().find("Reckless: no such MIDI output"), std::string::npos) << midi->LastError();
    ASSERT_TRUE(midi->OpenOutput(name, 5)) << midi->LastError();
    // Scheduler-timed as the suite decides it for CoreAudio's test device: a hosted runner's virtual machine.
    const auto audio              = reckless::audio::MakeAudioSystem("CoreAudio");
    const bool is_scheduler_timed = audio != nullptr && MakeRig("CoreAudio")->Find(audio->Devices()).is_scheduler_timed;
    SendAndReadBack(*midi, 5, arrived, "CoreMIDI", is_scheduler_timed);
}
#endif

TEST(MidiOutputs, RefuseANameThatIsNowhereAPortOutOfRangeAndAPortNotOpen)
{
    // On every platform's back end, with nothing opened: the outputs listed (never opened), a name that is nowhere
    // refused with the error naming it, a port number past the table refused, and a Send to a port not open refused.
    const auto midi = reckless::audio::MakeMidiSystem("Reckless MIDI test");
    ASSERT_NE(midi, nullptr);
    Table table("MIDI: the outputs");
    for (const std::string &output : midi->Outputs())
    {
        table.Row(output, "");
    }
    table.Print();
    EXPECT_FALSE(midi->OpenOutput("Reckless: no such MIDI output", 0));
    EXPECT_NE(midi->LastError().find("Reckless: no such MIDI output"), std::string::npos) << midi->LastError();
    EXPECT_FALSE(midi->OpenOutput("Reckless: no such MIDI output", reckless::audio::kMidiOutputPorts));
    EXPECT_FALSE(midi->OpenOutput("Reckless: no such MIDI output", -1));
    const std::uint8_t note[] = {0x90, 60, 100};
    for (int port = 0; port < reckless::audio::kMidiOutputPorts; ++port)
    {
        EXPECT_FALSE(midi->Send(port, NowNanoseconds(), note, 3)) << "port " << port << " is not open";
    }
    EXPECT_FALSE(midi->Send(-1, NowNanoseconds(), note, 3));
    midi->CloseAll();
}

TEST(MidiInputs, CarryWhatAPersonPlaysOnAKeyboard)
{
    // A real keyboard, played by a person at the machine: the input RECKLESS_MIDI_KEYBOARD names, opened by that name
    // as a settings file keeps it, and what the person plays - notes held and let go, a wheel, a slider - arriving as
    // whole MIDI 1.0 messages in the order played, on the index the input was opened under. No CI runner has a
    // keyboard or a person, so it runs only where the variable is set.
    const std::string name = Environment("RECKLESS_MIDI_KEYBOARD");
    if (name.empty())
    {
        GTEST_SKIP() << "RECKLESS_MIDI_KEYBOARD names no keyboard: this test is a person playing one";
    }
    const auto midi = reckless::audio::MakeMidiSystem("Reckless MIDI test");
    ASSERT_NE(midi, nullptr);
    Table inputs("MIDI: the inputs");
    for (const std::string &input : midi->Inputs())
    {
        inputs.Row(input, input == name ? "the keyboard" : "");
    }
    inputs.Print();
    Collected sink;
    ASSERT_TRUE(midi->Open(name, 1, sink)) << midi->LastError();
    constexpr int kSeconds = 20;
    std::fprintf(stderr, "\n  >>> Play a few notes on '%s', then move a wheel or a slider: %d s <<<\n\n", name.c_str(),
                 kSeconds);
    std::fflush(stderr);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(kSeconds);
    while (std::chrono::steady_clock::now() < until && sink.count() < Collected::kMost)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    midi->CloseAll();
    const int counted = std::min(sink.count(), Collected::kMost);

    // Each message read as what it is, the notes held paired with their release.
    int                notes_on = 0, notes_released = 0, controls = 0;
    std::vector<int>   held(16 * 128, 0);
    Table              played("MIDI: what the keyboard sent (time since the one before, bytes, what it is)");
    const std::int64_t first = counted > 0 ? sink.time_at(0) : 0;
    for (int i = 0; i < counted; ++i)
    {
        const std::vector<std::uint8_t> &bytes = sink.message_at(i);
        std::string                      hex;
        for (const std::uint8_t b : bytes)
        {
            char text[4];
            std::snprintf(text, sizeof text, "%02X ", b);
            hex += text;
        }
        const int   kind    = bytes.empty() ? 0 : bytes[0] & 0xF0;
        const int   channel = bytes.empty() ? 0 : bytes[0] & 0x0F;
        std::string what    = "other";
        if (bytes.size() == 3 && (kind == 0x90 || kind == 0x80))
        {
            const bool is_on = kind == 0x90 && bytes[2] > 0;
            int       &note  = held[static_cast<std::size_t>(channel * 128 + bytes[1])];
            if (is_on)
            {
                ++notes_on;
                ++note;
            }
            else if (note > 0)
            {
                ++notes_released;
                --note;
            }
            what = std::string(is_on ? "note on " : "note off ") + Str(static_cast<std::int64_t>(bytes[1]));
        }
        else if (kind == 0xB0 || kind == 0xE0 || kind == 0xD0 || kind == 0xA0)
        {
            ++controls;
            what = kind == 0xB0   ? "controller " + Str(static_cast<std::int64_t>(bytes.size() > 1 ? bytes[1] : 0))
                   : kind == 0xE0 ? std::string("pitch bend")
                                  : std::string("pressure");
        }
        const std::int64_t since = i == 0 ? 0 : sink.time_at(i) - sink.time_at(i - 1);
        played.Row("+" + Fixed(1e-6 * static_cast<double>(since), 1) + " ms", hex + " " + what);
    }
    played.Row("over", Fixed(1e-9 * static_cast<double>(counted > 0 ? sink.time_at(counted - 1) - first : 0), 2) + " s")
        .Row("notes played / let go", Str(notes_on) + " / " + Str(notes_released))
        .Row("wheels, sliders, pressure", Str(controls))
        .Print();

    ASSERT_GT(counted, 0) << "nothing arrived from '" << name << "' - was it played?";
    EXPECT_GT(notes_on, 0) << "no note was played";
    EXPECT_GT(notes_released, 0) << "no note played was let go";
    for (int i = 0; i < counted; ++i)
    {
        const std::vector<std::uint8_t> &bytes = sink.message_at(i);
        ASSERT_FALSE(bytes.empty());
        EXPECT_GE(bytes[0], 0x80) << "message " << i + 1 << " does not begin with its status";
        for (std::size_t b = 1; b < bytes.size(); ++b)
        {
            EXPECT_LT(bytes[b], 0x80) << "message " << i + 1 << " carries a status where its data belongs";
        }
        EXPECT_EQ(sink.port_at(i), 1);
        if (i > 0)
        {
            EXPECT_GE(sink.time_at(i), sink.time_at(i - 1)) << "stamped in the order they came";
        }
    }
}

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(AudioDevices);
INSTANTIATE_TEST_SUITE_P(Apis, AudioDevices, ::testing::ValuesIn(TestedApis()), ApiTestName);

int main(int argc, char **argv)
{
    return RunDeviceSuite(argc, argv);
}
