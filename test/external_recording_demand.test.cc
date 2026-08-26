#include "test/gmock.h"
#include "test/gtest.h"

#include "api/make_ref_counted.h"
#include "audio/audio_state.h"
#include "call/test/mock_audio_send_stream.h"
#include "modules/audio_device/include/mock_audio_device.h"
#include "modules/audio_mixer/audio_mixer_impl.h"
#include "modules/audio_processing/include/mock_audio_processing.h"

namespace webrtc {
namespace test {
namespace {

using ::testing::NiceMock;

TEST(ExternalRecordingDemandTest, LastSenderRemovalKeepsRecording) {
  AudioState::Config config;
  config.audio_mixer = AudioMixerImpl::Create();
  config.audio_processing =
      make_ref_counted<NiceMock<MockAudioProcessing>>();
  config.audio_device_module =
      make_ref_counted<NiceMock<MockAudioDeviceModule>>();

  scoped_refptr<internal::AudioState> audio_state =
      make_ref_counted<internal::AudioState>(config);
  auto* adm = static_cast<MockAudioDeviceModule*>(
      config.audio_device_module.get());
  MockAudioSendStream stream;

  EXPECT_CALL(*adm, InitRecording());
  EXPECT_CALL(*adm, StartRecording());
  audio_state->AddSendingStream(&stream, 16000, 1);

  adm->SetExternalRecordingDemand(true);
  EXPECT_CALL(*adm, StopRecording()).Times(0);
  audio_state->RemoveSendingStream(&stream);

  EXPECT_TRUE(adm->ExternalRecordingDemand());
}

}  // namespace
}  // namespace test
}  // namespace webrtc
