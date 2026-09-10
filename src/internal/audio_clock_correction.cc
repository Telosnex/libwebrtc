#include "src/internal/audio_clock_correction.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace libwebrtc {
AudioClockCorrection::AudioClockCorrection(bool supported)
    : supported_(supported) {
  // Backwards compatibility only. Explicit profile configuration takes
  // precedence; omission leaves the existing deployment behavior intact.
  int mode = 0;
  const char* drift = std::getenv("TSNX_DRIFT_SERVO");
  if (drift && drift[0] == '1') mode = 3;
  const char* hw = std::getenv("TSNX_HW_CLOCK_SERVO");
  if (hw && hw[0]) {
    if (strcmp(hw, "observe") == 0) mode = 1;
    else if (strcmp(hw, "control") == 0 || strcmp(hw, "1") == 0) mode = 2;
    else fprintf(stderr, "TSNX: invalid TSNX_HW_CLOCK_SERVO=%s\n", hw);
  }
  std::shared_ptr<webrtc::DriftServo> servo;
  if (mode != 0) {
    fprintf(stderr, "TSNX: drift servo enabled (legacy environment mode=%d)\n", mode);
    servo = std::make_shared<webrtc::DriftServo>();
    servo->SetHardwareClockMode(
        mode == 1 ? webrtc::DriftServo::HardwareClockMode::kObserve
        : mode == 2 ? webrtc::DriftServo::HardwareClockMode::kControl
                    : webrtc::DriftServo::HardwareClockMode::kDisabled);
    if (const char* seed = std::getenv("TSNX_DRIFT_PPM"); seed && seed[0]) {
      seed_ppm_ = atof(seed);
      servo->SeedRatio(seed_ppm_);
    }
  }
  policy_ = std::make_unique<Policy>(mode, std::move(servo));
}

int AudioClockCorrection::Configure(AudioClockCorrectionMode mode) {
  const int value = static_cast<int>(mode);
  if (value < 0 || value > 2 || (!supported_ && value != 0)) return -1;
  return policy_->Configure(value, [mode]() -> std::shared_ptr<webrtc::DriftServo> {
    if (mode == AudioClockCorrectionMode::kOff) return nullptr;
    auto servo = std::make_shared<webrtc::DriftServo>();
    servo->SetHardwareClockMode(mode == AudioClockCorrectionMode::kControl
        ? webrtc::DriftServo::HardwareClockMode::kControl
        : webrtc::DriftServo::HardwareClockMode::kObserve);
    return servo;
  });
}

AudioClockCorrectionState AudioClockCorrection::GetState() {
  const auto snapshot = Read();
  AudioClockCorrectionState state;
  state.mode = snapshot.mode;
  state.supported = supported_;
  state.capture_started = snapshot.capture_started;
  if (snapshot.servo) {
    const auto stats = snapshot.servo->GetStats();
    state.engaged = !snapshot.observe_only && stats.engaged;
    state.hardware_ready = stats.hardware_ready;
    state.hardware_controlling = !snapshot.observe_only && stats.hardware_controlling;
    state.applied_ppm = snapshot.observe_only ? 0 : stats.applied_ppm;
  }
  return state;
}
}  // namespace libwebrtc
