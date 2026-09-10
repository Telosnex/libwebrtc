#ifndef LIB_WEBRTC_CAPTURE_CLOCK_POLICY_H_
#define LIB_WEBRTC_CAPTURE_CLOCK_POLICY_H_

#include <memory>
#include <mutex>

namespace libwebrtc {
// Testable policy/ownership seam. A callback holds a shared snapshot; replacing
// the servo before capture cannot race render/hardware observers or free their
// instance. The first capture callback freezes configuration for this factory
// lifetime, including across RTP attach/detach and peerless capture restarts.
template <typename Servo>
class CaptureClockPolicy {
 public:
  struct Snapshot {
    int mode;
    bool observe_only;
    bool capture_started;
    std::shared_ptr<Servo> servo;
  };
  CaptureClockPolicy(int mode, std::shared_ptr<Servo> servo)
      : mode_(mode), servo_(std::move(servo)) {}

  template <typename Create>
  int Configure(int mode, Create create) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (mode < 0 || mode > 2) return -1;
    // Preserve estimator/FIFO state on repeated configuration. Explicit
    // observe is strictly bypass-only even if the legacy env mode wasn't.
    if (mode == mode_ && (mode != 1 || observe_only_)) return 0;
    if (capture_started_) return -2;
    servo_ = create();
    mode_ = mode;
    observe_only_ = mode == 1;
    return 0;
  }

  Snapshot Read(bool capture = false) {
    std::lock_guard<std::mutex> lock(mutex_);
    capture_started_ |= capture;
    return {mode_, observe_only_, capture_started_, servo_};
  }

 private:
  std::mutex mutex_;
  int mode_;
  bool observe_only_ = false;  // preserve legacy environment semantics
  bool capture_started_ = false;
  std::shared_ptr<Servo> servo_;
};
}  // namespace libwebrtc
#endif
