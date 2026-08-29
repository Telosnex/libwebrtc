#include "src/internal/hardware_clock_estimator.h"

#include <algorithm>
#include <cmath>

namespace webrtc {

void HardwareClockEstimator::Track::Clear() {
  begin = 0;
  size = 0;
  sample_rate_hz = 0;
  generation = 0;
  initialized = false;
}

void HardwareClockEstimator::Track::Push(Point point) {
  if (size == kMaxPoints) {
    begin = (begin + 1) % kMaxPoints;
    --size;
  }
  At(size++) = point;
}

void HardwareClockEstimator::Track::DropBefore(int64_t minimum_time_ns) {
  while (size > 1 && At(1).time_ns < minimum_time_ns) {
    begin = (begin + 1) % kMaxPoints;
    --size;
  }
}

HardwareClockEstimator::TrackResult HardwareClockEstimator::AddToTrack(
    Track* track, const AudioHardwareClockObservation& observation) {
  const Point point = {observation.monotonic_time_ns,
                       observation.position_frames};
  if (!track->initialized) {
    track->Clear();
    track->initialized = true;
    track->sample_rate_hz = observation.sample_rate_hz;
    track->generation = observation.generation;
    track->Push(point);
    return TrackResult::kAccepted;
  }

  if (observation.generation != track->generation ||
      observation.sample_rate_hz != track->sample_rate_hz) {
    track->Clear();
    track->initialized = true;
    track->sample_rate_hz = observation.sample_rate_hz;
    track->generation = observation.generation;
    track->Push(point);
    return TrackResult::kReset;
  }

  if (observation.monotonic_time_ns <= track->Back().time_ns ||
      observation.position_frames < track->Back().position_frames) {
    track->Clear();
    track->initialized = true;
    track->sample_rate_hz = observation.sample_rate_hz;
    track->generation = observation.generation;
    track->Push(point);
    return TrackResult::kRejected;
  }

  track->Push(point);
  track->DropBefore(observation.monotonic_time_ns -
                    static_cast<int64_t>(kMaxSpanSeconds * 1e9));
  return TrackResult::kAccepted;
}

std::optional<HardwareClockEstimator::Fit> HardwareClockEstimator::FitTrack(
    const Track& track, int64_t start_ns, int64_t end_ns) {
  if (!track.initialized || !track.sample_rate_hz || start_ns >= end_ns)
    return std::nullopt;

  size_t first = 0;
  while (first < track.size && track.At(first).time_ns < start_ns) ++first;
  size_t last = first;
  while (last < track.size && track.At(last).time_ns <= end_ns) ++last;
  const size_t count = last - first;
  if (count < 20) return std::nullopt;

  const int64_t base_time_ns = track.At(first).time_ns;
  const int64_t base_position = track.At(first).position_frames;
  double sum_x = 0.0;
  double sum_y = 0.0;
  for (size_t i = first; i < last; ++i) {
    const double x =
        static_cast<double>(track.At(i).time_ns - base_time_ns) * 1e-9;
    const double y =
        static_cast<double>(track.At(i).position_frames - base_position) /
        track.sample_rate_hz;
    sum_x += x;
    sum_y += y;
  }
  const double mean_x = sum_x / count;
  const double mean_y = sum_y / count;
  double sxx = 0.0;
  double sxy = 0.0;
  for (size_t i = first; i < last; ++i) {
    const double x =
        static_cast<double>(track.At(i).time_ns - base_time_ns) * 1e-9;
    const double y =
        static_cast<double>(track.At(i).position_frames - base_position) /
        track.sample_rate_hz;
    sxx += (x - mean_x) * (x - mean_x);
    sxy += (x - mean_x) * (y - mean_y);
  }
  if (sxx <= 0.0) return std::nullopt;
  const double slope = sxy / sxx;
  const double intercept = mean_y - slope * mean_x;
  double squared_error = 0.0;
  for (size_t i = first; i < last; ++i) {
    const double x =
        static_cast<double>(track.At(i).time_ns - base_time_ns) * 1e-9;
    const double y =
        static_cast<double>(track.At(i).position_frames - base_position) /
        track.sample_rate_hz;
    const double residual = y - (intercept + slope * x);
    squared_error += residual * residual;
  }

  Fit fit;
  fit.slope = static_cast<double>(slope);
  fit.points = count;
  fit.rms_residual_seconds =
      std::sqrt(static_cast<double>(squared_error / count));
  if (count > 2) {
    fit.slope_standard_error =
        std::sqrt(static_cast<double>((squared_error / (count - 2)) / sxx));
  }
  return fit;
}

std::optional<HardwareClockEstimator::Estimate>
HardwareClockEstimator::EstimateIfDue(int64_t now_ns) {
  if (last_estimate_ns_ && now_ns - last_estimate_ns_ < kUpdateIntervalNs)
    return std::nullopt;
  if (!playout_.initialized || !capture_.initialized) return std::nullopt;

  const int64_t start_ns =
      std::max(playout_.Front().time_ns, capture_.Front().time_ns);
  const int64_t end_ns =
      std::min(playout_.Back().time_ns, capture_.Back().time_ns);
  const double span_seconds = (end_ns - start_ns) * 1e-9;
  if (span_seconds < kMinSpanSeconds) return std::nullopt;

  const auto playout_fit = FitTrack(playout_, start_ns, end_ns);
  const auto capture_fit = FitTrack(capture_, start_ns, end_ns);
  if (!playout_fit || !capture_fit) return std::nullopt;
  if (playout_fit->slope < 0.98 || playout_fit->slope > 1.02 ||
      capture_fit->slope < 0.98 || capture_fit->slope > 1.02 ||
      playout_fit->rms_residual_seconds > kMaxFitResidualSeconds ||
      capture_fit->rms_residual_seconds > kMaxFitResidualSeconds) {
    ++rejected_;
    last_estimate_ns_ = now_ns;
    return std::nullopt;
  }

  const double relative_ratio = capture_fit->slope / playout_fit->slope;
  const double relative_standard_error = std::sqrt(
      std::pow(capture_fit->slope_standard_error / capture_fit->slope, 2) +
      std::pow(playout_fit->slope_standard_error / playout_fit->slope, 2));
  Estimate estimate;
  estimate.relative_ppm = (relative_ratio - 1.0) * 1e6;
  estimate.uncertainty_ppm =
      std::max(kMinUncertaintyPpm, 6.0e6 * relative_standard_error);
  estimate.span_seconds = span_seconds;
  estimate.playout_points = playout_fit->points;
  estimate.capture_points = capture_fit->points;
  last_estimate_ns_ = now_ns;
  return estimate;
}

HardwareClockEstimator::Update HardwareClockEstimator::Add(
    const AudioHardwareClockObservation& observation) {
  Update update;
  if (observation.monotonic_time_ns <= 0 || observation.position_frames < 0 ||
      observation.sample_rate_hz == 0) {
    ++rejected_;
    update.rejected = true;
    return update;
  }
  Track* track = observation.direction == AudioHardwareClockDirection::kPlayout
                     ? &playout_
                     : &capture_;
  const TrackResult result = AddToTrack(track, observation);
  if (result == TrackResult::kReset) {
    ++resets_;
    last_estimate_ns_ = 0;
    update.reset = true;
  } else if (result == TrackResult::kRejected) {
    ++resets_;
    ++rejected_;
    last_estimate_ns_ = 0;
    update.reset = true;
    update.rejected = true;
  }
  update.estimate = EstimateIfDue(observation.monotonic_time_ns);
  return update;
}

void HardwareClockEstimator::Reset() {
  playout_.Clear();
  capture_.Clear();
  last_estimate_ns_ = 0;
}

}  // namespace webrtc
