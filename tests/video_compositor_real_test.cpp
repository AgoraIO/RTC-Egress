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

// Include the REAL project headers
#include "../src/include/video_compositor.h"
#include "../src/include/video_frame.h"

using namespace agora::rtc;

// Frame Generator for testing real VideoCompositor
class VideoCompositorFrameGenerator {
public:
    // Generate YUV420P video frame data compatible with VideoFrame
    VideoFrame generateVideoFrame(const std::string& userId, uint64_t timestamp,
                                 uint32_t width = 640, uint32_t height = 480,
                                 uint8_t colorPattern = 128) {
        // Create raw YUV buffers first
        uint32_t ySize = width * height;
        uint32_t uvSize = (width / 2) * (height / 2);

        std::vector<uint8_t> yBuffer(ySize, colorPattern);
        std::vector<uint8_t> uBuffer(uvSize, 128);
        std::vector<uint8_t> vBuffer(uvSize, 128);

        // Use the proper VideoFrame initialization API
        VideoFrame frame;
        bool success = frame.initializeFromYUV(
            yBuffer.data(), uBuffer.data(), vBuffer.data(),
            width, width / 2, width / 2,  // strides
            width, height, timestamp, userId
        );

        if (!success) {
            // Return an invalid frame
            frame.clear();
        }

        return frame;
    }

    // Generate test pattern frames for visual verification
    VideoFrame generateTestPattern(const std::string& userId, uint64_t timestamp,
                                  const std::string& pattern = "gradient",
                                  uint32_t width = 640, uint32_t height = 480) {
        // Create raw YUV buffers with patterns
        uint32_t ySize = width * height;
        uint32_t uvSize = (width / 2) * (height / 2);

        std::vector<uint8_t> yBuffer(ySize, 128); // Default gray
        std::vector<uint8_t> uBuffer(uvSize, 128);
        std::vector<uint8_t> vBuffer(uvSize, 128);

        if (pattern == "gradient") {
            for (uint32_t y = 0; y < height; y++) {
                uint8_t value = static_cast<uint8_t>((y * 255) / height);
                for (uint32_t x = 0; x < width; x++) {
                    yBuffer[y * width + x] = value;
                }
            }
        } else if (pattern == "checkerboard") {
            for (uint32_t y = 0; y < height; y++) {
                for (uint32_t x = 0; x < width; x++) {
                    uint8_t value = ((x / 32) + (y / 32)) % 2 ? 200 : 50;
                    yBuffer[y * width + x] = value;
                }
            }
        } else if (pattern == "stripes") {
            for (uint32_t y = 0; y < height; y++) {
                uint8_t value = (y / 20) % 2 ? 200 : 50;
                for (uint32_t x = 0; x < width; x++) {
                    yBuffer[y * width + x] = value;
                }
            }
        } else if (pattern == "solid") {
            // Use distinct color for each user
            uint8_t color = 64 + (std::hash<std::string>{}(userId) % 128);
            std::fill(yBuffer.begin(), yBuffer.end(), color);
        }

        // Initialize VideoFrame with the pattern data
        VideoFrame frame;
        bool success = frame.initializeFromYUV(
            yBuffer.data(), uBuffer.data(), vBuffer.data(),
            width, width / 2, width / 2,  // strides
            width, height, timestamp, userId
        );

        if (!success) {
            frame.clear();
        }

        return frame;
    }
};

class RealVideoCompositorTest : public ::testing::Test {
protected:
    void SetUp() override {
        generator_ = std::make_unique<VideoCompositorFrameGenerator>();
        
        // Create temporary test directory
        testDir_ = "/tmp/compositor_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        std::filesystem::create_directories(testDir_);
    }

    void TearDown() override {
        // Clean up test files
        std::filesystem::remove_all(testDir_);
    }

    std::unique_ptr<VideoCompositorFrameGenerator> generator_;
    std::string testDir_;
};

// Test: Real VideoCompositor initialization
TEST_F(RealVideoCompositorTest, RealCompositorInitialization) {
    VideoCompositor::Config config;
    config.outputWidth = 1280;
    config.outputHeight = 720;
    config.maxUsers = 16;
    config.preserveAspectRatio = true;
    config.frameTimeoutMs = 1000;

    VideoCompositor compositor;
    ASSERT_TRUE(compositor.initialize(config));
    
    // Verify initialization succeeded (can't easily test user count without public API)
    EXPECT_EQ(compositor.getDroppedFrameCount(), 0);
}

// Test: Real VideoCompositor single user frame addition
TEST_F(RealVideoCompositorTest, RealCompositorSingleUser) {
    VideoCompositor::Config config;
    config.outputWidth = 1280;
    config.outputHeight = 720;
    config.maxUsers = 16;

    VideoCompositor compositor;
    ASSERT_TRUE(compositor.initialize(config));

    // Add frame from single user
    auto frame = generator_->generateTestPattern("user1", 1000, "gradient", 640, 480);
    
    bool success = compositor.addUserFrame(frame, "user1");
    EXPECT_TRUE(success);
    
    // Note: Cannot easily verify user count without public API, but adding frame should succeed
}

// Test: Real VideoCompositor multiple users
TEST_F(RealVideoCompositorTest, RealCompositorMultipleUsers) {
    VideoCompositor::Config config;
    config.outputWidth = 1280;
    config.outputHeight = 720;
    config.maxUsers = 16;

    VideoCompositor compositor;
    ASSERT_TRUE(compositor.initialize(config));

    std::vector<std::string> users = {"alice", "bob", "charlie", "david"};
    
    // Add frames from multiple users
    for (size_t i = 0; i < users.size(); i++) {
        std::string pattern = (i % 2 == 0) ? "gradient" : "checkerboard";
        auto frame = generator_->generateTestPattern(users[i], 1000 + i, pattern, 640, 480);
        
        bool success = compositor.addUserFrame(frame, users[i]);
        EXPECT_TRUE(success);
    }

    // Note: Cannot verify exact user count without public API, but all additions should succeed
}

// Test: Real VideoCompositor layout calculation
TEST_F(RealVideoCompositorTest, RealCompositorLayoutCalculation) {
    VideoCompositor compositor;
    
    // Test various user counts and their expected layouts
    struct LayoutTest {
        int userCount;
        int expectedCols;
        int expectedRows;
    };
    
    std::vector<LayoutTest> layoutTests = {
        {1, 1, 1},   // Single user - full screen
        {2, 2, 1},   // Two users - side by side  
        {3, 3, 1},   // Three users - 3x1 grid (based on actual implementation)
        {4, 2, 2},   // Four users - 2x2 grid
        {5, 3, 2},   // Five users - 3x2 grid
        {6, 3, 2},   // Six users - 3x2 grid
        {9, 3, 3},   // Nine users - 3x3 grid
        {16, 4, 4}   // Sixteen users - 4x4 grid
    };

    for (const auto& test : layoutTests) {
        auto layout = compositor.calculateOptimalLayout(test.userCount);
        
        EXPECT_EQ(layout.first, test.expectedCols) 
            << "Column count mismatch for " << test.userCount << " users";
        EXPECT_EQ(layout.second, test.expectedRows) 
            << "Row count mismatch for " << test.userCount << " users";
    }
}

// Test: Real VideoCompositor user removal
TEST_F(RealVideoCompositorTest, RealCompositorUserRemoval) {
    VideoCompositor::Config config;
    config.outputWidth = 1280;
    config.outputHeight = 720;

    VideoCompositor compositor;
    ASSERT_TRUE(compositor.initialize(config));

    std::vector<std::string> users = {"user1", "user2", "user3"};
    
    // Add users
    for (const auto& userId : users) {
        auto frame = generator_->generateTestPattern(userId, 1000, "solid", 640, 480);
        compositor.addUserFrame(frame, userId);
    }

    // Note: User count testing limited by available public API

    // Remove users one by one
    for (const auto& userId : users) {
        compositor.removeUser(userId);
        // Note: We can't easily test the exact count after removal without 
        // accessing internal state, but we can verify the operation doesn't crash
    }

    // Clear all users
    compositor.clearAllUsers();
    // Note: Cannot verify user count after clear without public API
}

// Test: Real VideoCompositor with composed frame callback
TEST_F(RealVideoCompositorTest, RealCompositorFrameCallback) {
    VideoCompositor::Config config;
    config.outputWidth = 1280;
    config.outputHeight = 720;
    config.minCompositeIntervalMs = 50; // Faster composition for testing

    VideoCompositor compositor;
    ASSERT_TRUE(compositor.initialize(config));

    std::atomic<int> callbackCount{0};
    std::atomic<bool> receivedValidFrame{false};

    // Set callback to capture composed frames
    compositor.setComposedVideoFrameCallback([&](const AVFrame* frame) {
        if (frame != nullptr) {
            EXPECT_EQ(frame->width, 1280);
            EXPECT_EQ(frame->height, 720);
            receivedValidFrame = true;
        }
        callbackCount++;
    });

    // Add frames from multiple users to trigger composition
    std::vector<std::string> users = {"user1", "user2"};
    
    for (int frameIdx = 0; frameIdx < 5; frameIdx++) {
        for (const auto& userId : users) {
            auto frame = generator_->generateTestPattern(userId, frameIdx * 33, "solid", 640, 480);
            compositor.addUserFrame(frame, userId);
        }
        
        // Create composite frame
        compositor.createCompositeFrameWithoutLock(users);
        
        std::this_thread::sleep_for(std::chrono::milliseconds(60)); // Allow callback processing
    }

    // Verify callback was called with valid frames
    EXPECT_GT(callbackCount.load(), 0);
    EXPECT_TRUE(receivedValidFrame.load());
}

// Test: Real VideoCompositor with different frame sizes
TEST_F(RealVideoCompositorTest, RealCompositorMixedFrameSizes) {
    VideoCompositor::Config config;
    config.outputWidth = 1280;
    config.outputHeight = 720;
    config.preserveAspectRatio = true;

    VideoCompositor compositor;
    ASSERT_TRUE(compositor.initialize(config));

    // Add frames with different resolutions
    struct FrameTest {
        std::string userId;
        uint32_t width;
        uint32_t height;
    };
    
    std::vector<FrameTest> frameTests = {
        {"user_vga", 640, 480},
        {"user_hd", 1280, 720},
        {"user_mobile", 480, 640}, // Portrait
        {"user_small", 320, 240}
    };

    for (const auto& test : frameTests) {
        auto frame = generator_->generateTestPattern(test.userId, 1000, "gradient", test.width, test.height);
        
        bool success = compositor.addUserFrame(frame, test.userId);
        EXPECT_TRUE(success) << "Failed to add frame for " << test.userId;
    }

    // Note: Cannot verify exact frame count without public API
}

// Test: Real VideoCompositor performance and threading
TEST_F(RealVideoCompositorTest, RealCompositorConcurrentFrames) {
    VideoCompositor::Config config;
    config.outputWidth = 1280;
    config.outputHeight = 720;
    config.maxUsers = 8;

    VideoCompositor compositor;
    ASSERT_TRUE(compositor.initialize(config));

    std::atomic<int> totalFramesAdded{0};
    std::atomic<int> successfulFrames{0};
    
    const int numUsers = 4;
    const int framesPerUser = 10;
    std::vector<std::thread> threads;

    // Launch threads to add frames concurrently
    for (int userId = 0; userId < numUsers; userId++) {
        threads.emplace_back([&, userId]() {
            std::string userIdStr = "user" + std::to_string(userId);
            
            for (int frameIdx = 0; frameIdx < framesPerUser; frameIdx++) {
                uint64_t timestamp = frameIdx * 33; // 30fps timing
                std::string pattern = (userId % 2 == 0) ? "gradient" : "stripes";
                
                auto frame = generator_->generateTestPattern(userIdStr, timestamp, pattern, 640, 480);
                
                bool success = compositor.addUserFrame(frame, userIdStr);
                if (success) {
                    successfulFrames++;
                }
                totalFramesAdded++;
                
                std::this_thread::sleep_for(std::chrono::milliseconds(5)); // Small delay
            }
        });
    }

    // Wait for all threads to complete
    for (auto& thread : threads) {
        thread.join();
    }

    // Verify results
    EXPECT_EQ(totalFramesAdded.load(), numUsers * framesPerUser);
    EXPECT_GT(successfulFrames.load(), 0); // At least some frames should succeed
    // Note: Cannot verify exact user count without public API
}

// Test: Real VideoCompositor cleanup
TEST_F(RealVideoCompositorTest, RealCompositorCleanup) {
    VideoCompositor::Config config;
    config.outputWidth = 1280;
    config.outputHeight = 720;

    VideoCompositor compositor;
    ASSERT_TRUE(compositor.initialize(config));

    // Add some users
    std::vector<std::string> users = {"cleanup1", "cleanup2", "cleanup3"};
    for (const auto& userId : users) {
        auto frame = generator_->generateTestPattern(userId, 1000, "solid", 640, 480);
        compositor.addUserFrame(frame, userId);
    }

    // Test cleanup
    compositor.cleanup();
    
    // After cleanup, should be able to reinitialize
    ASSERT_TRUE(compositor.initialize(config));
    // Note: Cannot verify user count after cleanup without public API
}

// Test: Real VideoCompositor expected users functionality
TEST_F(RealVideoCompositorTest, RealCompositorExpectedUsers) {
    VideoCompositor::Config config;
    config.outputWidth = 1280;
    config.outputHeight = 720;

    VideoCompositor compositor;
    ASSERT_TRUE(compositor.initialize(config));

    // Set expected users
    std::vector<std::string> expectedUsers = {"expected1", "expected2", "expected3"};
    compositor.setExpectedUsers(expectedUsers);

    // Add frames for expected users
    for (const auto& userId : expectedUsers) {
        auto frame = generator_->generateTestPattern(userId, 1000, "gradient", 640, 480);
        bool success = compositor.addUserFrame(frame, userId);
        EXPECT_TRUE(success);
    }

    // Add frame for unexpected user
    auto unexpectedFrame = generator_->generateTestPattern("unexpected", 1000, "stripes", 640, 480);
    bool success = compositor.addUserFrame(unexpectedFrame, "unexpected");
    EXPECT_TRUE(success); // Should still succeed, just might be handled differently
    
    // Note: Cannot verify user count without public API
}

// Test: Real VideoCompositor error conditions
TEST_F(RealVideoCompositorTest, RealCompositorErrorHandling) {
    VideoCompositor compositor;

    // Test with invalid config
    VideoCompositor::Config invalidConfig;
    invalidConfig.outputWidth = 0; // Invalid
    invalidConfig.outputHeight = 720;
    
    EXPECT_FALSE(compositor.initialize(invalidConfig));

    // Test with valid config
    VideoCompositor::Config validConfig;
    validConfig.outputWidth = 1280;
    validConfig.outputHeight = 720;
    
    ASSERT_TRUE(compositor.initialize(validConfig));

    // Test adding frame before proper initialization of internal state
    auto frame = generator_->generateTestPattern("error_test", 1000, "solid", 640, 480);
    bool success = compositor.addUserFrame(frame, "error_test");
    // Should handle gracefully (might succeed or fail depending on internal state)
    
    // Test operations after cleanup
    compositor.cleanup();
    
    // Operations after cleanup should be handled gracefully
    compositor.removeUser("nonexistent");
    compositor.clearAllUsers();
}