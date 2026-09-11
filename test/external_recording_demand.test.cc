#include "api/make_ref_counted.h"
#include "audio/audio_state.h"
#include "call/test/mock_audio_send_stream.h"
#include "modules/audio_device/include/mock_audio_device.h"
#include "modules/audio_mixer/audio_mixer_impl.h"
#include "modules/audio_processing/include/mock_audio_processing.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace webrtc {
namespace test {
namespace {

using ::testing::InSequence;
using ::testing::NiceMock;
using ::testing::Return;

TEST(ExternalRecordingDemandTest, DisablingWebRtcRecordingKeepsExternalDemand) {
  AudioState::Config config;
  config.audio_mixer = AudioMixerImpl::Create();
  config.audio_processing = make_ref_counted<NiceMock<MockAudioProcessing>>();
  config.audio_device_module =
      make_ref_counted<NiceMock<MockAudioDeviceModule>>();

  scoped_refptr<internal::AudioState> audio_state =
      make_ref_counted<internal::AudioState>(config);
  auto* adm =
      static_cast<MockAudioDeviceModule*>(config.audio_device_module.get());
  adm->SetExternalRecordingDemand(true);
  adm->SetWebRtcRecordingDemand(true);

  EXPECT_CALL(*adm, StopRecording()).Times(0);
  audio_state->SetRecording(false);

  EXPECT_TRUE(adm->ExternalRecordingDemand());
  EXPECT_FALSE(adm->WebRtcRecordingDemand());
}

TEST(ExternalRecordingDemandTest, LastSenderRemovalKeepsRecording) {
  AudioState::Config config;
  config.audio_mixer = AudioMixerImpl::Create();
  config.audio_processing = make_ref_counted<NiceMock<MockAudioProcessing>>();
  config.audio_device_module =
      make_ref_counted<NiceMock<MockAudioDeviceModule>>();

  scoped_refptr<internal::AudioState> audio_state =
      make_ref_counted<internal::AudioState>(config);
  auto* adm =
      static_cast<MockAudioDeviceModule*>(config.audio_device_module.get());
  MockAudioSendStream stream;

  EXPECT_CALL(*adm, InitRecording());
  EXPECT_CALL(*adm, StartRecording());
  audio_state->AddSendingStream(&stream, 16000, 1);

  adm->SetExternalRecordingDemand(true);
  EXPECT_CALL(*adm, StopRecording()).Times(0);
  audio_state->RemoveSendingStream(&stream);

  EXPECT_TRUE(adm->ExternalRecordingDemand());
  EXPECT_FALSE(adm->WebRtcRecordingDemand());
}

TEST(ExternalRecordingDemandTest, PeerlessAcquireStartsAndReleaseStops) {
  auto adm = make_ref_counted<NiceMock<MockAudioDeviceModule>>();
  {
    InSequence sequence;
    EXPECT_CALL(*adm, RecordingIsInitialized()).WillOnce(Return(false));
    EXPECT_CALL(*adm, InitRecording()).WillOnce(Return(0));
    EXPECT_CALL(*adm, Recording()).WillOnce(Return(false));
    EXPECT_CALL(*adm, StartRecording()).WillOnce(Return(0));
  }
  EXPECT_EQ(adm->AcquireExternalRecording(), 0);
  EXPECT_TRUE(adm->ExternalRecordingDemand());

  EXPECT_CALL(*adm, Recording()).WillOnce(Return(true));
  EXPECT_CALL(*adm, StopRecording()).WillOnce(Return(0));
  EXPECT_EQ(adm->ReleaseExternalRecording(), 0);
  EXPECT_FALSE(adm->ExternalRecordingDemand());
}

TEST(ExternalRecordingDemandTest, ReleaseDoesNotStopWebRtcOwnedRecording) {
  auto adm = make_ref_counted<NiceMock<MockAudioDeviceModule>>();
  adm->SetWebRtcRecordingDemand(true);
  adm->SetExternalRecordingDemand(true);

  EXPECT_CALL(*adm, Recording()).Times(0);
  EXPECT_CALL(*adm, StopRecording()).Times(0);
  EXPECT_EQ(adm->ReleaseExternalRecording(), 0);
  EXPECT_FALSE(adm->ExternalRecordingDemand());
  EXPECT_TRUE(adm->WebRtcRecordingDemand());
}

TEST(ExternalRecordingDemandTest, FailedReleaseRestoresDemand) {
  auto adm = make_ref_counted<NiceMock<MockAudioDeviceModule>>();
  adm->SetExternalRecordingDemand(true);
  EXPECT_CALL(*adm, Recording()).WillOnce(Return(true));
  EXPECT_CALL(*adm, StopRecording()).WillOnce(Return(-1));

  EXPECT_EQ(adm->ReleaseExternalRecording(), -1);
  EXPECT_TRUE(adm->ExternalRecordingDemand());
}

TEST(ExternalRecordingDemandTest, FailedAcquireRollsBackDemand) {
  auto adm = make_ref_counted<NiceMock<MockAudioDeviceModule>>();
  EXPECT_CALL(*adm, RecordingIsInitialized()).WillOnce(Return(false));
  EXPECT_CALL(*adm, InitRecording()).WillOnce(Return(-1));
  EXPECT_CALL(*adm, StartRecording()).Times(0);

  EXPECT_EQ(adm->AcquireExternalRecording(), -1);
  EXPECT_FALSE(adm->ExternalRecordingDemand());
}

}  // namespace
}  // namespace test
}  // namespace webrtc
