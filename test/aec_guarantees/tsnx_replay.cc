// Offline session replay: drives the real APM/AEC3 with a recorded tap-v2
// bundle (manifest.json + headerless PCM + microsecond pacing logs), optionally
// routing capture through DriftServo first. Legacy WAV/v1-log bundles remain
// readable for the original field evidence.
//
// Usage:
//   tsnx_replay <dir> [--servo [--seed <ppm>] | --ratio <ppm>]
//               [--output <wav>]
//
// The default output is <dir>/replay_after_aec_<mode>.wav. Per-second APM
// statistics are emitted as CSV on stdout. Set TSNX_REPLAY_FULL=1 to exercise
// the gate and RT-safe tap writer during replay as an ASAN/full-surface check.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/environment/environment_factory.h"
#include "common_audio/resampler/sinc_resampler.h"
#include "common_audio/wav_file.h"
#include "src/internal/drift_servo.h"
#include "src/internal/self_echo_gate.h"
#include "src/internal/session_tap.h"

namespace {

struct StreamFormat {
  uint32_t rate = 0;
  size_t channels = 0;
};

struct BundleFormat {
  bool v2 = false;
  StreamFormat render;
  StreamFormat capture_raw;
  StreamFormat capture_apm;
  double seed_ppm = 0.0;
};

struct Event {
  int64_t us = 0;
  char side = 0;
  size_t frames = 0;
  uint32_t rate = 0;
  size_t channels = 0;
};

[[noreturn]] void Fail(const std::string& message) {
  fprintf(stderr, "tsnx_replay: %s\n", message.c_str());
  exit(1);
}

bool FileExists(const std::string& path) {
  std::ifstream file(path, std::ios::binary);
  return file.good();
}

std::string ReadText(const std::string& path) {
  std::ifstream file(path);
  if (!file) Fail("cannot open " + path);
  return std::string(std::istreambuf_iterator<char>(file),
                     std::istreambuf_iterator<char>());
}

double ExtractNumber(const std::string& json, const std::string& object,
                     const std::string& field) {
  size_t at = 0;
  if (!object.empty()) {
    at = json.find("\"" + object + "\"");
    if (at == std::string::npos) Fail("manifest missing object: " + object);
  }
  at = json.find("\"" + field + "\"", at);
  if (at == std::string::npos) Fail("manifest missing field: " + field);
  at = json.find(':', at);
  if (at == std::string::npos) Fail("malformed manifest field: " + field);
  char* end = nullptr;
  const double value = strtod(json.c_str() + at + 1, &end);
  if (end == json.c_str() + at + 1)
    Fail("non-numeric manifest field: " + field);
  return value;
}

BundleFormat LoadBundleFormat(const std::string& dir) {
  BundleFormat format;
  if (!FileExists(dir + "/manifest.json")) {
    // Original tap-v1 field bundles were fixed at these formats.
    format.render = {48000, 2};
    format.capture_raw = {48000, 2};
    format.capture_apm = {48000, 1};
    return format;
  }
  const std::string json = ReadText(dir + "/manifest.json");
  if (json.find("\"format\": \"s16le\"") == std::string::npos)
    Fail("unsupported manifest format (expected s16le)");
  format.v2 = true;
  format.render = {
      static_cast<uint32_t>(ExtractNumber(json, "render", "rate")),
      static_cast<size_t>(ExtractNumber(json, "render", "channels"))};
  format.capture_raw = {
      static_cast<uint32_t>(ExtractNumber(json, "capture_raw", "rate")),
      static_cast<size_t>(ExtractNumber(json, "capture_raw", "channels"))};
  format.capture_apm = {
      static_cast<uint32_t>(ExtractNumber(json, "capture_apm", "rate")),
      static_cast<size_t>(ExtractNumber(json, "capture_apm", "channels"))};
  format.seed_ppm = ExtractNumber(json, "", "drift_seed_ppm");
  if (!format.render.rate || !format.render.channels ||
      !format.capture_raw.rate || !format.capture_raw.channels) {
    Fail("manifest contains an uninitialized render/raw-capture format");
  }
  // Tap-v2's first field build finalized the manifest before post-APM audio
  // arrived. Preserve replayability of those two historical bundles while
  // requiring all future writers (and their unit test) to emit the real value.
  if (!format.capture_apm.rate || !format.capture_apm.channels) {
    fprintf(stderr,
            "warning: historical manifest has no post-APM format; inferring "
            "%u Hz mono\n",
            format.capture_raw.rate);
    format.capture_apm = {format.capture_raw.rate, 1};
  }
  return format;
}

std::vector<int16_t> LoadPcm16(const std::string& path, size_t skip_bytes) {
  std::ifstream file(path, std::ios::binary);
  if (!file) Fail("cannot open " + path);
  file.seekg(0, std::ios::end);
  const std::streamoff bytes = file.tellg();
  if (bytes < static_cast<std::streamoff>(skip_bytes) ||
      (bytes - static_cast<std::streamoff>(skip_bytes)) % sizeof(int16_t) !=
          0) {
    Fail("invalid S16 file size: " + path);
  }
  file.seekg(skip_bytes);
  std::vector<int16_t> samples(
      static_cast<size_t>(bytes - static_cast<std::streamoff>(skip_bytes)) /
      sizeof(int16_t));
  file.read(reinterpret_cast<char*>(samples.data()),
            static_cast<std::streamsize>(samples.size() * sizeof(int16_t)));
  if (!file && !file.eof()) Fail("failed reading " + path);
  return samples;
}

std::vector<Event> LoadLog(const std::string& path, char side,
                           StreamFormat fallback, bool strict) {
  std::ifstream file(path);
  if (!file) Fail("cannot open " + path);
  std::vector<Event> events;
  size_t skipped = 0;
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty()) continue;
    std::istringstream fields(line);
    Event event;
    event.side = side;
    std::string second;
    bool valid = static_cast<bool>(fields >> event.us >> second);
    if (valid && second.size() == 1 && (second[0] == 'R' || second[0] == 'C')) {
      event.side = second[0];
      valid = static_cast<bool>(fields >> event.frames >> event.rate >>
                                event.channels);
    } else if (valid) {
      char* end = nullptr;
      event.frames = strtoull(second.c_str(), &end, 10);
      valid = end && *end == '\0';
      event.rate = fallback.rate;
      event.channels = fallback.channels;
    }
    if (!valid) {
      if (strict) Fail("malformed pacing log: " + path);
      ++skipped;  // v1 wrote on RT threads; two known lines were torn.
      continue;
    }
    events.push_back(event);
  }
  if (skipped)
    fprintf(stderr, "warning: skipped %zu malformed legacy log lines in %s\n",
            skipped, path.c_str());
  return events;
}

// Minimal constant-ratio stereo resampler for ceiling measurement.
struct FixedResampler : webrtc::SincResamplerCallback {
  struct Channel : webrtc::SincResamplerCallback {
    std::vector<float> fifo;
    std::unique_ptr<webrtc::SincResampler> resampler;
    void Run(size_t frames, float* destination) override {
      const size_t count = std::min(frames, fifo.size());
      std::copy(fifo.begin(), fifo.begin() + count, destination);
      std::fill(destination + count, destination + frames, 0.0f);
      fifo.erase(fifo.begin(), fifo.begin() + count);
    }
  };
  Channel channel[2];
  std::vector<int16_t> output;
  const double ratio;

  explicit FixedResampler(double ppm) : ratio(1.0 + ppm * 1e-6) {
    for (auto& ch : channel) {
      ch.resampler = std::make_unique<webrtc::SincResampler>(
          ratio, webrtc::SincResampler::kDefaultRequestSize, &ch);
    }
  }
  void Run(size_t, float*) override {}

  size_t Push(const int16_t* samples, size_t frames, size_t channels) {
    if (channels == 0 || channels > 2)
      Fail("fixed resampler supports 1-2 channels");
    for (size_t i = 0; i < frames; ++i) {
      for (size_t ch = 0; ch < channels; ++ch)
        channel[ch].fifo.push_back(samples[i * channels + ch] / 32768.0f);
    }
    constexpr size_t kBlock = 480;
    const size_t need = static_cast<size_t>(kBlock * ratio) + 576;
    std::vector<float> tmp(kBlock);
    while (channel[0].fifo.size() > need) {
      const size_t base = output.size();
      output.resize(base + kBlock * channels);
      for (size_t ch = 0; ch < channels; ++ch) {
        channel[ch].resampler->Resample(kBlock, tmp.data());
        for (size_t i = 0; i < kBlock; ++i) {
          output[base + i * channels + ch] = static_cast<int16_t>(
              std::lround(std::clamp(tmp[i], -1.0f, 1.0f) * 32767.0f));
        }
      }
    }
    return output.size() / (kBlock * channels);
  }

  bool Pop(int16_t* destination, size_t channels) {
    constexpr size_t kBlock = 480;
    const size_t count = kBlock * channels;
    if (output.size() < count) return false;
    std::copy(output.begin(), output.begin() + count, destination);
    output.erase(output.begin(), output.begin() + count);
    return true;
  }
};

struct Options {
  std::string dir;
  bool servo = false;
  bool fixed = false;
  double fixed_ppm = 0.0;
  bool seed = false;
  double seed_ppm = 0.0;
  std::string output;
};

Options ParseOptions(int argc, char** argv) {
  if (argc < 2) {
    Fail(
        "usage: tsnx_replay <dir> "
        "[--servo [--seed <ppm>] | --ratio <ppm>] [--output <wav>]");
  }
  Options options;
  options.dir = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string arg = argv[i];
    if (arg == "--servo") {
      if (options.fixed) Fail("--servo and --ratio are mutually exclusive");
      options.servo = true;
    } else if (arg == "--ratio") {
      if (options.servo || ++i >= argc)
        Fail("--ratio needs a value and cannot be combined with --servo");
      options.fixed = true;
      options.fixed_ppm = atof(argv[i]);
    } else if (arg == "--seed") {
      if (++i >= argc) Fail("--seed requires a ppm value");
      options.seed = true;
      options.seed_ppm = atof(argv[i]);
    } else if (arg == "--output") {
      if (++i >= argc) Fail("--output requires a path");
      options.output = argv[i];
    } else {
      Fail("unknown option: " + arg);
    }
  }
  if (options.seed && !options.servo)
    Fail("--seed requires --servo");
  if (options.seed &&
      (std::abs(options.seed_ppm) < webrtc::DriftServo::kEngagePpm ||
       std::abs(options.seed_ppm) > webrtc::DriftServo::kMaxCorrectionPpm)) {
    Fail("--seed is outside the production servo engagement rails");
  }
  const char* mode = options.fixed
                         ? "fixed"
                         : (options.seed
                                ? "seeded"
                                : (options.servo ? "servo" : "stock"));
  if (options.output.empty())
    options.output = options.dir + "/replay_after_aec_" + mode + ".wav";
  return options;
}

}  // namespace

int main(int argc, char** argv) {
  const Options options = ParseOptions(argc, argv);
  const char* mode = options.fixed
                         ? "fixed"
                         : (options.seed
                                ? "seeded"
                                : (options.servo ? "servo" : "stock"));
  const BundleFormat format = LoadBundleFormat(options.dir);
  if (options.fixed &&
      (format.capture_raw.rate != 48000 || format.capture_raw.channels > 2)) {
    Fail("--ratio currently requires 48 kHz capture with 1-2 channels");
  }

  const size_t header = format.v2 ? 0 : 44;
  const auto render = LoadPcm16(
      options.dir + (format.v2 ? "/render.pcm" : "/render.wav"), header);
  const auto capture = LoadPcm16(
      options.dir + (format.v2 ? "/capture_raw.pcm" : "/capture_raw.wav"),
      header);
  auto render_events =
      LoadLog(options.dir + "/render.log", 'R', format.render, format.v2);
  auto capture_events =
      LoadLog(options.dir + (format.v2 ? "/capture_raw.log" : "/capture.log"),
              'C', format.capture_raw, format.v2);

  std::vector<Event> events;
  events.reserve(render_events.size() + capture_events.size());
  size_t render_event = 0;
  size_t capture_event = 0;
  while (render_event < render_events.size() ||
         capture_event < capture_events.size()) {
    if (capture_event >= capture_events.size() ||
        (render_event < render_events.size() &&
         render_events[render_event].us <= capture_events[capture_event].us)) {
      events.push_back(render_events[render_event++]);
    } else {
      events.push_back(capture_events[capture_event++]);
    }
  }
  fprintf(stderr,
          "bundle=v%d events=%zu render/%zu capture mode=%s output=%s\n",
          format.v2 ? 2 : 1, render_events.size(), capture_events.size(),
          mode, options.output.c_str());

  webrtc::AudioProcessing::Config config;
  config.echo_canceller.enabled = true;
  config.noise_suppression.enabled = true;
  config.high_pass_filter.enabled = true;
  auto apm = webrtc::BuiltinAudioProcessingBuilder(config).Build(
      webrtc::CreateEnvironment());

  const webrtc::StreamConfig capture_in(format.capture_raw.rate,
                                        format.capture_raw.channels);
  const webrtc::StreamConfig capture_out(format.capture_raw.rate, 1);
  const webrtc::StreamConfig render_config(format.render.rate,
                                           format.render.channels);
  if (format.capture_raw.rate % 100 != 0)
    Fail("capture rate is not divisible into 10 ms blocks");
  const size_t capture_block = format.capture_raw.rate / 100;

  webrtc::DriftServo servo;
  if (options.seed) servo.SeedRatio(options.seed_ppm);
  std::unique_ptr<FixedResampler> fixed;
  if (options.fixed)
    fixed = std::make_unique<FixedResampler>(options.fixed_ppm);
  webrtc::SelfEchoGate gate;
  const char* full_root = getenv("TSNX_REPLAY_FULL");
  const bool full = full_root != nullptr;
  std::unique_ptr<webrtc::SessionTap> full_tap;
  if (full) {
    const std::string root =
        full_root[0] && strcmp(full_root, "1") != 0
            ? full_root
            : "/tmp/tsnx_replay_tap";
    full_tap = webrtc::SessionTap::Create(root, format.seed_ppm);
  }

  webrtc::WavWriter output(options.output, format.capture_raw.rate, 1);
  std::vector<int16_t> processed(capture_block);
  std::vector<int16_t> render_buffer;
  std::vector<int16_t> corrected(capture_block * format.capture_raw.channels);
  size_t render_position = 0;
  size_t capture_position = 0;
  const int64_t start_us = events.empty() ? 0 : events.front().us;
  int64_t next_report_us = 1000000;
  double render_energy = 0.0;
  size_t render_samples = 0;

  auto process_capture = [&](const int16_t* input, int64_t event_us) {
    if (apm->ProcessStream(input, capture_in, capture_out, processed.data()) !=
        webrtc::AudioProcessing::kNoError) {
      Fail("APM rejected a capture block");
    }
    output.WriteSamples(processed.data(), processed.size());
    if (full) {
      gate.PushCapture(processed.data(), processed.size(),
                       format.capture_raw.rate, 1);
      if (full_tap)
        full_tap->cap_apm().Push(processed.data(), processed.size(),
                                 format.capture_raw.rate, 1, event_us);
    }
  };

  printf("t_s,mode,render_active,erl_db,erle_db,servo_engaged,"
         "servo_measured_ppm,servo_applied_ppm,servo_windows,"
         "servo_anomalies\n");
  for (const Event& event : events) {
    if (event.side == 'R') {
      const size_t count = event.frames * event.channels;
      if (render_position + count > render.size())
        Fail("render pacing log exceeds render PCM length");
      render_buffer.assign(render.begin() + render_position,
                           render.begin() + render_position + count);
      render_position += count;
      for (int16_t sample : render_buffer)
        render_energy += std::abs(sample) / 32768.0;
      render_samples += count;
      if (apm->ProcessReverseStream(render_buffer.data(), render_config,
                                    render_config, render_buffer.data()) !=
          webrtc::AudioProcessing::kNoError) {
        Fail("APM rejected a render block");
      }
      if (options.servo) servo.OnRenderFrames(event.frames, event.rate);
      if (full) {
        gate.PushRender(render_buffer.data(), event.frames, event.rate,
                        event.channels);
        if (full_tap)
          full_tap->render().Push(render_buffer.data(), event.frames,
                                  event.rate, event.channels, event.us);
      }
    } else {
      const size_t count = event.frames * event.channels;
      if (capture_position + count > capture.size())
        Fail("capture pacing log exceeds capture PCM length");
      const int16_t* input = capture.data() + capture_position;
      capture_position += count;
      if (full && full_tap)
        full_tap->cap_raw().Push(input, event.frames, event.rate,
                                 event.channels, event.us);

      if (options.fixed) {
        const size_t blocks = fixed->Push(input, event.frames, event.channels);
        for (size_t block = 0; block < blocks; ++block) {
          if (!fixed->Pop(corrected.data(), event.channels)) break;
          process_capture(corrected.data(), event.us);
        }
      } else if (options.servo) {
        const size_t blocks = servo.PushCaptureAndCorrect(
            input, event.frames, event.rate, event.channels);
        if (servo.engaged()) {
          for (size_t block = 0; block < blocks; ++block) {
            if (!servo.PopBlock(corrected.data(), capture_block)) break;
            process_capture(corrected.data(), event.us);
          }
        } else {
          process_capture(input, event.us);
        }
      } else {
        process_capture(input, event.us);
      }
    }

    while (event.us - start_us >= next_report_us) {
      const auto stats = apm->GetStatistics();
      const double erl = stats.echo_return_loss.value_or(-99.0);
      const double erle = stats.echo_return_loss_enhancement.value_or(-99.0);
      const bool active =
          render_samples > 0 && render_energy / render_samples > 0.001;
      const auto servo_stats = servo.GetStats();
      printf("%.1f,%s,%d,%.2f,%.3f,%d,%.2f,%.2f,%lld,%lld\n",
             next_report_us / 1e6,
             mode, active, erl, erle, servo_stats.engaged,
             servo_stats.measured_ppm, servo_stats.applied_ppm,
             static_cast<long long>(servo_stats.windows),
             static_cast<long long>(servo_stats.anomalies));
      next_report_us += 1000000;
      render_energy = 0.0;
      render_samples = 0;
    }
  }

  const auto final_servo_stats = servo.GetStats();
  fprintf(stderr,
          "consumed render=%zu/%zu capture=%zu/%zu samples; wrote %s\n",
          render_position, render.size(), capture_position, capture.size(),
          options.output.c_str());
  if (options.servo) {
    fprintf(stderr,
            "servo final: engaged=%d measured=%.2f ppm applied=%.2f ppm "
            "windows=%lld anomalies=%lld\n",
            final_servo_stats.engaged, final_servo_stats.measured_ppm,
            final_servo_stats.applied_ppm,
            static_cast<long long>(final_servo_stats.windows),
            static_cast<long long>(final_servo_stats.anomalies));
  }
  return 0;
}
