#pragma once

#include <memory>
#include <mutex>
#include <string>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
}

namespace agora {
namespace rtc {

/**
 * SnapshotEncoder provides high-performance YUV to JPEG encoding
 */
class SnapshotEncoder {
   public:
    struct Config {
        int jpegQuality = 90;    // JPEG quality (1-100)
        bool useThreads = true;  // Enable multi-threading
    };

    struct Stats {
        uint64_t totalEncodes = 0;
        uint64_t failedEncodes = 0;
        uint64_t totalTimeMs = 0;
        uint64_t averageTimeMs = 0;
    };

    SnapshotEncoder();
    ~SnapshotEncoder();

    // Configuration
    bool initialize(const Config& config);

    // Encoding functions
    bool encodeYUVToJPEG(const uint8_t* yBuffer, const uint8_t* uBuffer, const uint8_t* vBuffer,
                         int32_t yStride, int32_t uStride, int32_t vStride, uint32_t width,
                         uint32_t height, const std::string& filename);

    bool encodeFrameToJPEG(const AVFrame* frame, const std::string& filename);

    // Statistics
    Stats getStats() const;
    void resetStats();

   private:
    struct EncoderContext {
        AVCodecContext* codecContext = nullptr;
        AVFrame* inputFrame = nullptr;
        AVFrame* outputFrame = nullptr;
        SwsContext* swsContext = nullptr;
        AVPacket* packet = nullptr;
        uint8_t* jpegBuffer = nullptr;
        int lastWidth = 0;
        int lastHeight = 0;
    };

    bool setupEncoder(int width, int height);
    bool writeJPEGFile(const uint8_t* data, size_t size, const std::string& filename);
    void cleanup();

    Config config_;
    std::unique_ptr<EncoderContext> context_;
    mutable std::mutex mutex_;
    Stats stats_;
};

}  // namespace rtc
}  // namespace agora