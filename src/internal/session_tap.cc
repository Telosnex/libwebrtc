#include "src/internal/session_tap.h"

#include <sys/stat.h>

#if defined(_WIN32)
#include <direct.h>
#endif

#include <cerrno>
#include <chrono>

#include "rtc_base/logging.h"

namespace webrtc {
namespace {
int64_t EpochMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}  // namespace

SessionTap::Stream::Stream(const std::string& prefix, size_t ring_samples)
    : prefix_(prefix) {
  // Power-of-two rings keep producer index math to a mask.
  size_t n = 1;
  while (n < ring_samples) n <<= 1;
  ring_.resize(n);
  ev_ring_.resize(4096);  // also a power of two
  pcm_ = fopen((prefix_ + ".pcm").c_str(), "wb");
  log_ = fopen((prefix_ + ".log").c_str(), "w");
}

void SessionTap::Stream::Push(const int16_t* samples, size_t frames,
                              uint32_t rate_hz, size_t channels, int64_t t_us) {
  // Publish channels first and rate last. An acquire-load of a non-zero rate
  // therefore observes both fields as one initialized format.
  ch_.store(channels, std::memory_order_relaxed);
  rate_.store(rate_hz, std::memory_order_release);
  const size_t n = frames * channels;
  if (total_pushed_ + static_cast<int64_t>(n) > kMaxTotalSamples) return;
  total_pushed_ += n;
  const size_t mask = ring_.size() - 1;
  const size_t w = w_.load(std::memory_order_relaxed);
  const size_t r = r_.load(std::memory_order_acquire);
  if (ring_.size() - (w - r) < n) {  // full: drop, never block (R1)
    dropped_.fetch_add(n, std::memory_order_relaxed);
    return;
  }
  for (size_t i = 0; i < n; ++i) ring_[(w + i) & mask] = samples[i];
  w_.store(w + n, std::memory_order_release);

  const size_t ew = ev_w_.load(std::memory_order_relaxed);
  const size_t er = ev_r_.load(std::memory_order_acquire);
  if (ev_ring_.size() - (ew - er) >= 1) {
    ev_ring_[ew & (ev_ring_.size() - 1)] = {t_us,
                                            static_cast<uint32_t>(frames)};
    ev_w_.store(ew + 1, std::memory_order_release);
  }
}

void SessionTap::Stream::Drain() {
  if (!pcm_) return;
  const size_t mask = ring_.size() - 1;
  size_t r = r_.load(std::memory_order_relaxed);
  const size_t w = w_.load(std::memory_order_acquire);
  // Disk-thread allocation is intentional; producers never touch this buffer.
  std::vector<int16_t> tmp;
  tmp.reserve(w - r);
  while (r < w) {
    tmp.push_back(ring_[r & mask]);
    ++r;
  }
  if (!tmp.empty()) {
    fwrite(tmp.data(), sizeof(int16_t), tmp.size(), pcm_);
    fflush(pcm_);
  }
  r_.store(r, std::memory_order_release);

  if (log_) {
    size_t er = ev_r_.load(std::memory_order_relaxed);
    const size_t ew = ev_w_.load(std::memory_order_acquire);
    const size_t emask = ev_ring_.size() - 1;
    while (er < ew) {
      const Event& e = ev_ring_[er & emask];
      fprintf(log_, "%lld %u\n", static_cast<long long>(e.t_us), e.frames);
      ++er;
    }
    fflush(log_);
    ev_r_.store(er, std::memory_order_release);
  }
}

void SessionTap::Stream::Close() {
  Drain();
  if (pcm_) {
    fclose(pcm_);
    pcm_ = nullptr;
  }
  if (log_) {
    fclose(log_);
    log_ = nullptr;
  }
}

std::unique_ptr<SessionTap> SessionTap::Create(const std::string& root,
                                               double seed_ppm) {
  const std::string dir = root + "/" + std::to_string(EpochMs());
#if defined(_WIN32)
  const auto make_dir = [](const std::string& path) {
    return _mkdir(path.c_str());
  };
#else
  const auto make_dir = [](const std::string& path) {
    return mkdir(path.c_str(), 0777);
  };
#endif
  if (make_dir(root) != 0 && errno != EEXIST) return nullptr;
  if (make_dir(dir) != 0) return nullptr;
  auto tap = std::unique_ptr<SessionTap>(new SessionTap(dir, seed_ppm));
  if (!tap->valid_) return nullptr;
  return tap;
}

SessionTap::SessionTap(std::string dir, double seed_ppm)
    : dir_(std::move(dir)), seed_ppm_(seed_ppm) {
  constexpr size_t kRing = 48000 * 2 * 4;  // 4 s stereo per stream
  render_ = std::make_unique<Stream>(dir_ + "/render", kRing);
  cap_raw_ = std::make_unique<Stream>(dir_ + "/capture_raw", kRing);
  cap_apm_ = std::make_unique<Stream>(dir_ + "/capture_apm", kRing);
  valid_ = render_->valid() && cap_raw_->valid() && cap_apm_->valid();
  if (!valid_) {
    RTC_LOG(LS_ERROR) << "SessionTap: failed opening one or more files in "
                      << dir_;
    return;
  }
  writer_ = std::thread([this] { WriterLoop(); });
  RTC_LOG(LS_INFO) << "SessionTap: recording to " << dir_;
  fprintf(stderr, "TSNX tap v2: %s\n", dir_.c_str());
}

SessionTap::~SessionTap() {
  stop_.store(true, std::memory_order_release);
  if (writer_.joinable()) writer_.join();
  render_->Close();
  cap_raw_->Close();
  cap_apm_->Close();
  if (!manifest_written_ && FormatsReady()) WriteManifest();
}

bool SessionTap::FormatsReady() const {
  return render_->rate() && render_->channels() && cap_raw_->rate() &&
         cap_raw_->channels() && cap_apm_->rate() && cap_apm_->channels();
}

void SessionTap::WriterLoop() {
  while (!stop_.load(std::memory_order_acquire)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    render_->Drain();
    cap_raw_->Drain();
    cap_apm_->Drain();
    if (!manifest_written_ && FormatsReady()) WriteManifest();
  }
}

void SessionTap::WriteManifest() {
  const std::string tmp_path = dir_ + "/manifest.json.tmp";
  const std::string final_path = dir_ + "/manifest.json";
  FILE* f = fopen(tmp_path.c_str(), "w");
  if (!f) return;
  const int written = fprintf(
      f,
      "{\n"
      "  \"version\": 2,\n"
      "  \"scope\": \"recorder_lifetime\",\n"
      "  \"format\": \"s16le\",\n"
      "  \"render\": {\"rate\": %u, \"channels\": %zu},\n"
      "  \"capture_raw\": {\"rate\": %u, \"channels\": %zu},\n"
      "  \"capture_apm\": {\"rate\": %u, \"channels\": %zu},\n"
      "  \"drift_seed_ppm\": %.1f,\n"
      "  \"pacing_log\": \"<stream>.log lines: t_us frames\",\n"
      "  \"note\": \"timing gaps delimit calls; any complete .pcm sample "
      "prefix is valid\"\n"
      "}\n",
      render_->rate(), render_->channels(), cap_raw_->rate(),
      cap_raw_->channels(), cap_apm_->rate(), cap_apm_->channels(), seed_ppm_);
  bool ok = written > 0;
  ok = fflush(f) == 0 && ok;
  ok = fclose(f) == 0 && ok;
  if (ok) ok = rename(tmp_path.c_str(), final_path.c_str()) == 0;
  if (!ok) {
    remove(tmp_path.c_str());
    return;
  }
  manifest_written_ = true;
}

}  // namespace webrtc
