#pragma once

#include <functional>
#include <memory>
#include <string>

typedef struct AVFrame AVFrame;

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

class FrameProcessor {
   public:
    using FrameCallback = std::function<void(const AVFrame* frame, int64_t timestamp_ms)>;

    FrameProcessor();
    ~FrameProcessor();

    bool initialize(const std::string& output_dir, int width, int height, int fps);
    void process_frame(const AVFrame* frame, int64_t timestamp_ms);
    void set_frame_callback(FrameCallback callback);
    void save_frame_as_image(const AVFrame* frame, const std::string& filename);

   private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};
