#ifndef LIB_WEBRTC_RTC_AUDIO_PROCESSING_HXX
#define LIB_WEBRTC_RTC_AUDIO_PROCESSING_HXX

#include "rtc_types.h"

namespace libwebrtc {

class RTCAudioProcessing : public RefCountInterface {
 public:
  struct Profile {
    bool echo_cancellation = true;
    bool noise_suppression = true;
    bool auto_gain_control = true;
    bool high_pass_filter = true;
  };

  struct ComponentState {
    bool software_active = false;
  };

  struct ProcessingState {
    bool has_audio_processing_module = false;
    ComponentState echo_cancellation;
    ComponentState noise_suppression;
    ComponentState auto_gain_control;
    ComponentState high_pass_filter;
  };

  class CustomProcessing {
   public:
    virtual void Initialize(int sample_rate_hz, int num_channels) = 0;

    virtual void Process(int num_bands, int num_frames, int buffer_size,
                         float* buffer) = 0;

    virtual void Reset(int new_rate) = 0;

    virtual void Release() = 0;

   protected:
    virtual ~CustomProcessing() {}
  };

 public:
  virtual void SetCapturePostProcessing(
      CustomProcessing* capture_post_processing) = 0;

  virtual void SetRenderPreProcessing(
      CustomProcessing* render_pre_processing) = 0;

  /** Applies the app-owned capture profile directly to the shared APM. */
  virtual int32_t ApplyCaptureProfile(const Profile& profile) = 0;

  /** Reads back the live software configuration from the shared APM. */
  virtual ProcessingState GetCaptureProcessingState() = 0;
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_RTC_AUDIO_PROCESSING_HXX