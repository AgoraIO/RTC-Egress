#define AG_LOG_TAG "FFmpegUtils"

#include "ffmpeg_utils.h"
#include <iostream>
#include "log.h"

extern "C" {
#include <libavutil/error.h>
}

namespace agora {
namespace common {

std::string getFFmpegErrorString(int error_code) {
    char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
    av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, error_code);
    return std::string(errbuf);
}

bool validateYUVBuffers(const uint8_t* yBuffer, const uint8_t* uBuffer,
                       const uint8_t* vBuffer, int32_t yStride, int32_t uStride,
                       int32_t vStride, uint32_t width, uint32_t height) {
    // Validate pointers
    if (!yBuffer || !uBuffer || !vBuffer) {
        AG_LOG_FAST(ERROR, "Invalid YUV buffer pointers");
        return false;
    }

    // Validate dimensions
    if (width == 0 || height == 0) {
        AG_LOG_FAST(ERROR, "Invalid dimensions: %ux%u", width, height);
        return false;
    }

    // Validate strides (must be at least as wide as the frame)
    if (yStride < static_cast<int32_t>(width)) {
        AG_LOG_FAST(ERROR, "Y stride (%d) less than width (%u)", yStride, width);
        return false;
    }

    // For YUV420P, U and V planes are half the width
    uint32_t chromaWidth = (width + 1) / 2;
    if (uStride < static_cast<int32_t>(chromaWidth) || vStride < static_cast<int32_t>(chromaWidth)) {
        AG_LOG_FAST(ERROR, "Chroma stride too small. U: %d, V: %d, required: %u",
                  uStride, vStride, chromaWidth);
        return false;
    }

    return true;
}

} // namespace common
} // namespace agora