// Copyright (c) Telosnex. Crash-safe, RT-safe diagnostic recorder.
//
// Records render / raw-capture / post-APM-capture PCM, per-callback pacing
// events, and ALSA hardware-clock positions for deterministic offline replay
// (test/aec_guarantees/ tsnx_replay.cc).
//
// RELIABILITY DESIGN (lessons from field sessions 1-3):
//   R1 Audio threads NEVER touch disk or locks shared with disk I/O:
//      producers write into fixed lock-free SPSC rings; a dedicated
//      low-priority writer thread drains to files every ~100 ms.
//      Ring overflow drops samples and counts them -- never blocks.
//   R2 Crash-safe audio format: headerless .pcm streams. There is no WAV
//      header to corrupt; any complete S16 sample prefix remains usable.
//   R3 One directory per recorder lifetime (tap root / <epoch_ms>/). Timing
//      gaps remain explicit in the pacing logs, so replay can identify
//      multiple calls recorded by a long-lived transport. A fresh directory
//      is guaranteed after process restart, not after every peer session.
//   R4 The manifest is finalized atomically only after all three streams have
//      supplied their rate/channel format. The drift seed is constructor state
//      established before the writer starts, never cross-thread mutable state.
//
// This is an opt-in diagnostic containing raw microphone audio. Production
// services MUST leave TSNX_TAP_DIR unset except during an explicitly approved
// recording session.
#ifndef INTERNAL_SESSION_TAP_H_
#define INTERNAL_SESSION_TAP_H_

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "api/audio/audio_device_defines.h"

namespace webrtc {

class SessionTap {
 public:
  // One PCM stream + its pacing-event ring. Single producer (one audio
  // thread), single consumer (writer thread).
  class Stream {
   public:
    Stream(const std::string& path_prefix, size_t ring_samples);
    // RT-safe: copies into ring or drops. Also records a pacing event.
    void Push(const int16_t* samples, size_t frames, uint32_t rate_hz,
              size_t channels, int64_t t_us);
    static constexpr int64_t kMaxTotalSamples = 48000LL * 2 * 60 * 15;
    void Drain();  // writer thread only
    void Close();
    uint32_t rate() const { return rate_.load(std::memory_order_acquire); }
    size_t channels() const { return ch_.load(std::memory_order_acquire); }
    int64_t dropped() const { return dropped_.load(std::memory_order_relaxed); }
    bool valid() const { return pcm_ && log_; }

   private:
    struct Event {
      int64_t t_us;
      uint32_t frames;
    };
    std::string prefix_;
    std::vector<int16_t> ring_;
    std::vector<Event> ev_ring_;
    std::atomic<size_t> w_{0}, r_{0};        // sample ring indices
    std::atomic<size_t> ev_w_{0}, ev_r_{0};  // event ring indices
    std::atomic<uint32_t> rate_{0};
    std::atomic<size_t> ch_{0};
    std::atomic<int64_t> dropped_{0};
    int64_t total_pushed_ = 0;  // producer thread only
    FILE* pcm_ = nullptr;
    FILE* log_ = nullptr;
  };

  // RT-safe: records raw ALSA hardware-clock observations into one SPSC ring
  // per native audio thread for later --hw-servo replay.
  void PushHardwareClockObservation(
      const AudioHardwareClockObservation& observation);

  // Creates <root>/<epoch_ms>/ and starts the writer thread. seed_ppm is
  // immutable recorder metadata and is installed before that thread starts.
  static std::unique_ptr<SessionTap> Create(const std::string& root,
                                            double seed_ppm = 0.0);
  ~SessionTap();

  Stream& render() { return *render_; }
  Stream& cap_raw() { return *cap_raw_; }
  Stream& cap_apm() { return *cap_apm_; }
  const std::string& dir() const { return dir_; }

 private:
  class ClockStream {
   public:
    explicit ClockStream(const std::string& path);
    void Push(const AudioHardwareClockObservation& observation);
    void Drain();
    void Close();
    bool valid() const { return file_ != nullptr; }

   private:
    struct Event {
      int64_t time_ns;
      int64_t position_frames;
      uint32_t sample_rate_hz;
      uint32_t generation;
    };
    std::vector<Event> ring_;
    std::atomic<size_t> write_{0};
    std::atomic<size_t> read_{0};
    std::atomic<int64_t> dropped_{0};
    FILE* file_ = nullptr;
  };

  SessionTap(std::string dir, double seed_ppm);
  void WriterLoop();
  bool FormatsReady() const;
  void WriteManifest();  // writer thread, or destructor after join

  std::string dir_;
  const double seed_ppm_;
  std::unique_ptr<Stream> render_, cap_raw_, cap_apm_;
  std::unique_ptr<ClockStream> hw_playout_, hw_capture_;
  std::thread writer_;
  std::atomic<bool> stop_{false};
  bool valid_ = false;
  bool manifest_written_ = false;
};

}  // namespace webrtc
#endif  // INTERNAL_SESSION_TAP_H_
