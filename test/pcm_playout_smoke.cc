// Explicit manual hardware smoke: SILENCE only, never opens the microphone.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

#include "libwebrtc.h"
#include "rtc_audio_device.h"
#include "rtc_logging.h"
#include "rtc_mediaconstraints.h"
#include "rtc_peerconnection.h"
#include "rtc_peerconnection_factory.h"
int main(int argc, char** argv) {
  using namespace libwebrtc;
  LibWebRTC::Initialize();
  LibWebRTCLogging::setMinDebugLogLevel(Verbose);
  auto factory =
      LibWebRTC::CreateRTCPeerConnectionFactory(RTCAudioBackend::kAlsa);
  if (!factory->Initialize()) return 2;
  RTCConfiguration config;
  auto anchor = factory->Create(config, RTCMediaConstraints::Create());
  auto device = factory->GetAudioDevice();
  for (int i = 0; i < device->PlayoutDevices(); ++i) {
    char name[128] = {}, guid[128] = {};
    device->PlayoutDeviceName(i, name, guid);
    std::printf("output %d: %s (%s)\n", i, name, guid);
  }
  if (argc == 2 && device->SetPlayoutDevice(std::atoi(argv[1])) != 0) return 6;
  const bool recording_before = device->GetRecordingState().recording;
  const int64_t generation = device->StartPcmPlayout();
  if (generation <= 0) {
    std::fprintf(stderr, "PCM start failed\n");
    return 3;
  }
  std::vector<uint8_t> silence(24000 * 2 / 5);
  if (device->WritePcmPlayout(generation, 0, silence.data(), silence.size()) !=
      0)
    return 4;
  RTCAudioDevice::PcmPlayoutState state;
  for (int i = 0; i < 100; ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
    state = device->GetPcmPlayoutState();
    if (state.consumed_frames == 4800) break;
  }
  const bool recording_after = device->GetRecordingState().recording;
  const int cleared = device->ClearPcmPlayout(generation, 1);
  const int stopped = device->StopPcmPlayout(generation);
  const auto final = device->GetPcmPlayoutState();
  std::printf(
      "pcm_smoke generation=%lld consumed=%lld render_callbacks=%lld "
      "playing=%d delay_ms=%d capture_before=%d capture_after=%d clear=%d "
      "stop=%d final_generation=%lld\n",
      (long long)generation, (long long)state.consumed_frames,
      (long long)state.render_callbacks, state.playing, state.delay_ms,
      recording_before, recording_after, cleared, stopped,
      (long long) final.generation);
  anchor->Close();
  factory->Delete(anchor);
  anchor = nullptr;
  device = nullptr;
  factory->Terminate();
  factory = nullptr;
  LibWebRTC::Terminate();
  return state.consumed_frames == 4800 && state.render_callbacks >= 20 &&
                 !recording_before && !recording_after && cleared == 0 &&
                 stopped == 0 && final.generation == 0
             ? 0
             : 5;
}
