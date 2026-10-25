// The audio layer on the real APIs (audio/DeviceBackends.h, whichever back end serves each API): for every API
// the build carries - CoreAudio, Windows Audio (WASAPI, in its shared, low-latency and exclusive modes), ASIO, JACK,
// ALSA - its device list, a stream run at a rate the
// device offers with its callbacks counted against what the rate and the block predict and the rate read by the clock,
// the samples a loop carries back (bit for bit through ALSA's loopback card and a JACK client wired to itself,
// correlated through WASAPI's loopback capture, the ASIO test driver's own sine and sawtooth exactly), an xrun the
// device reports, and a device that is not there; and on Linux, MIDI from a sequencer port of the test's own through
// ALSA's sequencer back end. Each API's test device is deterministic and set up by the suite or
// the CI job (devices/DeviceRig.h says which); the tables each test judges are printed. The harness is
// devices/DeviceSuite.h, which a consumer's suite of an engine playing through the same devices shares.
//
// Covered means it cannot skip silently. A missing test device skips its tests with the reason printed - unless the API
// is named in RECKLESS_AUDIO_REQUIRE (a comma-separated list of the series' API names, which each CI job sets for its
// runner's APIs), when it fails, naming what is missing. And the build holds itself to its configure: AudioApis() must
// be exactly the APIs the configure promised (RECKLESS_AUDIO_PROMISED_APIS - ASIO wherever the SDK is, JACK and ALSA
// wherever their headers are), so a build that silently loses one fails here.
#include "devices/DeviceSuite.h"

#if defined(RECKLESS_AUDIO_TEST_ALSA_SEQ)
#include <alsa/asoundlib.h>
#endif

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

    // A second probe lists the same devices.
    const std::vector<DeviceInfo> again = system_->Devices();
    ASSERT_EQ(again.size(), devices.size());
    for (std::size_t i = 0; i < devices.size(); ++i)
    {
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
    RunFor(1.2);
    system_->Stop();
    system_->Close();

    const std::size_t  recorded = recorder.recorded_frames();
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
        // noise played, then the correlation coefficient over everything after it.
        const auto        played = [](std::int64_t n) { return static_cast<double>(NoiseSample(n, 0)); };
        const std::size_t window = 4096, start = static_cast<std::size_t>(0.25 * rate);
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
        if (cable)
        {
            EXPECT_GE(fit0, 0.99) << "the first channel back is not what was played, filtered - is the cable patched "
                                     "as RECKLESS_COREAUDIO_LOOP says, and the input's gain up but not clipping?";
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
            EXPECT_LE(std::llabs(static_cast<std::int64_t>(lag) - predicted), static_cast<std::int64_t>(rate / 64))
                << "the latency the API reports is not the round trip a cable measures";
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
        EXPECT_EQ(format.reported_latency, (periods - 1) * format.block_size) << periods << " periods";
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
#endif

TEST(MidiInputs, CarryWhatAPersonPlaysOnAKeyboard)
{
    // A real keyboard, played by a person at the machine: the input RECKLESS_MIDI_KEYBOARD names, opened by that name
    // as a settings file keeps it, and what the person plays - notes held and let go, a wheel, a slider - arriving as
    // whole MIDI 1.0 messages in the order played, on the index the input was opened under. No CI runner has a
    // keyboard or a person, so it runs only where the variable is set.
    const std::string name = Trim(Environment("RECKLESS_MIDI_KEYBOARD"));
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
