#import "sdk/objc/api/peerconnection/RTCAudioDeviceModule+Private.h"
#import "sdk/objc/api/peerconnection/RTCAudioTrack.h"

#include "api/make_ref_counted.h"
#include "modules/audio_device/include/mock_audio_device.h"
#include "rtc_base/thread.h"
#include "test/gmock.h"
#include "test/gtest.h"

// Exercise the options-aware transaction without opening a microphone.
@interface TestOptionsRecordingModule : RTC_OBJC_TYPE
(RTCAudioDeviceModule) @property(nonatomic) NSInteger startResult;
@property(nonatomic) webrtc::Thread* expectedWorker;
@property(nonatomic, strong) RTC_OBJC_TYPE(RTCAudioProcessingOptions) * receivedOptions;
@end

@implementation TestOptionsRecordingModule
@synthesize startResult = _startResult;
@synthesize expectedWorker = _expectedWorker;
@synthesize receivedOptions = _receivedOptions;

- (NSInteger)initAndStartRecordingWithAudioProcessingOptions:
    (RTC_OBJC_TYPE(RTCAudioProcessingOptions) *)options {
  EXPECT_TRUE(self.expectedWorker->IsCurrent());
  EXPECT_TRUE(self.hasExternalRecordingDemand);
  self.receivedOptions = options;
  return self.startResult;
}
@end

namespace webrtc {
namespace test {
namespace {
using ::testing::NiceMock;
using ::testing::Return;

class ExternalRecordingDemandObjCTest : public ::testing::Test {
 protected:
  void SetUp() override {
    worker_ = Thread::Create();
    ASSERT_TRUE(worker_->Start());
    adm_ = make_ref_counted<NiceMock<MockAudioDeviceModule>>();
    module_ = [[RTC_OBJC_TYPE(RTCAudioDeviceModule) alloc]
         initWithNativeModule:adm_
                 workerThread:worker_.get()
        audioDeviceModuleType:RTC_OBJC_TYPE(RTCAudioDeviceModuleTypePlatformDefault)];
  }
  void TearDown() override {
    module_ = nil;
    adm_ = nullptr;
    worker_->Stop();
  }
  std::unique_ptr<Thread> worker_;
  scoped_refptr<NiceMock<MockAudioDeviceModule>> adm_;
  RTC_OBJC_TYPE(RTCAudioDeviceModule) * module_;
};

TEST_F(ExternalRecordingDemandObjCTest, PeerlessAcquireAndReleaseUseWorker) {
  EXPECT_CALL(*adm_, RecordingIsInitialized()).WillOnce(Return(false));
  EXPECT_CALL(*adm_, InitRecording()).WillOnce([&] {
    EXPECT_TRUE(worker_->IsCurrent());
    EXPECT_TRUE(adm_->ExternalRecordingDemand());
    return 0;
  });
  EXPECT_CALL(*adm_, Recording()).WillOnce(Return(false)).WillOnce(Return(true));
  EXPECT_CALL(*adm_, StartRecording()).WillOnce([&] {
    EXPECT_TRUE(worker_->IsCurrent());
    return 0;
  });
  EXPECT_EQ(0, [module_ acquireExternalRecordingWithAudioProcessingOptions:nil]);
  EXPECT_TRUE(module_.hasExternalRecordingDemand);
  EXPECT_CALL(*adm_, StopRecording()).WillOnce([&] {
    EXPECT_TRUE(worker_->IsCurrent());
    return 0;
  });
  EXPECT_EQ(0, [module_ releaseExternalRecording]);
  EXPECT_FALSE(module_.hasExternalRecordingDemand);
}

TEST_F(ExternalRecordingDemandObjCTest, ReleasePreservesActiveSender) {
  adm_->SetExternalRecordingDemand(true);
  adm_->SetWebRtcRecordingDemand(true);
  EXPECT_CALL(*adm_, StopRecording()).Times(0);
  EXPECT_EQ(0, [module_ releaseExternalRecording]);
  EXPECT_FALSE(module_.hasExternalRecordingDemand);
  EXPECT_TRUE(adm_->WebRtcRecordingDemand());
}

TEST_F(ExternalRecordingDemandObjCTest, FailedReleaseCanRetry) {
  adm_->SetExternalRecordingDemand(true);
  EXPECT_CALL(*adm_, Recording()).WillRepeatedly(Return(true));
  EXPECT_CALL(*adm_, StopRecording()).WillOnce(Return(-9)).WillOnce(Return(0));
  EXPECT_EQ(-9, [module_ releaseExternalRecording]);
  EXPECT_TRUE(module_.hasExternalRecordingDemand);
  EXPECT_EQ(0, [module_ releaseExternalRecording]);
  EXPECT_FALSE(module_.hasExternalRecordingDemand);
}

TEST_F(ExternalRecordingDemandObjCTest, FailedInitializationDoesNotStart) {
  EXPECT_CALL(*adm_, RecordingIsInitialized()).WillOnce(Return(false));
  EXPECT_CALL(*adm_, InitRecording()).WillOnce(Return(-7));
  EXPECT_CALL(*adm_, StartRecording()).Times(0);
  EXPECT_EQ(-7, [module_ acquireExternalRecordingWithAudioProcessingOptions:nil]);
  EXPECT_FALSE(module_.hasExternalRecordingDemand);
}

TEST_F(ExternalRecordingDemandObjCTest, OptionsAcquireUsesWorkerAndRollsBackFailure) {
  TestOptionsRecordingModule* engine = [[TestOptionsRecordingModule alloc]
       initWithNativeModule:adm_
               workerThread:worker_.get()
      audioDeviceModuleType:RTC_OBJC_TYPE(RTCAudioDeviceModuleTypeAudioEngine)];
  engine.expectedWorker = worker_.get();
  auto* options = [[RTC_OBJC_TYPE(RTCAudioProcessingOptions) alloc] initWithEchoCancellation:YES
                                                                            noiseSuppression:NO
                                                                             autoGainControl:NO
                                                                              highPassFilter:YES];
  engine.startResult = -8;
  EXPECT_EQ(-8, [engine acquireExternalRecordingWithAudioProcessingOptions:options]);
  EXPECT_EQ(options, engine.receivedOptions);
  EXPECT_FALSE(engine.hasExternalRecordingDemand);
  engine.startResult = 0;
  EXPECT_EQ(0, [engine acquireExternalRecordingWithAudioProcessingOptions:options]);
  EXPECT_TRUE(engine.hasExternalRecordingDemand);
  // A failed repeated acquisition must not erase an existing owner's demand.
  engine.startResult = -8;
  EXPECT_EQ(-8, [engine acquireExternalRecordingWithAudioProcessingOptions:options]);
  EXPECT_TRUE(engine.hasExternalRecordingDemand);
}
}  // namespace
}  // namespace test
}  // namespace webrtc
