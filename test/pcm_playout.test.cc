#include "api/make_ref_counted.h"
#include "audio/audio_state.h"
#include "call/test/mock_audio_receive_stream.h"
#include "modules/audio_device/include/mock_audio_device.h"
#include "modules/audio_mixer/audio_mixer_impl.h"
#include "modules/audio_processing/include/mock_audio_processing.h"
#include "src/internal/pcm_playout_source.h"
#include "test/gmock.h"
#include "test/gtest.h"
#include "test/time_controller/simulated_time_controller.h"

namespace libwebrtc {
namespace {
using namespace webrtc;
using namespace webrtc::test;
using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;
std::vector<uint8_t> Pcm(size_t frames, int16_t sample = 1000) {
  std::vector<uint8_t> bytes(frames * 2);
  for (size_t i = 0; i < frames; ++i) {
    bytes[2 * i] = sample & 255;
    bytes[2 * i + 1] = (sample >> 8) & 255;
  }
  return bytes;
}
TEST(PcmPlayout, BoundedWritesAreAtomicAndEpochRejectsStaleAudio) {
  PcmPlayoutSource source;
  auto generation = source.Start();
  EXPECT_GT(generation, 0);
  EXPECT_LT(source.Start(), 0);
  const auto bytes = Pcm(24000);
  for (int i = 0; i < 5; ++i)
    EXPECT_EQ(source.Write(generation, 0, bytes.data(), bytes.size()), 0);
  EXPECT_EQ(source.Write(generation, 0, bytes.data(), bytes.size()), -4);
  EXPECT_EQ(source.State().queued_frames, 120000);
  EXPECT_EQ(source.Clear(generation, 1), 0);
  EXPECT_EQ(source.State().discarded_frames, 120000);
  EXPECT_EQ(source.Write(generation, 0, bytes.data(), bytes.size()), -2);
  EXPECT_EQ(source.Write(generation, 1, bytes.data(), 1), -3);
  EXPECT_EQ(source.Write(generation, 1, bytes.data(), bytes.size()), 0);
  source.Stop();
  const auto next = source.Start();
  EXPECT_GT(next, generation);
  EXPECT_EQ(source.Clear(generation, 2), -2);
  EXPECT_EQ(source.Write(generation, 1, bytes.data(), bytes.size()), -2);
}
TEST(PcmPlayout, QuiescentOwnerRetainsGenerationButRejectsNewWrites) {
  PcmPlayoutSource source;
  const auto generation = source.Start();
  auto bytes = Pcm(240);
  ASSERT_EQ(source.Write(generation, 0, bytes.data(), bytes.size()), 0);
  ASSERT_EQ(source.Quiesce(generation), 0);
  EXPECT_EQ(source.State().generation, generation);
  EXPECT_EQ(source.State().queued_frames, 0);
  EXPECT_EQ(source.Write(generation, 0, bytes.data(), bytes.size()), -2);
  EXPECT_EQ(source.Clear(generation, 1), -2);
  EXPECT_EQ(source.Quiesce(generation), 0);
  source.Stop();
}
TEST(PcmPlayout, ResamplesMonoIntoMixerAndClearRemovesFilterTail) {
  PcmPlayoutSource source;
  const auto generation = source.Start();
  auto bytes = Pcm(2400);
  source.Write(generation, 0, bytes.data(), bytes.size());
  AudioFrame frame;
  for (int i = 0; i < 3; ++i)
    EXPECT_EQ(source.GetAudioFrameWithInfo(48000, &frame),
              AudioMixer::Source::AudioFrameInfo::kNormal);
  EXPECT_EQ(frame.samples_per_channel_, 480u);
  EXPECT_NEAR(frame.data()[300], 1000, 10);
  EXPECT_EQ(source.State().consumed_frames, 720);
  source.Clear(generation, 1);
  source.GetAudioFrameWithInfo(48000, &frame);
  EXPECT_TRUE(std::all_of(frame.data(), frame.data() + 480,
                          [](int16_t x) { return x == 0; }));
}
struct Fixture {
  webrtc::scoped_refptr<NiceMock<MockAudioDeviceModule>> adm =
      make_ref_counted<NiceMock<MockAudioDeviceModule>>();
  webrtc::scoped_refptr<NiceMock<MockAudioProcessing>> apm =
      make_ref_counted<NiceMock<MockAudioProcessing>>();
  webrtc::scoped_refptr<AudioMixer> mixer = AudioMixerImpl::Create();
  webrtc::scoped_refptr<webrtc::internal::AudioState> state;
  bool playing = false;
  PcmPlayoutSource source;
  Fixture() {
    AudioState::Config config;
    config.audio_mixer = mixer;
    config.audio_device_module = adm;
    config.audio_processing = apm;
    state = make_ref_counted<webrtc::internal::AudioState>(config);
    ON_CALL(*adm, Playing()).WillByDefault([this] { return playing; });
    ON_CALL(*adm, StartPlayout()).WillByDefault([this] {
      playing = true;
      return 0;
    });
    ON_CALL(*adm, StopPlayout()).WillByDefault([this] {
      playing = false;
      return 0;
    });
    source.Start();
  }
};
TEST(PcmPlayout, PeerlessDeviceLifetimeAndCaptureUntouched) {
  Fixture f;
  EXPECT_CALL(*f.adm, InitRecording()).Times(0);
  EXPECT_CALL(*f.adm, StartRecording()).Times(0);
  EXPECT_CALL(*f.adm, StopRecording()).Times(0);
  EXPECT_CALL(*f.adm, InitPlayout()).Times(1);
  EXPECT_CALL(*f.adm, StartPlayout()).Times(1);
  EXPECT_CALL(*f.adm, StopPlayout()).Times(1);
  ASSERT_EQ(f.state->AddExternalPlayoutSource(&f.source), 0);
  EXPECT_TRUE(f.playing);
  ASSERT_EQ(f.state->RemoveExternalPlayoutSource(&f.source), 0);
  EXPECT_FALSE(f.playing);
}
TEST(PcmPlayout, ReceiverAndPcmTeardownDoNotStopEachOther) {
  Fixture f;
  NiceMock<MockAudioReceiveStream> receiver;
  f.state->AddReceivingStream(&receiver);
  ASSERT_EQ(f.state->AddExternalPlayoutSource(&f.source), 0);
  EXPECT_CALL(*f.adm, StopPlayout()).Times(0);
  f.state->RemoveReceivingStream(&receiver);
  EXPECT_TRUE(f.playing);
  f.state->AddReceivingStream(&receiver);
  f.state->RemoveExternalPlayoutSource(&f.source);
  EXPECT_TRUE(f.playing);
  ::testing::Mock::VerifyAndClearExpectations(f.adm.get());
  EXPECT_CALL(*f.adm, StopPlayout()).Times(1);
  f.state->RemoveReceivingStream(&receiver);
}
TEST(PcmPlayout, FailedStartDetachesSourceAndFailedStopAllowsRetry) {
  Fixture f;
  EXPECT_CALL(*f.adm, StartPlayout()).WillOnce(Return(-1));
  EXPECT_NE(f.state->AddExternalPlayoutSource(&f.source), 0);
  ::testing::Mock::VerifyAndClearExpectations(f.adm.get());
  ASSERT_EQ(f.state->AddExternalPlayoutSource(&f.source), 0);
  EXPECT_CALL(*f.adm, StopPlayout()).WillOnce(Return(-1));
  EXPECT_NE(f.state->RemoveExternalPlayoutSource(&f.source), 0);
  ::testing::Mock::VerifyAndClearExpectations(f.adm.get());
  EXPECT_EQ(f.state->RemoveExternalPlayoutSource(&f.source), 0);
}
TEST(PcmPlayout, DeviceAndApmReceiveTheSameAppRenderMix) {
  Fixture f;
  f.state->AddExternalPlayoutSource(&f.source);
  const auto bytes = Pcm(2400);
  f.source.Write(f.source.State().generation, 0, bytes.data(), bytes.size());
  bool saw_voice = false;
  EXPECT_CALL(*f.apm, ProcessReverseStream(::testing::A<const int16_t*>(), _, _,
                                           ::testing::A<int16_t*>()))
      .WillRepeatedly([&](const int16_t* src, const StreamConfig& in,
                          const StreamConfig&, int16_t* dest) {
        EXPECT_EQ(in.num_channels(), 1u);
        for (size_t i = 0; i < in.num_frames(); ++i)
          if (std::abs(src[i]) > 100) saw_voice = true;
        EXPECT_EQ(src, dest);
        return 0;
      });
  int16_t pcm[480];
  size_t count;
  int64_t elapsed, ntp;
  for (int i = 0; i < 4; ++i)
    f.state->audio_transport()->NeedMorePlayData(480, 2, 1, 48000, pcm, count,
                                                 &elapsed, &ntp);
  EXPECT_TRUE(saw_voice);
  EXPECT_EQ(count, 480u);
  EXPECT_NEAR(pcm[300], 1000, 10);
  f.state->RemoveExternalPlayoutSource(&f.source);
}
TEST(PcmPlayout, NullPollerDoesNotConsumeSuspendedPcm) {
  GlobalSimulatedTimeController time(Timestamp::Seconds(1));
  time.GetMainThread()->BlockingCall([&] {
    Fixture f;
    NiceMock<MockAudioReceiveStream> receiver;
    ON_CALL(receiver, PreferredSampleRate()).WillByDefault(Return(48000));
    ON_CALL(receiver, GetAudioFrameWithInfo(_, _))
        .WillByDefault([](int rate, AudioFrame* frame) {
          frame->UpdateFrame(0, nullptr, rate / 100, rate,
                             AudioFrame::kNormalSpeech, AudioFrame::kVadUnknown,
                             1);
          return AudioMixer::Source::AudioFrameInfo::kMuted;
        });
    f.state->AddReceivingStream(&receiver);
    f.state->AddExternalPlayoutSource(&f.source);
    auto bytes = Pcm(2400);
    f.source.Write(f.source.State().generation, 0, bytes.data(), bytes.size());
    f.state->SetPlayout(false);
    time.AdvanceTime(TimeDelta::Millis(100));
    EXPECT_EQ(f.source.State().queued_frames, 2400);
    f.state->SetPlayout(true);
    EXPECT_TRUE(f.playing);
    f.state->RemoveExternalPlayoutSource(&f.source);
    f.state->RemoveReceivingStream(&receiver);
  });
}
}  // namespace
}  // namespace libwebrtc
