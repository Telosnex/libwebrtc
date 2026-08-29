#include "src/internal/drift_servo.h"

#include <algorithm>
#include <cmath>

#include "rtc_base/logging.h"

namespace webrtc {

DriftServo::DriftServo() = default;
DriftServo::~DriftServo() = default;

void DriftServo::OnRenderFrames(size_t frames, uint32_t sample_rate_hz) {
  if (sample_rate_hz == 0) return;
  MutexLock l(&lock_);
  render_seconds_ += static_cast<double>(frames) / sample_rate_hz;
  if (render_seconds_ - window_render_start_ >= kWindowSeconds) {
    UpdateEstimate();
  }
}

void DriftServo::UpdateEstimate() RTC_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
  const double rdelta = render_seconds_ - window_render_start_;
  const double cdelta = capture_seconds_ - window_capture_start_;
  window_render_start_ = render_seconds_;
  window_capture_start_ = capture_seconds_;
  // Both streams must have been flowing for (almost) the whole window;
  // otherwise a stream start/stop masquerades as drift. Freeze (G2).
  if (rdelta <= 0 || cdelta < rdelta * 0.9 || cdelta > rdelta * 1.1) {
    ++anomalies_;
    return;
  }
  const double ratio = cdelta / rdelta;  // capture clock / render clock
  const double ppm = (ratio - 1.0) * 1e6;
  if (std::abs(ppm) > kAnomalyPpm) {  // beyond plausible crystal error: xrun?
    ++anomalies_;
    return;
  }
  ++windows_;
  // Exponential forgetting (~2000 s horizon): tracks slow thermal wander
  // while retaining 1/T quantization-noise averaging.
  valid_capture_sum_ = 0.995 * valid_capture_sum_ + cdelta;
  valid_render_sum_ = 0.995 * valid_render_sum_ + rdelta;
  // Cumulative over valid windows: each window edge contributes +-1 block
  // (10 ms) of quantization, so error ~ 10ms/elapsed -> 100 ppm at 100 s,
  // 10 ppm at 1000 s, monotonically tightening.
  smoothed_ratio_ = valid_capture_sum_ / valid_render_sum_;
  have_estimate_ = true;
  const double drift_ppm = (smoothed_ratio_ - 1.0) * 1e6;
  if (hardware_controlling_) return;
  if (seeded_) {
    // Log-only watchdog: a wrong seed is bounded-harm (clamped correction,
    // worst case == uncorrected baseline), so observe, never auto-toggle.
    const double measured_ppm =
        (valid_capture_sum_ / valid_render_sum_ - 1.0) * 1e6;
    const double seed_ppm = (applied_ratio_ - 1.0) * 1e6;
    if (windows_ > 5 && std::abs(measured_ppm - seed_ppm) > kSeedVetoPpm) {
      RTC_LOG(LS_WARNING) << "DriftServo seed suspect: measured "
                          << measured_ppm << " vs seed " << seed_ppm;
    }
    return;
  }

  // Quantization noise bound: window edges are quantized to 10 ms blocks,
  // so the cumulative estimate carries ~20 ms of edge uncertainty spread
  // over the effective averaging horizon. Engagement requires the measured
  // drift to exceed the threshold BY MORE THAN this bound, so in-spec
  // hardware can never transiently engage on early noisy estimates (G1).
  const double noise_bound_ppm = 0.020 / valid_render_sum_ * 1e6;
  const double confident_ppm =
      std::max(0.0, std::abs(drift_ppm) - noise_bound_ppm);
  const bool currently = engaged_.load(std::memory_order_relaxed);
  if (!currently) {
    consecutive_over_threshold_ =
        confident_ppm > kEngagePpm ? consecutive_over_threshold_ + 1 : 0;
    if (consecutive_over_threshold_ >= kEngageConsecutiveWindows) {
      engaged_.store(true, std::memory_order_relaxed);
      // SNAP on first engagement: there is no converged AEC filter to
      // protect yet, so apply the full measured ratio immediately rather
      // than slewing toward it (slew applies to subsequent tracking only).
      applied_ratio_ =
          std::clamp(smoothed_ratio_, 1.0 - kMaxCorrectionPpm * 1e-6,
                     1.0 + kMaxCorrectionPpm * 1e-6);
      RTC_LOG(LS_WARNING) << "DriftServo ENGAGED (snap): " << drift_ppm
                          << " ppm";
    }
  } else if (std::abs(drift_ppm) + noise_bound_ppm < kDisengagePpm) {
    engaged_.store(false, std::memory_order_relaxed);
    applied_ratio_ = 1.0;
    RTC_LOG(LS_WARNING) << "DriftServo disengaged (drift " << drift_ppm
                        << " ppm)";
  }
  // Slew applied ratio toward target, clamped (G2).
  double target = std::clamp(smoothed_ratio_, 1.0 - kMaxCorrectionPpm * 1e-6,
                             1.0 + kMaxCorrectionPpm * 1e-6);
  const double max_step = kMaxSlewPpmPerUpdate * 1e-6;
  applied_ratio_ += std::clamp(target - applied_ratio_, -max_step, max_step);
  RTC_LOG(LS_INFO) << "DriftServo window " << windows_ << ": measured " << ppm
                   << " ppm, smoothed " << drift_ppm << " ppm, applied "
                   << (applied_ratio_ - 1.0) * 1e6 << " ppm, engaged "
                   << engaged_.load(std::memory_order_relaxed);
}

void DriftServo::SetHardwareClockMode(HardwareClockMode mode) {
  MutexLock l(&lock_);
  hardware_mode_ = mode;
  hardware_estimator_.Reset();
  hardware_ready_ = false;
  hardware_controlling_ = false;
  hardware_consecutive_ = 0;
  hardware_estimates_ = 0;
}

void DriftServo::OnHardwareClockObservation(
    const AudioHardwareClockObservation& observation) {
  MutexLock l(&lock_);
  if (hardware_mode_ == HardwareClockMode::kDisabled) return;
  const auto update = hardware_estimator_.Add(observation);
  if (update.reset || update.rejected) hardware_consecutive_ = 0;
  if (!update.estimate) return;

  const auto& estimate = *update.estimate;
  hardware_ready_ = true;
  hardware_measured_ppm_ = estimate.relative_ppm;
  hardware_uncertainty_ppm_ = estimate.uncertainty_ppm;
  hardware_span_seconds_ = estimate.span_seconds;
  ++hardware_estimates_;
  RTC_LOG(LS_INFO) << "DriftServo hw clock: measured " << hardware_measured_ppm_
                   << " +/- " << hardware_uncertainty_ppm_ << " ppm over "
                   << hardware_span_seconds_ << " s";

  if (hardware_mode_ != HardwareClockMode::kControl) return;
  if (std::abs(estimate.relative_ppm) > kAnomalyPpm ||
      estimate.uncertainty_ppm > kHardwareMaxUncertaintyPpm) {
    hardware_consecutive_ = 0;
    return;
  }

  // A seed is only a startup bridge: any stable hardware estimate may replace
  // it, including an in-spec estimate that correctly drives a bad seed to zero.
  const bool correction_confident =
      std::abs(estimate.relative_ppm) - estimate.uncertainty_ppm > kEngagePpm;
  if (!hardware_controlling_) {
    if (seeded_ || correction_confident) {
      ++hardware_consecutive_;
    } else {
      hardware_consecutive_ = 0;
    }
    if (hardware_consecutive_ < kHardwareConsecutiveEstimates) return;
    hardware_controlling_ = true;
    seeded_ = false;
    RTC_LOG(LS_WARNING) << "DriftServo hardware clock CONTROL at "
                        << estimate.relative_ppm << " ppm";
  }
  ApplyHardwareEstimate(estimate);
}

void DriftServo::ApplyHardwareEstimate(
    const HardwareClockEstimator::Estimate& estimate) {
  hardware_measured_ppm_ = estimate.relative_ppm;
  const bool in_spec =
      std::abs(estimate.relative_ppm) + estimate.uncertainty_ppm <
      kDisengagePpm;
  const double target = in_spec ? 1.0
                                : std::clamp(1.0 + estimate.relative_ppm * 1e-6,
                                             1.0 - kMaxCorrectionPpm * 1e-6,
                                             1.0 + kMaxCorrectionPpm * 1e-6);
  const bool currently = engaged_.load(std::memory_order_relaxed);
  if (!currently && !in_spec) {
    applied_ratio_ = target;
    engaged_.store(true, std::memory_order_relaxed);
    RTC_LOG(LS_WARNING) << "DriftServo hardware ENGAGED (snap): "
                        << estimate.relative_ppm << " ppm";
    return;
  }
  if (!currently) return;

  const double max_step = kMaxSlewPpmPerUpdate * 1e-6;
  applied_ratio_ += std::clamp(target - applied_ratio_, -max_step, max_step);
  if (in_spec && std::abs(applied_ratio_ - 1.0) < 1e-9) {
    applied_ratio_ = 1.0;
    engaged_.store(false, std::memory_order_relaxed);
    RTC_LOG(LS_WARNING) << "DriftServo hardware disengaged in-spec";
  }
}

size_t DriftServo::PushCaptureAndCorrect(const int16_t* samples, size_t frames,
                                         uint32_t sample_rate_hz,
                                         size_t channels) {
  if (sample_rate_hz == 0 || channels == 0) return 0;
  double applied;
  {
    MutexLock l(&lock_);
    capture_seconds_ += static_cast<double>(frames) / sample_rate_hz;
    applied = applied_ratio_;
  }
  if (!engaged_.load(std::memory_order_relaxed) || channels > kMaxChannels) {
    capture_saw_engaged_ = false;
    return 0;
  }
  if (resampler_rate_ != sample_rate_hz || active_channels_ != channels ||
      !capture_saw_engaged_) {
    for (size_t ch = 0; ch < channels; ++ch) {
      chan_[ch].rs = std::make_unique<SincResampler>(
          applied, SincResampler::kDefaultRequestSize, &chan_[ch]);
      chan_[ch].in_fifo.clear();
    }
    out_fifo_.clear();
    resampler_rate_ = sample_rate_hz;
    active_channels_ = channels;
    last_ratio_ = applied;
    capture_saw_engaged_ = true;
  }
  if (applied != last_ratio_) {
    for (size_t ch = 0; ch < channels; ++ch) chan_[ch].rs->SetRatio(applied);
    last_ratio_ = applied;
  }
  for (size_t ch = 0; ch < channels; ++ch)
    chan_[ch].in_fifo.reserve(chan_[ch].in_fifo.size() + frames);
  for (size_t i = 0; i < frames; ++i)
    for (size_t ch = 0; ch < channels; ++ch)
      chan_[ch].in_fifo.push_back(samples[i * channels + ch] / 32768.0f);

  const size_t block = sample_rate_hz / 100;  // 10 ms frames
  std::vector<float> tmp(block);
  const size_t need = static_cast<size_t>(block * applied) +
                      2 * SincResampler::kKernelSize +
                      SincResampler::kDefaultRequestSize;
  while (chan_[0].in_fifo.size() > need) {
    const size_t base = out_fifo_.size();
    out_fifo_.resize(base + block * channels);
    for (size_t ch = 0; ch < channels; ++ch) {
      chan_[ch].rs->Resample(block, tmp.data());
      for (size_t k = 0; k < block; ++k) {
        const float v = std::clamp(tmp[k], -1.0f, 1.0f);
        out_fifo_[base + k * channels + ch] =
            static_cast<int16_t>(std::lround(v * 32767.0f));
      }
    }
    if (out_fifo_.size() > kMaxFifoFrames * 4 * channels) {
      out_fifo_.erase(
          out_fifo_.begin(),
          out_fifo_.begin() + (out_fifo_.size() - kMaxFifoFrames * channels));
    }
  }
  return out_fifo_.size() / (block * channels);
}

bool DriftServo::PopBlock(int16_t* out, size_t frames_per_block) {
  const size_t n = frames_per_block * active_channels_;
  if (out_fifo_.size() < n || n == 0) return false;
  std::copy(out_fifo_.begin(), out_fifo_.begin() + n, out);
  out_fifo_.erase(out_fifo_.begin(), out_fifo_.begin() + n);
  return true;
}

void DriftServo::Chan::Run(size_t frames, float* destination) {
  const size_t n = std::min(frames, in_fifo.size());
  std::copy(in_fifo.begin(), in_fifo.begin() + n, destination);
  std::fill(destination + n, destination + frames, 0.0f);
  in_fifo.erase(in_fifo.begin(), in_fifo.begin() + n);
}

void DriftServo::SeedRatio(double ppm) {
  MutexLock l(&lock_);
  if (std::abs(ppm) < kEngagePpm || std::abs(ppm) > kMaxCorrectionPpm) return;
  seeded_ = true;
  smoothed_ratio_ = 1.0 + ppm * 1e-6;
  applied_ratio_ = smoothed_ratio_;
  engaged_.store(true, std::memory_order_relaxed);
  RTC_LOG(LS_WARNING) << "DriftServo SEEDED and engaged at " << ppm << " ppm";
}

DriftServo::Stats DriftServo::GetStats() const {
  MutexLock l(&lock_);
  Stats s;
  s.measured_ppm = hardware_controlling_ ? hardware_measured_ppm_
                                         : (smoothed_ratio_ - 1.0) * 1e6;
  s.applied_ppm = (applied_ratio_ - 1.0) * 1e6;
  s.engaged = engaged_.load(std::memory_order_relaxed);
  s.windows = windows_;
  s.anomalies = anomalies_;
  s.hardware_ready = hardware_ready_;
  s.hardware_controlling = hardware_controlling_;
  s.hardware_measured_ppm = hardware_measured_ppm_;
  s.hardware_uncertainty_ppm = hardware_uncertainty_ppm_;
  s.hardware_span_seconds = hardware_span_seconds_;
  s.hardware_estimates = hardware_estimates_;
  s.hardware_resets = hardware_estimator_.resets();
  s.hardware_rejected = hardware_estimator_.rejected();
  return s;
}

}  // namespace webrtc
