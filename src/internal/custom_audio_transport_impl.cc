#include "src/internal/custom_audio_transport_impl.h"

#include <cstdio>
#include <cstdlib>
#include <string>

#include "rtc_base/logging.h"
#include "rtc_base/time_utils.h"

namespace webrtc {
namespace {
bool EnvFlag(const char* name) {
  const char* v = std::getenv(name);
  return v && v[0] == '1';
}
}  // namespace

CustomAudioTransportImpl::CustomAudioTransportImpl(
    AudioMixer* mixer, AudioProcessing* audio_processing,
    AsyncAudioProcessing::Factory* async_audio_processing_factory)
    : audio_transport_impl_(std::make_unique<webrtc::AudioTransportImpl>(
          mixer, audio_processing, async_audio_processing_factory)) {
  if (EnvFlag("TSNX_DRIFT_SERVO")) {
    drift_servo_ = std::make_unique<DriftServo>();
    RTC_LOG(LS_INFO) << "TSNX: drift servo enabled (observing)";
    if (const char* s = std::getenv("TSNX_DRIFT_PPM"); s && s[0]) {
      seed_ppm_env_ = atof(s);
      drift_servo_->SeedRatio(seed_ppm_env_);
      fprintf(stderr, "TSNX: drift servo seeded %s ppm\n", s);
    }
  }
  if (EnvFlag("TSNX_SELF_ECHO_GATE")) {
    self_echo_gate_ = std::make_unique<SelfEchoGate>();
    RTC_LOG(LS_INFO) << "TSNX: self-echo gate enabled (observing)";
  }
  if (const char* d = std::getenv("TSNX_TAP_DIR"); d && d[0]) {
    tap_ = SessionTap::Create(d, seed_ppm_env_);
  }
}

DriftServo::Stats CustomAudioTransportImpl::GetDriftServoStats() const {
  return drift_servo_ ? drift_servo_->GetStats() : DriftServo::Stats();
}

SelfEchoGate::Stats CustomAudioTransportImpl::GetSelfEchoGateStats() const {
  return self_echo_gate_ ? self_echo_gate_->GetStats() : SelfEchoGate::Stats();
}

// TODO(bugs.webrtc.org/13620) Deprecate this function
int32_t CustomAudioTransportImpl::RecordedDataIsAvailable(
    const void* audioSamples, size_t nSamples, size_t nBytesPerSample,
    size_t nChannels, uint32_t samplesPerSec, uint32_t totalDelayMS,
    int32_t clockDrift, uint32_t currentMicLevel, bool keyPressed,
    uint32_t& newMicLevel) {
  cap_legacy_calls_.fetch_add(1, std::memory_order_relaxed);
  if (drift_servo_) {
    const size_t blocks = drift_servo_->PushCaptureAndCorrect(
        static_cast<const int16_t*>(audioSamples), nSamples, samplesPerSec,
        nChannels);
    if (drift_servo_->engaged() && nChannels <= 2) {
      const size_t block_frames = samplesPerSec / 100;
      servo_block_.resize(block_frames * nChannels);
      int32_t rv = 0;
      for (size_t i = 0; i < blocks; ++i) {
        if (!drift_servo_->PopBlock(servo_block_.data(), block_frames)) break;
        rv = audio_transport_impl_->RecordedDataIsAvailable(
            servo_block_.data(), block_frames, nBytesPerSample, nChannels,
            samplesPerSec, totalDelayMS, clockDrift, currentMicLevel,
            keyPressed, newMicLevel);
      }
      return rv;
    }
  }
  return audio_transport_impl_->RecordedDataIsAvailable(
      audioSamples, nSamples, nBytesPerSample, nChannels, samplesPerSec,
      totalDelayMS, clockDrift, currentMicLevel, keyPressed, newMicLevel);
}

int32_t CustomAudioTransportImpl::RecordedDataIsAvailable(
    const void* audioSamples, size_t nSamples, size_t nBytesPerSample,
    size_t nChannels, uint32_t samplesPerSec, uint32_t totalDelayMS,
    int32_t clockDrift, uint32_t currentMicLevel, bool keyPressed,
    uint32_t& newMicLevel, std::optional<int64_t> estimated_capture_time_ns) {
  cap_modern_calls_.fetch_add(1, std::memory_order_relaxed);
  if (tap_) {
    tap_->cap_raw().Push(static_cast<const int16_t*>(audioSamples), nSamples,
                         samplesPerSec, nChannels, TimeMicros());
  }
  if (drift_servo_) {
    const size_t blocks = drift_servo_->PushCaptureAndCorrect(
        static_cast<const int16_t*>(audioSamples), nSamples, samplesPerSec,
        nChannels);
    if (drift_servo_->engaged() && nChannels <= 2) {
      const size_t block_frames = samplesPerSec / 100;
      servo_block_.resize(block_frames * nChannels);
      int32_t rv = 0;
      for (size_t i = 0; i < blocks; ++i) {
        if (!drift_servo_->PopBlock(servo_block_.data(), block_frames)) break;
        rv = audio_transport_impl_->RecordedDataIsAvailable(
            servo_block_.data(), block_frames, nBytesPerSample, nChannels,
            samplesPerSec, totalDelayMS, clockDrift, currentMicLevel,
            keyPressed, newMicLevel, estimated_capture_time_ns);
      }
      return rv;
    }
  }
  return audio_transport_impl_->RecordedDataIsAvailable(
      audioSamples, nSamples, nBytesPerSample, nChannels, samplesPerSec,
      totalDelayMS, clockDrift, currentMicLevel, keyPressed, newMicLevel,
      estimated_capture_time_ns);
}

int32_t CustomAudioTransportImpl::NeedMorePlayData(
    size_t nSamples, size_t nBytesPerSample, size_t nChannels,
    uint32_t samplesPerSec, void* audioSamples, size_t& nSamplesOut,
    int64_t* elapsed_time_ms, int64_t* ntp_time_ms) {
  const int32_t rv = audio_transport_impl_->NeedMorePlayData(
      nSamples, nBytesPerSample, nChannels, samplesPerSec, audioSamples,
      nSamplesOut, elapsed_time_ms, ntp_time_ms);
  if (rv == 0 && nSamplesOut > 0) {
    // nSamplesOut is TOTAL samples (frames * channels): InterleavedView's
    // size(), see AudioTransportImpl::NeedMorePlayData. Convert to frames.
    const size_t render_frames = nChannels > 0 ? nSamplesOut / nChannels : 0;
    if (drift_servo_)
      drift_servo_->OnRenderFrames(render_frames, samplesPerSec);
    if (self_echo_gate_) {
      self_echo_gate_->PushRender(static_cast<const int16_t*>(audioSamples),
                                  render_frames, samplesPerSec, nChannels);
    }
    if (tap_) {
      tap_->render().Push(static_cast<const int16_t*>(audioSamples),
                          render_frames, samplesPerSec, nChannels,
                          TimeMicros());
    }
    const int64_t calls = render_calls_.fetch_add(1) + 1;
    if (calls % 3000 == 0) {
      if (drift_servo_) {
        const auto s = drift_servo_->GetStats();
        // stderr as well: journald captures it even without an RTC log sink.
        fprintf(stderr,
                "TSNX drift: measured %.0f ppm applied %.0f engaged %d"
                " windows %lld anomalies %lld cap_l %lld cap_m %lld\n",
                s.measured_ppm, s.applied_ppm, s.engaged, (long long)s.windows,
                (long long)s.anomalies,
                (long long)cap_legacy_calls_.load(std::memory_order_relaxed),
                (long long)cap_modern_calls_.load(std::memory_order_relaxed));
        RTC_LOG(LS_INFO) << "TSNX drift: measured " << s.measured_ppm
                         << " ppm, applied " << s.applied_ppm
                         << " ppm, engaged " << s.engaged << ", windows "
                         << s.windows << ", anomalies " << s.anomalies;
      }
      if (self_echo_gate_) {
        const auto g = self_echo_gate_->GetStats();
        fprintf(stderr,
                "TSNX gate: closed %d corr %.2f lag %d excess %.1f"
                " erl_off %.1f closed_evals %lld/%lld\n",
                g.closed, g.corr, g.lag_ms, g.slow_excess_db, g.offset_db,
                (long long)g.closed_evals, (long long)g.evals);
        RTC_LOG(LS_INFO) << "TSNX gate: closed " << g.closed << ", corr "
                         << g.corr << ", lag " << g.lag_ms << " ms, excess "
                         << g.slow_excess_db << " dB, erl_offset "
                         << g.offset_db << " dB, closed_evals "
                         << g.closed_evals << "/" << g.evals;
      }
    }
  }
  return rv;
}

void CustomAudioTransportImpl::PullRenderData(
    int bits_per_sample, int sample_rate, size_t number_of_channels,
    size_t number_of_frames, void* audio_data, int64_t* elapsed_time_ms,
    int64_t* ntp_time_ms) {
  audio_transport_impl_->PullRenderData(
      bits_per_sample, sample_rate, number_of_channels, number_of_frames,
      audio_data, elapsed_time_ms, ntp_time_ms);
}

void CustomAudioTransportImpl::UpdateAudioSenders(
    std::vector<AudioSender*> senders, int send_sample_rate_hz,
    size_t send_num_channels) {
  if (senders.size() > 0) {
    std::vector<AudioSender*> snds = std::vector<AudioSender*>();
    snds.push_back(this);
    audio_transport_impl_->UpdateAudioSenders(
        std::move(snds), send_sample_rate_hz, send_num_channels);
  } else {
    std::vector<AudioSender*> snds = std::vector<AudioSender*>();
    audio_transport_impl_->UpdateAudioSenders(
        std::move(snds), send_sample_rate_hz, send_num_channels);
  }
}

void CustomAudioTransportImpl::AddAudioSender(AudioSender* sender) {
  MutexLock lock(&capture_lock_);
  audio_senders_.push_back(sender);
}

void CustomAudioTransportImpl::RemoveAudioSender(AudioSender* sender) {
  MutexLock lock(&capture_lock_);
  auto it = std::remove(audio_senders_.begin(), audio_senders_.end(), sender);
  if (it != audio_senders_.end()) {
    audio_senders_.erase(it, audio_senders_.end());
  }
}

void CustomAudioTransportImpl::SetStereoChannelSwapping(bool enable) {
  audio_transport_impl_->SetStereoChannelSwapping(enable);
}

void CustomAudioTransportImpl::SendAudioData(
    std::unique_ptr<AudioFrame> audio_frame) {
  RTC_DCHECK_GT(audio_frame->samples_per_channel_, 0);
  if (self_echo_gate_) {
    self_echo_gate_->PushCapture(
        audio_frame->data(), audio_frame->samples_per_channel_,
        audio_frame->sample_rate_hz_, audio_frame->num_channels_);
  }
  if (tap_) {
    tap_->cap_apm().Push(audio_frame->data(), audio_frame->samples_per_channel_,
                         audio_frame->sample_rate_hz_,
                         audio_frame->num_channels_, TimeMicros());
  }
  MutexLock lock(&capture_lock_);
  if (audio_senders_.empty()) return;

  auto it = audio_senders_.begin();
  while (++it != audio_senders_.end()) {
    auto audio_frame_copy = std::make_unique<AudioFrame>();
    audio_frame_copy->CopyFrom(*audio_frame);
    (*it)->SendAudioData(std::move(audio_frame_copy));
  }
  // Send the original frame to the first stream w/o copying.
  (*audio_senders_.begin())->SendAudioData(std::move(audio_frame));
}
}  // namespace webrtc