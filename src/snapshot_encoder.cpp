#include "include/snapshot_encoder.h"

#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>

extern "C" {
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
}

namespace agora {
namespace rtc {

SnapshotEncoder::SnapshotEncoder() : context_(std::make_unique<EncoderContext>()) {}

SnapshotEncoder::~SnapshotEncoder() {
    cleanup();
}

bool SnapshotEncoder::initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    config_ = config;

    // Initialize FFmpeg if not already done
    static std::once_flag ffmpeg_init_flag;
    std::call_once(ffmpeg_init_flag, []() {
        av_log_set_level(AV_LOG_ERROR);  // Reduce log verbosity
    });

    std::cout << "[SnapshotEncoder] Initialized with quality=" << config_.jpegQuality
              << ", threads=" << (config_.useThreads ? "enabled" : "disabled") << std::endl;

    return true;
}

bool SnapshotEncoder::encodeYUVToJPEG(const uint8_t* yBuffer, const uint8_t* uBuffer,
                                      const uint8_t* vBuffer, int32_t yStride, int32_t uStride,
                                      int32_t vStride, uint32_t width, uint32_t height,
                                      const std::string& filename) {
    if (!yBuffer || !uBuffer || !vBuffer || width == 0 || height == 0) {
        std::cerr << "[SnapshotEncoder] Invalid input parameters" << std::endl;
        return false;
    }

    auto start = std::chrono::steady_clock::now();

    {
        std::lock_guard<std::mutex> lock(mutex_);

        // Setup encoder if needed (dimensions changed or first use)
        if (!context_->codecContext || context_->lastWidth != (int)width ||
            context_->lastHeight != (int)height) {
            if (!setupEncoder(width, height)) {
                stats_.failedEncodes++;
                return false;
            }
        }

        // Prepare input frame
        AVFrame* frame = context_->inputFrame;

        // Set frame properties
        frame->format = AV_PIX_FMT_YUV420P;
        frame->width = width;
        frame->height = height;

        // Ensure buffer is allocated
        if (av_frame_get_buffer(frame, 32) < 0) {
            std::cerr << "[SnapshotEncoder] Failed to allocate frame buffer" << std::endl;
            stats_.failedEncodes++;
            return false;
        }

        // Copy YUV data efficiently
        // Y plane
        if (yStride == frame->linesize[0]) {
            // Direct copy if strides match
            std::memcpy(frame->data[0], yBuffer, yStride * height);
        } else {
            // Line-by-line copy if strides differ
            for (uint32_t y = 0; y < height; y++) {
                std::memcpy(frame->data[0] + y * frame->linesize[0], yBuffer + y * yStride, width);
            }
        }

        // U plane
        uint32_t uvHeight = height / 2;
        uint32_t uvWidth = width / 2;
        if (uStride == frame->linesize[1]) {
            std::memcpy(frame->data[1], uBuffer, uStride * uvHeight);
        } else {
            for (uint32_t y = 0; y < uvHeight; y++) {
                std::memcpy(frame->data[1] + y * frame->linesize[1], uBuffer + y * uStride,
                            uvWidth);
            }
        }

        // V plane
        if (vStride == frame->linesize[2]) {
            std::memcpy(frame->data[2], vBuffer, vStride * uvHeight);
        } else {
            for (uint32_t y = 0; y < uvHeight; y++) {
                std::memcpy(frame->data[2] + y * frame->linesize[2], vBuffer + y * vStride,
                            uvWidth);
            }
        }

        // Encode frame
        AVPacket* pkt = context_->packet;
        av_packet_unref(pkt);  // Reset packet

        int ret = avcodec_send_frame(context_->codecContext, frame);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
            std::cerr << "[SnapshotEncoder] Send frame failed: " << errbuf << std::endl;
            stats_.failedEncodes++;
            return false;
        }

        ret = avcodec_receive_packet(context_->codecContext, pkt);
        if (ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE];
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
            std::cerr << "[SnapshotEncoder] Receive packet failed: " << errbuf << std::endl;
            stats_.failedEncodes++;
            return false;
        }

        // Write JPEG file
        if (!writeJPEGFile(pkt->data, pkt->size, filename)) {
            stats_.failedEncodes++;
            return false;
        }
    }

    // Update statistics
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();

    stats_.totalEncodes++;
    stats_.totalTimeMs += duration;
    stats_.averageTimeMs = stats_.totalTimeMs / stats_.totalEncodes;

    return true;
}

bool SnapshotEncoder::encodeFrameToJPEG(const AVFrame* frame, const std::string& filename) {
    if (!frame || frame->format != AV_PIX_FMT_YUV420P) {
        std::cerr << "[SnapshotEncoder] Invalid frame or unsupported format" << std::endl;
        return false;
    }

    return encodeYUVToJPEG(frame->data[0], frame->data[1], frame->data[2], frame->linesize[0],
                           frame->linesize[1], frame->linesize[2], frame->width, frame->height,
                           filename);
}

bool SnapshotEncoder::setupEncoder(int width, int height) {
    // Cleanup existing context
    if (context_->codecContext) {
        avcodec_free_context(&context_->codecContext);
    }
    if (context_->inputFrame) {
        av_frame_free(&context_->inputFrame);
    }
    if (context_->packet) {
        av_packet_free(&context_->packet);
    }

    // Find MJPEG encoder
    const AVCodec* codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        std::cerr << "[SnapshotEncoder] MJPEG encoder not found" << std::endl;
        return false;
    }

    // Allocate codec context
    context_->codecContext = avcodec_alloc_context3(codec);
    if (!context_->codecContext) {
        std::cerr << "[SnapshotEncoder] Failed to allocate codec context" << std::endl;
        return false;
    }

    // Set codec parameters
    AVCodecContext* ctx = context_->codecContext;
    ctx->bit_rate = 0;  // VBR mode
    ctx->width = width;
    ctx->height = height;
    ctx->time_base = {1, 25};  // Not critical for single frame
    ctx->framerate = {25, 1};
    ctx->pix_fmt = AV_PIX_FMT_YUV420P;
    ctx->color_range = AVCOL_RANGE_JPEG;                    // Use full-range YUV for JPEG
    ctx->strict_std_compliance = FF_COMPLIANCE_UNOFFICIAL;  // Allow non-standard YUV

    // Set JPEG quality
    ctx->global_quality = FF_QP2LAMBDA * (31 - (config_.jpegQuality * 31) / 100);
    ctx->flags |= AV_CODEC_FLAG_QSCALE;

    // Threading options
    if (config_.useThreads) {
        ctx->thread_count = 0;  // Auto-detect CPU count
        ctx->thread_type = FF_THREAD_FRAME;
    }

    // Open codec
    int ret = avcodec_open2(ctx, codec, nullptr);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE];
        av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
        std::cerr << "[SnapshotEncoder] Failed to open codec: " << errbuf << std::endl;
        return false;
    }

    // Allocate frame and packet
    context_->inputFrame = av_frame_alloc();
    context_->packet = av_packet_alloc();

    if (!context_->inputFrame || !context_->packet) {
        std::cerr << "[SnapshotEncoder] Failed to allocate frame/packet" << std::endl;
        return false;
    }

    // Update tracked dimensions
    context_->lastWidth = width;
    context_->lastHeight = height;

    std::cout << "[SnapshotEncoder] Setup encoder for " << width << "x" << height
              << ", quality=" << config_.jpegQuality << std::endl;

    return true;
}

bool SnapshotEncoder::writeJPEGFile(const uint8_t* data, size_t size, const std::string& filename) {
    std::ofstream file(filename, std::ios::binary);
    if (!file) {
        std::cerr << "[SnapshotEncoder] Failed to open file: " << filename << std::endl;
        return false;
    }

    file.write(reinterpret_cast<const char*>(data), size);
    if (!file.good()) {
        std::cerr << "[SnapshotEncoder] Failed to write JPEG data" << std::endl;
        return false;
    }

    return true;
}

void SnapshotEncoder::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (context_->codecContext) {
        avcodec_free_context(&context_->codecContext);
    }
    if (context_->inputFrame) {
        av_frame_free(&context_->inputFrame);
    }
    if (context_->outputFrame) {
        av_frame_free(&context_->outputFrame);
    }
    if (context_->swsContext) {
        sws_freeContext(context_->swsContext);
    }
    if (context_->packet) {
        av_packet_free(&context_->packet);
    }
    if (context_->jpegBuffer) {
        av_free(context_->jpegBuffer);
    }
}

SnapshotEncoder::Stats SnapshotEncoder::getStats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return stats_;
}

void SnapshotEncoder::resetStats() {
    std::lock_guard<std::mutex> lock(mutex_);
    stats_ = Stats{};
}

}  // namespace rtc
}  // namespace agora