// The audio layer on the real APIs (audio/RtAudioDevice.cpp, the suite's AudioSystem on RtAudio): for every API
// the build carries - CoreAudio, Windows Audio (WASAPI, in its shared, low-latency and exclusive modes), ASIO, JACK,
// ALSA - its device list, a stream run at a rate the
// device offers with its callbacks counted against what the rate and the block predict and the rate read by the clock,
// the samples a loop carries back (bit for bit through ALSA's loopback card and a JACK client wired to itself,
// correlated through WASAPI's loopback capture, the ASIO test driver's own sine and sawtooth exactly), an xrun the
// device reports, and a device that is not there. Each API's test device is deterministic and set up by the suite or
// the CI job (devices/DeviceRig.h says which); the tables each test judges are printed. The harness is
// devices/DeviceSuite.h, which a consumer's suite of an engine playing through the same devices shares.
//
// Covered means it cannot skip silently. A missing test device skips its tests with the reason printed - unless the API
// is named in RECKLESS_AUDIO_REQUIRE (a comma-separated list of the series' API names, which each CI job sets for its
// runner's APIs), when it fails, naming what is missing. And the build holds itself to its configure: AudioApis() must
// be exactly the APIs the configure promised (RECKLESS_AUDIO_PROMISED_APIS - ASIO wherever the SDK is, JACK and ALSA
// wherever their headers are), so a build that silently loses one fails here.
#include "devices/DeviceSuite.h"

using namespace reckless::audio::test::suite;

// ---- The build's APIs ----------------------------------------------------------------------------------------------

TEST(AudioApis, AreTheOnesTheConfigurePromised)
{
    const std::vector<std::string> apis = reckless::audio::AudioApis();
    Table("the build's audio APIs")
        .Row("RtAudio carries", JoinList(apis))
        .Row("the configure promised", JoinList(PromisedApis()))
        .Row("the run requires", JoinList(RequiredApis()))
        .Row("libraries", reckless::audio::AudioLibraryVersions())
        .Print();

    // A build that loses an API its configure found the means for - the ASIO SDK, JACK's or ALSA's headers - fails.
    EXPECT_EQ(apis, PromisedApis());

    // Every API the run requires is one of the series', and one this build carries.
    const std::vector<std::string> series = {
        "CoreAudio", "Windows Audio", "Windows Audio (Low Latency Mode)", "Windows Audio (Exclusive Mode)", "ASIO",
        "JACK",      "ALSA"};
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
    const TestDevice device = rig_->Find(devices);
    RECKLESS_NEED_TEST_DEVICE(device);
    Table(api_ + ": the test device")
        .Row("what it is", device.about)
        .Row("output", device.output)
        .Row("input", device.input.empty() ? std::string("none") : device.input)
        .Row("channels each way", Str(device.channels))
        .Row("rate, block", Str(device.sample_rate) + " Hz, " + Str(device.block_size))
        .Print();
    const int out = reckless::audio::FindDevice(devices, device.output, reckless::audio::Direction::kOutput);
    ASSERT_GE(out, 0);
    EXPECT_GE(devices[static_cast<std::size_t>(out)].output_channels, device.channels);
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
    // past ASCII too. RtAudio reads CoreAudio's names into UTF-8 only with UNICODE defined (the root CMakeLists), and
    // otherwise in the system's encoding: Mac OS Roman's bytes, which are not UTF-8, which a setting of the device kept
    // in UTF-8 never matches, and which the Standalone's settings would draw. RtAudio lists the device with its maker
    // before its name, so it is found by the name it ends in.
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
    ASSERT_TRUE(system_->Open(request, recorder)) << system_->LastError();
    ASSERT_TRUE(system_->IsOpen());
    const StreamFormat format = system_->Format();

    const std::int64_t started = NowNanoseconds();
    ASSERT_TRUE(system_->Start()) << system_->LastError();
    RunFor(1.2);
    const std::int64_t stopped = NowNanoseconds();
    system_->Stop();
    const std::int64_t after_stop = recorder.callbacks();
    RunFor(0.1);
    const std::int64_t later = recorder.callbacks();
    system_->Close();

    const Timing       timing    = Measure(recorder, started, stopped, format.sample_rate, format.block_size);
    const double       slack     = device.rate_tolerance * timing.predicted + 2.0;
    const std::int64_t allowance = XrunAllowance(timing.callbacks) + LoadAllowance(device, timing.callbacks);
    Table(api_ + ": a stream on " + device.output)
        .Row("rate asked / opened", Str(request.sample_rate) + " / " + Str(format.sample_rate) + " Hz")
        .Row("block asked / opened", Str(request.block_size) + " / " + Str(format.block_size))
        .Row("channels in / out", Str(format.input_channels) + " / " + Str(format.output_channels))
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

    // Every block carried the channels it opened, laid out as RtAudioDevice promises, on the API's own thread.
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
    const Recorder::Play play    = device.loop == Loop::kExact        ? Recorder::Play::kIndices
                                   : device.loop == Loop::kCorrelated ? Recorder::Play::kNoise
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
    else if (device.loop == Loop::kCorrelated)
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
        Table(api_ + ": the loop through the mixer")
            .Row("loopback delay", Str(static_cast<std::int64_t>(lag)) + " samples (" +
                                       Fixed(1e3 * static_cast<double>(lag) / rate, 2) + " ms)")
            .Row("correlation", Fixed(r, 4) + " (at least 0.9)")
            .Row("gain", yy > 0.0 ? Fixed(10.0 * std::log10(yy / xx), 2) + " dB" : std::string("silence"))
            .Row("xruns reported", Str(xruns))
            .Print();
        EXPECT_GE(r, 0.9) << "the loopback capture does not carry what the stream played - is the endpoint muted, "
                             "or did another sound play over it?";
    }
    else
    {
        // The ASIO test driver's inputs: its sine on the first, its sawtooth on the second, every block the same 256
        // 16-bit samples (AsioRig.cpp makes them as the driver does), each read as int16 / 32768 exactly. RtAudio hands
        // a callback the input of the block before - its own buffer, filled after the callback runs - so the first
        // callback's input is silence: one block of it, and no more, before the driver's samples.
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
            .Row("silent before the driver's samples", Str(static_cast<std::int64_t>(lead)) + " frames (one block: " +
                                                           Str(static_cast<std::int64_t>(format.block_size)) + ")")
            .Row("samples compared, exactly", Str(compared) + " over 2 channels")
            .Row("mismatches",
                 Str(mismatches) + (mismatches > 0
                                        ? " (first at frame " + Str(static_cast<std::int64_t>(first_mismatch)) + ")"
                                        : std::string()))
            .Row("xruns reported", Str(xruns))
            .Print();
        EXPECT_EQ(lead, static_cast<std::size_t>(format.block_size)) << "the driver's samples did not start a block in";
        EXPECT_EQ(mismatches, 0) << "an input sample is not the driver's sine or sawtooth read as int16 / 32768";
        EXPECT_GE(compared, static_cast<std::int64_t>(rate)); // two channels of at least half a second
    }
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

GTEST_ALLOW_UNINSTANTIATED_PARAMETERIZED_TEST(AudioDevices);
INSTANTIATE_TEST_SUITE_P(Apis, AudioDevices, ::testing::ValuesIn(TestedApis()), ApiTestName);

int main(int argc, char **argv)
{
    return RunDeviceSuite(argc, argv);
}
