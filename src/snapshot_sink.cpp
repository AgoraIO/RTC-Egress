#define AG_LOG_TAG "SnapshotSink"

#include "snapshot_sink.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <thread>

#include "common/log.h"

namespace agora {
namespace rtc {

namespace fs = std::filesystem;

SnapshotSink::SnapshotSink() {
    // Create output directory if it doesn't exist
    fs::create_directories("./snapshots");
}

SnapshotSink::~SnapshotSink() {
    stop();
}

bool SnapshotSink::initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isCapturing_.load()) {
        AG_LOG_FAST(ERROR, "Cannot initialize while capturing");
        return false;
    }

    config_ = config;

    // Create output directory if it doesn't exist
    if (!fs::exists(config_.outputDir)) {
        if (!fs::create_directories(config_.outputDir)) {
            AG_LOG_FAST(ERROR, "Failed to create output directory: %s", config_.outputDir.c_str());
            return false;
        }
    }

    // Initialize modular components
    encoder_ = std::make_unique<SnapshotEncoder>();
    config_.encoderConfig.jpegQuality = config_.quality;
    if (!encoder_->initialize(config_.encoderConfig)) {
        AG_LOG_FAST(ERROR, "Failed to initialize SnapshotEncoder");
        return false;
    }

    if (config_.enableComposition) {
        compositor_ = std::make_unique<VideoCompositor>();
        config_.compositorConfig.outputWidth = config_.width;
        config_.compositorConfig.outputHeight = config_.height;
        if (!compositor_->initialize(config_.compositorConfig)) {
            AG_LOG_FAST(ERROR, "Failed to initialize VideoCompositor");
            return false;
        }
    }

    return true;
}

void SnapshotSink::setIntervalInMs(int64_t intervalInMs) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.intervalInMs = intervalInMs;
}

bool SnapshotSink::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isCapturing_.load()) {
        AG_LOG_FAST(WARN, "Already capturing");
        return false;
    }

    stopRequested_ = false;
    isCapturing_ = true;
    frameCount_ = 0;
    startTimeMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();

    captureThread_ = std::make_unique<std::thread>(&SnapshotSink::captureThread, this);
    return true;
}

void SnapshotSink::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!isCapturing_.load()) {
            return;
        }

        stopRequested_ = true;
        cv_.notify_all();
    }

    if (captureThread_ && captureThread_->joinable()) {
        captureThread_->join();
        captureThread_.reset();
    }

    isCapturing_ = false;
    AG_LOG_FAST(INFO, "Stopped capturing snapshots");
}

void SnapshotSink::onVideoFrame(const uint8_t* yBuffer, const uint8_t* uBuffer,
                                const uint8_t* vBuffer, int32_t yStride, int32_t uStride,
                                int32_t vStride, uint32_t width, uint32_t height,
                                uint64_t timestamp, const std::string& userId) {
    if (!isCapturing_.load()) {
        return;
    }

    // Check if this user should be captured
    if (!shouldCaptureUser(userId)) {
        return;
    }

    // Validate input parameters
    if (!yBuffer || !uBuffer || !vBuffer) {
        AG_LOG_FAST(ERROR, "Invalid buffer pointers");
        return;
    }

    if (width == 0 || height == 0 || yStride <= 0 || uStride <= 0 || vStride <= 0) {
        AG_LOG_FAST(ERROR, "Invalid frame dimensions or strides");
        return;
    }

    // Validate stride vs width relationships
    if (yStride < static_cast<int32_t>(width) || uStride < static_cast<int32_t>(width / 2) ||
        vStride < static_cast<int32_t>(width / 2)) {
        AG_LOG_FAST(ERROR, "Invalid stride values");
        return;
    }

    auto now = std::chrono::system_clock::now();
    auto currentTimeMs =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::unique_lock<std::mutex> lock(frameMutex_);
    if ((currentTimeMs - lastSnapshotTimeMs_) < config_.intervalInMs) {
        // Not enough time has passed since last snapshot, skip this frame
        return;
    }

    try {
        // Create VideoFrame from YUV data
        VideoFrame videoFrame;
        if (!videoFrame.initializeFromYUV(yBuffer, uBuffer, vBuffer, yStride, uStride, vStride,
                                          width, height, timestamp, userId)) {
            AG_LOG_FAST(ERROR, "Failed to create VideoFrame from YUV data");
            return;
        }

        // Handle frame based on configuration
        if (config_.enableComposition && compositor_) {
            // Add frame to compositor for multi-user composition
            compositor_->addUserFrame(videoFrame, userId);
        } else {
            // Direct single-frame processing
            currentFrame_.frame = std::move(videoFrame);
            currentFrame_.timestamp = timestamp;
            currentFrame_.valid = true;
            currentFrame_.userId = userId;

            lock.unlock();

            if (!userId.empty()) {
                AG_LOG_FAST(INFO, "Received video frame from user %s: %dx%d", userId.c_str(), width,
                            height);
            } else {
                AG_LOG_FAST(INFO, "Received video frame: %dx%d", width, height);
            }

            cv_.notify_one();
        }

    } catch (const std::exception& e) {
        AG_LOG_FAST(ERROR, "Exception processing video frame: %s", e.what());
        return;
    }
}

void SnapshotSink::captureThread() {
    AG_LOG_FAST(INFO, "Capture thread started, interval: %dms", config_.intervalInMs);
    while (!stopRequested_.load()) {
        FrameData frameToSave;
        {
            std::unique_lock<std::mutex> lock(frameMutex_);
            // Wait for a new frame or stop request
            cv_.wait(lock, [this] { return currentFrame_.valid || stopRequested_.load(); });

            if (stopRequested_.load()) {
                break;
            }

            if (!currentFrame_.valid) {
                continue;
            }

            // Copy the current frame
            frameToSave = currentFrame_;
            currentFrame_.valid = false;
        }

        // Generate filename with timestamp
        auto now = std::chrono::system_clock::now();
        auto time = std::chrono::system_clock::to_time_t(now);
        auto msSinceEpoch =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

        std::ostringstream oss;

        // Get current user ID for filename
        std::string userId = frameToSave.userId;

        if (!userId.empty()) {
            oss << config_.outputDir << "/user_" << userId << "_snapshot_"
                << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S_") << std::setfill('0')
                << std::setw(3) << msSinceEpoch % 1000 << "_" << (frameCount_++) << ".jpg";
        } else {
            oss << config_.outputDir << "/snapshot_"
                << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S_") << std::setfill('0')
                << std::setw(3) << msSinceEpoch % 1000 << "_" << (frameCount_++) << ".jpg";
        }

        std::string filename = oss.str();
        AG_LOG_FAST(INFO, "Saving snapshot to: %s", filename.c_str());

        // Save the frame
        if (!saveFrame(frameToSave.frame, filename)) {
            AG_LOG_FAST(ERROR, "Failed to save frame: %s", filename.c_str());
        } else {
            AG_LOG_FAST(INFO, "Successfully saved snapshot: %s", filename.c_str());
            lastSnapshotTimeMs_ = msSinceEpoch;
        }
    }
    AG_LOG_FAST(INFO, "Capture thread stopped");
}

bool SnapshotSink::saveFrame(const VideoFrame& frame, const std::string& filename) {
    if (!frame.valid() || !encoder_) {
        return false;
    }

    // Convert VideoFrame to AVFrame and use that for encoding
    AVFrame* avFrame = frame.toAVFrame();
    if (!avFrame) {
        return false;
    }

    bool result = encoder_->encodeFrameToJPEG(avFrame, filename);
    av_frame_free(&avFrame);
    return result;
}

bool SnapshotSink::saveFrame(const AVFrame* frame, const std::string& filename) {
    if (!frame || !encoder_) {
        return false;
    }

    return encoder_->encodeFrameToJPEG(frame, filename);
}

bool SnapshotSink::shouldCaptureUser(const std::string& userId) const {
    // If no target users specified, capture all users
    if (config_.targetUsers.empty()) {
        return true;
    }

    // Check if user is in the target list
    return std::find(config_.targetUsers.begin(), config_.targetUsers.end(), userId) !=
           config_.targetUsers.end();
}

}  // namespace rtc
}  // namespace agora
