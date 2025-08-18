#include <gtest/gtest.h>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <fstream>
#include <filesystem>
#include <memory>

// Stub out Agora SDK dependencies for testing
namespace agora {
namespace rtc {

// Simplified VideoFrame for testing (without full SDK)
struct SimpleVideoFrame {
    std::vector<uint8_t> yBuffer;
    std::vector<uint8_t> uBuffer;
    std::vector<uint8_t> vBuffer;
    uint32_t width;
    uint32_t height;
    uint64_t timestamp;
    std::string userId;
};

// Simplified Audio Frame for testing
struct SimpleAudioFrame {
    std::vector<uint8_t> data;
    int samples;
    int sampleRate;
    int channels;
    uint64_t timestamp;
    std::string userId;
};

// Mock implementations to test the core logic
class TestableRecordingSink {
public:
    struct Config {
        std::string outputDir = "./recordings";
        uint32_t videoWidth = 1280;
        uint32_t videoHeight = 720;
        int videoFps = 30;
        bool recordVideo = true;
        bool recordAudio = true;
    };

    TestableRecordingSink() = default;
    
    bool initialize(const Config& config) {
        config_ = config;
        
        // Create output directory
        try {
            std::filesystem::create_directories(config_.outputDir);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool start() {
        isRecording_ = true;
        return true;
    }

    void stop() {
        isRecording_ = false;
    }

    bool isRecording() const {
        return isRecording_;
    }

    void onVideoFrame(const uint8_t* yBuffer, const uint8_t* uBuffer, const uint8_t* vBuffer,
                      int32_t yStride, int32_t uStride, int32_t vStride, uint32_t width,
                      uint32_t height, uint64_t timestamp, const std::string& userId = "") {
        if (!isRecording_) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        videoFrameCount_++;
        lastVideoTimestamp_ = timestamp;
        lastVideoUser_ = userId;
        
        // Simulate frame processing
        processedFrameSize_ = width * height * 3 / 2; // YUV420 size
    }

    void onAudioFrame(const uint8_t* audioBuffer, int samples, int sampleRate, int channels,
                      uint64_t timestamp, const std::string& userId = "") {
        if (!isRecording_) return;
        
        std::lock_guard<std::mutex> lock(mutex_);
        audioFrameCount_++;
        lastAudioTimestamp_ = timestamp;
        lastAudioUser_ = userId;
    }

    // Test accessors
    int getVideoFrameCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return videoFrameCount_;
    }

    int getAudioFrameCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return audioFrameCount_;
    }

    uint64_t getLastVideoTimestamp() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastVideoTimestamp_;
    }

    std::string getLastVideoUser() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastVideoUser_;
    }

    size_t getProcessedFrameSize() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return processedFrameSize_;
    }

private:
    Config config_;
    std::atomic<bool> isRecording_{false};
    mutable std::mutex mutex_;
    int videoFrameCount_ = 0;
    int audioFrameCount_ = 0;
    uint64_t lastVideoTimestamp_ = 0;
    uint64_t lastAudioTimestamp_ = 0;
    std::string lastVideoUser_;
    std::string lastAudioUser_;
    size_t processedFrameSize_ = 0;
};

class TestableSnapshotSink {
public:
    struct Config {
        std::string outputDir = "./snapshots";
        int width = 1280;
        int height = 720;
        int64_t intervalInMs = 1000;
        int quality = 90;
    };

    TestableSnapshotSink() = default;

    bool initialize(const Config& config) {
        config_ = config;
        
        try {
            std::filesystem::create_directories(config_.outputDir);
            return true;
        } catch (...) {
            return false;
        }
    }

    bool start() {
        if (isCapturing_) return false;
        
        isCapturing_ = true;
        stopRequested_ = false;
        
        // Start capture thread
        captureThread_ = std::make_unique<std::thread>([this]() {
            captureThreadFunc();
        });
        
        return true;
    }

    void stop() {
        if (!isCapturing_) return;
        
        stopRequested_ = true;
        cv_.notify_all();
        
        if (captureThread_ && captureThread_->joinable()) {
            captureThread_->join();
        }
        captureThread_.reset();
        
        isCapturing_ = false;
    }

    bool isCapturing() const {
        return isCapturing_;
    }

    void onVideoFrame(const uint8_t* yBuffer, const uint8_t* uBuffer, const uint8_t* vBuffer,
                      int32_t yStride, int32_t uStride, int32_t vStride, uint32_t width,
                      uint32_t height, uint64_t timestamp, const std::string& userId = "") {
        if (!isCapturing_) return;
        
        std::lock_guard<std::mutex> lock(frameMutex_);
        
        // Store current frame
        currentFrame_.timestamp = timestamp;
        currentFrame_.userId = userId;
        currentFrame_.width = width;
        currentFrame_.height = height;
        currentFrame_.valid = true;
        
        frameCount_++;
        cv_.notify_one();
    }

    int getSnapshotCount() const {
        std::lock_guard<std::mutex> lock(frameMutex_);
        return snapshotCount_;
    }

    int getFrameCount() const {
        std::lock_guard<std::mutex> lock(frameMutex_);
        return frameCount_;
    }

private:
    struct FrameData {
        uint64_t timestamp = 0;
        std::string userId;
        uint32_t width = 0;
        uint32_t height = 0;
        bool valid = false;
    };

    void captureThreadFunc() {
        auto lastSnapshotTime = std::chrono::steady_clock::now();
        
        while (!stopRequested_) {
            std::unique_lock<std::mutex> lock(frameMutex_);
            
            // Wait for frame or timeout
            cv_.wait_for(lock, std::chrono::milliseconds(100), [this]() {
                return stopRequested_ || currentFrame_.valid;
            });
            
            if (stopRequested_) break;
            
            auto now = std::chrono::steady_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - lastSnapshotTime).count();
            
            if (currentFrame_.valid && elapsed >= config_.intervalInMs) {
                // Simulate saving snapshot
                std::string filename = config_.outputDir + "/snapshot_" + 
                                     std::to_string(snapshotCount_) + ".jpg";
                
                // Create a dummy file to simulate snapshot
                std::ofstream file(filename);
                file << "Dummy JPEG data for testing - timestamp: " << currentFrame_.timestamp;
                file.close();
                
                snapshotCount_++;
                lastSnapshotTime = now;
                currentFrame_.valid = false;
            }
        }
    }

    Config config_;
    std::atomic<bool> isCapturing_{false};
    std::atomic<bool> stopRequested_{false};
    std::unique_ptr<std::thread> captureThread_;
    
    mutable std::mutex frameMutex_;
    std::condition_variable cv_;
    FrameData currentFrame_;
    int frameCount_ = 0;
    int snapshotCount_ = 0;
};

class TestableVideoCompositor {
public:
    struct Config {
        uint32_t outputWidth = 1280;
        uint32_t outputHeight = 720;
        uint32_t maxUsers = 16;
    };

    TestableVideoCompositor() = default;

    bool initialize(const Config& config) {
        config_ = config;
        return config.outputWidth > 0 && config.outputHeight > 0;
    }

    bool addUserFrame(const uint8_t* yBuffer, const uint8_t* uBuffer, const uint8_t* vBuffer,
                      int32_t yStride, int32_t uStride, int32_t vStride, uint32_t width,
                      uint32_t height, uint64_t timestamp, const std::string& userId) {
        std::lock_guard<std::mutex> lock(mutex_);
        
        UserFrame frame;
        frame.userId = userId;
        frame.timestamp = timestamp;
        frame.width = width;
        frame.height = height;
        frame.receivedTime = std::chrono::steady_clock::now();
        
        userFrames_[userId] = frame;
        frameCount_++;
        
        // Simulate composition when we have multiple users
        if (userFrames_.size() > 1) {
            compositeFrameCount_++;
        }
        
        return true;
    }

    void removeUser(const std::string& userId) {
        std::lock_guard<std::mutex> lock(mutex_);
        userFrames_.erase(userId);
    }

    size_t getActiveUserCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return userFrames_.size();
    }

    int getFrameCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frameCount_;
    }

    int getCompositeFrameCount() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return compositeFrameCount_;
    }

    std::pair<int, int> calculateOptimalLayout(int numUsers) const {
        if (numUsers <= 1) return {1, 1};
        if (numUsers <= 2) return {2, 1};
        if (numUsers <= 4) return {2, 2};
        if (numUsers <= 6) return {3, 2};
        if (numUsers <= 9) return {3, 3};
        if (numUsers <= 12) return {4, 3};
        return {4, 4};
    }

private:
    struct UserFrame {
        std::string userId;
        uint64_t timestamp;
        uint32_t width;
        uint32_t height;
        std::chrono::steady_clock::time_point receivedTime;
    };

    Config config_;
    mutable std::mutex mutex_;
    std::map<std::string, UserFrame> userFrames_;
    int frameCount_ = 0;
    int compositeFrameCount_ = 0;
};

} // namespace rtc
} // namespace agora

// Frame Generator for testing
class TestFrameGenerator {
public:
    struct VideoFrameData {
        std::vector<uint8_t> yBuffer;
        std::vector<uint8_t> uBuffer;
        std::vector<uint8_t> vBuffer;
        uint32_t width;
        uint32_t height;
        uint64_t timestamp;
        std::string userId;
    };

    VideoFrameData generateVideoFrame(const std::string& userId, uint64_t timestamp,
                                     uint32_t width = 640, uint32_t height = 480,
                                     uint8_t colorPattern = 128) {
        VideoFrameData frame;
        frame.width = width;
        frame.height = height;
        frame.timestamp = timestamp;
        frame.userId = userId;

        uint32_t ySize = width * height;
        uint32_t uvSize = (width / 2) * (height / 2);

        frame.yBuffer.resize(ySize, colorPattern);
        frame.uBuffer.resize(uvSize, 128);
        frame.vBuffer.resize(uvSize, 128);

        return frame;
    }

    std::vector<uint8_t> generateAudioFrame(int samples = 960, int sampleRate = 48000, 
                                           int channels = 2) {
        std::vector<int16_t> audioSamples(samples * channels);
        
        for (int i = 0; i < samples; i++) {
            double time = static_cast<double>(i) / sampleRate;
            double sampleValue = sin(2.0 * M_PI * 440.0 * time) * 0.3;
            int16_t sample = static_cast<int16_t>(sampleValue * 32767);
            audioSamples[i * 2] = sample;
            audioSamples[i * 2 + 1] = sample;
        }

        std::vector<uint8_t> audioBuffer(samples * channels * sizeof(int16_t));
        memcpy(audioBuffer.data(), audioSamples.data(), audioBuffer.size());
        
        return audioBuffer;
    }
};

class SimpleRealComponentTest : public ::testing::Test {
protected:
    void SetUp() override {
        generator_ = std::make_unique<TestFrameGenerator>();
        
        testDir_ = "/tmp/egress_simple_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(testDir_);
        std::filesystem::create_directories(testDir_ + "/snapshots");
        std::filesystem::create_directories(testDir_ + "/recordings");
    }

    void TearDown() override {
        std::filesystem::remove_all(testDir_);
    }

    std::unique_ptr<TestFrameGenerator> generator_;
    std::string testDir_;
};

// Test: Simple RecordingSink functionality
TEST_F(SimpleRealComponentTest, RecordingSinkBasicFunctionality) {
    using namespace agora::rtc;
    
    TestableRecordingSink::Config config;
    config.outputDir = testDir_ + "/recordings";
    config.videoWidth = 640;
    config.videoHeight = 480;

    TestableRecordingSink recordingSink;
    ASSERT_TRUE(recordingSink.initialize(config));
    ASSERT_TRUE(recordingSink.start());
    EXPECT_TRUE(recordingSink.isRecording());

    // Send video frames
    for (int i = 0; i < 10; i++) {
        auto frameData = generator_->generateVideoFrame("user1", i * 33, 640, 480);
        
        recordingSink.onVideoFrame(
            frameData.yBuffer.data(),
            frameData.uBuffer.data(), 
            frameData.vBuffer.data(),
            640, 320, 320,
            640, 480,
            frameData.timestamp,
            frameData.userId
        );
    }

    // Send audio frames
    for (int i = 0; i < 15; i++) {
        auto audioData = generator_->generateAudioFrame(960, 48000, 2);
        
        recordingSink.onAudioFrame(
            audioData.data(),
            960, 48000, 2,
            i * 20, // 20ms intervals
            "user1"
        );
    }

    recordingSink.stop();
    EXPECT_FALSE(recordingSink.isRecording());

    // Verify frame counts
    EXPECT_EQ(recordingSink.getVideoFrameCount(), 10);
    EXPECT_EQ(recordingSink.getAudioFrameCount(), 15);
    EXPECT_EQ(recordingSink.getLastVideoUser(), "user1");
    EXPECT_GT(recordingSink.getProcessedFrameSize(), 0);
}

// Test: Simple SnapshotSink functionality
TEST_F(SimpleRealComponentTest, SnapshotSinkBasicFunctionality) {
    using namespace agora::rtc;
    
    TestableSnapshotSink::Config config;
    config.outputDir = testDir_ + "/snapshots";
    config.intervalInMs = 100; // Fast snapshots for testing
    config.width = 640;
    config.height = 480;

    TestableSnapshotSink snapshotSink;
    ASSERT_TRUE(snapshotSink.initialize(config));
    ASSERT_TRUE(snapshotSink.start());
    EXPECT_TRUE(snapshotSink.isCapturing());

    // Send frames
    for (int i = 0; i < 5; i++) {
        auto frameData = generator_->generateVideoFrame("user1", i * 33, 640, 480);
        
        snapshotSink.onVideoFrame(
            frameData.yBuffer.data(),
            frameData.uBuffer.data(), 
            frameData.vBuffer.data(),
            640, 320, 320,
            640, 480,
            frameData.timestamp,
            frameData.userId
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds(120)); // Allow snapshots
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    snapshotSink.stop();
    EXPECT_FALSE(snapshotSink.isCapturing());

    // Verify snapshots were taken
    EXPECT_GT(snapshotSink.getSnapshotCount(), 0);
    EXPECT_EQ(snapshotSink.getFrameCount(), 5);

    // Verify snapshot files exist
    int fileCount = 0;
    for (const auto& entry : std::filesystem::directory_iterator(config.outputDir)) {
        if (entry.path().extension() == ".jpg") {
            fileCount++;
        }
    }
    EXPECT_GT(fileCount, 0);
}

// Test: Simple VideoCompositor functionality
TEST_F(SimpleRealComponentTest, VideoCompositorBasicFunctionality) {
    using namespace agora::rtc;
    
    TestableVideoCompositor::Config config;
    config.outputWidth = 1280;
    config.outputHeight = 720;
    config.maxUsers = 4;

    TestableVideoCompositor compositor;
    ASSERT_TRUE(compositor.initialize(config));

    // Add frames from multiple users
    std::vector<std::string> users = {"alice", "bob", "charlie"};
    
    for (int frameIdx = 0; frameIdx < 5; frameIdx++) {
        for (const auto& userId : users) {
            auto frameData = generator_->generateVideoFrame(userId, frameIdx * 33, 640, 480);
            
            bool success = compositor.addUserFrame(
                frameData.yBuffer.data(),
                frameData.uBuffer.data(),
                frameData.vBuffer.data(),
                640, 320, 320,
                640, 480,
                frameData.timestamp,
                frameData.userId
            );
            EXPECT_TRUE(success);
        }
    }

    // Verify composition
    EXPECT_EQ(compositor.getActiveUserCount(), users.size());
    EXPECT_GT(compositor.getFrameCount(), 0);
    EXPECT_GT(compositor.getCompositeFrameCount(), 0); // Should have composite frames

    // Test layout calculation
    auto layout = compositor.calculateOptimalLayout(users.size());
    EXPECT_EQ(layout.first, 2); // 3 users -> 2x2 layout (since we round up)
    EXPECT_EQ(layout.second, 2);

    // Test user removal
    compositor.removeUser("alice");
    EXPECT_EQ(compositor.getActiveUserCount(), 2);
}

// Test: Concurrent operations
TEST_F(SimpleRealComponentTest, ConcurrentOperations) {
    using namespace agora::rtc;
    
    // Setup both recording and snapshot
    TestableRecordingSink::Config recConfig;
    recConfig.outputDir = testDir_ + "/recordings";
    
    TestableSnapshotSink::Config snapConfig;
    snapConfig.outputDir = testDir_ + "/snapshots";
    snapConfig.intervalInMs = 200;

    TestableRecordingSink recordingSink;
    TestableSnapshotSink snapshotSink;
    
    ASSERT_TRUE(recordingSink.initialize(recConfig));
    ASSERT_TRUE(snapshotSink.initialize(snapConfig));
    
    ASSERT_TRUE(recordingSink.start());
    ASSERT_TRUE(snapshotSink.start());

    // Send frames to both simultaneously
    for (int i = 0; i < 10; i++) {
        auto frameData = generator_->generateVideoFrame("user1", i * 50, 640, 480);
        
        recordingSink.onVideoFrame(
            frameData.yBuffer.data(), frameData.uBuffer.data(), frameData.vBuffer.data(),
            640, 320, 320, 640, 480, frameData.timestamp, frameData.userId
        );
        
        snapshotSink.onVideoFrame(
            frameData.yBuffer.data(), frameData.uBuffer.data(), frameData.vBuffer.data(),
            640, 320, 320, 640, 480, frameData.timestamp, frameData.userId
        );
        
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    recordingSink.stop();
    snapshotSink.stop();

    // Verify both worked
    EXPECT_GT(recordingSink.getVideoFrameCount(), 0);
    EXPECT_GT(snapshotSink.getFrameCount(), 0);
}

// Test: Error handling
TEST_F(SimpleRealComponentTest, ErrorHandling) {
    using namespace agora::rtc;
    
    // Test invalid directory
    TestableRecordingSink::Config invalidConfig;
    invalidConfig.outputDir = "/invalid/nonexistent/path/that/cannot/be/created";
    
    TestableRecordingSink recordingSink;
    EXPECT_FALSE(recordingSink.initialize(invalidConfig)); // Should fail
    
    // Test valid config
    TestableRecordingSink::Config validConfig;
    validConfig.outputDir = testDir_ + "/valid";
    
    EXPECT_TRUE(recordingSink.initialize(validConfig)); // Should succeed
}