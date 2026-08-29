// Copyright (c) Telosnex. Self-echo gate: render-correlated capture detector.
//
// INVARIANT (appliance): anything this device renders is never content it
// wants to capture. Mic audio whose 12-band log-energy envelope correlates
// with the recent render envelope is self-echo, regardless of AEC state.
//
// This is deliberately NOT a canceller: it emits one bit ("suppress turn-
// taking endorsement") at 100 Hz plus diagnostics. Envelope features at 10 ms
// resolution are immune to the clock drift (measured -1700 ppm) that defeats
// AEC3's sample-aligned linear filter, and to speaker/display nonlinearity.
//
// Thresholds originated from one recorded field fixture suite. The gate is
// observer-only; release CI enforces the structural/generated-fixture behavior
// below, not a fleet-wide claim for those empirical thresholds.
//
// STRUCTURAL GUARANTEES (hardware-independent, unit-tested):
//   G1 Gate can only be CLOSED while render is recently active; render silent
//      => gate OPEN. The device can never stop hearing a user while idle.
//   G2 The gate never mutes or drops capture audio; consumers apply duck /
//      VAD-veto policy. Worst case == today's behavior.
//   G3 Asymmetric evidence: >=200 ms of clean correlation to close;
//      60 ms of strong contrary evidence (or render silence) to open.
#ifndef INTERNAL_SELF_ECHO_GATE_H_
#define INTERNAL_SELF_ECHO_GATE_H_

#include <array>
#include <cstdint>
#include <vector>

#include "rtc_base/synchronization/mutex.h"

namespace webrtc {

class SelfEchoGate {
 public:
  static constexpr int kBands = 12;
  static constexpr int kRing = 256;           // 2.56 s of 10 ms frames
  static constexpr int kWindow = 50;          // correlation window (500 ms)
  static constexpr int kMaxLag = 90;          // delay search 0..900 ms
  static constexpr int kEvalEveryFrames = 5;  // 50 ms cadence
  // Detector thresholds (validated on fixture suite; see header comment).
  static constexpr float kCloseCorr = 0.55f;
  static constexpr float kCorrCollapse = 0.35f;
  static constexpr float kSlowExcessDb = 5.0f;
  static constexpr float kFastExcessDb = 7.0f;
  static constexpr int kCloseStreak = 4;  // 4 evals = 200 ms
  static constexpr int kOpenStreak = 2;   // 2 evals = 100 ms (fast path 60ms
                                          // handled by 8-frame fast window)

  struct Stats {
    bool closed = false;          // true => suppress turn-taking endorsement
    float corr = 0.f;             // best masked envelope correlation
    int lag_ms = 0;               // at best lag
    float slow_excess_db = 99.f;  // mic energy above learned echo prediction
    float offset_db = 0.f;        // learned render->mic energy transfer
    int64_t evals = 0;
    int64_t closed_evals = 0;
  };

  SelfEchoGate();

  // Render side (playout thread): interleaved S16 as sent to the device.
  void PushRender(const int16_t* samples, size_t frames, uint32_t rate_hz,
                  size_t channels);
  // Capture side (capture thread): post-APM 10 ms frames headed to encoder.
  void PushCapture(const int16_t* samples, size_t frames, uint32_t rate_hz,
                   size_t channels);

  Stats GetStats() const;

 private:
  struct Biquad {
    float b0, b1, b2, a1, a2;
    float z1 = 0.f, z2 = 0.f;
    inline float Process(float x) {
      const float y = b0 * x + z1;
      z1 = b1 * x - a1 * y + z2;
      z2 = b2 * x - a2 * y;
      return y;
    }
  };
  struct Bank {
    std::array<Biquad, kBands> f;
    uint32_t rate = 0;
  };
  void InitBank(Bank& bank, uint32_t rate_hz);
  // Computes one 12-band log-energy envelope frame; returns mean level.
  float Envelope(Bank& bank, const int16_t* x, size_t frames, size_t channels,
                 float* out_db);
  void Evaluate() RTC_EXCLUSIVE_LOCKS_REQUIRED(lock_);
  bool RenderActiveAt(int64_t render_frames, int back) const
      RTC_EXCLUSIVE_LOCKS_REQUIRED(lock_);

  mutable Mutex lock_;
  Bank render_bank_ RTC_GUARDED_BY(lock_);
  Bank capture_bank_ RTC_GUARDED_BY(lock_);
  // Ring buffers of envelope frames.
  float ren_env_[kRing][kBands] RTC_GUARDED_BY(lock_);
  float mic_env_[kRing][kBands] RTC_GUARDED_BY(lock_);
  float ren_mean_[kRing] RTC_GUARDED_BY(lock_);
  float mic_mean_[kRing] RTC_GUARDED_BY(lock_);
  bool ren_act_[kRing] RTC_GUARDED_BY(lock_);
  int64_t ren_frames_ RTC_GUARDED_BY(lock_) = 0;
  int64_t mic_frames_ RTC_GUARDED_BY(lock_) = 0;
  float ren_peak_db_ RTC_GUARDED_BY(lock_) = -100.f;
  float mic_noise_db_ RTC_GUARDED_BY(lock_) = -60.f;
  // Learned broadband render->mic energy offset (dB), EWMA.
  bool have_offset_ RTC_GUARDED_BY(lock_) = false;
  float offset_db_ RTC_GUARDED_BY(lock_) = 0.f;
  // State machine.
  bool closed_ RTC_GUARDED_BY(lock_) = false;
  int ok_streak_ RTC_GUARDED_BY(lock_) = 0;
  int open_evid_ RTC_GUARDED_BY(lock_) = 0;
  Stats stats_ RTC_GUARDED_BY(lock_);
};

}  // namespace webrtc
#endif  // INTERNAL_SELF_ECHO_GATE_H_
