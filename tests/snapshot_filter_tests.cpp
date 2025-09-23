#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <thread>
#include <vector>

#include "../src/include/snapshot_sink.h"

using namespace agora::rtc;

class SnapshotFilterTest : public ::testing::Test {
   protected:
    void SetUp() override {
        outDir_ = "./snapshot_filter_test_output_" +
                  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(outDir_);
    }

    void TearDown() override {
        // Keep artifacts for inspection if needed
        // std::filesystem::remove_all(outDir_);
    }

    std::string outDir_;
};

// Verify that SnapshotSink respects targetUsers filtering when capturing individual frames
TEST_F(SnapshotFilterTest, CapturesOnlyTargetUsers) {
    SnapshotSink::Config cfg;
    cfg.outputDir = outDir_;
    cfg.width = 320;
    cfg.height = 240;
    cfg.intervalInMs = 1;  // Allow frequent snapshots
    cfg.quality = 80;
    cfg.mode = VideoCompositor::Mode::Individual;
    cfg.targetUsers = {"userA"};

    SnapshotSink sink;
    ASSERT_TRUE(sink.initialize(cfg));
    ASSERT_TRUE(sink.start());

    // Generate two simple frames for two users
    const uint32_t width = 320, height = 240;
    std::vector<uint8_t> y(width * height, 128);
    std::vector<uint8_t> u((width / 2) * (height / 2), 128);
    std::vector<uint8_t> v((width / 2) * (height / 2), 128);

    // Send a frame for non-target userB — should not produce a file
    sink.onVideoFrame(y.data(), u.data(), v.data(), width, width / 2, width / 2, width, height,
                      /*ts*/ 0, "userB");

    // Send a frame for target userA — should produce a file
    sink.onVideoFrame(y.data(), u.data(), v.data(), width, width / 2, width / 2, width, height,
                      /*ts*/ 10, "userA");

    // Allow the background capture thread to run
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    sink.stop();

    // Verify only userA files exist
    size_t userA = 0, userB = 0;
    for (const auto& entry : std::filesystem::directory_iterator(outDir_)) {
        const auto name = entry.path().filename().string();
        if (name.rfind("user_userA_snapshot_", 0) == 0) userA++;
        if (name.rfind("user_userB_snapshot_", 0) == 0) userB++;
    }

    EXPECT_GE(userA, 1u);
    EXPECT_EQ(userB, 0u);
}
