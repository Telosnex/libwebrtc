#include "src/internal/self_echo_gate.h"

#include <algorithm>
#include <cmath>

#include "rtc_base/logging.h"

namespace webrtc {
namespace {
constexpr float kAbsFloorDb = -75.f;
constexpr float kActBelowPeakDb = 35.f;
constexpr float kEps = 1e-10f;
constexpr double kPi = 3.14159265358979323846;

float MaskedMedian(std::vector<float>& v) {
  if (v.empty()) return 0.f;
  const size_t mid = v.size() / 2;
  std::nth_element(v.begin(), v.begin() + mid, v.end());
  return v[mid];
}
}  // namespace

SelfEchoGate::SelfEchoGate() {
  for (int i = 0; i < kRing; ++i) {
    ren_mean_[i] = mic_mean_[i] = -100.f;
    ren_act_[i] = false;
    for (int b = 0; b < kBands; ++b) ren_env_[i][b] = mic_env_[i][b] = -100.f;
  }
}

void SelfEchoGate::InitBank(Bank& bank, uint32_t rate_hz) {
  // 12 RBJ bandpass filters tiling 200..6000 Hz geometrically.
  const double lo = 200.0, hi = 6000.0;
  for (int b = 0; b < kBands; ++b) {
    const double e0 = lo * std::pow(hi / lo, static_cast<double>(b) / kBands);
    const double e1 =
        lo * std::pow(hi / lo, static_cast<double>(b + 1) / kBands);
    const double fc = std::sqrt(e0 * e1);
    const double q = fc / (e1 - e0);
    const double w0 = 2.0 * kPi * fc / rate_hz;
    const double alpha = std::sin(w0) / (2.0 * q);
    const double a0 = 1.0 + alpha;
    Biquad& f = bank.f[b];
    f.b0 = static_cast<float>(alpha / a0);
    f.b1 = 0.f;
    f.b2 = static_cast<float>(-alpha / a0);
    f.a1 = static_cast<float>(-2.0 * std::cos(w0) / a0);
    f.a2 = static_cast<float>((1.0 - alpha) / a0);
    f.z1 = f.z2 = 0.f;
  }
  bank.rate = rate_hz;
}

float SelfEchoGate::Envelope(Bank& bank, const int16_t* x, size_t frames,
                             size_t channels, float* out_db) {
  if (bank.rate == 0) return -100.f;
  double acc[kBands] = {0};
  for (size_t i = 0; i < frames; ++i) {
    float s = 0.f;  // downmix
    for (size_t c = 0; c < channels; ++c) s += x[i * channels + c];
    s /= (32768.f * channels);
    for (int b = 0; b < kBands; ++b) {
      const float y = bank.f[b].Process(s);
      acc[b] += static_cast<double>(y) * y;
    }
  }
  float mean = 0.f;
  for (int b = 0; b < kBands; ++b) {
    out_db[b] = 10.f * std::log10(static_cast<float>(acc[b]) + kEps);
    mean += out_db[b];
  }
  return mean / kBands;
}

void SelfEchoGate::PushRender(const int16_t* samples, size_t frames,
                              uint32_t rate_hz, size_t channels) {
  if (rate_hz == 0 || channels == 0 || frames == 0) return;
  MutexLock l(&lock_);
  if (render_bank_.rate != rate_hz) InitBank(render_bank_, rate_hz);
  const int idx = static_cast<int>(ren_frames_ % kRing);
  ren_mean_[idx] =
      Envelope(render_bank_, samples, frames, channels, ren_env_[idx]);
  ren_peak_db_ = std::max(ren_mean_[idx], ren_peak_db_ - 0.02f);  // slow decay
  ren_act_[idx] =
      ren_mean_[idx] > std::max(ren_peak_db_ - kActBelowPeakDb, kAbsFloorDb);
  ++ren_frames_;
}

void SelfEchoGate::PushCapture(const int16_t* samples, size_t frames,
                               uint32_t rate_hz, size_t channels) {
  if (rate_hz == 0 || channels == 0 || frames == 0) return;
  MutexLock l(&lock_);
  if (capture_bank_.rate != rate_hz) InitBank(capture_bank_, rate_hz);
  const int idx = static_cast<int>(mic_frames_ % kRing);
  mic_mean_[idx] =
      Envelope(capture_bank_, samples, frames, channels, mic_env_[idx]);
  // Track noise floor: fast down, very slow up.
  if (mic_mean_[idx] < mic_noise_db_) {
    mic_noise_db_ = 0.9f * mic_noise_db_ + 0.1f * mic_mean_[idx];
  } else {
    mic_noise_db_ += 0.005f;
  }
  ++mic_frames_;
  if (mic_frames_ % kEvalEveryFrames == 0 &&
      mic_frames_ > kWindow + kMaxLag + 2) {
    Evaluate();
  }
}

bool SelfEchoGate::RenderActiveAt(int64_t render_frames, int back) const
    RTC_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
  return ren_act_[(render_frames - 1 - back) % kRing];
}

void SelfEchoGate::Evaluate() RTC_EXCLUSIVE_LOCKS_REQUIRED(lock_) {
  // Windows are addressed backwards from the newest frame.
  const int64_t t = mic_frames_;
  auto mic_at = [&](int back) -> const float* {
    return mic_env_[(t - 1 - back) % kRing];
  };
  const int64_t rt = ren_frames_;
  auto ren_at = [&](int back) -> const float* {
    return ren_env_[(rt - 1 - back) % kRing];
  };
  if (rt < kWindow + kMaxLag + 2) return;

  // Best masked correlation over lags.
  float best_c = -1.f;
  int best_l = 0, best_na = 0;
  for (int L = 0; L < kMaxLag; ++L) {
    int na = 0;
    for (int i = 0; i < kWindow; ++i) na += RenderActiveAt(rt, L + i) ? 1 : 0;
    if (na < 12) continue;
    // z-normalized correlation over masked frames x bands.
    double sm = 0, sr = 0, smm = 0, srr = 0, smr = 0;
    int n = 0;
    for (int i = 0; i < kWindow; ++i) {
      if (!RenderActiveAt(rt, L + i)) continue;
      const float* M = mic_at(i);
      const float* R = ren_at(L + i);
      for (int b = 0; b < kBands; ++b) {
        sm += M[b];
        sr += R[b];
        smm += static_cast<double>(M[b]) * M[b];
        srr += static_cast<double>(R[b]) * R[b];
        smr += static_cast<double>(M[b]) * R[b];
        ++n;
      }
    }
    const double vm = smm - sm * sm / n, vr = srr - sr * sr / n;
    if (vm < 1e-9 || vr < 1e-9) continue;
    const float c =
        static_cast<float>((smr - sm * sr / n) / std::sqrt(vm * vr));
    if (c > best_c) {
      best_c = c;
      best_l = L;
      best_na = na;
    }
  }
  const bool act = best_na >= 12;

  float slow_excess = 99.f, fast_excess = 0.f;
  bool gap_voice = false;
  if (act) {
    if (best_c > 0.65f) {  // learn energy transfer only when confident
      std::vector<float> d;
      d.reserve(kWindow * kBands);
      for (int i = 0; i < kWindow; ++i) {
        if (!RenderActiveAt(rt, best_l + i)) continue;
        const float* M = mic_at(i);
        const float* R = ren_at(best_l + i);
        for (int b = 0; b < kBands; ++b) d.push_back(M[b] - R[b]);
      }
      const float med = MaskedMedian(d);
      offset_db_ = have_offset_ ? 0.95f * offset_db_ + 0.05f * med : med;
      have_offset_ = true;
    }
    if (have_offset_) {
      std::vector<float> slow, fast;
      for (int i = 0; i < 30; ++i) {
        if (!RenderActiveAt(rt, best_l + i)) continue;
        const float* M = mic_at(i);
        const float* R = ren_at(best_l + i);
        for (int b = 0; b < kBands; ++b) {
          const float v = M[b] - R[b];
          slow.push_back(v);
          if (i < 8) fast.push_back(v);
        }
      }
      if (slow.size() > 5 * kBands)
        slow_excess = MaskedMedian(slow) - offset_db_;
      else
        slow_excess = 0.f;
      if (!fast.empty()) {
        double s = 0;
        for (float v : fast) s += v;
        fast_excess = static_cast<float>(s / fast.size()) - offset_db_;
      }
    }
    // Voice in the render gaps: lag-aligned render quiet, mic active.
    bool rgap = true;
    for (int i = 0; i < 10; ++i) rgap = rgap && !RenderActiveAt(rt, best_l + i);
    if (rgap) {
      int loud = 0;
      for (int i = 0; i < 10; ++i)
        loud += mic_mean_[(t - 1 - i) % kRing] > mic_noise_db_ + 10.f ? 1 : 0;
      gap_voice = loud > 5;
    }
  }

  // Asymmetric state machine (G3). Opening is cheap; closing needs evidence.
  const bool ev =
      act && (slow_excess > kSlowExcessDb || fast_excess > kFastExcessDb ||
              best_c < kCorrCollapse || gap_voice);
  open_evid_ = ev ? open_evid_ + 1 : 0;
  if (closed_) {
    if (open_evid_ >= kOpenStreak || !act) {  // G1: render silent => open
      closed_ = false;
      ok_streak_ = 0;
    }
  } else {
    const bool ok =
        act && best_c > kCloseCorr && slow_excess < kSlowExcessDb && !gap_voice;
    ok_streak_ = ok ? ok_streak_ + 1 : 0;
    if (ok_streak_ >= kCloseStreak) closed_ = true;
  }

  ++stats_.evals;
  if (closed_) ++stats_.closed_evals;
  stats_.closed = closed_;
  stats_.corr = best_c;
  stats_.lag_ms = best_l * 10;
  stats_.slow_excess_db = slow_excess;
  stats_.offset_db = offset_db_;
}

SelfEchoGate::Stats SelfEchoGate::GetStats() const {
  MutexLock l(&lock_);
  return stats_;
}

}  // namespace webrtc
