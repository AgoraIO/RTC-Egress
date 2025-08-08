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

#include "metadata_manager.h"
#include "ts_segment_manager.h"
#include "video_compositor.h"
#include "video_frame.h"

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

// Using modular VideoFrame from video_frame.h

class RecordingSink {
   public:
    enum class OutputFormat { MP4, AVI, MKV, TS };

    struct Config {
        std::string outputDir = "./recordings";
        VideoCompositor::Mode mode = VideoCompositor::Mode::Composite;
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

        // TS-specific settings
        int tsSegmentDurationSeconds = 10;     // TS segment duration
        bool tsGeneratePlaylist = true;        // Generate HLS playlist
        bool tsKeepIncompleteSegments = true;  // Keep incomplete segments on crash

        // Metadata tracking
        std::string taskId = "";  // Task identifier for metadata
    };

    RecordingSink();
    ~RecordingSink();

    bool initialize(const Config& config);
    bool start();
    void stop();
    bool isRecording() const {
        return isRecording_.load() && !stopRequested_.load();
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

    // User filtering
    bool shouldRecordUser(const std::string& userId) const;

    // Compositing methods
    bool updateCompositeFrame(const VideoFrame& frame, const std::string& userId);
    void cleanupCompositeResources();
    void onComposedFrame(const AVFrame* composedFrame);

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
        // Unified RTC timestamp synchronization system
        uint64_t rtcTimeOrigin = 0;  // Unified time origin for RTC streams (milliseconds)
        bool hasTimeOrigin = false;  // Whether time origin has been established

        // Monotonic PTS tracking
        int64_t lastVideoPts = -1;  // Last video frame PTS
        int64_t lastAudioPts = -1;  // Last audio frame PTS

        // Stream state tracking
        bool videoStreamActive = false;  // Whether video stream is active
        bool audioStreamActive = false;  // Whether audio stream is active
        uint64_t lastVideoRtcTs = 0;     // Last received video RTC timestamp
        uint64_t lastAudioRtcTs = 0;     // Last received audio RTC timestamp

        // swsContext input resolution tracking
        int lastInputWidth = 0;   // Last input video width
        int lastInputHeight = 0;  // Last input video height

        // Audio sample buffering for AAC frame size requirements
        std::vector<int16_t> audioSampleBuffer;  // Buffer to accumulate audio samples
        uint64_t lastBufferedTimestamp =
            0;  // Track timestamp for buffered samples and PTS monotonic progression

        // TS-specific keyframe tracking (only used for TS format)
        bool lastFrameWasKeyframe = false;  // Track if last encoded frame was keyframe
        std::chrono::steady_clock::time_point lastKeyframeTime;  // Time of last keyframe
    };

    // Thread functions
    void recordingThread();
    void processVideoFrames();
    void processAudioFrames();

    // FFmpeg setup and cleanup
    bool initializeEncoder(const std::string& userId = "");
    void cleanupEncoder(const std::string& userId = "");
    void flushAllEncoders();  // Flush encoders to prevent blocking during shutdown
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

    // RTC timestamp synchronization core functions
    void initializeRtcTimeOrigin(UserContext* context, uint64_t rtcTimestamp);
    int64_t calculateVideoPTS(UserContext* context, uint64_t rtcTimestamp);
    int64_t calculateAudioPTS(UserContext* context, uint64_t rtcTimestamp);
    int64_t ensureMonotonicPTS(int64_t newPts, int64_t& lastPts, int64_t minIncrement);

    // Stream state management
    void updateStreamState(UserContext* context, bool isVideo, uint64_t rtcTimestamp);
    bool detectStreamRestart(UserContext* context, bool isVideo, uint64_t rtcTimestamp);

    // TS segment management
    void checkAndRotateSegmentIfNeeded();
    bool switchToNewTSSegment(UserContext* context);
    bool detectKeyframe(AVPacket* packet, UserContext* context);

    // Utilities
    std::string generateOutputFilename(const std::string& userId = "");
    std::string getFileExtension() const;
    bool createOutputDirectory();
    uint64_t getFileSize(const std::string& filepath) const;

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
    std::unique_ptr<VideoCompositor> videoCompositor_;

    // TS segment management
    std::unique_ptr<TSSegmentManager> tsSegmentManager_;
    std::atomic<bool> tsPendingSegmentRotation_{false};  // Thread-safe rotation flag
    std::mutex tsRotationMutex_;                         // Protect segment rotation operations

    // Metadata management
    std::unique_ptr<MetadataManager> metadataManager_;
    std::string currentOutputFilePrefix_;  // Current session file prefix

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

    // Audio mixing
    float maxAudioLevel_ = 0.0f;

    // Layout caching
    size_t lastLayoutUserCount_ = 0;
    int lastCols_ = 0;
    int lastRows_ = 0;

    // Timing
    std::chrono::steady_clock::time_point startTime_;
};

}  // namespace rtc
}  // namespace agora