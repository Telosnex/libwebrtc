// Copyright (c) Telosnex. Hardware-position audio clock estimator.
//
// Fits capture and playout hardware positions independently against the same
// monotonic clock, then returns their normalized rate ratio. Fixed-size rings
// make observation ingestion allocation-free on native audio threads.
#ifndef INTERNAL_HARDWARE_CLOCK_ESTIMATOR_H_
#define INTERNAL_HARDWARE_CLOCK_ESTIMATOR_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

#include "api/audio/audio_device_defines.h"

namespace webrtc {

class HardwareClockEstimator {
 public:
  struct Estimate {
    double relative_ppm = 0.0;
    double uncertainty_ppm = 0.0;  // conservative six-sigma fit uncertainty
    double span_seconds = 0.0;
    size_t playout_points = 0;
    size_t capture_points = 0;
  };

  struct Update {
    std::optional<Estimate> estimate;
    bool reset = false;
    bool rejected = false;
  };

  Update Add(const AudioHardwareClockObservation& observation);
  void Reset();

  int64_t resets() const { return resets_; }
  int64_t rejected() const { return rejected_; }

  static constexpr double kMinSpanSeconds = 5.0;
  static constexpr double kMaxSpanSeconds = 8.0;
  static constexpr int64_t kUpdateIntervalNs = 250000000LL;
  static constexpr double kMaxFitResidualSeconds = 0.003;
  static constexpr double kMinUncertaintyPpm = 20.0;

 private:
  struct Point {
    int64_t time_ns = 0;
    int64_t position_frames = 0;
  };

  static constexpr size_t kMaxPoints = 1024;
  struct Track {
    std::array<Point, kMaxPoints> points;
    size_t begin = 0;
    size_t size = 0;
    uint32_t sample_rate_hz = 0;
    uint32_t generation = 0;
    bool initialized = false;

    const Point& At(size_t index) const {
      return points[(begin + index) % kMaxPoints];
    }
    Point& At(size_t index) { return points[(begin + index) % kMaxPoints]; }
    const Point& Front() const { return At(0); }
    const Point& Back() const { return At(size - 1); }
    void Clear();
    void Push(Point point);
    void DropBefore(int64_t minimum_time_ns);
  };

  struct Fit {
    double slope = 0.0;  // nominal device seconds / monotonic second
    double slope_standard_error = 0.0;
    double rms_residual_seconds = 0.0;
    size_t points = 0;
  };

  enum class TrackResult { kAccepted, kReset, kRejected };
  static TrackResult AddToTrack(
      Track* track, const AudioHardwareClockObservation& observation);
  static std::optional<Fit> FitTrack(const Track& track, int64_t start_ns,
                                     int64_t end_ns);
  std::optional<Estimate> EstimateIfDue(int64_t now_ns);

  Track playout_;
  Track capture_;
  int64_t last_estimate_ns_ = 0;
  int64_t resets_ = 0;
  int64_t rejected_ = 0;
};

}  // namespace webrtc
#endif  // INTERNAL_HARDWARE_CLOCK_ESTIMATOR_H_
