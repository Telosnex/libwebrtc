// Copyright (c) Telosnex. Capture-clock drift servo.
//
// PROBLEM: AEC3 assumes render and capture share a sample clock (<~50 ppm
// relative). Split-clock hardware (USB mic + HDMI out) can exhibit large
// relative drift (measured: -1700 ppm on reference unit, 2026-08-27), which
// prevents the AEC3 linear filter from ever converging (ERLE ~0.2 dB).
//
// APPROACH: The transport layer sees both streams' true pacing:
//   - NeedMorePlayData pulls are paced by the PLAYOUT hardware clock.
//   - RecordedDataIsAvailable pushes are paced by the CAPTURE hardware clock.
// Counting frames (normalized to nominal seconds) on both sides over long
// windows yields the relative clock ratio without any device-specific API.
// A slew-limited SincResampler (Chrome's variable-rate resampler, built for
// exactly this job) re-times capture onto the render clock before the APM.
//
// GUARANTEES (fleet-free, by construction):
//   G1 If |measured drift| < kEngageppm, the resampler NEVER engages and the
//      byte path is identical to a build without this class (bit-exact).
//   G2 Correction ratio is clamped to +/-kMaxCorrectionPpm and slewed at
//      <= kMaxSlewPpmPerUpdate per estimator update; anomalous measurements
//      (|drift| > kAnomalyPpm, stream gaps, xrun-like jumps) freeze the servo
//      at its last ratio rather than chase them.
//   G3 Output framing to the delegate is exact 10 ms blocks; worst-case added
//      capture latency is bounded by kMaxFifoFrames (< 30 ms).
#ifndef INTERNAL_DRIFT_SERVO_H_
#define INTERNAL_DRIFT_SERVO_H_

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>

#include "common_audio/resampler/sinc_resampler.h"
#include "rtc_base/synchronization/mutex.h"

namespace webrtc {

class DriftServo {
 public:
  struct Stats {
    double measured_ppm = 0.0;   // raw relative drift estimate
    double applied_ppm = 0.0;    // current resampler correction
    bool engaged = false;
    int64_t windows = 0;         // completed estimator windows
    int64_t anomalies = 0;       // rejected measurements
  };

  DriftServo();
  // Seed a known device-pair ratio (e.g. from persisted self-test): engage
  // immediately at the seeded correction. The frame-count estimator then
  // acts only as a gross-anomaly watchdog (disengages if the measured
  // ratio ever contradicts the seed by more than kSeedVetoPpm with high
  // confidence), because arrival-time noise from deep capture buffering
  // makes fine frame-count measurement unreliable (see field session 3).
  void SeedRatio(double ppm);
  ~DriftServo();

  // Render side: call with frames delivered / nominal rate for that call.
  void OnRenderFrames(size_t frames, uint32_t sample_rate_hz);

  // Capture side. Input: interleaved S16, mono only (bypasses otherwise).
  // Returns number of complete 10 ms output blocks now available.
  // Caller then drains with PopBlock(). If the servo is bypassed/disengaged,
  // returns 0 and the caller must use the original buffer unchanged (G1).
  size_t PushCaptureAndCorrect(const int16_t* samples, size_t frames,
                               uint32_t sample_rate_hz, size_t channels);
  bool PopBlock(int16_t* out, size_t frames_per_block);

  bool engaged() const { return engaged_.load(std::memory_order_relaxed); }
  Stats GetStats() const;

  static constexpr double kEngagePpm = 100.0;    // engage above this
  static constexpr double kDisengagePpm = 50.0;  // hysteresis: release below
  static constexpr double kMaxCorrectionPpm = 2500.0;
  static constexpr double kSeedVetoPpm = 3000.0;
  static constexpr double kAnomalyPpm = 4000.0;
  static constexpr double kMaxSlewPpmPerUpdate = 100.0;
  static constexpr double kWindowSeconds = 10.0;
  static constexpr int kEngageConsecutiveWindows = 3;
  static constexpr size_t kMaxFifoFrames = 48 * 30;  // 30 ms @48k

 private:
  void UpdateEstimate();  // called with lock held, at window boundaries

  mutable Mutex lock_;
  // Frame accounting, in nominal seconds (frames / nominal_rate).
  double render_seconds_ RTC_GUARDED_BY(lock_) = 0.0;
  double capture_seconds_ RTC_GUARDED_BY(lock_) = 0.0;
  double window_render_start_ RTC_GUARDED_BY(lock_) = 0.0;
  double window_capture_start_ RTC_GUARDED_BY(lock_) = 0.0;
  double smoothed_ratio_ RTC_GUARDED_BY(lock_) = 1.0;
  // Sums over anomaly-free windows only: ratio estimate whose block-
  // quantization noise (+-10 ms per window edge) decays as 1/elapsed.
  double valid_capture_sum_ RTC_GUARDED_BY(lock_) = 0.0;
  double valid_render_sum_ RTC_GUARDED_BY(lock_) = 0.0;
  double applied_ratio_ RTC_GUARDED_BY(lock_) = 1.0;
  int consecutive_over_threshold_ RTC_GUARDED_BY(lock_) = 0;
  int64_t windows_ RTC_GUARDED_BY(lock_) = 0;
  int64_t anomalies_ RTC_GUARDED_BY(lock_) = 0;
  bool have_estimate_ RTC_GUARDED_BY(lock_) = false;
  bool seeded_ RTC_GUARDED_BY(lock_) = false;

  // Per-channel resampler + FIFOs (capture thread only). Mono and stereo
  // capture are both supported; channels share one measured ratio.
  struct Chan : public SincResamplerCallback {
    std::unique_ptr<SincResampler> rs;
    std::vector<float> in_fifo;
    void Run(size_t frames, float* destination) override;
  };
  static constexpr size_t kMaxChannels = 2;
  Chan chan_[kMaxChannels];
  std::vector<int16_t> out_fifo_;  // interleaved
  size_t active_channels_ = 0;
  uint32_t resampler_rate_ = 0;
  double last_ratio_ = 1.0;
  bool capture_saw_engaged_ = false;

  std::atomic<bool> engaged_{false};
};

}  // namespace webrtc
#endif  // INTERNAL_DRIFT_SERVO_H_
