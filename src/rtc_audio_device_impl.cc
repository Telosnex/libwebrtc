#include "rtc_audio_device_impl.h"

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
  worker_thread_->PostTask([this, index] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    if (audio_device_module_->Playing()) {
      audio_device_module_->StopPlayout();
      audio_device_module_->SetPlayoutDevice(index);
      audio_device_module_->InitPlayout();
      audio_device_module_->StartPlayout();
    } else {
      audio_device_module_->SetPlayoutDevice(index);
    }
  });
  return 0;
}

int32_t AudioDeviceImpl::SetRecordingDevice(uint16_t index) {
  worker_thread_->PostTask([this, index] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    if (audio_device_module_->Recording()) {
      audio_device_module_->StopRecording();
      audio_device_module_->SetRecordingDevice(index);
      audio_device_module_->InitRecording();
      audio_device_module_->StartRecording();
    } else {
      audio_device_module_->SetRecordingDevice(index);
    }
  });
  return 0;
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
    int32_t result = audio_device_module_->SetExternalRecordingDemand(true);
    if (result == 0 && !audio_device_module_->RecordingIsInitialized()) {
      result = audio_device_module_->InitRecording();
    }
    if (result == 0 && !audio_device_module_->Recording()) {
      result = audio_device_module_->StartRecording();
    }
    if (result != 0) {
      audio_device_module_->SetExternalRecordingDemand(false);
    }
    return result;
  });
}

int32_t AudioDeviceImpl::ReleaseRecording() {
  return worker_thread_->BlockingCall([&] {
    RTC_DCHECK_RUN_ON(worker_thread_);
    int32_t demand_result =
        audio_device_module_->SetExternalRecordingDemand(false);
    int32_t stop_result = audio_device_module_->Recording()
                              ? audio_device_module_->StopRecording()
                              : 0;
    return demand_result != 0 ? demand_result : stop_result;
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

int32_t AudioDeviceImpl::OnDeviceChange(OnDeviceChangeCallback listener) {
  listener_ = listener;
  return 0;
}

void AudioDeviceImpl::OnDevicesUpdated() {
  if (listener_) listener_();
}

}  // namespace libwebrtc
