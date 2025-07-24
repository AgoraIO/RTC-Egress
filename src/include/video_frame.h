#pragma once

#include <cstdint>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
}

namespace agora {
namespace rtc {

// Forward declarations
class VideoCompositor;
class RecordingSink;
class SnapshotSink;

/**
 * VideoFrame represents a video frame with YUV420P data
 * This class is used by both recording and snapshot functionality
 */
class VideoFrame {
   public:
    // Constructors
    VideoFrame() = default;
    VideoFrame(const VideoFrame& other);
    VideoFrame(VideoFrame&& other) noexcept;

    // Assignment operators
    VideoFrame& operator=(const VideoFrame& other);
    VideoFrame& operator=(VideoFrame&& other) noexcept;

    // Destructor
    ~VideoFrame() = default;

    // Initialize from raw YUV data
    bool initializeFromYUV(const uint8_t* yBuffer, const uint8_t* uBuffer, const uint8_t* vBuffer,
                           int32_t yStride, int32_t uStride, int32_t vStride, uint32_t width,
                           uint32_t height, uint64_t timestamp, const std::string& userId = "");

    // Initialize from AVFrame
    bool initializeFromAVFrame(const AVFrame* frame, uint64_t timestamp,
                               const std::string& userId = "");

    // Convert to AVFrame (caller owns the returned frame)
    AVFrame* toAVFrame() const;

    // Basic getters
    uint32_t width() const {
        return width_;
    }
    uint32_t height() const {
        return height_;
    }
    uint64_t timestamp() const {
        return timestamp_;
    }
    const std::string& userId() const {
        return userId_;
    }
    bool valid() const {
        return valid_;
    }

    // Validation
    bool validateDimensions() const;
    size_t getDataSize() const;

    // Clear the frame data
    void clear();

   private:
    friend class VideoCompositor;
    friend class RecordingSink;
    friend class SnapshotSink;
    std::vector<uint8_t> yData_;
    std::vector<uint8_t> uData_;
    std::vector<uint8_t> vData_;
    int32_t yStride_ = 0;
    int32_t uStride_ = 0;
    int32_t vStride_ = 0;
    uint32_t width_ = 0;
    uint32_t height_ = 0;
    uint64_t timestamp_ = 0;
    bool valid_ = false;
    std::string userId_;

    // Helper methods
    bool allocateBuffers();
    void copyYUVData(const uint8_t* yBuffer, const uint8_t* uBuffer, const uint8_t* vBuffer,
                     int32_t yStride, int32_t uStride, int32_t vStride, uint32_t width,
                     uint32_t height);
};

}  // namespace rtc
}  // namespace agora
