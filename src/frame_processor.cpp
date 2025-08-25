#define AG_LOG_TAG "FrameProcessor"

#include "frame_processor.h"

#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <sstream>

#include "common/log.h"

extern "C" {
#include <libavutil/imgutils.h>
#include <libswscale/swscale.h>
}

namespace fs = std::filesystem;

class FrameProcessor::Impl {
   public:
    Impl() = default;
    ~Impl() {
        if (sws_ctx_) {
            sws_freeContext(sws_ctx_);
        }
    }

    bool initialize(const std::string& output_dir, int width, int height, int fps) {
        output_dir_ = output_dir;
        width_ = width;
        height_ = height;
        fps_ = fps;
        frame_count_ = 0;
        last_save_time_ = std::chrono::system_clock::now();

        // Create output directory if it doesn't exist
        try {
            fs::create_directories(output_dir_);
        } catch (const std::exception& e) {
            AG_LOG_FAST(ERROR, "Failed to create output directory: %s", e.what());
            return false;
        }

        return true;
    }

    void process_frame(const AVFrame* frame, int64_t timestamp_ms) {
        if (!frame_callback_) return;

        // Convert frame to RGB if needed
        AVFrame* rgb_frame = nullptr;
        if (frame->format != AV_PIX_FMT_RGB24) {
            rgb_frame = av_frame_alloc();
            if (!rgb_frame) {
                AG_LOG_FAST(ERROR, "Could not allocate RGB frame");
                return;
            }

            rgb_frame->format = AV_PIX_FMT_RGB24;
            rgb_frame->width = frame->width;
            rgb_frame->height = frame->height;

            if (av_frame_get_buffer(rgb_frame, 0) < 0) {
                AG_LOG_FAST(ERROR, "Could not allocate RGB frame buffer");
                av_frame_free(&rgb_frame);
                return;
            }

            // Initialize scale context if needed
            if (!sws_ctx_) {
                sws_ctx_ = sws_getContext(frame->width, frame->height, (AVPixelFormat)frame->format,
                                          frame->width, frame->height, AV_PIX_FMT_RGB24,
                                          SWS_BILINEAR, nullptr, nullptr, nullptr);
                if (!sws_ctx_) {
                    AG_LOG_FAST(ERROR, "Could not initialize scale context");
                    av_frame_free(&rgb_frame);
                    return;
                }
            }

            // Convert to RGB
            sws_scale(sws_ctx_, frame->data, frame->linesize, 0, frame->height, rgb_frame->data,
                      rgb_frame->linesize);

            frame_callback_(rgb_frame, timestamp_ms);
            av_frame_free(&rgb_frame);
        } else {
            frame_callback_(frame, timestamp_ms);
        }

        // Save frame periodically
        auto now = std::chrono::system_clock::now();
        auto elapsed =
            std::chrono::duration_cast<std::chrono::milliseconds>(now - last_save_time_).count();

        if (elapsed >= 20000) {  // 20 seconds
            std::string filename = generate_filename();
            save_frame_as_image(frame, filename);
            last_save_time_ = now;
        }
    }

    void set_frame_callback(FrameCallback callback) {
        frame_callback_ = std::move(callback);
    }

    void save_frame_as_image(const AVFrame* frame, const std::string& filename) {
        // Implementation for saving frame as image
        // This is a simplified version - you might want to use stb_image_write or similar
        // for actual image writing
        printf("Saving frame to %s\n", filename.c_str());
    }

   private:
    std::string generate_filename() const {
        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::stringstream ss;
        ss << std::put_time(std::localtime(&in_time_t), "%Y%m%d_%H%M%S");
        ss << '_' << std::setfill('0') << std::setw(3) << ms.count() << ".jpg";

        return (fs::path(output_dir_) / ss.str()).string();
    }

    std::string output_dir_;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 30;
    int frame_count_ = 0;
    std::chrono::time_point<std::chrono::system_clock> last_save_time_;
    FrameCallback frame_callback_;
    struct SwsContext* sws_ctx_ = nullptr;
};

// FrameProcessor implementation
FrameProcessor::FrameProcessor() : impl_(std::make_unique<Impl>()) {}
FrameProcessor::~FrameProcessor() = default;

bool FrameProcessor::initialize(const std::string& output_dir, int width, int height, int fps) {
    return impl_->initialize(output_dir, width, height, fps);
}

void FrameProcessor::process_frame(const AVFrame* frame, int64_t timestamp_ms) {
    impl_->process_frame(frame, timestamp_ms);
}

void FrameProcessor::set_frame_callback(FrameCallback callback) {
    impl_->set_frame_callback(std::move(callback));
}

void FrameProcessor::save_frame_as_image(const AVFrame* frame, const std::string& filename) {
    impl_->save_frame_as_image(frame, filename);
}
