#include "snapshot_sink.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <opencv2/imgcodecs.hpp>
#include <sstream>
#include <thread>

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
        std::cerr << "Cannot initialize while capturing" << std::endl;
        return false;
    }

    config_ = config;

    // Create output directory if it doesn't exist
    if (!fs::exists(config_.outputDir)) {
        if (!fs::create_directories(config_.outputDir)) {
            std::cerr << "Failed to create output directory: " << config_.outputDir << std::endl;
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
        std::cerr << "Already capturing" << std::endl;
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
    std::cout << "Stopped capturing snapshots" << std::endl;
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
        std::cerr << "[SnapshotSink] Invalid buffer pointers" << std::endl;
        return;
    }

    if (width == 0 || height == 0 || yStride <= 0 || uStride <= 0 || vStride <= 0) {
        std::cerr << "[SnapshotSink] Invalid frame dimensions or strides" << std::endl;
        return;
    }

    // Validate stride vs width relationships
    if (yStride < static_cast<int32_t>(width) || uStride < static_cast<int32_t>(width / 2) ||
        vStride < static_cast<int32_t>(width / 2)) {
        std::cerr << "[SnapshotSink] Invalid stride values" << std::endl;
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
        // Convert YUV to BGR with proper error handling
        cv::Mat bgrFrame =
            yuvToBgr(yBuffer, uBuffer, vBuffer, yStride, uStride, vStride, width, height);

        if (bgrFrame.empty()) {
            std::cerr << "[SnapshotSink] Failed to convert YUV frame to BGR" << std::endl;
            return;
        }

        // Update current frame - use clone() to ensure deep copy and avoid memory sharing
        currentFrame_.frame = bgrFrame.clone();
        currentFrame_.timestamp = timestamp;
        currentFrame_.valid = true;
        currentFrame_.userId = userId;

        lock.unlock();

        if (!userId.empty()) {
            std::cout << "[SnapshotSink] Received video frame from user " << userId << ": " << width
                      << "x" << height << std::endl;
        } else {
            std::cout << "[SnapshotSink] Received video frame: " << width << "x" << height
                      << std::endl;
        }

        cv_.notify_one();

    } catch (const cv::Exception& e) {
        std::cerr << "[SnapshotSink] OpenCV exception: " << e.what() << std::endl;
        return;
    } catch (const std::exception& e) {
        std::cerr << "[SnapshotSink] Exception processing video frame: " << e.what() << std::endl;
        return;
    }
}

void SnapshotSink::captureThread() {
    std::cout << "[SnapshotSink] Capture thread started, interval: " << config_.intervalInMs << "ms"
              << std::endl;
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
        std::cout << "[SnapshotSink] Saving snapshot to: " << filename << std::endl;

        // Save the frame
        if (!saveFrame(frameToSave.frame, filename)) {
            std::cerr << "Failed to save frame: " << filename << std::endl;
        } else {
            std::cout << "[SnapshotSink] Successfully saved snapshot: " << filename << std::endl;
            lastSnapshotTimeMs_ = msSinceEpoch;
        }
    }
    std::cout << "[SnapshotSink] Capture thread stopped" << std::endl;
}

bool SnapshotSink::saveFrame(const cv::Mat& frame, const std::string& filename) {
    if (frame.empty()) {
        return false;
    }

    std::vector<int> params;
    params.push_back(cv::IMWRITE_JPEG_QUALITY);
    params.push_back(config_.quality);

    return cv::imwrite(filename, frame, params);
}

cv::Mat SnapshotSink::yuvToBgr(const uint8_t* yBuffer, const uint8_t* uBuffer,
                               const uint8_t* vBuffer, int32_t yStride, int32_t uStride,
                               int32_t vStride, uint32_t width, uint32_t height) {
    try {
        // Validate input parameters
        if (!yBuffer || !uBuffer || !vBuffer) {
            std::cerr << "[SnapshotSink] Invalid buffer pointers in yuvToBgr" << std::endl;
            return cv::Mat();
        }

        if (width <= 0 || height <= 0) {
            std::cerr << "[SnapshotSink] Invalid dimensions in yuvToBgr" << std::endl;
            return cv::Mat();
        }

        // Sanity check frame size to prevent excessive memory allocation
        const uint32_t MAX_DIMENSION = 4096;  // 4K max
        if (width > MAX_DIMENSION || height > MAX_DIMENSION) {
            std::cerr << "[SnapshotSink] Frame dimensions too large: " << width << "x" << height
                      << std::endl;
            return cv::Mat();
        }

        // Create YUV image with proper size validation
        int yuvHeight = height + height / 2;
        cv::Mat yuvImg(yuvHeight, width, CV_8UC1);

        if (yuvImg.empty()) {
            std::cerr << "[SnapshotSink] Failed to allocate YUV image" << std::endl;
            return cv::Mat();
        }

        // Copy Y plane with bounds checking
        for (uint32_t i = 0; i < height; ++i) {
            if (i * yStride + width <= static_cast<uint32_t>(yStride * height)) {
                std::memcpy(yuvImg.ptr(i), yBuffer + i * yStride, width);
            } else {
                std::cerr << "[SnapshotSink] Y plane copy bounds error at row " << i << std::endl;
                return cv::Mat();
            }
        }

        // Copy U and V planes (interleaved) with bounds checking
        uint8_t* uvPlane = yuvImg.ptr(height);
        uint32_t uvHeight = height / 2;
        uint32_t uvWidth = width / 2;

        for (uint32_t i = 0; i < uvHeight; ++i) {
            for (uint32_t j = 0; j < uvWidth; ++j) {
                uint32_t srcUOffset = i * uStride + j;
                uint32_t srcVOffset = i * vStride + j;
                uint32_t dstOffset = i * width + j * 2;

                if (dstOffset + 1 < static_cast<uint32_t>(uvHeight * width)) {
                    uvPlane[dstOffset] = uBuffer[srcUOffset];
                    uvPlane[dstOffset + 1] = vBuffer[srcVOffset];
                } else {
                    std::cerr << "[SnapshotSink] UV plane copy bounds error" << std::endl;
                    return cv::Mat();
                }
            }
        }

        // Convert YUV to BGR
        cv::Mat bgrImg;
        cv::cvtColor(yuvImg, bgrImg, cv::COLOR_YUV2BGR_NV12);

        if (bgrImg.empty()) {
            std::cerr << "[SnapshotSink] Failed to convert YUV to BGR" << std::endl;
            return cv::Mat();
        }

        // Release YUV image memory explicitly
        yuvImg.release();

        return bgrImg;

    } catch (const cv::Exception& e) {
        std::cerr << "[SnapshotSink] OpenCV exception in yuvToBgr: " << e.what() << std::endl;
        return cv::Mat();
    } catch (const std::exception& e) {
        std::cerr << "[SnapshotSink] Exception in yuvToBgr: " << e.what() << std::endl;
        return cv::Mat();
    }
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
