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
// One generation-scoped app source; consumed ONLY by WebRTC's render mixer.
// All queues bounded; no Dart callbacks, I/O, or device control on render
// thread.
class PcmPlayoutSource final : public webrtc::AudioMixer::Source {
 public:
  static constexpr size_t kCapacity = 24000 * 5;
  int64_t Start(int rate = 24000, int channels = 1, bool shared = false,
                int64_t generation = 0);
  int Write(int64_t generation, int64_t epoch, const uint8_t* bytes,
            size_t size);
  int Clear(int64_t generation, int64_t epoch);
  int Quiesce(int64_t generation);
  void Stop();
  RTCAudioDevice::PcmPlayoutState State() const;
  AudioFrameInfo GetAudioFrameWithInfo(int rate,
                                       webrtc::AudioFrame* frame) override;
  int Ssrc() const override { return -1; }
  int PreferredSampleRate() const override { return preferred_rate_; }

 private:
  mutable webrtc::Mutex mutex_;
  // Allocate once, before mixer registration; never resize on the render thread.
  std::unique_ptr<int16_t[]> samples_;
  size_t capacity_ = 0;
  int preferred_rate_ = 24000;
  size_t read_ = 0;
  size_t size_ = 0;
  int64_t counter_ = 0;
  RTCAudioDevice::PcmPlayoutState state_;
  // Render-only except reset while holding the same lock at clear/stop.
  std::unique_ptr<webrtc::PushResampler<int16_t>> resampler_;
  bool reset_resampler_ = true;
  bool accepting_ = false;
};

// Worker-thread registry. It outlives output sessions so tokens cannot be reused.
// The caller must detach sources from the mixer BEFORE Release/StopAll.
class PcmPlayoutSources {
 public:
  int64_t Acquire(int rate, int channels, bool shared);
  PcmPlayoutSource* Find(int64_t generation) const;
  int64_t ExclusiveGeneration() const;
  void Release(int64_t generation);
  void StopAll();
  bool empty() const { return !sources_[0] && !sources_[1]; }
  const auto& sources() const { return sources_; }

 private:
  int64_t counter_ = 0;
  std::array<std::unique_ptr<PcmPlayoutSource>, 2> sources_;
};
}  // namespace libwebrtc
#endif
