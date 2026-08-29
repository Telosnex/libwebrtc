// Runs the production ALSA ADM clock-observation path on real hardware while
// playing silence. This is a developer probe; it never enables correction.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#include "api/audio/audio_device.h"
#include "api/audio/create_audio_device_module.h"
#include "api/environment/environment_factory.h"
#include "src/internal/hardware_clock_estimator.h"

namespace {

class ProbeTransport final : public webrtc::AudioTransport {
 public:
  int32_t RecordedDataIsAvailable(const void*, size_t, size_t, size_t, uint32_t,
                                  uint32_t, int32_t, uint32_t, bool,
                                  uint32_t&) override {
    return 0;
  }

  int32_t NeedMorePlayData(size_t frames, size_t bytes_per_frame,
                           size_t channels, uint32_t, void* samples,
                           size_t& samples_out, int64_t* elapsed_time_ms,
                           int64_t* ntp_time_ms) override {
    memset(samples, 0, frames * bytes_per_frame);
    samples_out = frames * channels;
    if (elapsed_time_ms) *elapsed_time_ms = -1;
    if (ntp_time_ms) *ntp_time_ms = -1;
    return 0;
  }

  void PullRenderData(int, int, size_t, size_t, void*, int64_t*,
                      int64_t*) override {}

  void OnAudioHardwareClockObservation(
      const webrtc::AudioHardwareClockObservation& observation) override {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto update = estimator_.Add(observation);
    if (!update.estimate) return;
    latest_ = update.estimate;
    fprintf(stderr, "hw %.2f +/- %.2f ppm span %.2f s points %zu/%zu\n",
            latest_->relative_ppm, latest_->uncertainty_ppm,
            latest_->span_seconds, latest_->playout_points,
            latest_->capture_points);
  }

  std::optional<webrtc::HardwareClockEstimator::Estimate> latest() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return latest_;
  }
  int64_t resets() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return estimator_.resets();
  }
  int64_t rejected() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return estimator_.rejected();
  }

 private:
  mutable std::mutex mutex_;
  webrtc::HardwareClockEstimator estimator_;
  std::optional<webrtc::HardwareClockEstimator::Estimate> latest_;
};

int ParseInteger(const char* text, const char* option) {
  char* end = nullptr;
  const long value = strtol(text, &end, 10);
  if (!end || *end || value < 0 || value > 65535) {
    fprintf(stderr, "%s requires a non-negative integer\n", option);
    exit(2);
  }
  return static_cast<int>(value);
}

void PrintDevices(webrtc::AudioDeviceModule* adm) {
  printf("Playout devices:\n");
  const int16_t playout_count = adm->PlayoutDevices();
  for (int16_t index = 0; index < playout_count; ++index) {
    char name[webrtc::kAdmMaxDeviceNameSize] = {};
    char guid[webrtc::kAdmMaxGuidSize] = {};
    if (adm->PlayoutDeviceName(index, name, guid) == 0)
      printf("  %d: %s [%s]\n", index, name, guid);
  }
  printf("Capture devices:\n");
  const int16_t capture_count = adm->RecordingDevices();
  for (int16_t index = 0; index < capture_count; ++index) {
    char name[webrtc::kAdmMaxDeviceNameSize] = {};
    char guid[webrtc::kAdmMaxGuidSize] = {};
    if (adm->RecordingDeviceName(index, name, guid) == 0)
      printf("  %d: %s [%s]\n", index, name, guid);
  }
}

}  // namespace

int main(int argc, char** argv) {
  int playout_device = -1;
  int capture_device = -1;
  int duration_seconds = 12;
  bool list_only = false;
  for (int index = 1; index < argc; ++index) {
    const std::string option = argv[index];
    if (option == "--list") {
      list_only = true;
    } else if (option == "--playout" && ++index < argc) {
      playout_device = ParseInteger(argv[index], "--playout");
    } else if (option == "--capture" && ++index < argc) {
      capture_device = ParseInteger(argv[index], "--capture");
    } else if (option == "--seconds" && ++index < argc) {
      duration_seconds = ParseInteger(argv[index], "--seconds");
    } else {
      fprintf(stderr,
              "usage: tsnx_alsa_hw_clock_probe [--list] [--playout N] "
              "[--capture N] [--seconds N]\n");
      return 2;
    }
  }

  auto adm = webrtc::CreateAudioDeviceModule(
      webrtc::CreateEnvironment(), webrtc::AudioDeviceModule::kLinuxAlsaAudio,
      false);
  if (!adm || adm->Init() != 0) {
    fprintf(stderr, "could not initialize ALSA ADM\n");
    return 1;
  }
  PrintDevices(adm.get());
  if (list_only) {
    adm->Terminate();
    return 0;
  }
  if (playout_device < 0 || capture_device < 0) {
    fprintf(stderr, "--playout and --capture are required for probing\n");
    adm->Terminate();
    return 2;
  }

  ProbeTransport transport;
  if (adm->RegisterAudioCallback(&transport) != 0 ||
      adm->SetPlayoutDevice(playout_device) != 0 ||
      adm->SetRecordingDevice(capture_device) != 0 || adm->InitPlayout() != 0 ||
      adm->InitRecording() != 0 || adm->StartPlayout() != 0 ||
      adm->StartRecording() != 0) {
    fprintf(stderr, "could not start selected ALSA devices\n");
    adm->StopRecording();
    adm->StopPlayout();
    adm->Terminate();
    return 1;
  }

  std::this_thread::sleep_for(std::chrono::seconds(duration_seconds));
  adm->StopRecording();
  adm->StopPlayout();
  adm->Terminate();

  const auto estimate = transport.latest();
  if (!estimate) {
    fprintf(stderr, "no confident hardware clock estimate\n");
    return 1;
  }
  printf("FINAL %.2f +/- %.2f ppm span %.2f s resets %lld rejected %lld\n",
         estimate->relative_ppm, estimate->uncertainty_ppm,
         estimate->span_seconds, static_cast<long long>(transport.resets()),
         static_cast<long long>(transport.rejected()));
  return 0;
}
