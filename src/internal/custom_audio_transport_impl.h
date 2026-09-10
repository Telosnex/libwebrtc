#ifndef INTERNAL_CUSTOM_AUDIO_TRANSPORT_STATE_H_
#define INTERNAL_CUSTOM_AUDIO_TRANSPORT_STATE_H_

#include <atomic>
#include <map>
#include <memory>
#include <vector>

#include "api/sequence_checker.h"
#include "audio/audio_transport_impl.h"
#include "call/audio_sender.h"
#include "call/audio_state.h"
#include "rtc_base/containers/flat_set.h"
#include "rtc_base/ref_count.h"
#include "rtc_base/task_utils/repeating_task.h"
#include "rtc_base/thread_annotations.h"
#include "src/internal/audio_clock_correction.h"
#include "src/internal/self_echo_gate.h"
#include "src/internal/session_tap.h"

namespace webrtc {

class CustomAudioTransportImpl : public AudioTransport, public AudioSender {
 public:
  CustomAudioTransportImpl(
      AudioMixer* mixer, AudioProcessing* audio_processing,
      AsyncAudioProcessing::Factory* async_audio_processing_factory,
      std::shared_ptr<libwebrtc::AudioClockCorrection> clock_correction);
  ~CustomAudioTransportImpl() {}

  int32_t RecordedDataIsAvailable(const void* audioSamples, size_t nSamples,
                                  size_t nBytesPerSample, size_t nChannels,
                                  uint32_t samplesPerSec, uint32_t totalDelayMS,
                                  int32_t clockDrift, uint32_t currentMicLevel,
                                  bool keyPressed,
                                  uint32_t& newMicLevel) override;

  int32_t RecordedDataIsAvailable(
      const void* audioSamples, size_t nSamples, size_t nBytesPerSample,
      size_t nChannels, uint32_t samplesPerSec, uint32_t totalDelayMS,
      int32_t clockDrift, uint32_t currentMicLevel, bool keyPressed,
      uint32_t& newMicLevel,
      std::optional<int64_t> estimated_capture_time_ns) override;

  void OnAudioHardwareClockObservation(
      const AudioHardwareClockObservation& observation) override;

  int32_t NeedMorePlayData(size_t nSamples, size_t nBytesPerSample,
                           size_t nChannels, uint32_t samplesPerSec,
                           void* audioSamples, size_t& nSamplesOut,
                           int64_t* elapsed_time_ms,
                           int64_t* ntp_time_ms) override;

  void PullRenderData(int bits_per_sample, int sample_rate,
                      size_t number_of_channels, size_t number_of_frames,
                      void* audio_data, int64_t* elapsed_time_ms,
                      int64_t* ntp_time_ms) override;

  virtual void UpdateAudioSenders(std::vector<AudioSender*> senders,
                                  int send_sample_rate_hz,
                                  size_t send_num_channels) override;

  void AddAudioSender(AudioSender* sender);

  void RemoveAudioSender(AudioSender* sender);

  void SetStereoChannelSwapping(bool enable) override;

  void SendAudioData(std::unique_ptr<AudioFrame> audio_frame) override;

  // Diagnostics for wrapper/stats plumbing. Safe when features disabled.
  bool drift_servo_enabled() const {
    return clock_correction_->Read().servo != nullptr;
  }
  bool self_echo_gate_enabled() const { return self_echo_gate_ != nullptr; }
  DriftServo::Stats GetDriftServoStats() const;
  SelfEchoGate::Stats GetSelfEchoGateStats() const;

 private:
  std::unique_ptr<webrtc::AudioTransportImpl> audio_transport_impl_;
  // Shared factory-lifetime clock policy. Its servo is null when disabled;
  // self-echo diagnostics remain separately environment-gated.
  const std::shared_ptr<libwebrtc::AudioClockCorrection> clock_correction_;
  std::unique_ptr<SelfEchoGate> self_echo_gate_;
  std::vector<int16_t> servo_block_;
  std::atomic<int64_t> render_calls_{0};
  std::atomic<int64_t> cap_legacy_calls_{0};
  std::atomic<int64_t> cap_modern_calls_{0};
  double seed_ppm_env_ = 0.0;
  // Session tap v2 (TSNX_TAP_DIR): see session_tap.h for R1-R4.
  std::unique_ptr<SessionTap> tap_;
  mutable Mutex capture_lock_;
  std::vector<AudioSender*> audio_senders_ RTC_GUARDED_BY(capture_lock_);
};

class CustomAudioTransportFactory : public AudioTransportFactory {
 public:
  explicit CustomAudioTransportFactory(
      std::shared_ptr<libwebrtc::AudioClockCorrection> clock_correction)
      : clock_correction_(std::move(clock_correction)) {}
  ~CustomAudioTransportFactory() = default;
  std::unique_ptr<AudioTransport> Create(
      webrtc::AudioMixer* mixer, webrtc::AudioProcessing* audio_processing,
      webrtc::AsyncAudioProcessing::Factory* async_audio_processing_factory)
      override {
    std::unique_ptr<CustomAudioTransportImpl> transport =
        std::make_unique<CustomAudioTransportImpl>(
            mixer, audio_processing, async_audio_processing_factory,
            clock_correction_);

    audio_transport_impl_ = transport.get();
    return transport;
  }

  CustomAudioTransportImpl* audio_transport_impl() const {
    return audio_transport_impl_;
  }

 private:
  const std::shared_ptr<libwebrtc::AudioClockCorrection> clock_correction_;
  CustomAudioTransportImpl* audio_transport_impl_ = nullptr;
};

}  // namespace webrtc

#endif  // INTERNAL_CUSTOM_AUDIO_TRANSPORT_STATE_H_
