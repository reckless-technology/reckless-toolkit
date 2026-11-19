// What a stream opened as (audio/AudioDevice.h's StreamFormat): the latency beyond the block summed, and told apart
// by side. This suite holds the offline device and the struct's own promises; each API's own split is held, on the
// device, by the device suite (test_audio_devices.cpp) - and Windows Audio's, ASIO's and CoreAudio's by CI alone.

#include <cstdint>

#include <gtest/gtest.h>

#include "AudioDevice.h"
#include "OfflineDevice.h"

namespace
{
using reckless::audio::AudioCallback;
using reckless::audio::OfflineAudioSystem;
using reckless::audio::StreamFormat;
using reckless::audio::StreamRequest;

/// A callback that does nothing.
class QuietCallback final : public AudioCallback
{
  public:
    void Process(const float *const *, int, float *const *, int, int, std::int64_t, bool) noexcept override
    {
    }

    void StreamFailed() noexcept override
    {
    }
};
} // namespace

TEST(StreamFormat, ToldNothingItReportsNoLatencyOnEitherSideAndTheSumAsItWas)
{
    const StreamFormat format;
    EXPECT_EQ(format.reported_latency, 0);
    EXPECT_EQ(format.input_latency_frames, 0);
    EXPECT_EQ(format.output_latency_frames, 0);
}

TEST(StreamFormat, AnOffLineStreamHasNoLatencyOnEitherSideAndTheTwoAddUpToTheSum)
{
    OfflineAudioSystem system(2, 2);
    QuietCallback      callback;
    StreamRequest      request;
    request.input_device  = "Offline";
    request.output_device = "Offline";
    request.input_count   = 2;
    request.output_count  = 2;
    ASSERT_TRUE(system.Open(request, callback)) << system.LastError();
    const StreamFormat format = system.Format();
    EXPECT_EQ(format.reported_latency, 0);
    EXPECT_EQ(format.input_latency_frames, 0);
    EXPECT_EQ(format.output_latency_frames, 0);
    EXPECT_EQ(format.reported_latency, format.input_latency_frames + format.output_latency_frames);
    system.Close();
}
