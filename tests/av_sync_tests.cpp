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
#include "../src/include/video_frame.h"

using namespace agora::rtc;

// Advanced A/V Sync Test Framework
class AVSyncTestFramework {
   public:
    struct TimingMetrics {
        std::vector<uint64_t> videoTimestamps;
        std::vector<uint64_t> audioTimestamps;
        std::vector<double> avOffsets;
        double maxOffset = 0.0;
        double minOffset = 0.0;
        double avgOffset = 0.0;
        double stdDeviation = 0.0;
    };

    // Generate precise A/V frames with controllable timing
    struct PreciseAVFrame {
        std::vector<uint8_t> yBuffer;
        std::vector<uint8_t> uBuffer;
        std::vector<uint8_t> vBuffer;
        std::vector<uint8_t> audioBuffer;
        uint64_t videoTimestamp;
        uint64_t audioTimestamp;
        uint32_t width;
        uint32_t height;
        std::string userId;
    };

    PreciseAVFrame generatePreciseFrame(const std::string& userId, uint64_t baseTimestamp,
                                        int64_t avOffsetMs = 0, uint32_t width = 640,
                                        uint32_t height = 480) {
        PreciseAVFrame frame;
        frame.userId = userId;
        frame.width = width;
        frame.height = height;
        frame.videoTimestamp = baseTimestamp;
        frame.audioTimestamp = baseTimestamp + avOffsetMs;  // Intentional A/V offset for testing

        // Generate video YUV data with timestamp-based pattern
        uint32_t ySize = width * height;
        uint32_t uvSize = (width / 2) * (height / 2);

        frame.yBuffer.resize(ySize);
        frame.uBuffer.resize(uvSize, 128);
        frame.vBuffer.resize(uvSize, 128);

        // Create visual timestamp indicator (vertical bars moving based on timestamp)
        uint32_t barPosition = (baseTimestamp / 50) % width;  // Bar moves every 50ms
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                uint8_t baseColor = 64 + (std::hash<std::string>{}(userId) % 128);
                uint8_t value = (x >= barPosition && x < barPosition + 10) ? 255 : baseColor;
                frame.yBuffer[y * width + x] = value;
            }
        }

        // Generate audio with frequency based on timestamp (for audio timing verification)
        int audioSamples = 960;  // 20ms at 48kHz
        int channels = 2;
        std::vector<int16_t> audioData(audioSamples * channels);

        double frequency =
            440.0 + (baseTimestamp % 1000) / 10.0;  // Frequency changes with timestamp
        for (int i = 0; i < audioSamples; i++) {
            double time =
                static_cast<double>(baseTimestamp) / 1000.0 + static_cast<double>(i) / 48000.0;
            double sample = sin(2.0 * M_PI * frequency * time) * 0.3;
            int16_t sampleValue = static_cast<int16_t>(sample * 32767);

            for (int ch = 0; ch < channels; ch++) {
                audioData[i * channels + ch] = sampleValue;
            }
        }

        frame.audioBuffer.resize(audioSamples * channels * sizeof(int16_t));
        memcpy(frame.audioBuffer.data(), audioData.data(), frame.audioBuffer.size());

        return frame;
    }

    TimingMetrics analyzeAVSync(const std::vector<uint64_t>& videoTimestamps,
                                const std::vector<uint64_t>& audioTimestamps) {
        TimingMetrics metrics;
        metrics.videoTimestamps = videoTimestamps;
        metrics.audioTimestamps = audioTimestamps;

        // Calculate A/V offsets
        size_t minSize = std::min(videoTimestamps.size(), audioTimestamps.size());
        for (size_t i = 0; i < minSize; i++) {
            double offset =
                static_cast<double>(audioTimestamps[i]) - static_cast<double>(videoTimestamps[i]);
            metrics.avOffsets.push_back(offset);
        }

        if (!metrics.avOffsets.empty()) {
            metrics.maxOffset =
                *std::max_element(metrics.avOffsets.begin(), metrics.avOffsets.end());
            metrics.minOffset =
                *std::min_element(metrics.avOffsets.begin(), metrics.avOffsets.end());

            double sum = std::accumulate(metrics.avOffsets.begin(), metrics.avOffsets.end(), 0.0);
            metrics.avgOffset = sum / metrics.avOffsets.size();

            // Calculate standard deviation
            double variance = 0.0;
            for (double offset : metrics.avOffsets) {
                variance += std::pow(offset - metrics.avgOffset, 2);
            }
            metrics.stdDeviation = std::sqrt(variance / metrics.avOffsets.size());
        }

        return metrics;
    }
};

class AVSyncTest : public ::testing::Test {
   protected:
    void SetUp() override {
        framework_ = std::make_unique<AVSyncTestFramework>();
        testDir_ = "./av_sync_test_" +
                   std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(testDir_);
        std::filesystem::create_directories(testDir_ + "/recordings");
    }

    void TearDown() override {
        // Keep test files for inspection
        std::cout << "A/V sync test files kept in: " << testDir_ << std::endl;
    }

    std::unique_ptr<AVSyncTestFramework> framework_;
    std::string testDir_;
};

// Test: Perfect A/V synchronization
TEST_F(AVSyncTest, PerfectAVSynchronization) {
    RecordingSink::Config config;
    config.outputDir = testDir_ + "/recordings";
    config.mode = VideoCompositor::Mode::Individual;
    config.format = RecordingSink::OutputFormat::MP4;
    config.videoWidth = 640;
    config.videoHeight = 480;
    config.recordVideo = true;
    config.recordAudio = true;

    RecordingSink recordingSink;
    ASSERT_TRUE(recordingSink.initialize(config));
    ASSERT_TRUE(recordingSink.start());

    std::vector<uint64_t> videoTimestamps;
    std::vector<uint64_t> audioTimestamps;

    // Generate 60 frames (2 seconds) with perfect A/V sync
    for (int i = 0; i < 60; i++) {
        uint64_t timestamp = i * 33;  // 30fps
        auto frame =
            framework_->generatePreciseFrame("sync_perfect", timestamp, 0);  // 0ms A/V offset

        recordingSink.onVideoFrame(frame.yBuffer.data(), frame.uBuffer.data(), frame.vBuffer.data(),
                                   frame.width, frame.width / 2, frame.width / 2, frame.width,
                                   frame.height, frame.videoTimestamp, frame.userId);

        recordingSink.onAudioFrame(frame.audioBuffer.data(), 960, 48000, 2, frame.audioTimestamp,
                                   frame.userId);

        videoTimestamps.push_back(frame.videoTimestamp);
        audioTimestamps.push_back(frame.audioTimestamp);

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    recordingSink.stop();

    // Analyze A/V sync metrics
    auto metrics = framework_->analyzeAVSync(videoTimestamps, audioTimestamps);

    EXPECT_EQ(metrics.avgOffset, 0.0);     // Perfect sync
    EXPECT_LT(metrics.stdDeviation, 1.0);  // Very low deviation
    EXPECT_EQ(metrics.maxOffset, 0.0);
    EXPECT_EQ(metrics.minOffset, 0.0);
}

// Test: Audio leading video by 40ms (common in live streaming)
TEST_F(AVSyncTest, AudioLeadingVideo) {
    RecordingSink::Config config;
    config.outputDir = testDir_ + "/recordings";
    config.mode = VideoCompositor::Mode::Individual;
    config.format = RecordingSink::OutputFormat::MP4;
    config.videoWidth = 640;
    config.videoHeight = 480;
    config.recordVideo = true;
    config.recordAudio = true;

    RecordingSink recordingSink;
    ASSERT_TRUE(recordingSink.initialize(config));
    ASSERT_TRUE(recordingSink.start());

    std::vector<uint64_t> videoTimestamps;
    std::vector<uint64_t> audioTimestamps;

    // Generate frames with audio leading by 40ms
    for (int i = 0; i < 60; i++) {
        uint64_t timestamp = i * 33;
        auto frame = framework_->generatePreciseFrame("sync_audio_lead", timestamp,
                                                      -40);  // Audio 40ms ahead

        recordingSink.onVideoFrame(frame.yBuffer.data(), frame.uBuffer.data(), frame.vBuffer.data(),
                                   frame.width, frame.width / 2, frame.width / 2, frame.width,
                                   frame.height, frame.videoTimestamp, frame.userId);

        recordingSink.onAudioFrame(frame.audioBuffer.data(), 960, 48000, 2, frame.audioTimestamp,
                                   frame.userId);

        videoTimestamps.push_back(frame.videoTimestamp);
        audioTimestamps.push_back(frame.audioTimestamp);

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    recordingSink.stop();

    auto metrics = framework_->analyzeAVSync(videoTimestamps, audioTimestamps);

    EXPECT_NEAR(metrics.avgOffset, -40.0, 1.0);  // Audio should be 40ms ahead
    EXPECT_LT(metrics.stdDeviation, 5.0);        // Low deviation
}

// Test: Video leading audio by 60ms (network delay scenario)
TEST_F(AVSyncTest, VideoLeadingAudio) {
    RecordingSink::Config config;
    config.outputDir = testDir_ + "/recordings";
    config.mode = VideoCompositor::Mode::Individual;
    config.format = RecordingSink::OutputFormat::MP4;
    config.videoWidth = 640;
    config.videoHeight = 480;
    config.recordVideo = true;
    config.recordAudio = true;

    RecordingSink recordingSink;
    ASSERT_TRUE(recordingSink.initialize(config));
    ASSERT_TRUE(recordingSink.start());

    std::vector<uint64_t> videoTimestamps;
    std::vector<uint64_t> audioTimestamps;

    for (int i = 0; i < 60; i++) {
        uint64_t timestamp = i * 33;
        auto frame = framework_->generatePreciseFrame("sync_video_lead", timestamp,
                                                      60);  // Audio 60ms behind

        recordingSink.onVideoFrame(frame.yBuffer.data(), frame.uBuffer.data(), frame.vBuffer.data(),
                                   frame.width, frame.width / 2, frame.width / 2, frame.width,
                                   frame.height, frame.videoTimestamp, frame.userId);

        recordingSink.onAudioFrame(frame.audioBuffer.data(), 960, 48000, 2, frame.audioTimestamp,
                                   frame.userId);

        videoTimestamps.push_back(frame.videoTimestamp);
        audioTimestamps.push_back(frame.audioTimestamp);

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    recordingSink.stop();

    auto metrics = framework_->analyzeAVSync(videoTimestamps, audioTimestamps);

    EXPECT_NEAR(metrics.avgOffset, 60.0, 1.0);  // Audio should be 60ms behind
    EXPECT_LT(metrics.stdDeviation, 5.0);
}

// Test: Variable A/V sync (simulating network jitter)
TEST_F(AVSyncTest, VariableAVSync) {
    RecordingSink::Config config;
    config.outputDir = testDir_ + "/recordings";
    config.mode = VideoCompositor::Mode::Individual;
    config.format = RecordingSink::OutputFormat::MP4;
    config.videoWidth = 640;
    config.videoHeight = 480;
    config.recordVideo = true;
    config.recordAudio = true;

    RecordingSink recordingSink;
    ASSERT_TRUE(recordingSink.initialize(config));
    ASSERT_TRUE(recordingSink.start());

    std::vector<uint64_t> videoTimestamps;
    std::vector<uint64_t> audioTimestamps;

    // Generate frames with variable A/V offsets (simulating jitter)
    for (int i = 0; i < 60; i++) {
        uint64_t timestamp = i * 33;
        int64_t jitter = (i % 10 - 5) * 10;  // -50ms to +40ms jitter
        auto frame = framework_->generatePreciseFrame("sync_jitter", timestamp, jitter);

        recordingSink.onVideoFrame(frame.yBuffer.data(), frame.uBuffer.data(), frame.vBuffer.data(),
                                   frame.width, frame.width / 2, frame.width / 2, frame.width,
                                   frame.height, frame.videoTimestamp, frame.userId);

        recordingSink.onAudioFrame(frame.audioBuffer.data(), 960, 48000, 2, frame.audioTimestamp,
                                   frame.userId);

        videoTimestamps.push_back(frame.videoTimestamp);
        audioTimestamps.push_back(frame.audioTimestamp);

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    recordingSink.stop();

    auto metrics = framework_->analyzeAVSync(videoTimestamps, audioTimestamps);

    // With jitter, we expect higher deviation
    EXPECT_GT(metrics.stdDeviation, 10.0);  // High deviation due to jitter
    EXPECT_GE(metrics.maxOffset, 40.0);     // Max positive offset
    EXPECT_LE(metrics.minOffset, -50.0);    // Max negative offset
}