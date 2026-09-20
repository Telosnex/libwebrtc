#include "src/internal/pcm_playout_source.h"

#include <algorithm>
#include <limits>

namespace libwebrtc {
int64_t PcmPlayoutSource::Start(int rate, int channels, bool shared,
                                int64_t generation) {
  webrtc::MutexLock lock(&mutex_);
  if (!((rate == 24000 && channels == 1) || (rate == 48000 && channels == 2)))
    return -3;
  if (state_.generation || counter_ == std::numeric_limits<int64_t>::max())
    return -1;
  capacity_ = static_cast<size_t>(rate) * channels * 5;
  samples_ = std::make_unique<int16_t[]>(capacity_);
  state_ = {};
  state_.generation = generation > 0 ? generation : ++counter_;
  state_.sample_rate = rate;
  state_.channels = channels;
  state_.shared = shared;
  preferred_rate_ = shared ? 48000 : rate;
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
  const size_t frame_bytes = 2 * state_.channels;
  if (!bytes || size == 0 || size % frame_bytes ||
      size > state_.sample_rate * frame_bytes) return -3;
  const size_t count = size / 2;
  if (count > capacity_ - size_) return -4;  // Atomic reject.
  for (size_t i = 0; i < count; ++i) {
    const uint16_t value =
        bytes[2 * i] | (static_cast<uint16_t>(bytes[2 * i + 1]) << 8);
    samples_[(read_ + size_ + i) % capacity_] = static_cast<int16_t>(value);
  }
  size_ += count;
  state_.accepted_frames += count / state_.channels;
  return 0;
}
int PcmPlayoutSource::Clear(int64_t generation, int64_t epoch) {
  webrtc::MutexLock lock(&mutex_);
  if (!accepting_ || generation <= 0 || generation != state_.generation ||
      epoch <= state_.epoch)
    return -2;
  state_.discarded_frames += size_ / state_.channels;
  size_ = read_ = 0;
  state_.epoch = epoch;
  reset_resampler_ = true;
  return 0;
}
int PcmPlayoutSource::Quiesce(int64_t generation) {
  webrtc::MutexLock lock(&mutex_);
  if (generation <= 0 || generation != state_.generation) return -2;
  accepting_ = false;
  state_.discarded_frames += size_ / state_.channels;
  size_ = read_ = 0;
  reset_resampler_ = true;
  return 0;
}
void PcmPlayoutSource::Stop() {
  webrtc::MutexLock lock(&mutex_);
  state_.discarded_frames += size_ / state_.channels;
  state_.generation = 0;
  accepting_ = false;
  size_ = read_ = 0;
  reset_resampler_ = true;
}
RTCAudioDevice::PcmPlayoutState PcmPlayoutSource::State() const {
  webrtc::MutexLock lock(&mutex_);
  auto result = state_;
  result.queued_frames = size_ / state_.channels;
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
                     webrtc::AudioFrame::kVadUnknown, state_.channels);
  if (!state_.generation) return AudioFrameInfo::kMuted;
  // Rebuild on next render after clear: no old resampler tail may reappear.
  if (reset_resampler_) {
    resampler_ = std::make_unique<webrtc::PushResampler<int16_t>>();
    reset_resampler_ = false;
  }
  std::array<int16_t, 960> input{};
  const size_t frames = state_.sample_rate / 100;
  const size_t samples = frames * state_.channels;
  const size_t count = std::min(size_, samples);
  for (size_t i = 0; i < count; ++i)
    input[i] = samples_[(read_ + i) % capacity_];
  read_ = (read_ + count) % capacity_;
  size_ -= count;
  state_.consumed_frames += count / state_.channels;
  state_.render_callbacks++;
  if (count < samples) state_.underrun_callbacks++;
  // Always push the trailing zeros so a legitimate resampler tail is rendered.
  // On clear the resampler above is reset, so it cannot resurrect old speech.
  resampler_->Resample(
      webrtc::InterleavedView<const int16_t>(input.data(), frames, state_.channels),
      webrtc::InterleavedView<int16_t>(frame->mutable_data(), rate / 100,
                                      state_.channels));
  return AudioFrameInfo::kNormal;
}

int64_t PcmPlayoutSources::Acquire(int rate, int channels, bool shared) {
  if (!((rate == 24000 && channels == 1) || (rate == 48000 && channels == 2)))
    return -3;
  for (const auto& source : sources_) {
    if (source && (!shared || !source->State().shared)) return -5;  // Busy.
  }
  if (counter_ == std::numeric_limits<int64_t>::max()) return -1;
  for (auto& source : sources_) {
    if (source) continue;
    source = std::make_unique<PcmPlayoutSource>();
    return source->Start(rate, channels, shared, ++counter_);
  }
  return -4;  // Capacity, including quiescent owners awaiting stop retry.
}
PcmPlayoutSource* PcmPlayoutSources::Find(int64_t generation) const {
  if (generation <= 0) return nullptr;
  for (const auto& source : sources_)
    if (source && source->State().generation == generation) return source.get();
  return nullptr;
}
int64_t PcmPlayoutSources::ExclusiveGeneration() const {
  for (const auto& source : sources_)
    if (source && !source->State().shared) return source->State().generation;
  return 0;
}
void PcmPlayoutSources::Release(int64_t generation) {
  for (auto& source : sources_)
    if (source && source->State().generation == generation) source.reset();
}
void PcmPlayoutSources::StopAll() {
  for (auto& source : sources_) source.reset();
}
}  // namespace libwebrtc
