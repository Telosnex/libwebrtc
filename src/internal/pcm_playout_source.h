#ifndef LIBWEBRTC_PCM_PLAYOUT_SOURCE_H_
#define LIBWEBRTC_PCM_PLAYOUT_SOURCE_H_
#include <array>
#include <cstdint>
#include <memory>

#include "api/audio/audio_mixer.h"
#include "common_audio/resampler/include/push_resampler.h"
#include "rtc_audio_device.h"
#include "rtc_base/synchronization/mutex.h"

namespace libwebrtc {
// Single, generation-scoped app source; consumed ONLY by WebRTC's render mixer.
// All queues bounded; no Dart callbacks, I/O, or device control on render
// thread.
class PcmPlayoutSource final : public webrtc::AudioMixer::Source {
 public:
  static constexpr size_t kCapacity = 24000 * 5;
  int64_t Start();
  int Write(int64_t generation, int64_t epoch, const uint8_t* bytes,
            size_t size);
  int Clear(int64_t generation, int64_t epoch);
  int Quiesce(int64_t generation);
  void Stop();
  RTCAudioDevice::PcmPlayoutState State() const;
  AudioFrameInfo GetAudioFrameWithInfo(int rate,
                                       webrtc::AudioFrame* frame) override;
  int Ssrc() const override { return -1; }
  int PreferredSampleRate() const override { return 24000; }

 private:
  mutable webrtc::Mutex mutex_;
  std::array<int16_t, kCapacity> samples_{};
  size_t read_ = 0;
  size_t size_ = 0;
  int64_t counter_ = 0;
  RTCAudioDevice::PcmPlayoutState state_;
  // Render-only except reset while holding the same lock at clear/stop.
  std::unique_ptr<webrtc::PushResampler<int16_t>> resampler_;
  bool reset_resampler_ = true;
  bool accepting_ = false;
};
}  // namespace libwebrtc
#endif
