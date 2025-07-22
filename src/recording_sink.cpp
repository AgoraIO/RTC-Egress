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
        std::cerr << "Cannot initialize while recording" << std::endl;
        return false;
    }

    config_ = config;

    // Create output directory if it doesn't exist
    if (!createOutputDirectory()) {
        return false;
    }

    return true;
}

bool RecordingSink::start() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (isRecording_.load()) {
        std::cerr << "Already recording" << std::endl;
        return false;
    }

    stopRequested_ = false;
    isRecording_ = true;
    startTime_ = std::chrono::steady_clock::now();

    // Create recording thread
    recordingThread_ = std::make_unique<std::thread>(&RecordingSink::recordingThread, this);

    std::cout << "[RecordingSink] Started recording in "
              << (config_.mode == RecordingMode::INDIVIDUAL ? "individual" : "composite") << " mode"
              << std::endl;

    return true;
}

void RecordingSink::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);

        if (!isRecording_.load()) {
            return;
        }

        stopRequested_ = true;
        cv_.notify_all();
        videoQueueCv_.notify_all();
        audioQueueCv_.notify_all();
    }

    if (recordingThread_ && recordingThread_->joinable()) {
        recordingThread_->join();
        recordingThread_.reset();
    }

    // Ensure all frames are processed before cleanup
    processVideoFrames();
    processAudioFrames();

    // Cleanup all user contexts
    {
        std::lock_guard<std::mutex> lock(userContextsMutex_);

        // Process composite context first
        if (compositeContext_) {
            cleanupEncoder("");
            compositeContext_.reset();
        }

        // Process individual user contexts
        for (auto& pair : userContexts_) {
            cleanupEncoder(pair.first);
        }
        userContexts_.clear();
    }

    // Cleanup performance caches
    cleanupCompositeResources();

    isRecording_ = false;
    std::cout << "[RecordingSink] Stopped recording and saved files" << std::endl;
}

void RecordingSink::onVideoFrame(const uint8_t* yBuffer, const uint8_t* uBuffer,
                                 const uint8_t* vBuffer, int32_t yStride, int32_t uStride,
                                 int32_t vStride, uint32_t width, uint32_t height,
                                 uint64_t timestamp, const std::string& userId) {
    if (!isRecording_.load()) {
        return;
    }

    // Check if this user should be recorded
    if (!shouldRecordUser(userId)) {
        return;
    }

    // Validate input parameters to prevent memory corruption
    if (!yBuffer || !uBuffer || !vBuffer) {
        std::cerr << "[RecordingSink] Invalid buffer pointers" << std::endl;
        return;
    }

    if (width == 0 || height == 0 || yStride <= 0 || uStride <= 0 || vStride <= 0) {
        std::cerr << "[RecordingSink] Invalid frame dimensions or strides" << std::endl;
        return;
    }

    // Validate stride vs width relationships to prevent buffer overruns
    if (yStride < static_cast<int32_t>(width) || uStride < static_cast<int32_t>(width / 2) ||
        vStride < static_cast<int32_t>(width / 2)) {
        std::cerr << "[RecordingSink] Invalid stride values" << std::endl;
        return;
    }

    VideoFrame frame;
    frame.width = width;
    frame.height = height;
    frame.timestamp = timestamp;
    frame.userId = userId;
    frame.yStride = yStride;
    frame.uStride = uStride;
    frame.vStride = vStride;

    try {
        // Calculate actual buffer sizes with bounds checking
        size_t ySize = static_cast<size_t>(yStride) * height;
        size_t uSize = static_cast<size_t>(uStride) * height / 2;
        size_t vSize = static_cast<size_t>(vStride) * height / 2;

        // Sanity check: prevent excessive memory allocation
        const size_t MAX_FRAME_SIZE = 8 * 1024 * 1024;  // 8MB per plane max
        if (ySize > MAX_FRAME_SIZE || uSize > MAX_FRAME_SIZE || vSize > MAX_FRAME_SIZE) {
            std::cerr << "[RecordingSink] Frame size too large: Y=" << ySize << ", U=" << uSize
                      << ", V=" << vSize << std::endl;
            return;
        }

        // Pre-allocate and copy Y data
        frame.yData.reserve(ySize);
        frame.yData.resize(ySize);
        std::memcpy(frame.yData.data(), yBuffer, ySize);

        // Pre-allocate and copy U data
        frame.uData.reserve(uSize);
        frame.uData.resize(uSize);
        std::memcpy(frame.uData.data(), uBuffer, uSize);

        // Pre-allocate and copy V data
        frame.vData.reserve(vSize);
        frame.vData.resize(vSize);
        std::memcpy(frame.vData.data(), vBuffer, vSize);

        frame.valid = true;

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
        std::cerr << "[RecordingSink] Memory allocation failed: " << e.what() << std::endl;
        return;
    } catch (const std::exception& e) {
        std::cerr << "[RecordingSink] Error processing video frame: " << e.what() << std::endl;
        return;
    }
}

void RecordingSink::onAudioFrame(const uint8_t* audioBuffer, int samples, int sampleRate,
                                 int channels, uint64_t timestamp, const std::string& userId) {
    if (!isRecording_.load()) {
        return;
    }

    // Check if this user should be recorded
    if (!shouldRecordUser(userId)) {
        return;
    }

    static int audio_log_count = 0;
    if (audio_log_count % 50 == 0) {  // Log every 50th audio frame to reduce spam
        std::cout << "[RecordingSink] Received audio frame: " << samples << " samples, "
                  << sampleRate << "Hz, " << channels << " channels, user: " << userId << std::endl;
    }
    audio_log_count++;

    // Validate input parameters
    if (!audioBuffer) {
        std::cerr << "[RecordingSink] Invalid audio buffer pointer" << std::endl;
        return;
    }

    if (samples <= 0 || sampleRate <= 0 || channels <= 0 || channels > 8) {
        std::cerr << "[RecordingSink] Invalid audio parameters: samples=" << samples
                  << ", sampleRate=" << sampleRate << ", channels=" << channels << std::endl;
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
            std::cerr << "[RecordingSink] Audio frame size too large: " << dataSize << " bytes"
                      << std::endl;
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
        std::cerr << "[RecordingSink] Audio memory allocation failed: " << e.what() << std::endl;
        return;
    } catch (const std::exception& e) {
        std::cerr << "[RecordingSink] Error processing audio frame: " << e.what() << std::endl;
        return;
    }
}

void RecordingSink::recordingThread() {
    while (!stopRequested_.load()) {
        // Process video frames
        if (config_.recordVideo && !stopRequested_.load()) {
            processVideoFrames();
        }

        // Process audio frames
        if (config_.recordAudio && !stopRequested_.load()) {
            processAudioFrames();
        }

        // Check for timeout
        auto elapsed = std::chrono::steady_clock::now() - startTime_;
        if (std::chrono::duration_cast<std::chrono::seconds>(elapsed).count() >=
            config_.maxDurationSeconds) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void RecordingSink::processVideoFrames() {
    std::unique_lock<std::mutex> lock(videoQueueMutex_);

    while (!videoFrameQueue_.empty()) {
        VideoFrame frame = videoFrameQueue_.front();
        videoFrameQueue_.pop();
        lock.unlock();

        if (frame.valid) {
            encodeVideoFrame(frame, frame.userId);
        }

        lock.lock();
    }
}

void RecordingSink::processAudioFrames() {
    std::unique_lock<std::mutex> lock(audioQueueMutex_);

    while (!audioFrameQueue_.empty()) {
        AudioFrame frame = audioFrameQueue_.front();
        audioFrameQueue_.pop();
        lock.unlock();

        if (frame.valid) {
            encodeAudioFrame(frame, frame.userId);
        }

        lock.lock();
    }
}

bool RecordingSink::initializeEncoder(const std::string& userId) {
    std::unique_ptr<UserContext> context = std::make_unique<UserContext>();
    context->startTime = std::chrono::steady_clock::now();
    context->filename = generateOutputFilename(userId);

    // Setup output format
    if (!setupOutputFormat(&context->formatContext, context->filename)) {
        std::cerr << "[RecordingSink] Failed to setup output format for user " << userId
                  << std::endl;
        return false;
    }

    // Setup video encoder if enabled
    if (config_.recordVideo) {
        if (!setupVideoEncoder(&context->videoCodecContext, userId)) {
            std::cerr << "[RecordingSink] Failed to setup video encoder for user " << userId
                      << std::endl;
            return false;
        }

        context->videoStream =
            avformat_new_stream(context->formatContext, context->videoCodecContext->codec);
        if (!context->videoStream) {
            std::cerr << "[RecordingSink] Failed to create video stream for user " << userId
                      << std::endl;
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
            std::cerr << "[RecordingSink] Failed to allocate video frame for user " << userId
                      << std::endl;
            return false;
        }

        context->videoFrame->format = context->videoCodecContext->pix_fmt;
        context->videoFrame->width = context->videoCodecContext->width;
        context->videoFrame->height = context->videoCodecContext->height;

        if (av_frame_get_buffer(context->videoFrame, 32) < 0) {
            std::cerr << "[RecordingSink] Failed to allocate video frame buffer for user " << userId
                      << std::endl;
            return false;
        }

        // Note: Scaling context will be created dynamically when we know actual frame dimensions
        context->swsContext = nullptr;
    }

    // Setup audio encoder if enabled
    if (config_.recordAudio) {
        std::cout << "[RecordingSink] Setting up audio encoder for user " << userId << std::endl;
        if (!setupAudioEncoder(&context->audioCodecContext, userId)) {
            std::cerr << "[RecordingSink] Failed to setup audio encoder for user " << userId
                      << std::endl;
            return false;
        }
        std::cout << "[RecordingSink] Audio encoder setup successful for user " << userId
                  << std::endl;

        context->audioStream =
            avformat_new_stream(context->formatContext, context->audioCodecContext->codec);
        if (!context->audioStream) {
            std::cerr << "[RecordingSink] Failed to create audio stream for user " << userId
                      << std::endl;
            return false;
        }

        avcodec_parameters_from_context(context->audioStream->codecpar, context->audioCodecContext);
        context->audioStream->time_base = context->audioCodecContext->time_base;

        // Allocate audio frame for output
        context->audioFrame = av_frame_alloc();
        if (!context->audioFrame) {
            std::cerr << "[RecordingSink] Failed to allocate audio frame for user " << userId
                      << std::endl;
            return false;
        }

        // Initialize audio resampling context for format conversion
        // We'll create this dynamically when we receive the first audio frame
        context->swrContext = nullptr;
    }

    // Write header
    if (avformat_write_header(context->formatContext, nullptr) < 0) {
        std::cerr << "[RecordingSink] Failed to write header for user " << userId << std::endl;
        return false;
    }

    // Log the actual time bases after header is written
    std::cout << "[RecordingSink] Stream time base: " << context->videoStream->time_base.num << "/"
              << context->videoStream->time_base.den
              << ", Codec time base: " << context->videoCodecContext->time_base.num << "/"
              << context->videoCodecContext->time_base.den << std::endl;

    // Save filename before moving context
    std::string filename = context->filename;

    // Store context (mutex already held by caller)
    if (config_.mode == RecordingMode::INDIVIDUAL) {
        userContexts_[userId] = std::move(context);
    } else {
        compositeContext_ = std::move(context);
    }

    std::cout << "[RecordingSink] Initialized encoder for user " << userId
              << ", output file: " << filename << std::endl;

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
        std::cerr << "[RecordingSink] Video codec not found: " << config_.videoCodec << std::endl;
        return false;
    }

    *videoCodecContext = avcodec_alloc_context3(codec);
    if (!*videoCodecContext) {
        std::cerr << "[RecordingSink] Failed to allocate video codec context" << std::endl;
        return false;
    }

    (*videoCodecContext)->bit_rate = config_.videoBitrate;
    (*videoCodecContext)->width = config_.videoWidth;
    (*videoCodecContext)->height = config_.videoHeight;
    (*videoCodecContext)->time_base = {1, config_.videoFps};  // Standard time base for video
    (*videoCodecContext)->framerate = {config_.videoFps, 1};
    (*videoCodecContext)->gop_size = config_.videoFps;
    (*videoCodecContext)->max_b_frames = 0;  // Disable B-frames for stability
    (*videoCodecContext)->pix_fmt = AV_PIX_FMT_YUV420P;

    // Add encoder options for stability
    (*videoCodecContext)->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

    // Set codec options for stability
    AVDictionary* opts = nullptr;
    av_dict_set(&opts, "preset", "fast", 0);
    av_dict_set(&opts, "tune", "zerolatency", 0);

    if (avcodec_open2(*videoCodecContext, codec, &opts) < 0) {
        std::cerr << "[RecordingSink] Failed to open video codec" << std::endl;
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
        std::cerr << "[RecordingSink] Audio codec not found: " << config_.audioCodec << std::endl;
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
    (*audioCodecContext)->time_base = {1, config_.audioSampleRate};

    if (avcodec_open2(*audioCodecContext, codec, nullptr) < 0) {
        return false;
    }

    return true;
}

bool RecordingSink::encodeVideoFrame(const VideoFrame& frame, const std::string& userId) {
    if (config_.mode == RecordingMode::INDIVIDUAL) {
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
        return updateCompositeFrame(frame, userId);
    }
}

bool RecordingSink::encodeIndividualFrame(const VideoFrame& frame, UserContext* context) {
    if (!context || !context->videoCodecContext || !context->videoFrame) {
        return false;
    }

    // Create scaling context dynamically if not already created or if frame dimensions changed
    if (!context->swsContext) {
        context->swsContext = sws_getContext(
            frame.width, frame.height, AV_PIX_FMT_YUV420P,  // Use actual incoming frame dimensions
            context->videoCodecContext->width, context->videoCodecContext->height,
            context->videoCodecContext->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr);

        if (!context->swsContext) {
            std::cerr << "[RecordingSink] Failed to create scaling context for frame "
                      << frame.width << "x" << frame.height << " -> "
                      << context->videoCodecContext->width << "x"
                      << context->videoCodecContext->height << std::endl;
            return false;
        }
    }

    // Convert YUV frame to encoder format
    const uint8_t* srcData[3] = {frame.yData.data(), frame.uData.data(), frame.vData.data()};
    int srcLinesize[3] = {frame.yStride, frame.uStride, frame.vStride};

    // Ensure frame is writable before scaling
    if (av_frame_make_writable(context->videoFrame) < 0) {
        std::cerr << "[RecordingSink] Failed to make frame writable" << std::endl;
        return false;
    }

    sws_scale(context->swsContext, srcData, srcLinesize, 0, frame.height, context->videoFrame->data,
              context->videoFrame->linesize);

    // Initialize first frame timestamp for synchronization
    if (!context->hasFirstVideoFrame) {
        context->firstVideoTimestamp = frame.timestamp;
        context->hasFirstVideoFrame = true;
    }

    // Calculate PTS based on the timestamp from the frame
    context->videoFrame->pts = (frame.timestamp - context->firstVideoTimestamp) *
                               context->videoCodecContext->time_base.den /
                               (context->videoCodecContext->time_base.num * 1000);

    static int pts_log_count = 0;
    if (pts_log_count % 30 == 0) {
        std::cout << "[RecordingSink] Video PTS: " << context->videoFrame->pts
                  << ", timestamp: " << frame.timestamp
                  << ", first_timestamp: " << context->firstVideoTimestamp << std::endl;
    }
    pts_log_count++;

    context->videoFrameCount++;

    // Encode frame
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    int ret = avcodec_send_frame(context->videoCodecContext, context->videoFrame);
    if (ret < 0) {
        av_packet_free(&packet);
        return false;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(context->videoCodecContext, packet);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            av_packet_free(&packet);
            return false;
        }

        // Set the stream index
        packet->stream_index = context->videoStream->index;

        // Rescale timestamps to stream time base for proper muxing
        av_packet_rescale_ts(packet, context->videoCodecContext->time_base,
                             context->videoStream->time_base);

        // Write packet - use av_interleaved_write_frame for proper timestamp ordering
        int write_ret = av_interleaved_write_frame(context->formatContext, packet);
        if (write_ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, write_ret);
            std::cerr << "[RecordingSink] Failed to write video frame: " << errbuf << std::endl;
        } else {
            static int video_log_count = 0;
            if (video_log_count % 30 == 0) {  // Log every 30th frame to reduce spam
                std::cout << "[RecordingSink] Successfully wrote video packet " << video_log_count
                          << ", pts: " << packet->pts << std::endl;
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
        std::cout << "[RecordingSink] Encoding audio frame for user: " << userId
                  << ", input: " << frame.sampleRate << "Hz, " << frame.channels << " channels"
                  << ", target: " << config_.audioSampleRate << "Hz, " << config_.audioChannels
                  << " channels" << std::endl;
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
        std::cout << "[RecordingSink] Composite mode: mixing audio from user " << userId
                  << std::endl;
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

    std::cout << "[RecordingSink] Audio format check: input=" << frame.sampleRate << "Hz/"
              << frame.channels << "ch, target=" << config_.audioSampleRate << "Hz/"
              << config_.audioChannels << "ch, needsResampling=" << (needsResampling ? "YES" : "NO")
              << std::endl;

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
            std::cerr << "[RecordingSink] Failed to allocate resampling context" << std::endl;
            return false;
        }

        ret = swr_init(context->swrContext);
        if (ret < 0) {
            std::cerr << "[RecordingSink] Failed to initialize resampling context" << std::endl;
            swr_free(&context->swrContext);
            return false;
        }

        std::cout << "[RecordingSink] Initialized audio resampler: " << frame.sampleRate << "Hz "
                  << frame.channels << "ch -> " << config_.audioSampleRate << "Hz "
                  << config_.audioChannels << "ch" << std::endl;
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
            std::cout << "[RecordingSink] Buffering audio: " << context->audioSampleBuffer.size()
                      << "/" << required_samples << " samples" << std::endl;
        }
        buffer_log_count++;
        return true;  // Successfully buffered, but not ready to encode yet
    }

    std::cout << "[RecordingSink] Ready to encode: " << context->audioSampleBuffer.size()
              << " samples (need " << required_samples << ")" << std::endl;

    // Initialize first frame timestamp for synchronization
    if (!context->hasFirstAudioFrame) {
        context->firstAudioTimestamp = frame.timestamp;
        context->hasFirstAudioFrame = true;
    }

    // Calculate PTS based on the timestamp from the frame
    context->audioFrame->pts = (frame.timestamp - context->firstAudioTimestamp) *
                               context->audioCodecContext->time_base.den /
                               (context->audioCodecContext->time_base.num * 1000);

    static int audio_pts_log_count = 0;
    if (audio_pts_log_count % 50 == 0) {
        std::cout << "[RecordingSink] Audio PTS: " << context->audioFrame->pts
                  << ", timestamp: " << frame.timestamp
                  << ", first_timestamp: " << context->firstAudioTimestamp << std::endl;
    }
    audio_pts_log_count++;

    // Set up audio frame properties for output
    context->audioFrame->nb_samples = samples_per_frame;
    context->audioFrame->format = context->audioCodecContext->sample_fmt;
    context->audioFrame->sample_rate = context->audioCodecContext->sample_rate;
    av_channel_layout_copy(&context->audioFrame->ch_layout, &context->audioCodecContext->ch_layout);

    // Set PTS BEFORE encoding
    int64_t audioPTS = (frame.timestamp - context->firstAudioTimestamp) *
                       context->audioCodecContext->time_base.den /
                       (context->audioCodecContext->time_base.num * 1000);
    context->audioFrame->pts = audioPTS;

    // Allocate buffer for audio frame
    if (av_frame_get_buffer(context->audioFrame, 0) < 0) {
        std::cerr << "[RecordingSink] Failed to make audio frame writable" << std::endl;
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
            std::cerr << "[RecordingSink] Audio resampling failed" << std::endl;
            return false;
        }

        // Update the actual number of samples produced
        context->audioFrame->nb_samples = output_samples;

        static int resample_log_count = 0;
        if (resample_log_count % 100 == 0) {
            std::cout << "[RecordingSink] Resampled " << input_samples_for_resampling << " -> "
                      << output_samples << " samples" << std::endl;
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

        // Debug: Check PTS before rescaling
        std::cout << "[RecordingSink] BEFORE rescale - packet PTS: " << packet->pts
                  << ", codec time_base: " << context->audioCodecContext->time_base.num << "/"
                  << context->audioCodecContext->time_base.den
                  << ", stream time_base: " << context->audioStream->time_base.num << "/"
                  << context->audioStream->time_base.den << std::endl;

        // Rescale timestamps to stream time base for proper muxing
        av_packet_rescale_ts(packet, context->audioCodecContext->time_base,
                             context->audioStream->time_base);

        // Debug: Check PTS after rescaling
        std::cout << "[RecordingSink] AFTER rescale - packet PTS: " << packet->pts << std::endl;

        // Write packet using interleaved write for proper timestamp ordering
        int write_ret = av_interleaved_write_frame(context->formatContext, packet);
        if (write_ret < 0) {
            char errbuf[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_make_error_string(errbuf, AV_ERROR_MAX_STRING_SIZE, write_ret);
            std::cerr << "[RecordingSink] Failed to write audio frame: " << errbuf << std::endl;
        } else {
            static int audio_write_log_count = 0;
            if (audio_write_log_count % 50 == 0) {
                std::cout << "[RecordingSink] Successfully wrote audio packet "
                          << audio_write_log_count << ", pts: " << packet->pts << std::endl;
            }
            audio_write_log_count++;
        }
        av_packet_unref(packet);
    }

    av_packet_free(&packet);
    return true;
}

bool RecordingSink::mixAudioFromMultipleUsers(const AudioFrame& frame, const std::string& userId) {
    std::cout << "[RecordingSink] mixAudioFromMultipleUsers called for user " << userId << " with "
              << frame.data.size() << " bytes" << std::endl;

    // Convert audio frame to float format for mixing
    std::vector<float> audioSamples;
    int input_samples = frame.data.size() / sizeof(int16_t) / frame.channels;
    const int16_t* input_data = reinterpret_cast<const int16_t*>(frame.data.data());

    // Convert to float and normalize
    audioSamples.resize(input_samples * frame.channels);
    for (int i = 0; i < input_samples * frame.channels; i++) {
        audioSamples[i] = static_cast<float>(input_data[i]) / 32768.0f;
    }

    // Store user's audio in mixing buffer
    {
        std::lock_guard<std::mutex> lock(audioMixingMutex_);
        audioMixingBuffer_[userId] = audioSamples;
        std::cout << "[RecordingSink] Audio mixing buffer now has " << audioMixingBuffer_.size()
                  << " users" << std::endl;
    }

    // Trigger mixing immediately when new audio data is available
    return createMixedAudioFrame();
}

bool RecordingSink::createMixedAudioFrame() {
    std::lock_guard<std::mutex> mixLock(audioMixingMutex_);
    std::lock_guard<std::mutex> contextLock(userContextsMutex_);

    std::cout << "[RecordingSink] createMixedAudioFrame called with " << audioMixingBuffer_.size()
              << " users in buffer" << std::endl;

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
    mixedFrame.timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count();
    mixedFrame.valid = true;

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
        std::cerr << "[RecordingSink] Failed to write packet: " << errbuf << std::endl;
        return false;
    }
    return true;
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
    auto now = std::chrono::system_clock::now();
    auto time = std::chrono::system_clock::to_time_t(now);
    auto msSinceEpoch =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();

    std::ostringstream oss;

    if (config_.mode == RecordingMode::INDIVIDUAL && !userId.empty()) {
        oss << config_.outputDir << "/user_" << userId << "_recording_"
            << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S_") << std::setfill('0')
            << std::setw(3) << msSinceEpoch % 1000 << getFileExtension();
    } else {
        oss << config_.outputDir << "/recording_"
            << std::put_time(std::localtime(&time), "%Y%m%d_%H%M%S_") << std::setfill('0')
            << std::setw(3) << msSinceEpoch % 1000 << getFileExtension();
    }

    return oss.str();
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
    try {
        if (!fs::exists(config_.outputDir)) {
            if (!fs::create_directories(config_.outputDir)) {
                std::cerr << "[RecordingSink] Failed to create output directory: "
                          << config_.outputDir << std::endl;
                return false;
            }
        }
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[RecordingSink] Exception creating output directory: " << e.what()
                  << std::endl;
        return false;
    }
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
    {
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
                std::cout << "[RecordingSink] Removing old frame for user " << user
                          << " (age: " << (currentTime - frameTime) << "ms)" << std::endl;
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
    return createCompositeFrame();
}

bool RecordingSink::createCompositeFrame() {
    std::lock_guard<std::mutex> bufferLock(compositeBufferMutex_);
    std::lock_guard<std::mutex> contextLock(userContextsMutex_);

    // Initialize composite context if needed
    if (!compositeContext_) {
        if (!initializeEncoder("")) {
            return false;
        }
    }

    UserContext* context = compositeContext_.get();
    if (!context || !context->videoCodecContext || !context->videoFrame) {
        return false;
    }

    // Determine the users to composite
    std::vector<std::string> usersToComposite;
    for (const auto& pair : compositeFrameBuffer_) {
        usersToComposite.push_back(pair.first);
    }

    if (usersToComposite.empty()) {
        return true;  // No frames to composite
    }

    // Check if the layout needs to be updated
    if (usersToComposite.size() != lastLayoutUserCount_) {
        lastLayoutUserCount_ = usersToComposite.size();
        auto layout = calculateOptimalLayout(lastLayoutUserCount_);
        lastCols_ = layout.first;
        lastRows_ = layout.second;
    }

    int canvasWidth = context->videoCodecContext->width;
    int canvasHeight = context->videoCodecContext->height;
    int cellWidth = canvasWidth / lastCols_;
    int cellHeight = canvasHeight / lastRows_;

    // Clear the composite frame to black
    if (av_frame_make_writable(context->videoFrame) < 0) {
        std::cerr << "[RecordingSink] Failed to make composite frame writable" << std::endl;
        return false;
    }
    memset(context->videoFrame->data[0], 0, context->videoFrame->linesize[0] * canvasHeight);
    memset(context->videoFrame->data[1], 128, context->videoFrame->linesize[1] * canvasHeight / 2);
    memset(context->videoFrame->data[2], 128, context->videoFrame->linesize[2] * canvasHeight / 2);

    // Composite each user's frame
    for (size_t i = 0; i < usersToComposite.size(); ++i) {
        const std::string& userId = usersToComposite[i];
        const VideoFrame& userFrame = compositeFrameBuffer_[userId];

        int col = i % lastCols_;
        int row = i / lastCols_;
        int x = col * cellWidth;
        int y = row * cellHeight;

        // Get or create cached scaling context for this user
        SwsContext* userSwsContext = userScalingContexts_[userId];
        if (!userSwsContext) {
            userSwsContext = sws_getContext(userFrame.width, userFrame.height, AV_PIX_FMT_YUV420P,
                                            cellWidth, cellHeight, AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                                            nullptr, nullptr, nullptr);
            userScalingContexts_[userId] = userSwsContext;
        }

        // Scale user frame to cell size
        const uint8_t* srcData[3] = {userFrame.yData.data(), userFrame.uData.data(),
                                     userFrame.vData.data()};
        int srcLinesize[3] = {userFrame.yStride, userFrame.uStride, userFrame.vStride};

        // Create a temporary AVFrame for the scaled output
        AVFrame* scaledFrame = av_frame_alloc();
        scaledFrame->format = AV_PIX_FMT_YUV420P;
        scaledFrame->width = cellWidth;
        scaledFrame->height = cellHeight;
        av_frame_get_buffer(scaledFrame, 32);

        sws_scale(userSwsContext, srcData, srcLinesize, 0, userFrame.height, scaledFrame->data,
                  scaledFrame->linesize);

        // Copy scaled frame to composite canvas
        for (int plane = 0; plane < 3; ++plane) {
            int planeHeight = (plane == 0) ? cellHeight : cellHeight / 2;
            int planeWidth = (plane == 0) ? cellWidth : cellWidth / 2;
            int destX = (plane == 0) ? x : x / 2;
            int destY = (plane == 0) ? y : y / 2;

            for (int py = 0; py < planeHeight; ++py) {
                memcpy(context->videoFrame->data[plane] +
                           (destY + py) * context->videoFrame->linesize[plane] + destX,
                       scaledFrame->data[plane] + py * scaledFrame->linesize[plane], planeWidth);
            }
        }
        av_frame_free(&scaledFrame);
    }

    // Set timestamp from the most recent frame
    uint64_t latestTimestamp = 0;
    for (const auto& pair : compositeFrameBuffer_) {
        latestTimestamp = std::max(latestTimestamp, pair.second.timestamp);
    }

    // Initialize first frame timestamp for synchronization
    if (!context->hasFirstVideoFrame) {
        context->firstVideoTimestamp = latestTimestamp;
        context->hasFirstVideoFrame = true;
    }

    // Calculate PTS based on the timestamp from the frame
    context->videoFrame->pts = (latestTimestamp - context->firstVideoTimestamp) *
                               context->videoCodecContext->time_base.den /
                               (context->videoCodecContext->time_base.num * 1000);

    context->videoFrameCount++;

    // Encode composite frame
    AVPacket* packet = av_packet_alloc();
    if (!packet) {
        return false;
    }

    int ret = avcodec_send_frame(context->videoCodecContext, context->videoFrame);
    if (ret < 0) {
        av_packet_free(&packet);
        return false;
    }

    while (ret >= 0) {
        ret = avcodec_receive_packet(context->videoCodecContext, packet);

        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        } else if (ret < 0) {
            av_packet_free(&packet);
            return false;
        }

        packet->stream_index = context->videoStream->index;
        av_packet_rescale_ts(packet, context->videoCodecContext->time_base,
                             context->videoStream->time_base);

        if (!writePacket(packet, context->formatContext, context->videoStream)) {
            av_packet_free(&packet);
            return false;
        }
    }

    av_packet_free(&packet);
    return true;
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

    std::cout << "[RecordingSink] Cleaned up composite performance caches" << std::endl;
}

}  // namespace rtc
}  // namespace agora