#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace agora {
namespace rtc {

struct AudioFrame {
    std::vector<uint8_t> data;
    uint64_t timestamp;
    int sampleRate;
    int channels;
    bool valid = false;
    std::string userId;
};

struct VideoFrame {
    std::vector<uint8_t> yData;
    std::vector<uint8_t> uData;
    std::vector<uint8_t> vData;
    int yStride;
    int uStride;
    int vStride;
    uint32_t width;
    uint32_t height;
    uint64_t timestamp;
    bool valid = false;
    std::string userId;
};

class RecordingSink {
   public:
    enum class RecordingMode {
        INDIVIDUAL,  // Record each user separately
        COMPOSITE    // Record mixed stream
    };

    enum class OutputFormat { MP4, AVI, MKV };

    struct Config {
        std::string outputDir = "./recordings";
        RecordingMode mode = RecordingMode::INDIVIDUAL;
        OutputFormat format = OutputFormat::MP4;

        // Video settings
        uint32_t videoWidth = 1280;
        uint32_t videoHeight = 720;
        int videoFps = 30;
        int videoBitrate = 2000000;  // 2Mbps
        std::string videoCodec = "libx264";

        // Audio settings
        int audioSampleRate = 48000;
        int audioChannels = 2;
        int audioBitrate = 128000;  // 128kbps
        std::string audioCodec = "aac";

        // Recording settings
        int maxDurationSeconds = 3600;  // 1 hour max
        bool recordVideo = true;
        bool recordAudio = true;

        // Buffer settings
        int videoBufferSize = 30;   // frames
        int audioBufferSize = 100;  // frames

        // User filtering
        std::vector<std::string> targetUsers;  // Empty means record all users
    };

    RecordingSink();
    ~RecordingSink();

    bool initialize(const Config& config);
    bool start();
    void stop();
    bool isRecording() const {
        return isRecording_.load();
    }

    // Video frame input
    void onVideoFrame(const uint8_t* yBuffer, const uint8_t* uBuffer, const uint8_t* vBuffer,
                      int32_t yStride, int32_t uStride, int32_t vStride, uint32_t width,
                      uint32_t height, uint64_t timestamp, const std::string& userId = "");

    // Audio frame input
    void onAudioFrame(const uint8_t* audioBuffer, int samples, int sampleRate, int channels,
                      uint64_t timestamp, const std::string& userId = "");

    // Configuration
    void setMaxDuration(int seconds);
    void setOutputFormat(OutputFormat format);
    void setRecordingMode(RecordingMode mode);

    // User filtering
    bool shouldRecordUser(const std::string& userId) const;

    // Compositing methods
    bool updateCompositeFrame(const VideoFrame& frame, const std::string& userId);
    bool createCompositeFrame();
    void cleanupCompositeResources();

    // Audio mixing methods
    bool mixAudioFromMultipleUsers(const AudioFrame& frame, const std::string& userId);
    bool createMixedAudioFrame();

    // Layout calculation methods
    std::pair<int, int> calculateOptimalLayout(int numUsers);

   private:
    // FFmpeg contexts per user (for individual mode)
    struct UserContext {
        AVFormatContext* formatContext = nullptr;
        AVCodecContext* videoCodecContext = nullptr;
        AVCodecContext* audioCodecContext = nullptr;
        AVStream* videoStream = nullptr;
        AVStream* audioStream = nullptr;
        SwsContext* swsContext = nullptr;
        SwrContext* swrContext = nullptr;
        AVFrame* videoFrame = nullptr;
        AVFrame* audioFrame = nullptr;
        std::string filename;
        std::chrono::steady_clock::time_point startTime;
        int64_t videoFrameCount = 0;
        int64_t audioFrameCount = 0;
        bool headerWritten = false;
        int64_t lastDts = -1;
        uint64_t firstVideoTimestamp = 0;  // Store first video frame timestamp for relative timing
        uint64_t firstAudioTimestamp = 0;  // Store first audio frame timestamp for relative timing
        bool hasFirstVideoFrame = false;
        bool hasFirstAudioFrame = false;

        // Audio sample buffering for AAC frame size requirements
        std::vector<int16_t> audioSampleBuffer;  // Buffer to accumulate audio samples
        uint64_t lastBufferedTimestamp = 0;      // Track timestamp for buffered samples
    };

    // Thread functions
    void recordingThread();
    void processVideoFrames();
    void processAudioFrames();

    // FFmpeg setup and cleanup
    bool initializeEncoder(const std::string& userId = "");
    void cleanupEncoder(const std::string& userId = "");
    bool setupVideoEncoder(AVCodecContext** videoCodecContext, const std::string& userId = "");
    bool setupAudioEncoder(AVCodecContext** audioCodecContext, const std::string& userId = "");
    bool setupOutputFormat(AVFormatContext** formatContext, const std::string& filename);

    // Frame processing
    bool encodeVideoFrame(const VideoFrame& frame, const std::string& userId);
    bool encodeIndividualFrame(const VideoFrame& frame, UserContext* context);
    bool encodeAudioFrame(const AudioFrame& frame, const std::string& userId);
    bool encodeIndividualAudioFrame(const AudioFrame& frame, UserContext* context,
                                    const std::string& userId);
    bool writePacket(AVPacket* packet, AVFormatContext* formatContext, AVStream* stream);

    // Utilities
    std::string generateOutputFilename(const std::string& userId = "");
    std::string getFileExtension() const;
    bool createOutputDirectory();

    // Configuration
    Config config_;
    std::atomic<bool> isRecording_{false};
    std::atomic<bool> stopRequested_{false};

    // Threading
    std::unique_ptr<std::thread> recordingThread_;
    std::mutex mutex_;
    std::condition_variable cv_;

    // Frame buffers
    std::queue<VideoFrame> videoFrameQueue_;
    std::queue<AudioFrame> audioFrameQueue_;
    std::mutex videoQueueMutex_;
    std::mutex audioQueueMutex_;
    std::condition_variable videoQueueCv_;
    std::condition_variable audioQueueCv_;

    std::map<std::string, std::unique_ptr<UserContext>> userContexts_;
    std::mutex userContextsMutex_;

    // For composite mode
    std::unique_ptr<UserContext> compositeContext_;

    // Composite frame buffer - stores latest frame from each user with timestamps
    std::map<std::string, VideoFrame> compositeFrameBuffer_;
    std::map<std::string, uint64_t>
        compositeFrameTimestamps_;  // Track when each frame was received
    std::mutex compositeBufferMutex_;
    const uint64_t COMPOSITE_FRAME_TIMEOUT_MS = 1000;  // Keep frames for 1 second

    // Audio mixing for composite mode
    std::map<std::string, std::vector<float>> audioMixingBuffer_;  // Per-user audio buffer
    std::mutex audioMixingMutex_;
    uint64_t lastAudioMixTime_ = 0;
    const uint64_t AUDIO_MIX_INTERVAL_MS = 20;  // Mix audio every 20ms

    // Performance optimizations
    std::map<std::string, SwsContext*> userScalingContexts_;  // Cached scaling contexts
    std::map<std::string, AVFrame*> scaledFramePool_;         // Pre-allocated scaled frames
    uint64_t lastCompositeTime_ = 0;                          // Frame rate limiting
    uint64_t currentCompositeInterval_ = 16;                  // Start at ~60 FPS (16ms)
    const uint64_t MIN_COMPOSITE_INTERVAL_MS = 16;            // 60 FPS max
    const uint64_t MAX_COMPOSITE_INTERVAL_MS = 100;           // 10 FPS min

    // Performance monitoring
    uint64_t frameProcessingStartTime_ = 0;
    uint64_t totalFrameProcessingTime_ = 0;
    uint64_t frameProcessingCount_ = 0;
    uint64_t lastPerformanceCheck_ = 0;
    uint64_t droppedFrames_ = 0;

    // Timing
    std::chrono::steady_clock::time_point startTime_;
};

}  // namespace rtc
}  // namespace agora