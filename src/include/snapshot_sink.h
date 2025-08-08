#pragma once

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

#include "snapshot_encoder.h"
#include "video_compositor.h"
#include "video_frame.h"

namespace agora {
namespace rtc {

class SnapshotSink {
   public:
    struct Config {
        std::string outputDir = "./snapshots";  // Directory to save snapshots
        int width = 1280;                       // Frame width
        int height = 720;                       // Frame height
        int64_t intervalInMs = 20000;           // Interval between snapshots in millisecsonds
        int quality = 90;                       // JPEG quality (1-100)
        std::vector<std::string> targetUsers;   // Empty means capture all users
        VideoCompositor::Mode mode = VideoCompositor::Mode::Individual;
        VideoCompositor::Config compositorConfig;  // Video compositor configuration
        SnapshotEncoder::Config encoderConfig;     // Snapshot encoder configuration
    };

    SnapshotSink();
    ~SnapshotSink();

    // Initialize the sink
    bool initialize(const Config& config);

    // Set the interval between snapshots in milliseconds
    void setIntervalInMs(int64_t intervalInMs);

    // Process a video frame
    void onVideoFrame(const uint8_t* yBuffer, const uint8_t* uBuffer, const uint8_t* vBuffer,
                      int32_t yStride, int32_t uStride, int32_t vStride, uint32_t width,
                      uint32_t height, uint64_t timestamp, const std::string& userId = "");

    // Start capturing
    bool start();

    // Stop capturing
    void stop();

    // Check if capturing is active
    bool isCapturing() const {
        return isCapturing_.load();
    }

    // User filtering
    bool shouldCaptureUser(const std::string& userId) const;

    // Callback for composed frames from VideoCompositor
    void onComposedFrame(const AVFrame* composedFrame);

   private:
    // Worker thread function
    void captureThread();

    // Save a single frame using new encoder
    bool saveFrame(const VideoFrame& frame, const std::string& filename);
    bool saveFrame(const AVFrame* frame, const std::string& filename);

   private:
    Config config_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> isCapturing_{false};
    std::atomic<bool> stopRequested_{false};
    std::unique_ptr<std::thread> captureThread_;

    // Frame buffer using new VideoFrame
    struct FrameData {
        uint64_t timestamp = 0;
        VideoFrame frame;
        bool valid = false;
        std::string userId;  // User ID for organizing snapshots
    };

    FrameData currentFrame_;
    std::mutex frameMutex_;
    std::atomic<int64_t> frameCount_{0};
    int64_t startTimeMs_ = 0;
    int fileCounter_ = 0;
    int64_t lastSnapshotTimeMs_ = 0;

    // Modular components
    std::unique_ptr<VideoCompositor> compositor_;
    std::unique_ptr<SnapshotEncoder> encoder_;
};

}  // namespace rtc
}  // namespace agora
