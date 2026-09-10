#ifndef LIB_WEBRTC_RTC_AUDIO_PROCESSING_IMPL_HXX
#define LIB_WEBRTC_RTC_AUDIO_PROCESSING_IMPL_HXX

#include <memory>

#include "modules/audio_processing/include/audio_processing.h"
#include "rtc_audio_processing.h"
#include "src/internal/audio_clock_correction.h"
#include "rtc_base/synchronization/mutex.h"

namespace libwebrtc {

class CustomProcessingAdapter;

class RTCAudioProcessingImpl : public RTCAudioProcessing {
 public:
  explicit RTCAudioProcessingImpl(bool clock_correction_supported = false);
  const std::shared_ptr<AudioClockCorrection>& clock_correction() const {
    return clock_correction_;
  }
  ~RTCAudioProcessingImpl();
  void SetCapturePostProcessing(
      RTCAudioProcessing::CustomProcessing* capture_post_processing) override;

  void SetRenderPreProcessing(
      RTCAudioProcessing::CustomProcessing* render_pre_processing) override;

  int32_t ApplyCaptureProfile(
      const RTCAudioProcessing::Profile& profile) override;

  RTCAudioProcessing::ProcessingState GetCaptureProcessingState() override;

  virtual webrtc::scoped_refptr<webrtc::AudioProcessing> GetAudioProcessing() {
    return apm_;
  }

 private:
  const std::shared_ptr<AudioClockCorrection> clock_correction_;
  CustomProcessingAdapter* capture_post_processor_;
  CustomProcessingAdapter* render_pre_processor_;
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm_;
  webrtc::Mutex profile_mutex_;
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_RTC_AUDIO_PROCESSING_IMPL_HXX
