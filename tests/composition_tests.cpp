#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

// Include the REAL project headers
#include "../src/include/recording_sink.h"
#include "../src/include/snapshot_sink.h"
#include "../src/include/video_compositor.h"
#include "../src/include/video_frame.h"

using namespace agora::rtc;

// Advanced Composition Testing Framework
class CompositionTestFramework {
   public:
    // Generate user-specific video frames with distinct visual patterns
    std::vector<uint8_t> generateUserYUVFrame(const std::string& userId, uint64_t timestamp,
                                              uint32_t width, uint32_t height,
                                              const std::string& pattern = "user_specific") {
        uint32_t ySize = width * height;
        std::vector<uint8_t> yBuffer(ySize);

        if (pattern == "user_specific") {
            // Each user gets a unique base color and pattern
            uint8_t baseColor = 64 + (std::hash<std::string>{}(userId) % 128);
            uint32_t userHash = std::hash<std::string>{}(userId);

            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    uint8_t value = baseColor;

                    // Add user-specific pattern overlay
                    if (userHash % 4 == 0) {  // Vertical stripes
                        value += (x / 20) % 2 ? 40 : -40;
                    } else if (userHash % 4 == 1) {  // Horizontal stripes
                        value += (y / 20) % 2 ? 40 : -40;
                    } else if (userHash % 4 == 2) {  // Checkerboard
                        value += ((x / 20) + (y / 20)) % 2 ? 40 : -40;
                    } else {  // Diagonal stripes
                        value += ((x + y) / 30) % 2 ? 40 : -40;
                    }

                    // Add timestamp-based animation
                    uint32_t animFrame = (timestamp / 100) % 60;
                    if ((x + animFrame) % 80 < 10) value = 255;  // Moving highlight

                    yBuffer[y * width + x] = std::clamp(static_cast<int>(value), 0, 255);
                }
            }
        } else if (pattern == "grid_test") {
            // Grid pattern for layout testing
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    uint8_t value = 128;
                    if (x % 50 == 0 || y % 50 == 0) value = 255;  // Grid lines
                    if (x < 100 && y < 50) {                      // User ID indicator area
                        value = 64 + (std::hash<std::string>{}(userId) % 128);
                    }
                    yBuffer[y * width + x] = value;
                }
            }
        }

        return yBuffer;
    }

    // Generate multi-user test scenario
    struct MultiUserScenario {
        std::vector<std::string> userIds;
        std::vector<std::pair<uint32_t, uint32_t>> resolutions;  // width, height pairs
        std::vector<std::string> patterns;
        uint32_t durationFrames;
        uint32_t targetWidth;
        uint32_t targetHeight;
    };

    MultiUserScenario createScenario(const std::string& scenarioName) {
        MultiUserScenario scenario;

        if (scenarioName == "small_meeting") {
            scenario.userIds = {"alice", "bob", "charlie"};
            scenario.resolutions = {{640, 480}, {640, 480}, {640, 480}};
            scenario.patterns = {"user_specific", "user_specific", "user_specific"};
            scenario.durationFrames = 90;  // 3 seconds at 30fps
            scenario.targetWidth = 1280;
            scenario.targetHeight = 720;
        } else if (scenarioName == "large_meeting") {
            scenario.userIds = {"user1", "user2", "user3", "user4",
                                "user5", "user6", "user7", "user8"};
            scenario.resolutions = {{640, 480}, {1280, 720}, {480, 640},  {320, 240},
                                    {640, 480}, {800, 600},  {1024, 768}, {720, 480}};
            scenario.patterns = {"user_specific", "user_specific", "user_specific",
                                 "user_specific", "user_specific", "user_specific",
                                 "user_specific", "user_specific"};
            scenario.durationFrames = 150;  // 5 seconds
            scenario.targetWidth = 1920;
            scenario.targetHeight = 1080;
        } else if (scenarioName == "mixed_resolutions") {
            scenario.userIds = {"mobile", "desktop", "tablet", "4k_user"};
            scenario.resolutions = {{480, 640}, {1920, 1080}, {1024, 768}, {3840, 2160}};
            scenario.patterns = {"user_specific", "user_specific", "user_specific",
                                 "user_specific"};
            scenario.durationFrames = 120;  // 4 seconds
            scenario.targetWidth = 1920;
            scenario.targetHeight = 1080;
        } else if (scenarioName == "dynamic_users") {
            // Users join and leave during the test
            scenario.userIds = {"early_joiner", "late_joiner", "mid_leaver"};
            scenario.resolutions = {{640, 480}, {640, 480}, {640, 480}};
            scenario.patterns = {"user_specific", "user_specific", "user_specific"};
            scenario.durationFrames = 180;  // 6 seconds
            scenario.targetWidth = 1280;
            scenario.targetHeight = 720;
        }

        return scenario;
    }
};

class CompositionTest : public ::testing::Test {
   protected:
    void SetUp() override {
        framework_ = std::make_unique<CompositionTestFramework>();
        testDir_ = "./composition_test_" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(testDir_);
        std::filesystem::create_directories(testDir_ + "/recordings");
        std::filesystem::create_directories(testDir_ + "/snapshots");
    }

    void TearDown() override {
        std::cout << "Composition test files kept in: " << testDir_ << std::endl;
    }

    std::unique_ptr<CompositionTestFramework> framework_;
    std::string testDir_;
};

// Test: Small meeting composition (3 users)
TEST_F(CompositionTest, SmallMeetingComposition) {
    auto scenario = framework_->createScenario("small_meeting");

    RecordingSink::Config config;
    config.outputDir = testDir_ + "/recordings";
    config.mode = VideoCompositor::Mode::Composite;
    config.format = RecordingSink::OutputFormat::MP4;
    config.videoWidth = scenario.targetWidth;
    config.videoHeight = scenario.targetHeight;
    config.recordVideo = true;
    config.recordAudio = true;

    RecordingSink recordingSink;
    ASSERT_TRUE(recordingSink.initialize(config));
    ASSERT_TRUE(recordingSink.start());

    std::atomic<int> totalFramesSent{0};

    for (uint32_t frameIdx = 0; frameIdx < scenario.durationFrames; frameIdx++) {
        uint64_t timestamp = frameIdx * 33;  // 30fps

        // Send frames from all users simultaneously
        for (size_t userIdx = 0; userIdx < scenario.userIds.size(); userIdx++) {
            auto yBuffer = framework_->generateUserYUVFrame(
                scenario.userIds[userIdx], timestamp, scenario.resolutions[userIdx].first,
                scenario.resolutions[userIdx].second, scenario.patterns[userIdx]);

            uint32_t width = scenario.resolutions[userIdx].first;
            uint32_t height = scenario.resolutions[userIdx].second;
            std::vector<uint8_t> uBuffer(width * height / 4, 128);
            std::vector<uint8_t> vBuffer(width * height / 4, 128);

            recordingSink.onVideoFrame(yBuffer.data(), uBuffer.data(), vBuffer.data(), width,
                                       width / 2, width / 2, width, height, timestamp,
                                       scenario.userIds[userIdx]);

            // Generate simple audio for each user
            std::vector<int16_t> audioSamples(960 * 2);
            float frequency = 440.0f + userIdx * 100;  // Different frequency per user
            for (int i = 0; i < 960; i++) {
                double time =
                    static_cast<double>(timestamp) / 1000.0 + static_cast<double>(i) / 48000.0;
                int16_t sample =
                    static_cast<int16_t>(sin(2.0 * M_PI * frequency * time) * 0.2 * 32767);
                audioSamples[i * 2] = sample;
                audioSamples[i * 2 + 1] = sample;
            }

            recordingSink.onAudioFrame(reinterpret_cast<uint8_t*>(audioSamples.data()), 960, 48000,
                                       2, timestamp, scenario.userIds[userIdx]);

            totalFramesSent++;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    recordingSink.stop();

    // Verify composite recording was created
    bool foundComposite = false;
    size_t fileSize = 0;

    for (const auto& entry : std::filesystem::directory_iterator(config.outputDir)) {
        if (entry.path().extension() == ".mp4") {
            foundComposite = true;
            fileSize = std::filesystem::file_size(entry.path());
            break;
        }
    }

    EXPECT_TRUE(foundComposite);
    EXPECT_GT(fileSize, 100000);                                      // Should have reasonable size
    EXPECT_GT(totalFramesSent.load(), scenario.userIds.size() * 80);  // All users sent frames
}

// Test: Large meeting composition (8 users)
TEST_F(CompositionTest, LargeMeetingComposition) {
    auto scenario = framework_->createScenario("large_meeting");

    RecordingSink::Config config;
    config.outputDir = testDir_ + "/recordings";
    config.mode = VideoCompositor::Mode::Composite;
    config.format = RecordingSink::OutputFormat::MP4;
    config.videoWidth = scenario.targetWidth;
    config.videoHeight = scenario.targetHeight;
    config.recordVideo = true;
    config.recordAudio = true;

    RecordingSink recordingSink;
    ASSERT_TRUE(recordingSink.initialize(config));
    ASSERT_TRUE(recordingSink.start());

    std::atomic<int> totalFramesSent{0};

    // Use multiple threads to simulate concurrent users
    std::vector<std::thread> userThreads;

    for (size_t userIdx = 0; userIdx < scenario.userIds.size(); userIdx++) {
        userThreads.emplace_back([&, userIdx]() {
            for (uint32_t frameIdx = 0; frameIdx < scenario.durationFrames; frameIdx++) {
                uint64_t timestamp = frameIdx * 33;

                auto yBuffer = framework_->generateUserYUVFrame(
                    scenario.userIds[userIdx], timestamp, scenario.resolutions[userIdx].first,
                    scenario.resolutions[userIdx].second, scenario.patterns[userIdx]);

                uint32_t width = scenario.resolutions[userIdx].first;
                uint32_t height = scenario.resolutions[userIdx].second;
                std::vector<uint8_t> uBuffer(width * height / 4, 128);
                std::vector<uint8_t> vBuffer(width * height / 4, 128);

                recordingSink.onVideoFrame(yBuffer.data(), uBuffer.data(), vBuffer.data(), width,
                                           width / 2, width / 2, width, height, timestamp,
                                           scenario.userIds[userIdx]);

                totalFramesSent++;
                std::this_thread::sleep_for(std::chrono::milliseconds(33));
            }
        });
    }

    // Wait for all user threads
    for (auto& thread : userThreads) {
        thread.join();
    }

    recordingSink.stop();

    // Verify large composite was created
    bool foundComposite = false;
    size_t fileSize = 0;

    for (const auto& entry : std::filesystem::directory_iterator(config.outputDir)) {
        if (entry.path().extension() == ".mp4") {
            foundComposite = true;
            fileSize = std::filesystem::file_size(entry.path());
            break;
        }
    }

    EXPECT_TRUE(foundComposite);
    EXPECT_GT(fileSize, 300000);  // Large composite should be bigger
    EXPECT_GT(totalFramesSent.load(), scenario.userIds.size() * 140);  // All users sent frames
}

// Test: Mixed resolution composition
TEST_F(CompositionTest, MixedResolutionComposition) {
    auto scenario = framework_->createScenario("mixed_resolutions");

    RecordingSink::Config config;
    config.outputDir = testDir_ + "/recordings";
    config.mode = VideoCompositor::Mode::Composite;
    config.format = RecordingSink::OutputFormat::MP4;
    config.videoWidth = scenario.targetWidth;
    config.videoHeight = scenario.targetHeight;
    config.recordVideo = true;
    config.recordAudio = true;

    RecordingSink recordingSink;
    ASSERT_TRUE(recordingSink.initialize(config));
    ASSERT_TRUE(recordingSink.start());

    std::atomic<int> totalFramesSent{0};

    for (uint32_t frameIdx = 0; frameIdx < scenario.durationFrames; frameIdx++) {
        uint64_t timestamp = frameIdx * 33;

        for (size_t userIdx = 0; userIdx < scenario.userIds.size(); userIdx++) {
            auto yBuffer = framework_->generateUserYUVFrame(
                scenario.userIds[userIdx], timestamp, scenario.resolutions[userIdx].first,
                scenario.resolutions[userIdx].second,
                "grid_test"  // Use grid pattern to see resolution differences
            );

            uint32_t width = scenario.resolutions[userIdx].first;
            uint32_t height = scenario.resolutions[userIdx].second;
            std::vector<uint8_t> uBuffer(width * height / 4, 128);
            std::vector<uint8_t> vBuffer(width * height / 4, 128);

            recordingSink.onVideoFrame(yBuffer.data(), uBuffer.data(), vBuffer.data(), width,
                                       width / 2, width / 2, width, height, timestamp,
                                       scenario.userIds[userIdx]);

            totalFramesSent++;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    recordingSink.stop();

    // Verify mixed resolution composite was created
    bool foundComposite = false;

    for (const auto& entry : std::filesystem::directory_iterator(config.outputDir)) {
        if (entry.path().extension() == ".mp4") {
            foundComposite = true;
            break;
        }
    }

    EXPECT_TRUE(foundComposite);
    EXPECT_GT(totalFramesSent.load(), scenario.userIds.size() * 110);
}

// Test: Dynamic user joining/leaving
TEST_F(CompositionTest, DynamicUserComposition) {
    auto scenario = framework_->createScenario("dynamic_users");

    RecordingSink::Config config;
    config.outputDir = testDir_ + "/recordings";
    config.mode = VideoCompositor::Mode::Composite;
    config.format = RecordingSink::OutputFormat::MP4;
    config.videoWidth = scenario.targetWidth;
    config.videoHeight = scenario.targetHeight;
    config.recordVideo = true;
    config.recordAudio = true;

    RecordingSink recordingSink;
    ASSERT_TRUE(recordingSink.initialize(config));
    ASSERT_TRUE(recordingSink.start());

    std::atomic<int> totalFramesSent{0};

    for (uint32_t frameIdx = 0; frameIdx < scenario.durationFrames; frameIdx++) {
        uint64_t timestamp = frameIdx * 33;

        // Simulate dynamic user behavior
        std::vector<bool> userActive(scenario.userIds.size(), false);

        // early_joiner: active from start
        userActive[0] = true;

        // late_joiner: joins after 2 seconds (frame 60)
        if (frameIdx >= 60) userActive[1] = true;

        // mid_leaver: leaves after 4 seconds (frame 120)
        if (frameIdx < 120) userActive[2] = true;

        for (size_t userIdx = 0; userIdx < scenario.userIds.size(); userIdx++) {
            if (!userActive[userIdx]) continue;

            auto yBuffer = framework_->generateUserYUVFrame(
                scenario.userIds[userIdx], timestamp, scenario.resolutions[userIdx].first,
                scenario.resolutions[userIdx].second, scenario.patterns[userIdx]);

            uint32_t width = scenario.resolutions[userIdx].first;
            uint32_t height = scenario.resolutions[userIdx].second;
            std::vector<uint8_t> uBuffer(width * height / 4, 128);
            std::vector<uint8_t> vBuffer(width * height / 4, 128);

            recordingSink.onVideoFrame(yBuffer.data(), uBuffer.data(), vBuffer.data(), width,
                                       width / 2, width / 2, width, height, timestamp,
                                       scenario.userIds[userIdx]);

            totalFramesSent++;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    recordingSink.stop();

    // Verify dynamic composition was created
    bool foundComposite = false;

    for (const auto& entry : std::filesystem::directory_iterator(config.outputDir)) {
        if (entry.path().extension() == ".mp4") {
            foundComposite = true;
            break;
        }
    }

    EXPECT_TRUE(foundComposite);
    // Should have fewer frames than full participation due to dynamic joining/leaving
    EXPECT_GT(totalFramesSent.load(), 200);
    EXPECT_LT(totalFramesSent.load(), scenario.userIds.size() * scenario.durationFrames);
}