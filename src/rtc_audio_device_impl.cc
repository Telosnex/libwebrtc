#include "rtc_audio_device_impl.h"

#include <cstring>
#include <string>

#include "rtc_base/logging.h"

namespace libwebrtc {

AudioDeviceImpl::AudioDeviceImpl(
    webrtc::scoped_refptr<webrtc::AudioDeviceModule> audio_device_module,
    webrtc::Thread* worker_thread)
    : audio_device_module_(audio_device_module), worker_thread_(worker_thread) {
  audio_device_module_->SetObserver(this);
}

AudioDeviceImpl::~AudioDeviceImpl() {
  RTC_LOG(LS_INFO) << __FUNCTION__ << ": dtor ";
}

int16_t AudioDeviceImpl::PlayoutDevices() {
  return audio_device_module_->PlayoutDevices();
}

int16_t AudioDeviceImpl::RecordingDevices() {
  return audio_device_module_->RecordingDevices();
}

int32_t AudioDeviceImpl::PlayoutDeviceName(uint16_t index,
                                           char name[kAdmMaxDeviceNameSize],
                                           char guid[kAdmMaxGuidSize]) {
  return worker_thread_->BlockingCall([&] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    return audio_device_module_->PlayoutDeviceName(index, name, guid);
  });
}

int32_t AudioDeviceImpl::RecordingDeviceName(uint16_t index,
                                             char name[kAdmMaxDeviceNameSize],
                                             char guid[kAdmMaxGuidSize]) {
  return worker_thread_->BlockingCall([&] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    return audio_device_module_->RecordingDeviceName(index, name, guid);
  });
}

int32_t AudioDeviceImpl::SetPlayoutDevice(uint16_t index) {
  return worker_thread_->BlockingCall([this, index] {
    RTC_DCHECK_RUN_ON(worker_thread_);

    char name[kAdmMaxDeviceNameSize] = {0};
    char guid[kAdmMaxGuidSize] = {0};
    const int32_t name_result =
        audio_device_module_->PlayoutDeviceName(index, name, guid);
    const bool was_playing = audio_device_module_->Playing();
    if (was_playing) {
      const int32_t stop_result = audio_device_module_->StopPlayout();
      if (stop_result != 0) return stop_result;
    }

    const int32_t set_result = audio_device_module_->SetPlayoutDevice(index);
    if (set_result == 0) {
#if defined(WEBRTC_LINUX)
      // Pulse/ALSA reserve index zero for the moving system-default route.
      selected_playout_device_id_ = index == 0 || name_result != 0
                                        ? std::string()
                                        : std::string(guid[0] ? guid : name);
#else
      selected_playout_device_id_ =
          name_result == 0 ? std::string(guid[0] ? guid : name) : std::string();
#endif
    }

    int32_t restart_result = 0;
    if (was_playing) {
      restart_result = audio_device_module_->InitPlayout();
      if (restart_result == 0) {
        restart_result = audio_device_module_->StartPlayout();
      }
    }
    return set_result != 0 ? set_result : restart_result;
  });
}

int32_t AudioDeviceImpl::SetRecordingDevice(uint16_t index) {
  return worker_thread_->BlockingCall([this, index] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    const bool was_recording = audio_device_module_->Recording();
    if (was_recording) {
      const int32_t stop_result = audio_device_module_->StopRecording();
      if (stop_result != 0) return stop_result;
    }

    const int32_t set_result = audio_device_module_->SetRecordingDevice(index);
    int32_t restart_result = 0;
    if (was_recording) {
      restart_result = audio_device_module_->InitRecording();
      if (restart_result == 0) {
        restart_result = audio_device_module_->StartRecording();
      }
    }
    return set_result != 0 ? set_result : restart_result;
  });
}

int32_t AudioDeviceImpl::SetMicrophoneVolume(uint32_t volume) {
  return worker_thread_->BlockingCall([&, volume] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    return audio_device_module_->SetMicrophoneVolume(volume);
  });
}

int32_t AudioDeviceImpl::MicrophoneVolume(uint32_t& volume) {
  uint32_t* volume_ = &volume;
  return worker_thread_->BlockingCall([&, volume_] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    return audio_device_module_->MicrophoneVolume(volume_);
  });
}

int32_t AudioDeviceImpl::SetSpeakerVolume(uint32_t volume) {
  return worker_thread_->BlockingCall([&, volume] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    return audio_device_module_->SetSpeakerVolume(volume);
  });
}

int32_t AudioDeviceImpl::SpeakerVolume(uint32_t& volume) {
  uint32_t* volume_ = &volume;
  return worker_thread_->BlockingCall([&, volume_] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    return audio_device_module_->SpeakerVolume(volume_);
  });
}

int32_t AudioDeviceImpl::AcquireRecording() {
  return worker_thread_->BlockingCall([&] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    return audio_device_module_->AcquireExternalRecording();
  });
}

int32_t AudioDeviceImpl::ReleaseRecording() {
  return worker_thread_->BlockingCall([&] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    return audio_device_module_->ReleaseExternalRecording();
  });
}

RTCAudioDevice::RecordingState AudioDeviceImpl::GetRecordingState() {
  return worker_thread_->BlockingCall([&] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    RecordingState state;
    state.initialized = audio_device_module_->RecordingIsInitialized();
    state.recording = audio_device_module_->Recording();
    state.external_demand = audio_device_module_->ExternalRecordingDemand();
    return state;
  });
}

int32_t AudioDeviceImpl::ActivePlayoutDeviceName(
    char name[kAdmMaxDeviceNameSize], char guid[kAdmMaxGuidSize]) {
  return worker_thread_->BlockingCall([&] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    if (name == nullptr) return -1;
    std::memset(name, 0, kAdmMaxDeviceNameSize);
    if (guid != nullptr) std::memset(guid, 0, kAdmMaxGuidSize);

    const int16_t device_count = audio_device_module_->PlayoutDevices();
    if (device_count <= 0) return -1;

    if (!selected_playout_device_id_.empty()) {
      char candidate_name[kAdmMaxDeviceNameSize] = {0};
      char candidate_guid[kAdmMaxGuidSize] = {0};
      for (uint16_t index = 0; index < device_count; ++index) {
        if (audio_device_module_->PlayoutDeviceName(index, candidate_name,
                                                    candidate_guid) != 0) {
          continue;
        }
        const char* candidate_id =
            candidate_guid[0] ? candidate_guid : candidate_name;
        if (selected_playout_device_id_ == candidate_id) {
          std::strncpy(name, candidate_name, kAdmMaxDeviceNameSize - 1);
          if (guid != nullptr) {
            std::strncpy(guid, candidate_guid, kAdmMaxGuidSize - 1);
          }
          return 0;
        }
      }
      // The explicitly selected endpoint disappeared. The ADM falls back to
      // its platform default; do not silently jump back if it is reattached.
      selected_playout_device_id_.clear();
    }

#if defined(WEBRTC_WIN)
    constexpr uint16_t kDefaultPlayoutDevice =
        static_cast<uint16_t>(-1);  // Default communications endpoint.
#else
    constexpr uint16_t kDefaultPlayoutDevice = 0;
#endif
    return audio_device_module_->PlayoutDeviceName(kDefaultPlayoutDevice, name,
                                                   guid);
  });
}

int32_t AudioDeviceImpl::OnDeviceChange(OnDeviceChangeCallback listener) {
  listener_ = listener;
  return 0;
}

void AudioDeviceImpl::OnDevicesUpdated() {
  if (listener_) listener_();
}

}  // namespace libwebrtc
