#pragma once

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "layout_detector.h"
#include "video_frame.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace agora {
namespace rtc {

/**
 * VideoCompositor handles multi-user video composition
 * Can be used by both recording and snapshot functionality
 */
class VideoCompositor {
   public:
    struct Config {
        uint32_t outputWidth = 1280;
        uint32_t outputHeight = 720;
        uint32_t maxUsers = 25;
        uint64_t frameTimeoutMs = 1000;  // Keep frames for 1 second
        bool preserveAspectRatio = true;
        uint64_t minCompositeIntervalMs = 16;  // 60fps max

        // Layout detection configuration (always enabled for layout stability)
        LayoutDetector::Config layoutDetectorConfig;
    };

    enum Mode {
        Individual,
        Composite,
    };

    // Callback for composed frames
    using ComposedVideoFrameCallback = std::function<void(const AVFrame* frame)>;

    VideoCompositor();
    ~VideoCompositor();

    // Configuration
    bool initialize(const Config& config);
    void setComposedVideoFrameCallback(ComposedVideoFrameCallback callback);

    // Frame input
    bool addUserFrame(const VideoFrame& frame, const std::string& userId);
    bool addUserFrame(const uint8_t* yBuffer, const uint8_t* uBuffer, const uint8_t* vBuffer,
                      int32_t yStride, int32_t uStride, int32_t vStride, uint32_t width,
                      uint32_t height, uint64_t timestamp, const std::string& userId);

    // Composition control
    bool createCompositeFrameWithoutLock(const std::vector<std::string>& activeUsers);
    void removeUser(const std::string& userId);
    void clearAllUsers();

    // Layout management
    std::pair<int, int> calculateOptimalLayout(int numUsers) const;

    // Statistics
    size_t inline getActiveUserCountWithoutLock() const;
    uint64_t getDroppedFrameCount() const {
        return droppedFrames_;
    }

    // Layout detection
    void setExpectedUsers(const std::vector<std::string>& users);

    // Cleanup
    void cleanup();

   private:
    struct UserFrameInfo {
        VideoFrame frame;
        uint64_t receivedTime;
    };

    // Configuration
    Config config_;
    bool initialized_ = false;
    std::atomic<bool> stopRequested_{false};

    // Frame buffer and timing
    std::map<std::string, UserFrameInfo> frameBuffer_;
    mutable std::mutex frameBufferMutex_;
    uint64_t lastCompositeTime_ = 0;
    uint64_t droppedFrames_ = 0;

    // Layout caching
    mutable size_t lastLayoutUserCount_ = 0;
    mutable int lastCols_ = 0;
    mutable int lastRows_ = 0;

    // Scaling contexts (cached for performance)
    std::map<std::string, SwsContext*> scalingContexts_;

    // Output frame
    AVFrame* compositeFrame_ = nullptr;

    // Callback
    ComposedVideoFrameCallback videoFrameCallback_;

    // Layout detection (always enabled for stability)
    std::unique_ptr<LayoutDetector> layoutDetector_;

    // Helper methods
    bool setupCompositeFrame();
    void cleanupOldFramesWithoutLock();
    void cleanupScalingContexts();
    uint64_t getCurrentTimeMs() const;
    void onLayoutChange(const std::vector<std::string>& activeUsers);  // LayoutDetector callback

    struct ScaledFrameInfo {
        int scaledWidth;
        int scaledHeight;
        int offsetX;
        int offsetY;
    };

    ScaledFrameInfo calculateScaledFrameInfo(const VideoFrame& frame, int cellWidth,
                                             int cellHeight) const;
    std::string getScalingContextKey(const std::string& userId, int width, int height) const;
};

}  // namespace rtc
}  // namespace agora