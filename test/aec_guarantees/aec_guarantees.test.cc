#include <unistd.h>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "src/internal/drift_servo.h"
#include "src/internal/self_echo_gate.h"
#include "src/internal/session_tap.h"
#include "test/gtest.h"

namespace webrtc {
namespace {

constexpr uint32_t kRate = 48000;
constexpr size_t kBlock = kRate / 100;

std::vector<int16_t> SignalFrame(int frame_index, float gain = 1.0f) {
  std::vector<int16_t> out(kBlock);
  const double frame_gain =
      gain * (0.35 + 0.65 * (0.5 + 0.5 * std::sin(frame_index * 0.071)));
  const double f1 = 260.0 + 37.0 * (frame_index % 23);
  const double f2 = 900.0 + 83.0 * (frame_index % 41);
  for (size_t i = 0; i < out.size(); ++i) {
    const double t = (frame_index * kBlock + i) / static_cast<double>(kRate);
    const double sample = frame_gain * (0.55 * std::sin(2.0 * M_PI * f1 * t) +
                                        0.35 * std::sin(2.0 * M_PI * f2 * t));
    out[i] = static_cast<int16_t>(
        std::lround(std::clamp(sample, -0.95, 0.95) * 32767.0));
  }
  return out;
}

TEST(DriftServoGuaranteeTest, InSpecClockNeverEngages) {
  DriftServo servo;
  std::vector<int16_t> zeros(kBlock, 0);
  for (int i = 0; i < 6000; ++i) {
    servo.PushCaptureAndCorrect(zeros.data(), kBlock, kRate, 1);
    servo.OnRenderFrames(kBlock, kRate);
  }
  const auto stats = servo.GetStats();
  EXPECT_FALSE(stats.engaged);
  EXPECT_NEAR(stats.measured_ppm, 0.0, 1.0);
  EXPECT_GE(stats.windows, 5);
}

TEST(DriftServoGuaranteeTest, LargeClockErrorEngagesWithoutSeed) {
  DriftServo servo;
  std::vector<int16_t> zeros(kBlock, 0);
  constexpr double kCapturePerRender = 1.0 - 1700.0e-6;
  double capture_credit = 0.0;
  for (int i = 0; i < 8000; ++i) {
    capture_credit += kCapturePerRender;
    while (capture_credit >= 1.0) {
      servo.PushCaptureAndCorrect(zeros.data(), kBlock, kRate, 1);
      capture_credit -= 1.0;
    }
    servo.OnRenderFrames(kBlock, kRate);
  }
  const auto stats = servo.GetStats();
  EXPECT_TRUE(stats.engaged);
  EXPECT_NEAR(stats.measured_ppm, -1700.0, 150.0);
  EXPECT_LE(std::abs(stats.applied_ppm), DriftServo::kMaxCorrectionPpm);
}

TEST(DriftServoGuaranteeTest,
     ValidSeedEngagesImmediatelyAndInvalidSeedDoesNot) {
  DriftServo valid;
  valid.SeedRatio(-1700.0);
  EXPECT_TRUE(valid.GetStats().engaged);
  EXPECT_NEAR(valid.GetStats().applied_ppm, -1700.0, 0.1);

  DriftServo too_small;
  too_small.SeedRatio(DriftServo::kEngagePpm / 2.0);
  EXPECT_FALSE(too_small.GetStats().engaged);

  DriftServo too_large;
  too_large.SeedRatio(DriftServo::kMaxCorrectionPpm + 1.0);
  EXPECT_FALSE(too_large.GetStats().engaged);
}

TEST(SelfEchoGateGuaranteeTest, PureEchoClosesAndRenderSilenceReopens) {
  SelfEchoGate gate;
  for (int frame = 0; frame < 260; ++frame) {
    const auto signal = SignalFrame(frame, 0.5f);
    gate.PushRender(signal.data(), signal.size(), kRate, 1);
    gate.PushCapture(signal.data(), signal.size(), kRate, 1);
  }
  ASSERT_TRUE(gate.GetStats().closed);
  EXPECT_GT(gate.GetStats().corr, SelfEchoGate::kCloseCorr);

  std::vector<int16_t> silence(kBlock, 0);
  for (int frame = 0; frame < 120; ++frame) {
    gate.PushRender(silence.data(), silence.size(), kRate, 1);
    gate.PushCapture(silence.data(), silence.size(), kRate, 1);
  }
  EXPECT_FALSE(gate.GetStats().closed);
}

TEST(SelfEchoGateGuaranteeTest, NearEndOnlyNeverClosesOrMutatesInput) {
  SelfEchoGate gate;
  std::vector<int16_t> silence(kBlock, 0);
  for (int frame = 0; frame < 260; ++frame) {
    auto near = SignalFrame(frame, 0.5f);
    const auto before = near;
    gate.PushRender(silence.data(), silence.size(), kRate, 1);
    gate.PushCapture(near.data(), near.size(), kRate, 1);
    EXPECT_EQ(near, before);
  }
  EXPECT_FALSE(gate.GetStats().closed);
}

TEST(SelfEchoGateGuaranteeTest, StrongDoubleTalkReopens) {
  SelfEchoGate gate;
  for (int frame = 0; frame < 260; ++frame) {
    const auto echo = SignalFrame(frame, 0.35f);
    gate.PushRender(echo.data(), echo.size(), kRate, 1);
    gate.PushCapture(echo.data(), echo.size(), kRate, 1);
  }
  ASSERT_TRUE(gate.GetStats().closed);

  for (int frame = 260; frame < 300; ++frame) {
    const auto echo = SignalFrame(frame, 0.35f);
    const auto near = SignalFrame(frame + 1000, 0.85f);
    std::vector<int16_t> mixed(kBlock);
    for (size_t i = 0; i < mixed.size(); ++i) {
      mixed[i] = static_cast<int16_t>(
          std::clamp<int>(static_cast<int>(echo[i]) + static_cast<int>(near[i]),
                          -32768, 32767));
    }
    gate.PushRender(echo.data(), echo.size(), kRate, 1);
    gate.PushCapture(mixed.data(), mixed.size(), kRate, 1);
  }
  EXPECT_FALSE(gate.GetStats().closed);
}

TEST(SessionTapGuaranteeTest, FinalManifestContainsEveryFormatAndStableSeed) {
  const std::string root =
      "/tmp/tsnx_aec_guarantees_" + std::to_string(getpid());
  std::string dir;
  {
    auto tap = SessionTap::Create(root, -1700.0);
    ASSERT_NE(tap, nullptr);
    dir = tap->dir();
    std::vector<int16_t> stereo(kBlock * 2, 17);
    std::vector<int16_t> mono(kBlock, 23);
    tap->render().Push(stereo.data(), kBlock, kRate, 2, 1000000);
    tap->cap_raw().Push(stereo.data(), kBlock, kRate, 2, 1000100);
    tap->cap_apm().Push(mono.data(), kBlock, kRate, 1, 1000200);
  }

  std::ifstream manifest_file(dir + "/manifest.json");
  ASSERT_TRUE(manifest_file.good());
  const std::string manifest((std::istreambuf_iterator<char>(manifest_file)),
                             std::istreambuf_iterator<char>());
  EXPECT_NE(manifest.find("\"scope\": \"recorder_lifetime\""),
            std::string::npos);
  EXPECT_NE(manifest.find("\"capture_apm\": {\"rate\": 48000, "
                          "\"channels\": 1}"),
            std::string::npos);
  EXPECT_NE(manifest.find("\"drift_seed_ppm\": -1700.0"), std::string::npos);
  std::ifstream temporary_manifest(dir + "/manifest.json.tmp");
  EXPECT_FALSE(temporary_manifest.good());

  auto file_size = [](const std::string& path) -> std::streamoff {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    return file.good() ? static_cast<std::streamoff>(file.tellg())
                       : static_cast<std::streamoff>(-1);
  };
  EXPECT_EQ(file_size(dir + "/render.pcm"),
            static_cast<std::streamoff>(kBlock * 2 * sizeof(int16_t)));
  EXPECT_EQ(file_size(dir + "/capture_raw.pcm"),
            static_cast<std::streamoff>(kBlock * 2 * sizeof(int16_t)));
  EXPECT_EQ(file_size(dir + "/capture_apm.pcm"),
            static_cast<std::streamoff>(kBlock * sizeof(int16_t)));

  for (const char* name :
       {"render.pcm", "render.log", "capture_raw.pcm", "capture_raw.log",
        "capture_apm.pcm", "capture_apm.log", "manifest.json"}) {
    std::remove((dir + "/" + name).c_str());
  }
  rmdir(dir.c_str());
  rmdir(root.c_str());
}

TEST(SessionTapGuaranteeTest, IncompleteFormatsNeverPublishManifest) {
  const std::string root =
      "/tmp/tsnx_aec_incomplete_" + std::to_string(getpid());
  std::string dir;
  {
    auto tap = SessionTap::Create(root, -1700.0);
    ASSERT_NE(tap, nullptr);
    dir = tap->dir();
    std::vector<int16_t> stereo(kBlock * 2, 17);
    tap->render().Push(stereo.data(), kBlock, kRate, 2, 1000000);
    tap->cap_raw().Push(stereo.data(), kBlock, kRate, 2, 1000100);
    std::this_thread::sleep_for(std::chrono::milliseconds(150));
    std::ifstream premature(dir + "/manifest.json");
    EXPECT_FALSE(premature.good());
  }
  std::ifstream final_manifest(dir + "/manifest.json");
  EXPECT_FALSE(final_manifest.good());
  std::ifstream temporary_manifest(dir + "/manifest.json.tmp");
  EXPECT_FALSE(temporary_manifest.good());

  for (const char* name : {"render.pcm", "render.log", "capture_raw.pcm",
                           "capture_raw.log", "capture_apm.pcm",
                           "capture_apm.log"}) {
    std::remove((dir + "/" + name).c_str());
  }
  rmdir(dir.c_str());
  rmdir(root.c_str());
}

}  // namespace
}  // namespace webrtc
