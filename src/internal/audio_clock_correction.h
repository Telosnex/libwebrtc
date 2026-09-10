#ifndef LIB_WEBRTC_AUDIO_CLOCK_CORRECTION_INTERNAL_H_
#define LIB_WEBRTC_AUDIO_CLOCK_CORRECTION_INTERNAL_H_

#include "rtc_audio_clock_correction.h"
#include "src/internal/capture_clock_policy.h"
#include "src/internal/drift_servo.h"

namespace libwebrtc {
class AudioClockCorrection {
 public:
  using Policy = CaptureClockPolicy<webrtc::DriftServo>;
  explicit AudioClockCorrection(bool supported);
  int Configure(AudioClockCorrectionMode mode);
  AudioClockCorrectionState GetState();
  Policy::Snapshot Read(bool capture = false) { return policy_->Read(capture); }
  double seed_ppm() const { return seed_ppm_; }

 private:
  const bool supported_;
  double seed_ppm_ = 0;
  std::unique_ptr<Policy> policy_;
};
}  // namespace libwebrtc
#endif
