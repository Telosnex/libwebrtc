#include "rtc_audio_processing_impl.h"

#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
#include "modules/audio_processing/audio_buffer.h"
#include "modules/audio_processing/ns/ns_common.h"
#include "rtc_base/logging.h"
#include "rtc_base/synchronization/mutex.h"

namespace libwebrtc {

class CustomProcessingAdapter : public webrtc::CustomProcessing {
 public:
  CustomProcessingAdapter() = default;
  ~CustomProcessingAdapter() override {
    SetExternalAudioProcessing(nullptr);
  }

  void SetExternalAudioProcessing(
      RTCAudioProcessing::CustomProcessing* processor) {
    RTCAudioProcessing::CustomProcessing* previous = nullptr;
    {
      webrtc::MutexLock lock(&mutex_);
      if (custom_processor_ == processor) {
        return;
      }
      previous = custom_processor_;
      custom_processor_ = processor;
      if (custom_processor_ && initialized_) {
        custom_processor_->Initialize(sample_rate_hz_, num_channels_);
      }
    }
    if (previous) {
      previous->Release();
    }
  }

  void SetBypassFlag(bool bypass) {
    webrtc::MutexLock lock(&mutex_);
    bypass_flag_ = bypass;
  }

 private:
  void Initialize(int sample_rate_hz, int num_channels) override {
    webrtc::MutexLock lock(&mutex_);
    sample_rate_hz_ = sample_rate_hz;
    num_channels_ = num_channels;
    if (custom_processor_) {
      custom_processor_->Initialize(sample_rate_hz, num_channels);
    }
    initialized_ = true;
  }

  void Process(webrtc::AudioBuffer* audio) override {
    webrtc::MutexLock lock(&mutex_);
    if (!custom_processor_ || bypass_flag_ || !initialized_) {
      return;
    }

    size_t num_frames = audio->num_frames();
    size_t num_bands = audio->num_bands();

    // 1 buffer = 10ms of frames
    int rate = num_frames * 100;

    if (rate != sample_rate_hz_) {
      custom_processor_->Reset(rate);
      sample_rate_hz_ = rate;
    }

    custom_processor_->Process(num_bands, num_frames,
                               webrtc::kNsFrameSize * num_bands,
                               audio->channels()[0]);
  }

  std::string ToString() const override { return "ExternalAudioProcessor"; }
  void SetRuntimeSetting(
      webrtc::AudioProcessing::RuntimeSetting setting) override {}

 private:
  mutable webrtc::Mutex mutex_;
  RTCAudioProcessing::CustomProcessing* custom_processor_ = nullptr;
  bool bypass_flag_ = false;
  bool initialized_ = false;
  int sample_rate_hz_ = 0;
  int num_channels_ = 0;
};

RTCAudioProcessingImpl::RTCAudioProcessingImpl(bool clock_correction_supported)
    : clock_correction_(std::make_shared<AudioClockCorrection>(
          clock_correction_supported)) {
  capture_post_processor_ = new CustomProcessingAdapter();
  std::unique_ptr<webrtc::CustomProcessing> capture_post_processor(
      capture_post_processor_);

  render_pre_processor_ = new CustomProcessingAdapter();
  std::unique_ptr<webrtc::CustomProcessing> render_pre_processor(
      render_pre_processor_);

  apm_ = webrtc::BuiltinAudioProcessingBuilder()
             .SetCapturePostProcessing(std::move(capture_post_processor))
             .SetRenderPreProcessing(std::move(render_pre_processor))
             .Build(webrtc::CreateEnvironment());

  webrtc::AudioProcessing::Config config;
  apm_->ApplyConfig(config);
}

RTCAudioProcessingImpl::~RTCAudioProcessingImpl() {}

void RTCAudioProcessingImpl::SetCapturePostProcessing(
    RTCAudioProcessing::CustomProcessing* processor) {
  capture_post_processor_->SetExternalAudioProcessing(processor);
}

void RTCAudioProcessingImpl::SetRenderPreProcessing(
    RTCAudioProcessing::CustomProcessing* processor) {
  render_pre_processor_->SetExternalAudioProcessing(processor);
}

int32_t RTCAudioProcessingImpl::ApplyCaptureProfile(
    const RTCAudioProcessing::Profile& profile) {
  webrtc::MutexLock lock(&profile_mutex_);
  webrtc::AudioProcessing::Config config = apm_->GetConfig();
  config.echo_canceller.enabled = profile.echo_cancellation;
  config.noise_suppression.enabled = profile.noise_suppression;
  config.noise_suppression.level =
      webrtc::AudioProcessing::Config::NoiseSuppression::Level::kHigh;
  config.gain_controller1.enabled = profile.auto_gain_control;
  config.gain_controller1.mode =
      webrtc::AudioProcessing::Config::GainController1::kAdaptiveAnalog;
  config.high_pass_filter.enabled = profile.high_pass_filter;
  apm_->ApplyConfig(config);
  return 0;
}

RTCAudioProcessing::ProcessingState
RTCAudioProcessingImpl::GetCaptureProcessingState() {
  webrtc::MutexLock lock(&profile_mutex_);
  webrtc::AudioProcessing::Config config = apm_->GetConfig();
  ProcessingState state;
  state.has_audio_processing_module = apm_ != nullptr;
  auto component = [](bool active) {
    ComponentState state;
    state.software_active = active;
    return state;
  };
  state.echo_cancellation = component(config.echo_canceller.enabled);
  state.noise_suppression = component(config.noise_suppression.enabled);
  state.auto_gain_control = component(
      config.gain_controller1.enabled || config.gain_controller2.enabled);
  state.high_pass_filter = component(config.high_pass_filter.enabled);
  return state;
}

int ConfigureAudioClockCorrectionV1(RTCAudioProcessing* processing,
                                   AudioClockCorrectionMode mode) {
  if (!processing) return -1;
  return static_cast<RTCAudioProcessingImpl*>(processing)
      ->clock_correction()->Configure(mode);
}

AudioClockCorrectionState GetAudioClockCorrectionStateV1(
    RTCAudioProcessing* processing) {
  if (!processing) return {};
  return static_cast<RTCAudioProcessingImpl*>(processing)
      ->clock_correction()->GetState();
}

}  // namespace libwebrtc
