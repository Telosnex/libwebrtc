#ifndef LIB_WEBRTC_RTC_AUDIO_CLOCK_CORRECTION_H_
#define LIB_WEBRTC_RTC_AUDIO_CLOCK_CORRECTION_H_

#include "rtc_audio_processing.h"

// Versioned, additive API: no changes to the existing Profile/ProcessingState
// layouts or RTCAudioProcessing vtable. Consumers must use matching new headers
// and binaries. Plugins built with older headers report unsupported instead.
namespace libwebrtc {
enum class AudioClockCorrectionMode { kOff = 0, kObserve = 1, kControl = 2 };
struct AudioClockCorrectionState {
  // 0 off, 1 observe, 2 control, 3 legacy callback-only environment policy.
  int mode = 0;
  bool supported = false;  // hardware producer currently available only on ALSA
  bool capture_started = false;
  bool engaged = false;
  bool hardware_ready = false;
  bool hardware_controlling = false;
  double applied_ppm = 0;
};

// processing must come from this library's RTCPeerConnectionFactory.
// Configure before the first capture callback in this factory lifetime. Same
// policy is idempotent afterward; conflicting changes return -2. No capture,
// playout, RTP sender or ADM restart is performed. -1 means unsupported/invalid.
LIB_PORTABLE_API int ConfigureAudioClockCorrectionV1(
    RTCAudioProcessing* processing, AudioClockCorrectionMode mode);
LIB_PORTABLE_API AudioClockCorrectionState GetAudioClockCorrectionStateV1(
    RTCAudioProcessing* processing);
}  // namespace libwebrtc
#endif
