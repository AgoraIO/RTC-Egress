#pragma once

#include <memory>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>
}

namespace agora {
namespace common {

/**
 * RAII wrapper for AVFrame with automatic memory management
 */
class AVFramePtr {
public:
    AVFramePtr() : frame_(av_frame_alloc()) {}

    explicit AVFramePtr(AVFrame* frame) : frame_(frame) {}

    ~AVFramePtr() {
        if (frame_) {
            av_frame_free(&frame_);
        }
    }

    // Move constructor
    AVFramePtr(AVFramePtr&& other) noexcept : frame_(other.frame_) {
        other.frame_ = nullptr;
    }

    // Move assignment
    AVFramePtr& operator=(AVFramePtr&& other) noexcept {
        if (this != &other) {
            if (frame_) {
                av_frame_free(&frame_);
            }
            frame_ = other.frame_;
            other.frame_ = nullptr;
        }
        return *this;
    }

    // Disable copy constructor and assignment
    AVFramePtr(const AVFramePtr&) = delete;
    AVFramePtr& operator=(const AVFramePtr&) = delete;

    AVFrame* get() const { return frame_; }
    AVFrame* operator->() const { return frame_; }
    AVFrame& operator*() const { return *frame_; }

    bool valid() const { return frame_ != nullptr; }

    AVFrame* release() {
        AVFrame* temp = frame_;
        frame_ = nullptr;
        return temp;
    }

    void reset(AVFrame* new_frame = nullptr) {
        if (frame_) {
            av_frame_free(&frame_);
        }
        frame_ = new_frame;
    }

    /**
     * Allocate buffer for the frame with given format and dimensions
     */
    bool allocateBuffer(int width, int height, AVPixelFormat format, int align = 32) {
        if (!frame_) return false;

        frame_->format = format;
        frame_->width = width;
        frame_->height = height;

        return av_frame_get_buffer(frame_, align) >= 0;
    }

private:
    AVFrame* frame_;
};

/**
 * RAII wrapper for AVPacket with automatic memory management
 */
class AVPacketPtr {
public:
    AVPacketPtr() : packet_(av_packet_alloc()) {}

    explicit AVPacketPtr(AVPacket* packet) : packet_(packet) {}

    ~AVPacketPtr() {
        if (packet_) {
            av_packet_free(&packet_);
        }
    }

    // Move constructor
    AVPacketPtr(AVPacketPtr&& other) noexcept : packet_(other.packet_) {
        other.packet_ = nullptr;
    }

    // Move assignment
    AVPacketPtr& operator=(AVPacketPtr&& other) noexcept {
        if (this != &other) {
            if (packet_) {
                av_packet_free(&packet_);
            }
            packet_ = other.packet_;
            other.packet_ = nullptr;
        }
        return *this;
    }

    // Disable copy constructor and assignment
    AVPacketPtr(const AVPacketPtr&) = delete;
    AVPacketPtr& operator=(const AVPacketPtr&) = delete;

    AVPacket* get() const { return packet_; }
    AVPacket* operator->() const { return packet_; }
    AVPacket& operator*() const { return *packet_; }

    bool valid() const { return packet_ != nullptr; }

    AVPacket* release() {
        AVPacket* temp = packet_;
        packet_ = nullptr;
        return temp;
    }

    void reset(AVPacket* new_packet = nullptr) {
        if (packet_) {
            av_packet_free(&packet_);
        }
        packet_ = new_packet;
    }

private:
    AVPacket* packet_;
};

/**
 * RAII wrapper for SwsContext with automatic cleanup
 */
class SwsContextPtr {
public:
    SwsContextPtr() : context_(nullptr) {}

    SwsContextPtr(int srcW, int srcH, AVPixelFormat srcFormat,
                  int dstW, int dstH, AVPixelFormat dstFormat,
                  int flags = SWS_BILINEAR) {
        context_ = sws_getContext(srcW, srcH, srcFormat, dstW, dstH, dstFormat,
                                 flags, nullptr, nullptr, nullptr);
    }

    ~SwsContextPtr() {
        if (context_) {
            sws_freeContext(context_);
        }
    }

    // Move constructor
    SwsContextPtr(SwsContextPtr&& other) noexcept : context_(other.context_) {
        other.context_ = nullptr;
    }

    // Move assignment
    SwsContextPtr& operator=(SwsContextPtr&& other) noexcept {
        if (this != &other) {
            if (context_) {
                sws_freeContext(context_);
            }
            context_ = other.context_;
            other.context_ = nullptr;
        }
        return *this;
    }

    // Disable copy constructor and assignment
    SwsContextPtr(const SwsContextPtr&) = delete;
    SwsContextPtr& operator=(const SwsContextPtr&) = delete;

    SwsContext* get() const { return context_; }

    bool valid() const { return context_ != nullptr; }

    SwsContext* release() {
        SwsContext* temp = context_;
        context_ = nullptr;
        return temp;
    }

    void reset(SwsContext* new_context = nullptr) {
        if (context_) {
            sws_freeContext(context_);
        }
        context_ = new_context;
    }

private:
    SwsContext* context_;
};

/**
 * Utility function to get FFmpeg error string
 */
std::string getFFmpegErrorString(int error_code);

/**
 * Validate YUV buffer parameters
 */
bool validateYUVBuffers(const uint8_t* yBuffer, const uint8_t* uBuffer,
                       const uint8_t* vBuffer, int32_t yStride, int32_t uStride,
                       int32_t vStride, uint32_t width, uint32_t height);

} // namespace common
} // namespace agora