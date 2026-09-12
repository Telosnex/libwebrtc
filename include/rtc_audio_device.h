#ifndef LIB_WEBRTC_RTC_AUDIO_DEVICE_HXX
#define LIB_WEBRTC_RTC_AUDIO_DEVICE_HXX

#include "rtc_types.h"
#include <cstddef>
#include <cstdint>
#define LIBWEBRTC_PCM_PLAYOUT_V1 1

namespace libwebrtc {

/**
 * The RTCAudioDevice class is an abstract class used for managing the audio
 * devices used by WebRTC. It provides methods for device enumeration and
 * selection.
 */
class RTCAudioDevice : public RefCountInterface {
 public:
  typedef fixed_size_function<void()> OnDeviceChangeCallback;

  struct RecordingState {
    bool initialized = false;
    bool recording = false;
    bool external_demand = false;
  };

 public:
  struct PcmPlayoutState {
    int64_t generation = 0;
    int64_t epoch = 0;
    int64_t queued_frames = 0;
    int64_t accepted_frames = 0;
    int64_t consumed_frames = 0;
    int64_t discarded_frames = 0;
    int64_t render_callbacks = 0;
    int64_t underrun_callbacks = 0;
    bool playing = false;
    int delay_ms = -1; // ADM estimate, NOT hardware-played acknowledgement.
  };
  // V1: PCM16 little-endian, mono 24000 Hz, max 1s/write, max 5s backlog.
  // One owner per factory. Positive Start result is its generation.
  virtual int64_t StartPcmPlayout() = 0;
  virtual int WritePcmPlayout(int64_t generation, int64_t epoch, const uint8_t* bytes, size_t size) = 0;
  virtual int ClearPcmPlayout(int64_t generation, int64_t epoch) = 0;
  virtual int StopPcmPlayout(int64_t generation) = 0;
  virtual PcmPlayoutState GetPcmPlayoutState() = 0;

  static const int kAdmMaxDeviceNameSize = 128;
  static const int kAdmMaxFileNameSize = 512;
  static const int kAdmMaxGuidSize = 128;

 public:
  /**
   * Returns the number of playout devices available.
   *
   * @return int16_t - The number of playout devices available.
   */
  virtual int16_t PlayoutDevices() = 0;

  /**
   * Returns the number of recording devices available.
   *
   * @return int16_t - The number of recording devices available.
   */
  virtual int16_t RecordingDevices() = 0;

  /**
   * Retrieves the name and GUID of the specified playout device.
   *
   * @param index - The index of the device.
   * @param name - The device name.
   * @param guid - The device GUID.
   * @return int32_t - 0 if successful, otherwise an error code.
   */
  virtual int32_t PlayoutDeviceName(uint16_t index,
                                    char name[kAdmMaxDeviceNameSize],
                                    char guid[kAdmMaxGuidSize]) = 0;

  /**
   * Retrieves the name and GUID of the specified recording device.
   *
   * @param index - The index of the device.
   * @param name - The device name.
   * @param guid - The device GUID.
   * @return int32_t - 0 if successful, otherwise an error code.
   */
  virtual int32_t RecordingDeviceName(uint16_t index,
                                      char name[kAdmMaxDeviceNameSize],
                                      char guid[kAdmMaxGuidSize]) = 0;

  /**
   * Sets the playout device to use.
   *
   * @param index - The index of the device.
   * @return int32_t - 0 if successful, otherwise an error code.
   */
  virtual int32_t SetPlayoutDevice(uint16_t index) = 0;

  /**
   * Sets the recording device to use.
   *
   * @param index - The index of the device.
   * @return int32_t - 0 if successful, otherwise an error code.
   */
  virtual int32_t SetRecordingDevice(uint16_t index) = 0;

  /**
   * Registers a listener to be called when audio devices are added or removed.
   *
   * @param listener - The callback function to register.
   * @return int32_t - 0 if successful, otherwise an error code.
   */
  virtual int32_t OnDeviceChange(OnDeviceChangeCallback listener) = 0;

  virtual int32_t SetMicrophoneVolume(uint32_t volume) = 0;

  virtual int32_t MicrophoneVolume(uint32_t& volume) = 0;

  virtual int32_t SetSpeakerVolume(uint32_t volume) = 0;

  virtual int32_t SpeakerVolume(uint32_t& volume) = 0;

  /**
   * Acquires app-owned recording demand and starts the shared ADM recording
   * path. Safe to call from outside the WebRTC worker thread.
   */
  virtual int32_t AcquireRecording() = 0;

  /**
   * Releases app-owned recording demand and stops the ADM. Call only after all
   * peers borrowing the app-owned track have been disposed.
   */
  virtual int32_t ReleaseRecording() = 0;

  /** Returns one worker-thread-consistent recording state snapshot. */
  virtual RecordingState GetRecordingState() = 0;

  /**
   * Resolves the concrete output endpoint currently used for playout.
   *
   * When playout follows the system default, this resolves that default to its
   * current device name and stable identifier instead of returning a generic
   * "default" token.
   */
  virtual int32_t ActivePlayoutDeviceName(char name[kAdmMaxDeviceNameSize],
                                          char guid[kAdmMaxGuidSize]) = 0;

 protected:
  virtual ~RTCAudioDevice() {}
};

}  // namespace libwebrtc

#endif  // LIB_WEBRTC_RTC_AUDIO_DEVICE_HXX
