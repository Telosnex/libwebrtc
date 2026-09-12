#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/make_ref_counted.h"
#include "modules/audio_device/include/mock_audio_device.h"
#include "modules/audio_processing/include/mock_audio_processing.h"
#include "rtc_base/thread.h"
#include "test/gmock.h"
#include "test/gtest.h"

namespace webrtc {
using ::testing::_;
using ::testing::NiceMock;
using ::testing::Return;
TEST(PcmFactory, OwnsMediaEngineWithoutPeerAndDispatchesAllPcmWorkToWorker) {
  auto worker = Thread::Create();
  ASSERT_TRUE(worker->Start());
  auto signal = Thread::Create();
  ASSERT_TRUE(signal->Start());
  auto network = Thread::CreateWithSocketServer();
  ASSERT_TRUE(network->Start());
  auto adm = make_ref_counted<NiceMock<test::MockAudioDeviceModule>>();
  auto apm = make_ref_counted<NiceMock<test::MockAudioProcessing>>();
  bool playing = false, fail_stop = false;
  ON_CALL(*adm, Playing()).WillByDefault([&] { return playing; });
  ON_CALL(*adm, StartPlayout()).WillByDefault([&] {
    EXPECT_TRUE(worker->IsCurrent());
    playing = true;
    return 0;
  });
  ON_CALL(*adm, StopPlayout()).WillByDefault([&] {
    EXPECT_TRUE(worker->IsCurrent());
    if (fail_stop) return -1;
    playing = false;
    return 0;
  });
  EXPECT_CALL(*adm, StartRecording()).Times(0);
  auto factory = CreatePeerConnectionFactory(
      network.get(), worker.get(), signal.get(), adm,
      CreateBuiltinAudioEncoderFactory(), CreateBuiltinAudioDecoderFactory(),
      nullptr, nullptr, nullptr, apm);
  ASSERT_TRUE(factory);
  const auto gen = factory->StartPcmPlayout();
  ASSERT_GT(gen, 0);
  EXPECT_LT(factory->StartPcmPlayout(), 0);
  EXPECT_EQ(factory->WritePcmPlayout(gen, 0, std::vector<uint8_t>(480)), 0);
  EXPECT_EQ(factory->GetPcmPlayoutState()[2], 240);
  EXPECT_EQ(factory->ClearPcmPlayout(gen, 1), 0);
  EXPECT_EQ(factory->WritePcmPlayout(gen, 0, std::vector<uint8_t>(480)), -2);
  fail_stop = true;
  EXPECT_NE(factory->StopPcmPlayout(gen), 0);
  EXPECT_EQ(factory->GetPcmPlayoutState()[0], gen);
  EXPECT_NE(factory->WritePcmPlayout(gen, 1, std::vector<uint8_t>(480)), 0);
  fail_stop = false;
  EXPECT_EQ(factory->StopPcmPlayout(gen), 0);
  EXPECT_EQ(factory->GetPcmPlayoutState()[0], 0);
  auto next = factory->StartPcmPlayout();
  EXPECT_GT(next, gen);
  // Destruction must quiesce/detach even if the device cannot acknowledge stop.
  fail_stop = true;
  factory = nullptr;
  network->Stop();
  signal->Stop();
  worker->Stop();
}
}  // namespace webrtc
