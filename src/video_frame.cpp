#define AG_LOG_TAG "VideoFrame"

#include "include/video_frame.h"

#include <algorithm>
#include <cstring>
#include <iostream>

#include "common/log.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace agora {
namespace rtc {

// Copy constructor
VideoFrame::VideoFrame(const VideoFrame& other)
    : yData_(other.yData_),
      uData_(other.uData_),
      vData_(other.vData_),
      yStride_(other.yStride_),
      uStride_(other.uStride_),
      vStride_(other.vStride_),
      width_(other.width_),
      height_(other.height_),
      timestamp_(other.timestamp_),
      valid_(other.valid_),
      userId_(other.userId_) {}

// Move constructor
VideoFrame::VideoFrame(VideoFrame&& other) noexcept
    : yData_(std::move(other.yData_)),
      uData_(std::move(other.uData_)),
      vData_(std::move(other.vData_)),
      yStride_(other.yStride_),
      uStride_(other.uStride_),
      vStride_(other.vStride_),
      width_(other.width_),
      height_(other.height_),
      timestamp_(other.timestamp_),
      valid_(other.valid_),
      userId_(std::move(other.userId_)) {
    // Reset source object
    other.yStride_ = other.uStride_ = other.vStride_ = 0;
    other.width_ = other.height_ = 0;
    other.timestamp_ = 0;
    other.valid_ = false;
}

// Copy assignment operator
VideoFrame& VideoFrame::operator=(const VideoFrame& other) {
    if (this != &other) {
        yData_ = other.yData_;
        uData_ = other.uData_;
        vData_ = other.vData_;
        yStride_ = other.yStride_;
        uStride_ = other.uStride_;
        vStride_ = other.vStride_;
        width_ = other.width_;
        height_ = other.height_;
        timestamp_ = other.timestamp_;
        valid_ = other.valid_;
        userId_ = other.userId_;
    }
    return *this;
}

// Move assignment operator
VideoFrame& VideoFrame::operator=(VideoFrame&& other) noexcept {
    if (this != &other) {
        yData_ = std::move(other.yData_);
        uData_ = std::move(other.uData_);
        vData_ = std::move(other.vData_);
        yStride_ = other.yStride_;
        uStride_ = other.uStride_;
        vStride_ = other.vStride_;
        width_ = other.width_;
        height_ = other.height_;
        timestamp_ = other.timestamp_;
        valid_ = other.valid_;
        userId_ = std::move(other.userId_);

        // Reset source object
        other.yStride_ = other.uStride_ = other.vStride_ = 0;
        other.width_ = other.height_ = 0;
        other.timestamp_ = 0;
        other.valid_ = false;
    }
    return *this;
}

bool VideoFrame::initializeFromYUV(const uint8_t* yBuffer, const uint8_t* uBuffer,
                                   const uint8_t* vBuffer, int32_t yStride, int32_t uStride,
                                   int32_t vStride, uint32_t width, uint32_t height,
                                   uint64_t timestamp, const std::string& userId) {
    // Validate input parameters
    if (!yBuffer || !uBuffer || !vBuffer) {
        AG_LOG_FAST(ERROR, "Invalid buffer pointers");
        return false;
    }

    if (width == 0 || height == 0 || yStride <= 0 || uStride <= 0 || vStride <= 0) {
        AG_LOG_FAST(ERROR, "Invalid frame dimensions or strides");
        return false;
    }

    // Validate stride vs width relationships
    if (yStride < static_cast<int32_t>(width) || uStride < static_cast<int32_t>(width / 2) ||
        vStride < static_cast<int32_t>(width / 2)) {
        AG_LOG_FAST(ERROR, "Invalid stride values");
        return false;
    }

    // Set frame properties
    width_ = width;
    height_ = height;
    timestamp_ = timestamp;
    userId_ = userId;
    yStride_ = yStride;
    uStride_ = uStride;
    vStride_ = vStride;

    try {
        // Calculate buffer sizes with bounds checking
        size_t ySize = static_cast<size_t>(yStride) * height;
        size_t uSize = static_cast<size_t>(uStride) * height / 2;
        size_t vSize = static_cast<size_t>(vStride) * height / 2;

        // Sanity check: prevent excessive memory allocation
        const size_t MAX_FRAME_SIZE = 8 * 1024 * 1024;  // 8MB per plane max
        if (ySize > MAX_FRAME_SIZE || uSize > MAX_FRAME_SIZE || vSize > MAX_FRAME_SIZE) {
            AG_LOG_FAST(ERROR, "Frame size too large: Y=%zu, U=%zu, V=%zu", ySize, uSize, vSize);
            return false;
        }

        // Allocate and copy Y data
        yData_.resize(ySize);
        std::memcpy(yData_.data(), yBuffer, ySize);

        // Allocate and copy U data
        uData_.resize(uSize);
        std::memcpy(uData_.data(), uBuffer, uSize);

        // Allocate and copy V data
        vData_.resize(vSize);
        std::memcpy(vData_.data(), vBuffer, vSize);

        valid_ = true;
        return true;

    } catch (const std::bad_alloc& e) {
        AG_LOG_FAST(ERROR, "Memory allocation failed: %s", e.what());
        clear();
        return false;
    } catch (const std::exception& e) {
        AG_LOG_FAST(ERROR, "Error initializing frame: %s", e.what());
        clear();
        return false;
    }
}

bool VideoFrame::initializeFromAVFrame(const AVFrame* frame, uint64_t timestamp,
                                       const std::string& userId) {
    if (!frame) {
        AG_LOG_FAST(ERROR, "Invalid AVFrame pointer");
        return false;
    }

    if (frame->format != AV_PIX_FMT_YUV420P) {
        AG_LOG_FAST(ERROR, "Unsupported pixel format: %d", frame->format);
        return false;
    }

    return initializeFromYUV(frame->data[0], frame->data[1], frame->data[2], frame->linesize[0],
                             frame->linesize[1], frame->linesize[2], frame->width, frame->height,
                             timestamp, userId);
}

AVFrame* VideoFrame::toAVFrame() const {
    if (!valid_) {
        return nullptr;
    }

    AVFrame* avFrame = av_frame_alloc();
    if (!avFrame) {
        return nullptr;
    }

    avFrame->format = AV_PIX_FMT_YUV420P;
    avFrame->width = width_;
    avFrame->height = height_;
    avFrame->pts = timestamp_;

    // Allocate buffer for the frame
    if (av_frame_get_buffer(avFrame, 32) < 0) {
        av_frame_free(&avFrame);
        return nullptr;
    }

    // Copy Y plane
    for (uint32_t y = 0; y < height_; y++) {
        std::memcpy(avFrame->data[0] + y * avFrame->linesize[0], yData_.data() + y * yStride_,
                    width_);
    }

    // Copy U plane
    for (uint32_t y = 0; y < height_ / 2; y++) {
        std::memcpy(avFrame->data[1] + y * avFrame->linesize[1], uData_.data() + y * uStride_,
                    width_ / 2);
    }

    // Copy V plane
    for (uint32_t y = 0; y < height_ / 2; y++) {
        std::memcpy(avFrame->data[2] + y * avFrame->linesize[2], vData_.data() + y * vStride_,
                    width_ / 2);
    }

    return avFrame;
}

bool VideoFrame::validateDimensions() const {
    if (width_ == 0 || height_ == 0) {
        return false;
    }

    if (width_ > 7680 || height_ > 4320) {  // 8K max
        return false;
    }

    if (yStride_ < static_cast<int32_t>(width_) || uStride_ < static_cast<int32_t>(width_ / 2) ||
        vStride_ < static_cast<int32_t>(width_ / 2)) {
        return false;
    }

    return true;
}

size_t VideoFrame::getDataSize() const {
    if (!valid_) {
        return 0;
    }
    return yData_.size() + uData_.size() + vData_.size();
}

void VideoFrame::clear() {
    yData_.clear();
    uData_.clear();
    vData_.clear();
    yStride_ = uStride_ = vStride_ = 0;
    width_ = height_ = 0;
    timestamp_ = 0;
    valid_ = false;
    userId_.clear();
}

bool VideoFrame::allocateBuffers() {
    if (width_ == 0 || height_ == 0) {
        return false;
    }

    try {
        size_t ySize = static_cast<size_t>(yStride_) * height_;
        size_t uSize = static_cast<size_t>(uStride_) * height_ / 2;
        size_t vSize = static_cast<size_t>(vStride_) * height_ / 2;

        yData_.resize(ySize);
        uData_.resize(uSize);
        vData_.resize(vSize);

        return true;
    } catch (const std::bad_alloc& e) {
        AG_LOG_FAST(ERROR, "Buffer allocation failed: %s", e.what());
        clear();
        return false;
    }
}

void VideoFrame::copyYUVData(const uint8_t* yBuffer, const uint8_t* uBuffer, const uint8_t* vBuffer,
                             int32_t yStride, int32_t uStride, int32_t vStride, uint32_t width,
                             uint32_t height) {
    // Copy Y plane
    for (uint32_t y = 0; y < height; y++) {
        std::memcpy(yData_.data() + y * yStride_, yBuffer + y * yStride, width);
    }

    // Copy U plane
    for (uint32_t y = 0; y < height / 2; y++) {
        std::memcpy(uData_.data() + y * uStride_, uBuffer + y * uStride, width / 2);
    }

    // Copy V plane
    for (uint32_t y = 0; y < height / 2; y++) {
        std::memcpy(vData_.data() + y * vStride_, vBuffer + y * vStride, width / 2);
    }
}

}  // namespace rtc
}  // namespace agora