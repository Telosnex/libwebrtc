#include "src/internal/pcm_playout_source.h"

#include <algorithm>
#include <limits>

namespace libwebrtc {
int64_t PcmPlayoutSource::Start() {
  webrtc::MutexLock lock(&mutex_);
  if (state_.generation || counter_ == std::numeric_limits<int64_t>::max())
    return -1;
  state_ = {};
  state_.generation = ++counter_;
  accepting_ = true;
  size_ = read_ = 0;
  reset_resampler_ = true;
  return state_.generation;
}
int PcmPlayoutSource::Write(int64_t generation, int64_t epoch,
                            const uint8_t* bytes, size_t size) {
  webrtc::MutexLock lock(&mutex_);
  if (!accepting_ || generation <= 0 || generation != state_.generation ||
      epoch != state_.epoch)
    return -2;
  if (!bytes || size == 0 || size % 2 || size > 48000) return -3;
  const size_t count = size / 2;
  if (count > kCapacity - size_) return -4;  // Atomic reject, never skip words.
  for (size_t i = 0; i < count; ++i) {
    const uint16_t value =
        bytes[2 * i] | (static_cast<uint16_t>(bytes[2 * i + 1]) << 8);
    samples_[(read_ + size_ + i) % kCapacity] = static_cast<int16_t>(value);
  }
  size_ += count;
  state_.accepted_frames += count;
  return 0;
}
int PcmPlayoutSource::Clear(int64_t generation, int64_t epoch) {
  webrtc::MutexLock lock(&mutex_);
  if (!accepting_ || generation <= 0 || generation != state_.generation ||
      epoch <= state_.epoch)
    return -2;
  state_.discarded_frames += size_;
  size_ = read_ = 0;
  state_.epoch = epoch;
  reset_resampler_ = true;
  return 0;
}
int PcmPlayoutSource::Quiesce(int64_t generation) {
  webrtc::MutexLock lock(&mutex_);
  if (generation <= 0 || generation != state_.generation) return -2;
  accepting_ = false;
  state_.discarded_frames += size_;
  size_ = read_ = 0;
  reset_resampler_ = true;
  return 0;
}
void PcmPlayoutSource::Stop() {
  webrtc::MutexLock lock(&mutex_);
  state_.discarded_frames += size_;
  state_.generation = 0;
  accepting_ = false;
  size_ = read_ = 0;
  reset_resampler_ = true;
}
RTCAudioDevice::PcmPlayoutState PcmPlayoutSource::State() const {
  webrtc::MutexLock lock(&mutex_);
  auto result = state_;
  result.queued_frames = size_;
  return result;
}
webrtc::AudioMixer::Source::AudioFrameInfo
PcmPlayoutSource::GetAudioFrameWithInfo(int rate, webrtc::AudioFrame* frame) {
  if (!frame || (rate != 8000 && rate != 16000 && rate != 24000 &&
                 rate != 32000 && rate != 48000))
    return AudioFrameInfo::kError;
  webrtc::MutexLock lock(&mutex_);
  frame->UpdateFrame(0, nullptr, rate / 100, rate,
                     webrtc::AudioFrame::kNormalSpeech,
                     webrtc::AudioFrame::kVadUnknown, 1);
  if (!state_.generation) return AudioFrameInfo::kMuted;
  // Rebuild on next render after clear: no old resampler tail may reappear.
  if (reset_resampler_) {
    resampler_ = std::make_unique<webrtc::PushResampler<int16_t>>();
    reset_resampler_ = false;
  }
  std::array<int16_t, 240> input{};
  const size_t count = std::min(size_, input.size());
  for (size_t i = 0; i < count; ++i)
    input[i] = samples_[(read_ + i) % kCapacity];
  read_ = (read_ + count) % kCapacity;
  size_ -= count;
  state_.consumed_frames += count;
  state_.render_callbacks++;
  if (count < input.size()) state_.underrun_callbacks++;
  // Always push the trailing zeros so a legitimate resampler tail is rendered.
  // On clear the resampler above is reset, so it cannot resurrect old speech.
  resampler_->Resample(
      webrtc::InterleavedView<const int16_t>(input.data(), input.size(), 1),
      webrtc::InterleavedView<int16_t>(frame->mutable_data(), rate / 100, 1));
  return AudioFrameInfo::kNormal;
}
}  // namespace libwebrtc
