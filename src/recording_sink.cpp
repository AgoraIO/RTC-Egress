#define AG_LOG_TAG "RecordingSink"

#include "include/recording_sink.h"

#include <libavformat/avformat.h>
#include <libswresample/swresample.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "common/ffmpeg_utils.h"
#include "common/file_utils.h"
#include "common/log.h"

namespace agora {
namespace rtc {

namespace fs = std::filesystem;

RecordingSink::RecordingSink() {
    // Initialize FFmpeg
    av_log_set_level(AV_LOG_ERROR);

    // Initialize performance monitoring variables
    lastPerformanceCheck_ = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
}

RecordingSink::~RecordingSink() {
    stop();
}

bool RecordingSink::initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isRecording_.load()) {
        AG_LOG_FAST(ERROR, "Cannot initialize while recording");
        return false;
    }

    config_ = config;

    // Create output directory if it doesn't exist
    if (!createOutputDirectory()) {
        return false;
    }

    // Initialize VideoCompositor for composite mode
    if (config_.mode == RecordingMode::COMPOSITE) {
        videoCompositor_ = std::make_unique<VideoCompositor>();
        VideoCompositor::Config compositorConfig;
        compositorConfig.outputWidth = config_.videoWidth;
        compositorConfig.outputHeight = config_.videoHeight;
        compositorConfig.preserveAspectRatio = true;
        compositorConfig.minCompositeIntervalMs = 1000 / config_.videoFps;  // Match video FPS

        if (!videoCompositor_->initialize(compositorConfig)) {
            AG_LOG_FAST(ERROR, "Failed to initialize VideoCompositor");
            return false;
        }

        // Set callback to receive composed frames for encoding
        videoCompositor_->setComposedVideoFrameCallback(
            [this](const AVFrame* composedFrame) { this->onComposedFrame(composedFrame); });

        AG_LOG_TS(INFO, "VideoCompositor initialized for composite mode");
    }

    return true;
}

bool RecordingSink::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isRecording_.load()) {
        AG_LOG_FAST(WARN, "Already recording");
        return false;
    }

    stopRequested_ = false;
    isRecording_ = true;
    startTime_ = std::chrono::steady_clock::now();

    // Create recording thread
    recordingThread_ = std::make_unique<std::thread>(&RecordingSink::recordingThread, this);

    AG_LOG_FAST(INFO, "Started recording in %s mode",
                (config_.mode == RecordingMode::INDIVIDUAL ? "individual" : "composite"));

    return true;
}

void RecordingSink::stop() {
    // std::cout << "[RecordingSink] stop() called, requesting thread shutdown..." << std::endl;
    // std::flush(std::cout);

    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!isRecording_.load()) {
            AG_LOG_TS(INFO, "Already stopped, returning early");
            return;
        }

        stopRequested_ = true;

        cv_.notify_all();
        videoQueueCv_.notify_all();
        audioQueueCv_.notify_all();
    }

    // Give the recording thread a moment to see the stop flag
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    flushAllEncoders();

    if (videoCompositor_) {
        videoCompositor_->cleanup();
    }

    if (recordingThread_ && recordingThread_->joinable()) {
        AG_LOG_TS(INFO, "About to join recording thread...");

        recordingThread_->join();

        AG_LOG_TS(INFO, "Recording thread joined successfully");

        recordingThread_.reset();
    }

    // Ensure all frames are processed before cleanup
    AG_LOG_TS(INFO, "Processing remaining frames before cleanup...");

    processVideoFrames();
    AG_LOG_TS(INFO, "Final video frames processed");

    processAudioFrames();
    AG_LOG_TS(INFO, "Final audio frames processed");

    // Cleanup all user contexts
    AG_LOG_TS(INFO, "Starting cleanup of user contexts...");

    {
        std::lock_guard<std::mutex> lock(userContextsMutex_);

        // Process composite context first
        if (compositeContext_) {
            AG_LOG_TS(INFO, "Cleaning up composite context...");
            cleanupEncoder("");
            compositeContext_.reset();
            AG_LOG_TS(INFO, "Composite context cleaned up");
        }

        // Process individual user contexts
        AG_LOG_FAST(INFO, "Cleaning up %zu user contexts...", userContexts_.size());

        for (auto& pair : userContexts_) {
            AG_LOG_FAST(INFO, "Cleaning up encoder for user: %s", pair.first.c_str());
            cleanupEncoder(pair.first);
            AG_LOG_TS(INFO, "Encoder cleaned up for user: %s", pair.first.c_str());
        }
        userContexts_.clear();

        AG_LOG_TS(INFO, "All user contexts cleared");
    }

    // Cleanup performance caches
    AG_LOG_TS(INFO, "Cleaning up composite resources...");

    cleanupCompositeResources();

    AG_LOG_TS(INFO, "Composite resources cleaned up");

    isRecording_ = false;
    AG_LOG_TS(INFO, "Stopped recording and saved files");
}

void RecordingSink::onVideoFrame(const uint8_t* yBuffer, const uint8_t* uBuffer,
                                 const uint8_t* vBuffer, int32_t yStride, int32_t uStride,
                                 int32_t vStride, uint32_t width, uint32_t height,
                                 uint64_t timestamp, const std::string& userId) {
    if (!isRecording()) {
        return;
    }

    // Check if this user should be recorded
    if (!shouldRecordUser(userId)) {
        return;
    }

    // Validate input parameters using shared utility
    if (!agora::common::validateYUVBuffers(yBuffer, uBuffer, vBuffer, yStride, uStride, vStride,
                                           width, height)) {
        AG_LOG_FAST(ERROR, "YUV buffer validation failed");
        return;
    }

    VideoFrame frame;
    if (!frame.initializeFromYUV(yBuffer, uBuffer, vBuffer, yStride, uStride, vStride, width,
                                 height, timestamp, userId)) {
        AG_LOG_FAST(ERROR, "Failed to initialize VideoFrame");
        return;
    }

    try {
        {
            std::unique_lock<std::mutex> lock(videoQueueMutex_);
            // Drop oldest frames if buffer is full to prevent unbounded memory growth
            while (videoFrameQueue_.size() >= config_.videoBufferSize) {
                videoFrameQueue_.pop();
            }
            videoFrameQueue_.push(std::move(frame));  // Use move to avoid extra copy
        }

        videoQueueCv_.notify_one();

    } catch (const std::bad_alloc& e) {
        AG_LOG_FAST(ERROR, "Memory allocation failed: %s", e.what());
        return;
    } catch (const std::exception& e) {
        AG_LOG_FAST(ERROR, "Error processing video frame: %s", e.what());
        return;
    }
}

void RecordingSink::onAudioFrame(const uint8_t* audioBuffer, int samples, int sampleRate,
                                 int channels, uint64_t timestamp, const std::string& userId) {
    if (!isRecording()) {
        return;
    }

    // Check if this user should be recorded
    if (!shouldRecordUser(userId)) {
        return;
    }

    static int audio_log_count = 0;
    if (audio_log_count % 200 == 0) {  // Log every 50th audio frame to reduce spam
        AG_LOG_FAST(INFO, "Received audio frame: %d samples, %dHz, %d channels, user: %s", samples,
                    sampleRate, channels, userId.c_str());
    }
    audio_log_count++;

    // Validate input parameters
    if (!audioBuffer) {
        AG_LOG_FAST(ERROR, "Invalid audio buffer pointer");
        return;
    }

    if (samples <= 0 || sampleRate <= 0 || channels <= 0 || channels > 8) {
        AG_LOG_FAST(ERROR, "Invalid audio parameters: samples=%d, sampleRate=%d, channels=%d",
                    samples, sampleRate, channels);
        return;
    }

    AudioFrame frame;
    frame.sampleRate = sampleRate;
    frame.channels = channels;
    frame.timestamp = timestamp;
    frame.userId = userId;

    try {
        // Calculate buffer size with overflow protection
        size_t dataSize =
            static_cast<size_t>(samples) * static_cast<size_t>(channels) * sizeof(int16_t);

        // Sanity check: prevent excessive memory allocation (max 10MB audio frame)
        const size_t MAX_AUDIO_FRAME_SIZE = 10 * 1024 * 1024;
        if (dataSize > MAX_AUDIO_FRAME_SIZE) {
            AG_LOG_FAST(ERROR, "Audio frame size too large: %zu bytes", dataSize);
            return;
        }

        frame.data.reserve(dataSize);
        frame.data.resize(dataSize);
        std::memcpy(frame.data.data(), audioBuffer, dataSize);
        frame.valid = true;

        {
            std::unique_lock<std::mutex> lock(audioQueueMutex_);
            // Drop oldest frames if buffer is full to prevent unbounded memory growth
            while (audioFrameQueue_.size() >= config_.audioBufferSize) {
                audioFrameQueue_.pop();
            }
            audioFrameQueue_.push(std::move(frame));  // Use move to avoid extra copy
        }

        audioQueueCv_.notify_one();

    } catch (const std::bad_alloc& e) {
        AG_LOG_FAST(ERROR, "Audio memory allocation failed: %s", e.what());
        return;
    } catch (const std::exception& e) {
        AG_LOG_FAST(ERROR, "Error processing audio frame: %s", e.what());
        return;
    }
}

void RecordingSink::recordingThread() {
    AG_LOG_FAST(INFO, "Recording thread started");

    while (!stopRequested_.load()) {
        // Process video frames
        if (config_.recordVideo) {
            processVideoFrames();
        }

        // Process audio frames
        if (config_.recordAudio) {
            processAudioFrames();
        }

        // Check for timeout
        auto elapsed = std::chrono::steady_clock::now() - startTime_;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >=
            config_.maxDurationSeconds) {
            AG_LOG_FAST(INFO, "Max duration reached, breaking from recording loop");
            break;
        }

        // std::cout
        //     << "[RecordingSink] Recording thread loop iteration completed, waiting with
        //     timeout..."
        //     << std::endl;
        // std::flush(std::cout);

        // Use condition variable with timeout instead of sleep_for
        // This allows the thread to be woken up immediately when stop is requested
        std::unique_lock<std::mutex> lock(mutex_);
        bool shouldStop = cv_.wait_for(lock, std::chrono::milliseconds(10),
                                       [this] { return stopRequested_.load(); });

        // If we were woken up due to stop request, exit immediately
        if (shouldStop || stopRequested_.load()) {
            AG_LOG_TS(INFO, "Stop condition detected after wait, breaking from loop");
            break;
        }
    }
}

void RecordingSink::processVideoFrames() {
    // Early exit if stop is requested - don't even try to process frames
    if (stopRequested_.load()) {
        AG_LOG_FAST(INFO,
                    "processVideoFrames() - stopRequested detected at entry, exiting immediately");
        return;
    }

    std::unique_lock<std::mutex> lock(videoQueueMutex_);

    static int process_log_count = 0;
    if (process_log_count % 30 == 0) {  // Log every 50th encode to reduce spam
        AG_LOG_FAST(INFO, "processVideoFrames() - queue size: %zu", videoFrameQueue_.size());
    }
    process_log_count++;

    while (!videoFrameQueue_.empty()) {
        // Check stop condition at the beginning of each iteration
        if (stopRequested_.load()) {
            AG_LOG_FAST(INFO, "processVideoFrames() - stopRequested detected, breaking");
            break;
        }

        VideoFrame frame = videoFrameQueue_.front();
        videoFrameQueue_.pop();
        lock.unlock();

        if (frame.valid() && !stopRequested_.load()) {
            encodeVideoFrame(frame, frame.userId());
        }

        lock.lock();

        // Check stop again after potentially blocking encode operation
        if (stopRequested_.load()) {
            AG_LOG_FAST(INFO, "processVideoFrames() - stopRequested after encode, breaking");
            break;
        }
    }
}

void RecordingSink::processAudioFrames() {
    // Early exit if stop is requested - don't even try to process frames
    if (stopRequested_.load()) {
        // std::cout << "[RecordingSink] processAudioFrames() - stopRequested detected at entry, "
        //              "exiting immediately"
        //           << std::endl;
        // std::flush(std::cout);
        return;
    }

    std::unique_lock<std::mutex> lock(audioQueueMutex_);

    // std::cout << "[RecordingSink] processAudioFrames() - queue size: " << audioFrameQueue_.size()
    //           << std::endl;
    // std::flush(std::cout);

    while (!audioFrameQueue_.empty()) {
        // Check stop condition at the beginning of each iteration
        if (stopRequested_.load()) {
            // std::cout << "[RecordingSink] processAudioFrames() - stopRequested detected,
            // breaking"
            // << std::endl;
            // std::flush(std::cout);
            break;
        }

        AudioFrame frame = audioFrameQueue_.front();
        audioFrameQueue_.pop();
        lock.unlock();

        // std::cout << "[RecordingSink] processAudioFrames() - about to encode frame" << std::endl;
        // std::flush(std::cout);

        if (frame.valid && !stopRequested_.load()) {
            encodeAudioFrame(frame, frame.userId);
        }

        // std::cout << "[RecordingSink] processAudioFrames() - frame encoded, checking stop"
        //           << std::endl;
        // std::flush(std::cout);

        lock.lock();

        // Check stop again after potentially blocking encode operation
        if (stopRequested_.load()) {
            AG_LOG_FAST(INFO, "processAudioFrames() - stopRequested after encode, breaking");
            break;
        }
    }

    // std::cout << "[RecordingSink] processAudioFrames() - exiting" << std::endl;
    // std::flush(std::cout);
}

bool RecordingSink::initializeEncoder(const std::string& userId) {
    std::unique_ptr<UserContext> context = std::make_unique<UserContext>();
    context->startTime = std::chrono::steady_clock::now();
    context->filename = generateOutputFilename(userId);

    // Setup output format
    if (!setupOutputFormat(&context->formatContext, context->filename)) {
        AG_LOG_FAST(ERROR, "Failed to setup output format for user %s", userId.c_str());
        return false;
    }

    // Setup video encoder if enabled
    if (config_.recordVideo) {
        if (!setupVideoEncoder(&context->videoCodecContext, userId)) {
            AG_LOG_FAST(ERROR, "Failed to setup video encoder for user %s", userId.c_str());
            return false;
        }

        context->videoStream =
            avformat_new_stream(context->formatContext, context->videoCodecContext->codec);
        if (!context->videoStream) {
            AG_LOG_FAST(ERROR, "Failed to create video stream for user %s", userId.c_str());
            return false;
        }

        avcodec_parameters_from_context(context->videoStream->codecpar, context->videoCodecContext);
        // Set stream time base to codec time base - MP4 will still override but this helps
        context->videoStream->time_base = context->videoCodecContext->time_base;
        context->videoStream->avg_frame_rate = {config_.videoFps, 1};
        context->videoStream->r_frame_rate = {config_.videoFps, 1};

        // Allocate video frame
        context->videoFrame = av_frame_alloc();
        if (!context->videoFrame) {
            AG_LOG_FAST(ERROR, "Failed to allocate video frame for user %s", userId.c_str());
            return false;
        }

        context->videoFrame->format = context->videoCodecContext->pix_fmt;
        context->videoFrame->width = context->videoCodecContext->width;
        context->videoFrame->height = context->videoCodecContext->height;

        if (av_frame_get_buffer(context->videoFrame, 32) < 0) {
            AG_LOG_FAST(ERROR, "Failed to allocate video frame buffer for user %s", userId.c_str());
            return false;
        }

        // Note: Scaling context will be created dynamically when we know actual frame dimensions
        context->swsContext = nullptr;
    }

    // Setup audio encoder if enabled
    if (config_.recordAudio) {
        AG_LOG_FAST(INFO, "Setting up audio encoder for user %s", userId.c_str());
        if (!setupAudioEncoder(&context->audioCodecContext, userId)) {
            AG_LOG_FAST(ERROR, "Failed to setup audio encoder for user %s", userId.c_str());
            return false;
        }
        AG_LOG_FAST(INFO, "Audio encoder setup successful for user %s", userId.c_str());

        context->audioStream =
            avformat_new_stream(context->formatContext, context->audioCodecContext->codec);
        if (!context->audioStream) {
            AG_LOG_FAST(ERROR, "Failed to create audio stream for user %s", userId.c_str());
            return false;
        }

        avcodec_parameters_from_context(context->audioStream->codecpar, context->audioCodecContext);
        context->audioStream->time_base = context->audioCodecContext->time_base;

        // Allocate audio frame for output
        context->audioFrame = av_frame_alloc();
        if (!context->audioFrame) {
            AG_LOG_FAST(ERROR, "Failed to allocate audio frame for user %s", userId.c_str());
            return false;
        }

        // Initialize audio resampling context for format conversion
        // We'll create this dynamically when we receive the first audio frame
        context->swrContext = nullptr;
    }

    // Write header
    if (avformat_write_header(context->formatContext, nullptr) < 0) {
        AG_LOG_FAST(ERROR, "Failed to write header for user %s", userId.c_str());
        return false;
    }

    // Log the actual time bases after header is written
    AG_LOG_FAST(INFO, "Stream time base: %d/%d, Codec time base: %d/%d",
                context->videoStream->time_base.num, context->videoStream->time_base.den,
                context->videoCodecContext->time_base.num,
                context->videoCodecContext->time_base.den);

    // Save filename before moving context
    std::string filename = context->filename;

    // Store context (mutex already held by caller)
    if (config_.mode == RecordingMode::INDIVIDUAL) {
        userContexts_[userId] = std::move(context);
    } else {
        compositeContext_ = std::move(context);
    }

    AG_LOG_FAST(INFO, "Initialized encoder for user %s, output file: %s", userId.c_str(),
                filename.c_str());

    return true;
}

bool RecordingSink::setupOutputFormat(AVFormatContext** formatContext,
                                      const std::string& filename) {
    if (avformat_alloc_output_context2(formatContext, nullptr, nullptr, filename.c_str()) < 0) {
        return false;
    }

    if (!((*formatContext)->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&(*formatContext)->pb, filename.c_str(), AVIO_FLAG_WRITE) < 0) {
            return false;
        }
    }

    return true;
}

bool RecordingSink::setupVideoEncoder(AVCodecContext** videoCodecContext,
                                      const std::string& userId) {
    const AVCodec* codec = avcodec_find_encoder_by_name(config_.videoCodec.c_str());
    if (!codec) {
        AG_LOG_FAST(ERROR, "Video codec not found: %s", config_.videoCodec.c_str());
        return false;
    }

    *videoCodecContext = avcodec_alloc_context3(codec);
    if (!*videoCodecContext) {
        AG_LOG_FAST(ERROR, "Failed to allocate video codec context");
        return false;
    }

    (*videoCodecContext)->bit_rate = config_.videoBitrate;
    (*videoCodecContext)->width = config_.videoWidth;
    (*videoCodecContext)->height = config_.videoHeight;
    // Modern best practice time base for video: 1/90000 (MPEG-2/H.264 standard)
    // This is the standard time base used in modern video containers and codecs
    (*videoCodecContext)->time_base = {1, 90000};
    (*videoCodecContext)->framerate = {config_.videoFps, 1};
    (*videoCodecContext)->gop_size = config_.videoFps;
    (*videoCodecContext)->max_b_frames = 0;  // Disable B-frames for stability
    (*videoCodecContext)->pix_fmt = AV_PIX_FMT_YUV420P;

    // Add encoder options for stability and non-blocking behavior
    (*videoCodecContext)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // CRITICAL: Configure threading to prevent blocking
    (*videoCodecContext)->thread_count = 1;  // Single thread for predictable behavior
    (*videoCodecContext)->thread_type = FF_THREAD_SLICE;

    // Set codec options for stability and low-latency
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", "fast", 0);  // Fast encoding with better quality than ultrafast
    av_dict_set(&opts, "tune", "zerolatency", 0);
    av_dict_set(&opts, "x264-params", "force-cfr=1", 0);  // Force constant frame rate
    av_dict_set(&opts, "fflags", "+flush_packets", 0);    // Flush packets immediately

    if (avcodec_open2(*videoCodecContext, codec, &opts) < 0) {
        AG_LOG_FAST(ERROR, "Failed to open video codec");
        av_dict_free(&opts);
        return false;
    }

    av_dict_free(&opts);

    return true;
}

bool RecordingSink::setupAudioEncoder(AVCodecContext** audioCodecContext,
                                      const std::string& userId) {
    const AVCodec* codec = avcodec_find_encoder_by_name(config_.audioCodec.c_str());
    if (!codec) {
        AG_LOG_FAST(ERROR, "Audio codec not found: %s", config_.audioCodec.c_str());
        return false;
    }

    *audioCodecContext = avcodec_alloc_context3(codec);
    if (!*audioCodecContext) {
        return false;
    }

    (*audioCodecContext)->bit_rate = config_.audioBitrate;
    (*audioCodecContext)->sample_fmt = AV_SAMPLE_FMT_FLTP;
    // Use the target audio parameters from config (48kHz stereo)
    (*audioCodecContext)->sample_rate = config_.audioSampleRate;
    av_channel_layout_default(&(*audioCodecContext)->ch_layout, config_.audioChannels);
    // Modern best practice time base for audio: 1/90000 (MPEG standard)
    // This provides high precision while avoiding overflow and is MP4/H.264 standard
    (*audioCodecContext)->time_base = {1, 90000};

    if (avcodec_open2(*audioCodecContext, codec, nullptr) < 0) {
        return false;
    }

    return true;
}

bool RecordingSink::encodeVideoFrame(const VideoFrame& frame, const std::string& userId) {
    // Check if stop was requested before doing expensive video encoding
    if (stopRequested_.load()) {
        // std::cout << "[RecordingSink] encodeVideoFrame() - stopRequested detected, returning
        // early"
        //           << std::endl;
        // std::flush(std::cout);
        return false;
    }

    // std::cout << "[RecordingSink] encodeVideoFrame() - stopRequested check passed" << std::endl;
    // std::flush(std::cout);

    if (config_.mode == RecordingMode::INDIVIDUAL) {
        // std::cout << "[RecordingSink] encodeVideoFrame() - using INDIVIDUAL mode" << std::endl;
        // std::flush(std::cout);
        // Individual mode - encode each user separately
        std::lock_guard<std::mutex> lock(userContextsMutex_);

        auto it = userContexts_.find(userId);
        if (it == userContexts_.end()) {
            // Initialize encoder for new user
            if (!initializeEncoder(userId)) {
                return false;
            }
            it = userContexts_.find(userId);
        }

        return encodeIndividualFrame(frame, it->second.get());
    } else {
        // Composite mode - update composite buffer and potentially create composite frame
        bool result = updateCompositeFrame(frame, userId);
        return result;
    }
}

bool RecordingSink::encodeIndividualFrame(const VideoFrame& frame, UserContext* context) {
    if (!context || !context->videoCodecContext || !context->videoFrame) {
        return false;
    }

    // Create scaling context dynamically if not already created or if frame dimensions changed
    if (!context->swsContext) {
        context->swsContext = sws_getContext(
            frame.width(), frame.height(),
            AV_PIX_FMT_YUV420P,  // Use actual incoming frame dimensions
            context->videoCodecContext->width, context->videoCodecContext->height,
            context->videoCodecContext->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!context->swsContext) {
            AG_LOG_FAST(ERROR, "Failed to create scaling context for frame %ux%u -> %dx%d",
                        frame.width(), frame.height(), context->videoCodecContext->width,
                        context->videoCodecContext->height);
            return false;
        }
    }

    // Convert VideoFrame to AVFrame for processing
    AVFrame* srcFrame = frame.toAVFrame();
    if (!srcFrame) {
        AG_LOG_FAST(ERROR, "Failed to convert VideoFrame to AVFrame");
        return false;
    }

    // Ensure frame is writable before scaling
    if (av_frame_make_writable(context->videoFrame) < 0) {
        AG_LOG_FAST(ERROR, "Failed to make frame writable");
        av_frame_free(&srcFrame);
        return false;
    }

    sws_scale(context->swsContext, srcFrame->data, srcFrame->linesize, 0, frame.height(),
              context->videoFrame->data, context->videoFrame->linesize);

    av_frame_free(&srcFrame);

    // Initialize first frame timestamp for synchronization
    if (!context->hasFirstVideoFrame) {
        context->firstVideoTimestamp = frame.timestamp();
        context->hasFirstVideoFrame = true;
        AG_LOG_TS(INFO, "FIRST VIDEO timestamp: %ld", context->firstVideoTimestamp);

        // Compare timestamps if audio timestamp is already set
        if (context->hasFirstAudioFrame) {
            int64_t diff = context->firstAudioTimestamp - context->firstVideoTimestamp;
            AG_LOG_TS(INFO,
                      "TIMESTAMP COMPARISON - Audio vs Video base difference: %ldms (Audio: %ld, "
                      "Video: %ld)",
                      diff, context->firstAudioTimestamp, context->firstVideoTimestamp);
        }
    }

    // Use relative timestamps to prevent overflow
    // Calculate PTS using frame-based counting for video precision
    int64_t timestamp_diff_ms = frame.timestamp() - context->firstVideoTimestamp;

    // Ensure we don't have negative or excessive differences
    if (timestamp_diff_ms < 0) {
        timestamp_diff_ms = 0;
    } else if (timestamp_diff_ms > INT64_MAX / 90000) {
        // Prevent overflow during rescaling to 90kHz time base
        AG_LOG_FAST(WARN, "Video timestamp diff too large: %ld ms, using frame count",
                    timestamp_diff_ms);
        timestamp_diff_ms = context->videoFrameCount * 1000 / config_.videoFps;
    }

    // Convert milliseconds to video time base (frame intervals)
    context->videoFrame->pts =
        av_rescale_q(timestamp_diff_ms, {1, 1000}, context->videoCodecContext->time_base);

    static int pts_log_count = 0;
    if (pts_log_count % 30 == 0) {
        AG_LOG_FAST(INFO, "Video PTS: %ld, timestamp: %ld, first_timestamp: %ld",
                    context->videoFrame->pts, frame.timestamp(), context->firstVideoTimestamp);
    }
    pts_log_count++;

    context->videoFrameCount++;

    // Encode frame
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    AG_LOG_FAST(INFO, "About to call avcodec_send_frame()");

    int ret = avcodec_send_frame(context->videoCodecContext, context->videoFrame);

    AG_LOG_FAST(INFO, "avcodec_send_frame() returned: %d", ret);

    if (ret < 0) {
        av_packet_free(&packet);
        return false;
    }

    AG_LOG_FAST(INFO, "Entering avcodec_receive_packet() loop");

    while (ret >= 0) {
        // Check if stop was requested during encoding loop
        if (stopRequested_.load()) {
            AG_LOG_FAST(
                INFO,
                "encodeIndividualFrame() - stopRequested detected in encoding loop, breaking");
            av_packet_free(&packet);
            return false;
        }

        AG_LOG_FAST(INFO, "About to call avcodec_receive_packet()");

        ret = avcodec_receive_packet(context->videoCodecContext, packet);

        AG_LOG_FAST(INFO, "avcodec_receive_packet() returned: %d", ret);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            av_packet_free(&packet);
            return false;
        }

        // Set the stream index
        packet->stream_index = context->videoStream->index;

        // Fix: Use manual rescaling to avoid overflow
        if (packet->pts != AV_NOPTS_VALUE) {
            // Manual rescaling with bounds checking to prevent overflow
            int64_t rescaled_pts = av_rescale_q(packet->pts, context->videoCodecContext->time_base,
                                                context->videoStream->time_base);

            // Bounds check to prevent overflow
            if (rescaled_pts != AV_NOPTS_VALUE && rescaled_pts >= 0) {
                packet->pts = rescaled_pts;
            } else {
                // Fallback: use frame count based PTS
                packet->pts =
                    context->videoFrameCount * av_rescale_q(1, (AVRational){1, config_.videoFps},
                                                            context->videoStream->time_base);
            }
        } else {
            // If encoder didn't set PTS, use frame count based PTS
            packet->pts =
                context->videoFrameCount *
                av_rescale_q(1, (AVRational){1, config_.videoFps}, context->videoStream->time_base);
        }

        // Set DTS equal to PTS for video (no B-frames)
        packet->dts = packet->pts;

        // Debug: Log PTS/DTS values
        static int rescale_log_count = 0;
        if (rescale_log_count % 30 == 0) {
            AG_LOG_FAST(INFO, "Video packet - PTS: %ld, DTS: %ld, frame: %lu", packet->pts,
                        packet->dts, context->videoFrameCount);
        }
        rescale_log_count++;

        // Set packet duration for proper MP4 timing
        if (packet->duration <= 0) {
            packet->duration =
                av_rescale_q(1, (AVRational){1, config_.videoFps}, context->videoStream->time_base);
        }

        // Capture PTS value before writing (muxer may modify packet)
        int64_t original_pts = packet->pts;

        // Write packet - use av_interleaved_write_frame for proper timestamp ordering
        int write_ret = av_interleaved_write_frame(context->formatContext, packet);
        if (write_ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, write_ret);
            AG_LOG_FAST(ERROR, "Failed to write video frame: %s", errbuf);
        } else {
            static int video_log_count = 0;
            if (video_log_count % 30 == 0) {  // Log every 30th frame to reduce spam
                AG_LOG_FAST(INFO, "Successfully wrote video packet %d, pts: %ld", video_log_count,
                            original_pts);
            }
            video_log_count++;
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return true;
}

bool RecordingSink::encodeAudioFrame(const AudioFrame& frame, const std::string& userId) {
    static int encode_log_count = 0;
    if (encode_log_count % 50 == 0) {  // Log every 50th encode to reduce spam
        AG_LOG_FAST(INFO,
                    "Encoding audio frame for user: %s, input: %dHz, %d channels, target: %dHz, %d "
                    "channels",
                    userId.c_str(), frame.sampleRate, frame.channels, config_.audioSampleRate,
                    config_.audioChannels);
    }
    encode_log_count++;

    if (config_.mode == RecordingMode::INDIVIDUAL) {
        // Individual mode - encode each user separately
        std::lock_guard<std::mutex> lock(userContextsMutex_);

        auto it = userContexts_.find(userId);
        if (it == userContexts_.end()) {
            // Initialize encoder for new user if not already done
            if (!initializeEncoder(userId)) {
                return false;
            }
            it = userContexts_.find(userId);
        }
        UserContext* context = it->second.get();
        return encodeIndividualAudioFrame(frame, context, userId);
    } else {
        // Composite mode - mix audio from multiple users
        // std::cout << "[RecordingSink] Composite mode: mixing audio from user " << userId
        //           << std::endl;
        return mixAudioFromMultipleUsers(frame, userId);
    }
}

bool RecordingSink::encodeIndividualAudioFrame(const AudioFrame& frame, UserContext* context,
                                               const std::string& userId) {
    if (!context || !context->audioCodecContext || !context->audioFrame) {
        return false;
    }

    // Initialize resampling context if needed (when format doesn't match)
    bool needsResampling =
        (frame.sampleRate != config_.audioSampleRate || frame.channels != config_.audioChannels);

    // std::cout << "[RecordingSink] Audio format check: input=" << frame.sampleRate << "Hz/"
    //           << frame.channels << "ch, target=" << config_.audioSampleRate << "Hz/"
    //           << config_.audioChannels << "ch, needsResampling=" << (needsResampling ? "YES" :
    //           "NO")
    //           << std::endl;

    if (needsResampling && !context->swrContext) {
        // Create resampling context to convert input format to target format
        AVChannelLayout in_ch_layout, out_ch_layout;
        av_channel_layout_default(&in_ch_layout, frame.channels);
        av_channel_layout_default(&out_ch_layout, config_.audioChannels);

        int ret = swr_alloc_set_opts2(&context->swrContext, &out_ch_layout, AV_SAMPLE_FMT_FLTP,
                                      config_.audioSampleRate,                             // output
                                      &in_ch_layout, AV_SAMPLE_FMT_S16, frame.sampleRate,  // input
                                      0, nullptr);

        if (ret < 0 || !context->swrContext) {
            AG_LOG_FAST(ERROR, "Failed to allocate resampling context");
            return false;
        }

        ret = swr_init(context->swrContext);
        if (ret < 0) {
            AG_LOG_FAST(ERROR, "Failed to initialize resampling context");
            swr_free(&context->swrContext);
            return false;
        }

        AG_LOG_FAST(INFO, "Initialized audio resampler: %dHz %dch -> %dHz %dch", frame.sampleRate,
                    frame.channels, config_.audioSampleRate, config_.audioChannels);
    }

    // Calculate input samples from the frame data
    int input_samples = frame.data.size() / sizeof(int16_t) / frame.channels;
    const int16_t* input_data = reinterpret_cast<const int16_t*>(frame.data.data());

    int samples_per_frame = context->audioCodecContext->frame_size;
    if (samples_per_frame == 0) {
        samples_per_frame = 1024;  // Default frame size for AAC
    }

    // Add incoming samples to buffer
    size_t current_buffer_size = context->audioSampleBuffer.size();
    size_t new_samples_count = input_samples * frame.channels;
    context->audioSampleBuffer.resize(current_buffer_size + new_samples_count);

    // Copy new samples to buffer
    std::memcpy(context->audioSampleBuffer.data() + current_buffer_size, input_data,
                new_samples_count * sizeof(int16_t));

    // Update buffered timestamp (use latest frame timestamp)
    context->lastBufferedTimestamp = frame.timestamp;

    // Check if we have enough samples for AAC encoding (1024 samples per channel)
    size_t required_samples = samples_per_frame * frame.channels;  // e.g., 1024 * 1 = 1024 for mono
    if (context->audioSampleBuffer.size() < required_samples) {
        // Not enough samples yet, return and wait for more
        static int buffer_log_count = 0;
        if (buffer_log_count % 50 == 0) {
            AG_LOG_FAST(INFO, "Buffering audio: %zu/%d samples", context->audioSampleBuffer.size(),
                        required_samples);
        }
        buffer_log_count++;
        return true;  // Successfully buffered, but not ready to encode yet
    }

    // std::cout << "[RecordingSink] Ready to encode: " << context->audioSampleBuffer.size()
    //           << " samples (need " << required_samples << ")" << std::endl;

    // Initialize first frame timestamp for synchronization
    if (!context->hasFirstAudioFrame) {
        context->firstAudioTimestamp = frame.timestamp;
        context->hasFirstAudioFrame = true;
        AG_LOG_TS(INFO, "FIRST AUDIO timestamp: %ld", context->firstAudioTimestamp);

        // Compare timestamps if video timestamp is already set
        if (context->hasFirstVideoFrame) {
            int64_t diff = context->firstAudioTimestamp - context->firstVideoTimestamp;
            AG_LOG_TS(INFO,
                      "TIMESTAMP COMPARISON - Audio vs Video base difference: %ldms (Audio: %ld, "
                      "Video: %ld)",
                      diff, context->firstAudioTimestamp, context->firstVideoTimestamp);
        }
    }

    static int audio_pts_log_count = 0;
    if (audio_pts_log_count % 50 == 0) {
        AG_LOG_FAST(INFO, "Audio frame %ld, timestamp: %ld, firstTimestamp: %ld",
                    context->audioFrameCount, frame.timestamp, context->firstAudioTimestamp);
    }
    audio_pts_log_count++;

    // Set up audio frame properties for output
    context->audioFrame->nb_samples = samples_per_frame;
    context->audioFrame->format = context->audioCodecContext->sample_fmt;
    context->audioFrame->sample_rate = context->audioCodecContext->sample_rate;
    av_channel_layout_copy(&context->audioFrame->ch_layout, &context->audioCodecContext->ch_layout);

    // Use relative timestamps to prevent overflow
    // Calculate PTS using sample-based counting for audio precision
    int64_t timestamp_diff_ms = frame.timestamp - context->firstAudioTimestamp;

    // Ensure we don't have negative or excessive differences
    if (timestamp_diff_ms < 0) {
        timestamp_diff_ms = 0;
    } else if (timestamp_diff_ms > INT64_MAX / 90000) {
        // Prevent overflow during rescaling to 90kHz time base
        AG_LOG_FAST(WARN, "Audio timestamp diff too large: %ld ms, using frame count",
                    timestamp_diff_ms);
        timestamp_diff_ms = context->audioFrameCount * 1000 /
                            (config_.audioSampleRate / context->audioCodecContext->frame_size);
    }

    // Convert milliseconds to audio time base (samples)
    int64_t pts = av_rescale_q(timestamp_diff_ms, {1, 1000}, context->audioCodecContext->time_base);

    context->audioFrame->pts = pts;
    context->lastBufferedTimestamp = frame.timestamp;  // Track for debugging

    // std::cout << "[RecordingSink] Individual audio PTS: " << pts
    //           << " from timestamp: " << frame.timestamp << std::endl;

    // Allocate buffer for audio frame
    if (av_frame_get_buffer(context->audioFrame, 0) < 0) {
        AG_LOG_FAST(ERROR, "Failed to make audio frame writable");
        return false;
    }

    // Process exactly the required number of samples for AAC
    size_t samples_to_encode = samples_per_frame;  // 1024 samples for AAC

    if (needsResampling && context->swrContext) {
        // Use resampling to convert format from buffered samples
        const uint8_t* input[1] = {
            reinterpret_cast<const uint8_t*>(context->audioSampleBuffer.data())};

        // Calculate how many input samples we're using (need to account for channels)
        int input_samples_for_resampling =
            samples_to_encode * frame.channels / config_.audioChannels;

        int output_samples = swr_convert(context->swrContext, context->audioFrame->data,
                                         samples_per_frame, input, input_samples_for_resampling);

        if (output_samples < 0) {
            AG_LOG_FAST(ERROR, "Audio resampling failed");
            return false;
        }

        // Update the actual number of samples produced
        context->audioFrame->nb_samples = output_samples;

        static int resample_log_count = 0;
        if (resample_log_count % 100 == 0) {
            AG_LOG_FAST(INFO, "Resampled %d -> %d samples", input_samples_for_resampling,
                        output_samples);
        }
        resample_log_count++;

    } else {
        // Direct conversion without resampling (when formats match)
        if (context->audioCodecContext->sample_fmt == AV_SAMPLE_FMT_FLTP) {
            // Convert to planar float format using buffered samples
            const int16_t* buffer_data = context->audioSampleBuffer.data();

            if (frame.channels == config_.audioChannels) {
                // Channels match - direct conversion
                for (int ch = 0; ch < config_.audioChannels; ch++) {
                    float* output_channel = (float*)context->audioFrame->data[ch];
                    for (int i = 0; i < samples_per_frame; i++) {
                        int16_t sample = buffer_data[i * frame.channels + ch];
                        output_channel[i] = static_cast<float>(sample) / 32768.0f;
                    }
                }
            } else {
                // Channel conversion needed (mono to stereo or vice versa)
                for (int ch = 0; ch < config_.audioChannels; ch++) {
                    float* output_channel = (float*)context->audioFrame->data[ch];
                    for (int i = 0; i < samples_per_frame; i++) {
                        int16_t sample;
                        if (frame.channels == 1) {
                            // Mono to stereo - duplicate channel
                            sample = buffer_data[i];
                        } else {
                            // Multi-channel to fewer channels - take first channel or mix
                            sample =
                                buffer_data[i * frame.channels + std::min(ch, frame.channels - 1)];
                        }
                        output_channel[i] = static_cast<float>(sample) / 32768.0f;
                    }
                }
            }

            context->audioFrame->nb_samples = samples_per_frame;
        }
    }

    // Remove processed samples from buffer
    size_t samples_consumed = samples_to_encode * frame.channels;
    if (context->audioSampleBuffer.size() > samples_consumed) {
        // Move remaining samples to beginning of buffer
        std::memmove(context->audioSampleBuffer.data(),
                     context->audioSampleBuffer.data() + samples_consumed,
                     (context->audioSampleBuffer.size() - samples_consumed) * sizeof(int16_t));
        context->audioSampleBuffer.resize(context->audioSampleBuffer.size() - samples_consumed);
    } else {
        // Used all samples
        context->audioSampleBuffer.clear();
    }

    context->audioFrameCount++;

    // Encode audio frame
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    int ret = avcodec_send_frame(context->audioCodecContext, context->audioFrame);
    if (ret < 0) {
        av_packet_free(&packet);
        return false;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(context->audioCodecContext, packet);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            av_packet_free(&packet);
            return false;
        }

        // Set proper timestamps for audio packet
        packet->stream_index = context->audioStream->index;

        // Fix: Use safe audio timestamp handling
        if (packet->pts != AV_NOPTS_VALUE) {
            // Audio packets: rescale with bounds checking
            int64_t rescaled_pts = av_rescale_q(packet->pts, context->audioCodecContext->time_base,
                                                context->audioStream->time_base);

            if (rescaled_pts != AV_NOPTS_VALUE && rescaled_pts >= 0) {
                packet->pts = rescaled_pts;
            } else {
                // Fallback: frame-based calculation
                packet->pts = context->audioFrameCount * context->audioCodecContext->frame_size;
            }
        } else {
            // If encoder didn't set PTS, calculate from frame count
            packet->pts = context->audioFrameCount * context->audioCodecContext->frame_size;
        }

        // For audio, DTS = PTS (no reordering)
        packet->dts = packet->pts;

        // Debug: Log audio packet info
        static int audio_rescale_log_count = 0;
        if (audio_rescale_log_count % 50 == 0) {
            AG_LOG_FAST(INFO, "Audio packet - PTS: %ld, DTS: %ld, frame: %lu", packet->pts,
                        packet->dts, context->audioFrameCount);
        }
        audio_rescale_log_count++;

        // Capture PTS value before writing (muxer may modify packet)
        int64_t original_pts = packet->pts;

        // Write packet using interleaved write for proper timestamp ordering
        int write_ret = av_interleaved_write_frame(context->formatContext, packet);
        if (write_ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, write_ret);
            AG_LOG_FAST(ERROR, "Failed to write audio frame: %s", errbuf);
        } else {
            static int audio_write_log_count = 0;
            if (audio_write_log_count % 50 == 0) {
                AG_LOG_FAST(INFO, "Successfully wrote audio packet %d, pts: %ld",
                            audio_write_log_count, original_pts);
            }
            audio_write_log_count++;
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return true;
}

bool RecordingSink::mixAudioFromMultipleUsers(const AudioFrame& frame, const std::string& userId) {
    // std::cout << "[RecordingSink] mixAudioFromMultipleUsers called for user " << userId << " with
    // "
    //           << frame.data.size() << " bytes" << std::endl;

    // Convert audio frame to float format for mixing
    std::vector<float> audioSamples;
    int input_samples = frame.data.size() / sizeof(int16_t) / frame.channels;
    const int16_t* input_data = reinterpret_cast<const int16_t*>(frame.data.data());

    // Convert to float and normalize
    audioSamples.resize(input_samples * frame.channels);
    for (int i = 0; i < input_samples * frame.channels; i++) {
        audioSamples[i] = static_cast<float>(input_data[i]) / 32768.0f;
    }

    // Store user's audio in mixing buffer AND preserve the original RTC timestamp
    {
        std::lock_guard<std::mutex> lock(audioMixingMutex_);
        audioMixingBuffer_[userId] = audioSamples;

        // Store the most recent RTC timestamp for mixed audio frame generation
        // This preserves the original RTC client timing like lastBufferedTimestamp does
        lastAudioMixTime_ = frame.timestamp;

        // std::cout << "[RecordingSink] Audio mixing buffer now has " << audioMixingBuffer_.size()
        //           << " users, using RTC timestamp: " << frame.timestamp << std::endl;
    }

    // Trigger mixing immediately when new audio data is available
    return createMixedAudioFrame();
}

bool RecordingSink::createMixedAudioFrame() {
    std::lock_guard<std::mutex> mixLock(audioMixingMutex_);
    std::lock_guard<std::mutex> contextLock(userContextsMutex_);

    // std::cout << "[RecordingSink] createMixedAudioFrame called with " <<
    // audioMixingBuffer_.size()
    //           << " users in buffer" << std::endl;

    if (audioMixingBuffer_.empty()) {
        return true;  // No audio to mix
    }

    // Initialize composite context if needed
    if (!compositeContext_) {
        if (!initializeEncoder("")) {
            return false;
        }
    }

    // Find the maximum number of samples across all users
    size_t maxSamples = 0;
    for (const auto& pair : audioMixingBuffer_) {
        maxSamples = std::max(maxSamples, pair.second.size());
    }

    if (maxSamples == 0) {
        return true;  // No samples to mix
    }

    // Create mixed audio buffer
    std::vector<float> mixedAudio(maxSamples, 0.0f);

    // Mix all users' audio
    for (const auto& pair : audioMixingBuffer_) {
        const std::vector<float>& userAudio = pair.second;
        for (size_t i = 0; i < userAudio.size() && i < maxSamples; i++) {
            mixedAudio[i] += userAudio[i];
        }
    }

    // Normalize mixed audio to prevent clipping using a running average
    float current_max = 0.0f;
    for (float sample : mixedAudio) {
        current_max = std::max(current_max, std::abs(sample));
    }

    // Update running average of max audio level
    maxAudioLevel_ = (maxAudioLevel_ * 0.95f) + (current_max * 0.05f);

    if (maxAudioLevel_ > 1.0f) {
        float scale = 1.0f / maxAudioLevel_;
        for (float& sample : mixedAudio) {
            sample *= scale;
        }
    }

    // Convert back to int16_t format
    std::vector<int16_t> mixedAudioInt16(maxSamples);
    for (size_t i = 0; i < maxSamples; i++) {
        mixedAudioInt16[i] = static_cast<int16_t>(mixedAudio[i] * 32767.0f);
    }

    // Create AudioFrame from mixed data
    AudioFrame mixedFrame;
    mixedFrame.data.resize(maxSamples * sizeof(int16_t));
    std::memcpy(mixedFrame.data.data(), mixedAudioInt16.data(), maxSamples * sizeof(int16_t));
    mixedFrame.sampleRate = config_.audioSampleRate;
    mixedFrame.channels = config_.audioChannels;
    // Best Approach: RTC-Anchored with Monotonic Safety
    // Primary: Use RTC timestamp for perfect A/V sync
    // Fallback: Minimal increment only when RTC would go backwards
    uint64_t rtcTimestamp = lastAudioMixTime_;  // From latest RTC frame

    if (compositeContext_ && compositeContext_->lastBufferedTimestamp > 0) {
        // Check if RTC timestamp would violate monotonic requirement
        if (rtcTimestamp <= compositeContext_->lastBufferedTimestamp) {
            // Minimal safety increment (1ms) to maintain monotonic progression
            mixedFrame.timestamp = compositeContext_->lastBufferedTimestamp + 1;
            AG_LOG_FAST(
                INFO, "Mixed audio monotonic safety: %lu (RTC: %lu would go backwards, last: %lu)",
                mixedFrame.timestamp, rtcTimestamp, compositeContext_->lastBufferedTimestamp);
        } else {
            // Use real RTC timestamp for perfect sync
            mixedFrame.timestamp = rtcTimestamp;
            // std::cout << "[RecordingSink] Mixed audio using RTC timestamp: " <<
            // mixedFrame.timestamp
            //           << std::endl;
        }
    } else {
        // First frame: always use RTC timestamp
        mixedFrame.timestamp = rtcTimestamp;
        AG_LOG_TS(INFO, "Mixed audio first RTC timestamp: %ld", mixedFrame.timestamp);
    }
    mixedFrame.valid = true;

    // Update lastBufferedTimestamp for next frame
    if (compositeContext_) {
        compositeContext_->lastBufferedTimestamp = mixedFrame.timestamp;
    }

    // Clear the buffer after mixing
    audioMixingBuffer_.clear();

    // Encode the mixed audio frame
    return encodeIndividualAudioFrame(mixedFrame, compositeContext_.get(), "composite");
}

std::pair<int, int> RecordingSink::calculateOptimalLayout(int numUsers) {
    // Returns (cols, rows) for optimal grid layout
    switch (numUsers) {
        case 1:
            return {1, 1};  // Full screen
        case 2:
            return {2, 1};  // Half-half (side by side)
        case 3:
        case 4:
            return {2, 2};  // 2x2 grid
        case 5:
        case 6:
            return {3, 2};  // 3x2 grid
        case 7:
        case 8:
        case 9:
            return {3, 3};  // 3x3 grid
        case 10:
        case 11:
        case 12:
            return {4, 3};  // 4x3 grid
        case 13:
        case 14:
        case 15:
        case 16:
            return {4, 4};  // 4x4 grid
        case 17:
        case 18:
        case 19:
        case 20:
            return {5, 4};  // 5x4 grid
        case 21:
        case 22:
        case 23:
        case 24:
            return {6, 4};  // 6x4 grid
        default:
            // For more than 24 users, calculate dynamically
            int cols = static_cast<int>(std::ceil(std::sqrt(numUsers)));
            int rows = static_cast<int>(std::ceil(static_cast<double>(numUsers) / cols));
            return {cols, rows};
    }
}

bool RecordingSink::writePacket(AVPacket* packet, AVFormatContext* formatContext,
                                AVStream* stream) {
    // Write packet using interleaved write for proper timestamp ordering
    int ret = av_interleaved_write_frame(formatContext, packet);
    if (ret < 0) {
        char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, ret);
        AG_LOG_FAST(ERROR, "Failed to write packet: %s", errbuf);
        return false;
    }
    return true;
}

void RecordingSink::flushAllEncoders() {
    AG_LOG_TS(INFO, "Flushing all encoders to prevent blocking...");

    std::lock_guard<std::mutex> lock(userContextsMutex_);

    // Flush composite encoder
    if (compositeContext_ && compositeContext_->videoCodecContext) {
        AG_LOG_TS(INFO, "Flushing composite video encoder");
        // Send NULL frame to signal end-of-stream and flush buffers
        avcodec_send_frame(compositeContext_->videoCodecContext, nullptr);

        // Drain remaining packets to unblock encoder
        AVPacket* pkt = av_packet_alloc();
        if (pkt) {
            while (avcodec_receive_packet(compositeContext_->videoCodecContext, pkt) == 0) {
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
    }

    if (compositeContext_ && compositeContext_->audioCodecContext) {
        AG_LOG_TS(INFO, "Flushing composite audio encoder");
        avcodec_send_frame(compositeContext_->audioCodecContext, nullptr);

        AVPacket* pkt = av_packet_alloc();
        if (pkt) {
            while (avcodec_receive_packet(compositeContext_->audioCodecContext, pkt) == 0) {
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
    }

    // Flush individual user encoders
    for (auto& pair : userContexts_) {
        const std::string& userId = pair.first;
        UserContext* context = pair.second.get();

        if (context && context->videoCodecContext) {
            AG_LOG_FAST(INFO, "Flushing video encoder for user: %s", userId.c_str());
            avcodec_send_frame(context->videoCodecContext, nullptr);

            AVPacket* pkt = av_packet_alloc();
            if (pkt) {
                while (avcodec_receive_packet(context->videoCodecContext, pkt) == 0) {
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);
            }
        }

        if (context && context->audioCodecContext) {
            AG_LOG_FAST(INFO, "Flushing audio encoder for user: %s", userId.c_str());
            avcodec_send_frame(context->audioCodecContext, nullptr);

            AVPacket* pkt = av_packet_alloc();
            if (pkt) {
                while (avcodec_receive_packet(context->audioCodecContext, pkt) == 0) {
                    av_packet_unref(pkt);
                }
                av_packet_free(&pkt);
            }
        }
    }

    AG_LOG_TS(INFO, "All encoders flushed successfully");
}

void RecordingSink::cleanupEncoder(const std::string& userId) {
    UserContext* context = nullptr;
    bool isComposite = (userId.empty() || config_.mode == RecordingMode::COMPOSITE);

    if (isComposite) {
        context = compositeContext_.get();
    } else {
        auto it = userContexts_.find(userId);
        if (it != userContexts_.end()) {
            context = it->second.get();
        }
    }

    if (!context) {
        return;
    }

    // Flush remaining audio samples first to prevent cutting
    if (context->audioCodecContext && !context->audioSampleBuffer.empty()) {
        AG_LOG_FAST(INFO, "Flushing remaining %zu audio samples for user %s",
                    context->audioSampleBuffer.size(), userId.c_str());

        int samples_per_frame = context->audioCodecContext->frame_size;
        if (samples_per_frame == 0) samples_per_frame = 1024;

        size_t required_samples = samples_per_frame * config_.audioChannels;

        // Pad with silence if necessary
        if (context->audioSampleBuffer.size() < required_samples) {
            size_t old_size = context->audioSampleBuffer.size();
            context->audioSampleBuffer.resize(required_samples, 0);  // Pad with silence
            AG_LOG_FAST(INFO, "Padded audio buffer from %zu to %zu samples", old_size,
                        required_samples);
        }

        // Create final audio frame
        AudioFrame finalFrame;
        finalFrame.data.resize(required_samples * sizeof(int16_t));
        std::memcpy(finalFrame.data.data(), context->audioSampleBuffer.data(),
                    finalFrame.data.size());
        finalFrame.sampleRate = config_.audioSampleRate;
        finalFrame.channels = config_.audioChannels;
        finalFrame.timestamp = context->lastBufferedTimestamp + 20;
        finalFrame.valid = true;
        finalFrame.userId = userId;

        encodeIndividualAudioFrame(finalFrame, context, userId);
        context->audioSampleBuffer.clear();
    }

    // Write any buffered frames
    if (context->videoCodecContext) {
        AVPacket* pkt = av_packet_alloc();
        if (pkt) {
            // Flush the encoder
            avcodec_send_frame(context->videoCodecContext, nullptr);
            while (avcodec_receive_packet(context->videoCodecContext, pkt) == 0) {
                // Set stream index and proper timestamps
                pkt->stream_index = context->videoStream->index;
                int64_t ticks_per_frame = context->videoStream->time_base.den / config_.videoFps;
                pkt->pts = context->videoFrameCount * ticks_per_frame;
                pkt->dts = pkt->pts;
                pkt->duration = ticks_per_frame;
                context->videoFrameCount++;

                av_interleaved_write_frame(context->formatContext, pkt);
                av_packet_unref(pkt);
            }
            av_packet_free(&pkt);
        }
    }

    // Write trailer
    if (context->formatContext) {
        av_write_trailer(context->formatContext);
    }

    // Cleanup resources
    if (context->videoCodecContext) {
        avcodec_free_context(&context->videoCodecContext);
    }
    if (context->audioCodecContext) {
        avcodec_free_context(&context->audioCodecContext);
    }
    if (context->formatContext) {
        if (!(context->formatContext->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&context->formatContext->pb);
        }
        avformat_free_context(context->formatContext);
    }
    if (context->swsContext) {
        sws_freeContext(context->swsContext);
    }
    if (context->swrContext) {
        swr_free(&context->swrContext);
    }
}

std::string RecordingSink::generateOutputFilename(const std::string& userId) {
    std::string baseFilename = agora::common::generateTimestampedFilename(
        "recording",
        agora::common::getFileExtension(getFileExtension().substr(1)),  // Remove dot from extension
        userId.empty() ? "" : userId,
        true  // Include milliseconds
    );

    return config_.outputDir + "/" + baseFilename;
}

std::string RecordingSink::getFileExtension() const {
    switch (config_.format) {
        case OutputFormat::MP4:
            return ".mp4";
        case OutputFormat::AVI:
            return ".avi";
        case OutputFormat::MKV:
            return ".mkv";
        default:
            return ".mp4";
    }
}

bool RecordingSink::createOutputDirectory() {
    return agora::common::createDirectoriesIfNotExists(config_.outputDir);
}

void RecordingSink::setMaxDuration(int seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.maxDurationSeconds = seconds;
}

void RecordingSink::setOutputFormat(OutputFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.format = format;
}

void RecordingSink::setRecordingMode(RecordingMode mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    config_.mode = mode;
}

bool RecordingSink::shouldRecordUser(const std::string& userId) const {
    // If no target users specified, record all users
    if (config_.targetUsers.empty()) {
        return true;
    }

    // Check if user is in the target list
    return std::find(config_.targetUsers.begin(), config_.targetUsers.end(), userId) !=
           config_.targetUsers.end();
}

bool RecordingSink::updateCompositeFrame(const VideoFrame& frame, const std::string& userId) {
    // std::cout << "[RecordingSink] updateCompositeFrame() - ENTRY, userId=" << userId <<
    // std::endl; std::flush(std::cout);

    {
        // std::cout
        //     << "[RecordingSink] updateCompositeFrame() - about to acquire compositeBufferMutex_"
        //     << std::endl;
        // std::flush(std::cout);
        std::lock_guard<std::mutex> lock(compositeBufferMutex_);

        // Store the latest frame from this user
        compositeFrameBuffer_[userId] = frame;

        // Track when this frame was received
        uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
        compositeFrameTimestamps_[userId] = currentTime;

        // Remove old frames that are too old (frame persistence cleanup)
        auto it = compositeFrameBuffer_.begin();
        while (it != compositeFrameBuffer_.end()) {
            const std::string& user = it->first;
            uint64_t frameTime = compositeFrameTimestamps_[user];

            if (currentTime - frameTime > COMPOSITE_FRAME_TIMEOUT_MS) {
                AG_LOG_FAST(INFO, "Removing old frame for user %s (age: %lums)", user.c_str(),
                            (currentTime - frameTime));
                compositeFrameTimestamps_.erase(user);
                it = compositeFrameBuffer_.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Always try to create composite frame - don't drop frames too aggressively
    // This ensures we don't miss users' frames due to timing differences
    uint64_t currentTime = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();

    // Only skip if we're processing too fast (less than 16ms = 60fps max)
    if (currentTime - lastCompositeTime_ < 16) {
        droppedFrames_++;
        return true;  // Skip this frame to maintain reasonable frame rate
    }

    // Start performance measurement
    frameProcessingStartTime_ = currentTime;
    lastCompositeTime_ = currentTime;

    // Use VideoCompositor for all video composition
    // std::cout
    //     << "[RecordingSink] updateCompositeFrame() - about to call
    //     videoCompositor_->addUserFrame()"
    //     << std::endl;
    // std::flush(std::cout);

    if (videoCompositor_) {
        // std::cout
        //     << "[RecordingSink] updateCompositeFrame() - calling
        //     videoCompositor_->addUserFrame()"
        //     << std::endl;
        // std::flush(std::cout);

        // Skip VideoCompositor if stop was requested to avoid blocking
        if (stopRequested_.load()) {
            // std::cout << "[RecordingSink] updateCompositeFrame() - stopRequested detected, "
            //              "skipping VideoCompositor"
            //           << std::endl;
            // std::flush(std::cout);
            return true;
        }

        bool result = videoCompositor_->addUserFrame(frame, userId);
        // std::cout << "[RecordingSink] updateCompositeFrame() - videoCompositor_->addUserFrame() "
        //              "returned: "
        //           << result << std::endl;
        // std::flush(std::cout);
        return result;
    } else {
        AG_LOG_FAST(ERROR, "VideoCompositor not initialized for composite mode");
        return false;
    }
}

void RecordingSink::onComposedFrame(const AVFrame* composedFrame) {
    if (!composedFrame || config_.mode != RecordingMode::COMPOSITE) {
        return;
    }

    static int debug_composed_count = 0;
    debug_composed_count++;
    if (debug_composed_count % 30 == 0) {
        AG_LOG_FAST(INFO, "DEBUG: onComposedFrame() #%d - Raw composedFrame->pts: %ld",
                    debug_composed_count, composedFrame->pts);
    }

    std::lock_guard<std::mutex> contextLock(userContextsMutex_);

    // Initialize composite context if needed
    if (!compositeContext_) {
        if (!initializeEncoder("")) {
            AG_LOG_FAST(ERROR, "Failed to initialize encoder for composite frame");
            return;
        }
    }

    UserContext* context = compositeContext_.get();
    if (!context || !context->videoCodecContext || !context->videoFrame) {
        AG_LOG_FAST(ERROR, "Invalid composite context for encoding");
        return;
    }

    // Copy the composed frame to our encoding frame
    if (av_frame_copy(context->videoFrame, composedFrame) < 0) {
        AG_LOG_FAST(ERROR, "Failed to copy composed frame");
        return;
    }

    // Initialize first frame timestamp for synchronization in composite mode
    if (!context->hasFirstVideoFrame) {
        context->firstVideoTimestamp = composedFrame->pts;
        context->hasFirstVideoFrame = true;
        AG_LOG_TS(INFO, "FIRST VIDEO timestamp (composite): %ld", context->firstVideoTimestamp);

        // Compare timestamps if audio timestamp is already set
        if (context->hasFirstAudioFrame) {
            int64_t diff = context->firstAudioTimestamp - context->firstVideoTimestamp;
            AG_LOG_TS(INFO,
                      "TIMESTAMP COMPARISON - Audio vs Video base difference: %ldms (Audio: %ld, "
                      "Video: %ld)",
                      diff, context->firstAudioTimestamp, context->firstVideoTimestamp);
        }
    }

    // Use relative timestamps to prevent overflow
    // Convert RTC timestamp to video timebase using frame-based precision
    int64_t timestamp_diff_ms = composedFrame->pts - context->firstVideoTimestamp;

    // Ensure we don't have negative or excessive differences
    if (timestamp_diff_ms < 0) {
        timestamp_diff_ms = 0;
    } else if (timestamp_diff_ms > INT64_MAX / 90000) {
        // Prevent overflow during rescaling to 90kHz time base
        AG_LOG_FAST(WARN, "Composite timestamp diff too large: %ld ms, using frame count",
                    timestamp_diff_ms);
        timestamp_diff_ms = context->videoFrameCount * 1000 / config_.videoFps;
    }

    // Convert milliseconds to video time base (frame intervals)
    context->videoFrame->pts =
        av_rescale_q(timestamp_diff_ms, {1, 1000}, context->videoCodecContext->time_base);

    static int composite_pts_log_count = 0;
    if (composite_pts_log_count % 30 == 0) {
        AG_LOG_FAST(INFO,
                    "COMPOSITE FRAME PTS FIX - Original RTC: %ld, diff_ms: %ld, FIXED PTS: %ld",
                    composedFrame->pts, timestamp_diff_ms, context->videoFrame->pts);
    }
    composite_pts_log_count++;

    context->videoFrameCount++;

    // Encode the composed frame
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        AG_LOG_FAST(ERROR, "Failed to allocate packet for composed frame");
        return;
    }

    // std::cout << "[RecordingSink] Composite: About to call avcodec_send_frame()" << std::endl;
    // std::flush(std::cout);

    int ret = avcodec_send_frame(context->videoCodecContext, context->videoFrame);

    // std::cout << "[RecordingSink] Composite: avcodec_send_frame() returned: " << ret <<
    // std::endl; std::flush(std::cout);

    if (ret < 0) {
        av_packet_free(&packet);
        AG_LOG_FAST(ERROR, "Failed to send composed frame to encoder");
        return;
    }

    // std::cout << "[RecordingSink] Composite: Entering avcodec_receive_packet() loop" <<
    // std::endl; std::flush(std::cout);

    while (ret >= 0) {
        // std::cout << "[RecordingSink] Composite: About to call avcodec_receive_packet()"
        //           << std::endl;
        // std::flush(std::cout);

        ret = avcodec_receive_packet(context->videoCodecContext, packet);

        // std::cout << "[RecordingSink] Composite: avcodec_receive_packet() returned: " << ret
        //           << std::endl;
        // std::flush(std::cout);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            av_packet_free(&packet);
            AG_LOG_FAST(ERROR, "Failed to receive packet from encoder");
            return;
        }

        packet->stream_index = context->videoStream->index;

        static int rescale_log_count = 0;
        if (rescale_log_count % 30 == 0) {
            AG_LOG_FAST(
                INFO, "BEFORE rescale - PTS: %ld, codec time_base: %d/%d, stream time_base: %d/%d",
                packet->pts, context->videoCodecContext->time_base.num,
                context->videoCodecContext->time_base.den, context->videoStream->time_base.num,
                context->videoStream->time_base.den);
        }
        rescale_log_count++;

        // Fix: Use manual rescaling to avoid overflow (same as individual mode)
        if (packet->pts != AV_NOPTS_VALUE) {
            int64_t rescaled_pts = av_rescale_q(packet->pts, context->videoCodecContext->time_base,
                                                context->videoStream->time_base);
            if (rescaled_pts != AV_NOPTS_VALUE && rescaled_pts >= 0) {
                packet->pts = rescaled_pts;
            } else {
                packet->pts =
                    context->videoFrameCount * av_rescale_q(1, (AVRational){1, config_.videoFps},
                                                            context->videoStream->time_base);
            }
        } else {
            packet->pts =
                context->videoFrameCount *
                av_rescale_q(1, (AVRational){1, config_.videoFps}, context->videoStream->time_base);
        }

        // Set DTS equal to PTS for video (no B-frames)
        packet->dts = packet->pts;

        if (rescale_log_count % 30 == 0) {
            AG_LOG_FAST(INFO, "AFTER rescale - PTS: %ld", packet->pts);
        }

        // Capture PTS value before writing (muxer may modify packet)
        int64_t original_pts = packet->pts;

        if (!writePacket(packet, context->formatContext, context->videoStream)) {
            AG_LOG_FAST(ERROR, "Failed to write composed frame packet");
        } else {
            static int composed_log_count = 0;
            if (composed_log_count % 30 == 0) {
                AG_LOG_FAST(INFO, "Successfully encoded composed frame %d, pts: %ld",
                            composed_log_count, original_pts);
            }
            composed_log_count++;
        }

        av_packet_unref(packet);
    }

    av_packet_free(&packet);
}

void RecordingSink::cleanupCompositeResources() {
    std::lock_guard<std::mutex> lock(compositeBufferMutex_);

    // Cleanup cached scaling contexts
    for (auto& pair : userScalingContexts_) {
        if (pair.second) {
            sws_freeContext(pair.second);
        }
    }
    userScalingContexts_.clear();

    // Cleanup pre-allocated frames
    for (auto& pair : scaledFramePool_) {
        if (pair.second) {
            av_frame_free(&pair.second);
        }
    }
    scaledFramePool_.clear();

    // Clear composite frame buffers
    compositeFrameBuffer_.clear();
    compositeFrameTimestamps_.clear();

    // Clear audio mixing buffers
    {
        std::lock_guard<std::mutex> audioLock(audioMixingMutex_);
        audioMixingBuffer_.clear();
    }

    AG_LOG_FAST(INFO, "Cleaned up composite performance caches");
}

}  // namespace rtc
}  // namespace agora