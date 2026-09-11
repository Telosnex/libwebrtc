// Generated signals only; no field audio belongs in the repository.
#include <cmath>
#include <vector>

#include "src/internal/drift_servo.h"
#include "test/gtest.h"

namespace webrtc {
namespace {
constexpr uint32_t kRate = 48000;
constexpr size_t kBlock = kRate / 100;

TEST(DriftPacingRegressionTest, SmallCorrectionDoesNotCreatePeriodicBursts) {
  for (const size_t channels : {1u, 2u}) {
    for (const double ppm : {-200.0, 200.0}) {
      SCOPED_TRACE(channels);
      SCOPED_TRACE(ppm);
      DriftServo servo;
      servo.SeedRatio(ppm);
      std::vector<int16_t> input(kBlock * channels, 137);
      std::vector<int16_t> output(kBlock * channels);
      size_t empty_callbacks = 0;
      size_t burst_callbacks = 0;
      constexpr int kWarmup = 100;
      constexpr int kMeasured = 2000;
      for (int i = 0; i < kWarmup + kMeasured; ++i) {
        const size_t blocks =
            servo.PushCaptureAndCorrect(input.data(), kBlock, kRate, channels);
        if (i >= kWarmup) {
          empty_callbacks += blocks == 0;
          burst_callbacks += blocks > 1;
        }
        for (size_t b = 0; b < blocks; ++b) {
          ASSERT_TRUE(servo.PopBlock(output.data(), kBlock));
        }
        EXPECT_FALSE(servo.PopBlock(output.data(), kBlock));
      }
      // 200 ppm accumulates only 4 ms in 20 seconds. Permit a boundary
      // crossing, not the ~125 skip/burst pairs caused by 512 vs 480 reads.
      EXPECT_LE(empty_callbacks, 1u);
      EXPECT_LE(burst_callbacks, 1u);
    }
  }
}

TEST(DriftPacingRegressionTest, ReturningToUnityKeepsTheBufferedTimeline) {
  DriftServo servo;
  servo.SetHardwareClockMode(DriftServo::HardwareClockMode::kControl);
  servo.SeedRatio(-200.0);
  std::vector<int16_t> input(kBlock, 137);
  std::vector<int16_t> output(kBlock);
  for (int i = 0; i < 100; ++i) {
    const size_t blocks =
        servo.PushCaptureAndCorrect(input.data(), kBlock, kRate, 1);
    for (size_t b = 0; b < blocks; ++b) {
      ASSERT_TRUE(servo.PopBlock(output.data(), kBlock));
    }
  }

  // An initially seeded/correcting path gets a confident unity hardware
  // estimate. Removing correction must not throw away its queued samples.
  for (int i = 0; i < 700; ++i) {
    for (const auto direction : {AudioHardwareClockDirection::kPlayout,
                                 AudioHardwareClockDirection::kCapture}) {
      servo.OnHardwareClockObservation(
          {direction, 1000000000LL + i * 10000000LL,
           static_cast<int64_t>(i) * static_cast<int64_t>(kBlock), kRate, 1});
    }
  }
  const auto stats = servo.GetStats();
  ASSERT_TRUE(stats.hardware_controlling);
  ASSERT_NEAR(stats.applied_ppm, 0.0, 1e-6);
  ASSERT_TRUE(stats.engaged) << "Unity must retain the resampler timeline";
  for (int i = 0; i < 100; ++i) {
    ASSERT_EQ(servo.PushCaptureAndCorrect(input.data(), kBlock, kRate, 1), 1u);
    ASSERT_TRUE(servo.PopBlock(output.data(), kBlock));
    EXPECT_FALSE(servo.PopBlock(output.data(), kBlock));
  }
}
}  // namespace
}  // namespace webrtc
