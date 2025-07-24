// rtc_client.h
#pragma once

// System includes
#include <sys/stat.h>

// Agora SDK includes
#include <AgoraBase.h>
#include <AgoraMediaBase.h>
#include <AgoraRefCountedObject.h>  // For RefCountedObject
#include <AgoraRefPtr.h>
#include <IAgoraService.h>
#include <NGIAgoraLocalUser.h>
#include <NGIAgoraMediaNode.h>
#include <NGIAgoraMediaNodeFactory.h>
#include <NGIAgoraRtcConnection.h>
#include <NGIAgoraVideoTrack.h>  // For IVideoSinkBase

// Standard C++ includes
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <future>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
// Modular video processing components
#include <sstream>
#include <string>
#include <thread>

#include "layout_detector.h"
#include "snapshot_encoder.h"
#include "video_compositor.h"
#include "video_frame.h"

namespace agora {
namespace rtc {

struct AgoraServiceDeleter {
    void operator()(agora::base::IAgoraService* ptr) const {
        if (ptr) ptr->release();
    }
};

class RtcClient {
   public:
    struct Config {
        std::string appId;
        std::string channel;
        std::string accessToken;
        std::string egressUid = "42";
        std::string EGRESS_HOME;
        std::string remoteUserId;
        std::string outputVideoFile = "received_video.yuv";
        bool enableVideo = true;
        bool enableAudio = false;
        bool autoSubscribeAudio = false;
        bool autoSubscribeVideo = false;
        agora::rtc::CLIENT_ROLE_TYPE role = agora::rtc::CLIENT_ROLE_AUDIENCE;

        // Audio playback parameters for egress - defaults to 16kHz mono but configurable
        int audioPlaybackSampleRate = 16000;
        int audioPlaybackChannels = 1;
        int audioPlaybackSamplesPerCall = 160;  // 16000 / 100 = 160 samples per 10ms frame
    };

    using FrameCallback =
        std::function<void(const agora::media::base::VideoFrame&, const std::string& userId)>;
    using AudioFrameCallback =
        std::function<void(const agora::media::base::AudioPcmFrame&, const std::string& userId)>;

    RtcClient();
    ~RtcClient();

    bool initialize(const Config& config);
    bool connect();
    void disconnect();
    void teardown();
    bool isConnected() const {
        return connected_;
    }

    void setVideoFrameCallback(FrameCallback callback) {
        frameCallback_ = std::move(callback);
    }

    void setAudioFrameCallback(AudioFrameCallback callback) {
        audioFrameCallback_ = std::move(callback);
    }

    void setChannel(const std::string& channel) {
        config_.channel = channel;
    }
    void setAccessToken(const std::string& token) {
        config_.accessToken = token;
    }

   private:
    class YuvFrameObserver : public agora::RefCountedObject<agora::rtc::IVideoFrameObserver2> {
       public:
        explicit YuvFrameObserver(FrameCallback callback) : callback_(std::move(callback)) {}
        virtual ~YuvFrameObserver() = default;

        // IVideoFrameObserver2 interface
        void onFrame(const char* channelId, agora::user_id_t remoteUid,
                     const agora::media::base::VideoFrame* videoFrame) override {
            if (callback_ && videoFrame) {
                std::string userId = (remoteUid == nullptr) ? "unknown" : std::string(remoteUid);
                callback_(*videoFrame, userId);
            }
        }

       private:
        FrameCallback callback_;
    };

    class AudioFrameObserver
        : public agora::RefCountedObject<agora::media::IAudioFrameObserverBase> {
       public:
        explicit AudioFrameObserver(AudioFrameCallback callback, int sampleRate = 16000,
                                    int channels = 1)
            : callback_(std::move(callback)),
              audioPlaybackSampleRate_(sampleRate),
              audioPlaybackChannels_(channels),
              audioPlaybackSamplesPerCall_(sampleRate / 100) {}  // 10ms frame
        virtual ~AudioFrameObserver() = default;

        // IAudioFrameObserverBase interface - implement all required methods
        bool onRecordAudioFrame(const char* channelId, AudioFrame& audioFrame) override {
            return true;  // Not used for our case
        }

        bool onPlaybackAudioFrame(const char* channelId, AudioFrame& audioFrame) override {
            return true;  // Not used for our case
        }

        bool onMixedAudioFrame(const char* channelId, AudioFrame& audioFrame) override {
            return true;  // Not used for our case
        }

        bool onEarMonitoringAudioFrame(AudioFrame& audioFrame) override {
            return true;  // Not used for our case
        }

        bool onPlaybackAudioFrameBeforeMixing(const char* channelId, agora::user_id_t uid,
                                              AudioFrame& audioFrame) override {
            if (callback_) {
                // Validate audio frame data before processing
                if (!audioFrame.buffer || audioFrame.samplesPerChannel <= 0 ||
                    audioFrame.channels <= 0 || audioFrame.samplesPerSec <= 0) {
                    return true;  // Skip invalid frames
                }

                // Convert AudioFrame to AudioPcmFrame for callback
                agora::media::base::AudioPcmFrame pcmFrame;
                pcmFrame.samples_per_channel_ = audioFrame.samplesPerChannel;
                pcmFrame.sample_rate_hz_ = audioFrame.samplesPerSec;
                pcmFrame.num_channels_ = audioFrame.channels;
                pcmFrame.capture_timestamp = audioFrame.renderTimeMs;
                pcmFrame.bytes_per_sample = audioFrame.bytesPerSample;

                // Get user ID for debugging and callback
                std::string userId = (uid == nullptr) ? "unknown" : std::string(uid);

                // Copy audio data with proper validation
                size_t dataSize = audioFrame.samplesPerChannel * audioFrame.channels;
                if (dataSize > 0 &&
                    dataSize <= agora::media::base::AudioPcmFrame::kMaxDataSizeSamples) {
                    // Clear the buffer first to avoid garbage data
                    memset(pcmFrame.data_, 0, sizeof(pcmFrame.data_));

                    // Copy the audio data - ensure we don't copy more than available
                    size_t bytesToCopy = dataSize * sizeof(int16_t);
                    memcpy(pcmFrame.data_, audioFrame.buffer, bytesToCopy);

                    // DEBUG: Dump PCM audio to file for debugging - per user
                    static std::map<std::string, FILE*> pcmFiles;
                    static std::map<std::string, int> frameCounts;
                    static bool dirCreated = false;

                    if (pcmFiles.find(userId) == pcmFiles.end()) {
                        // Create debug directory if it doesn't exist
                        if (!dirCreated) {
                            struct stat st = {0};
                            if (stat("./debug", &st) == -1) {
                                mkdir("./debug", 0755);
                            }
                            dirCreated = true;
                        }
                        // Create filename with audio format info and user ID
                        char filename[256];
                        snprintf(filename, sizeof(filename),
                                 "./debug/debug_audio_user_%s_%dHz_%dch_s16le.pcm", userId.c_str(),
                                 audioFrame.samplesPerSec, audioFrame.channels);
                        pcmFiles[userId] = fopen(filename, "wb");
                        frameCounts[userId] = 0;
                        printf(
                            "[RtcClient] PCM dump started: %s (user: %s, %dHz, %dch, s16le, %d "
                            "samples per "
                            "frame)\n",
                            filename, userId.c_str(), audioFrame.samplesPerSec, audioFrame.channels,
                            audioFrame.samplesPerChannel);
                        printf("[RtcClient] To play: ffplay -f s16le -ar %d -ac %d %s\n",
                               audioFrame.samplesPerSec, audioFrame.channels, filename);
                    }

                    FILE* pcmFile = pcmFiles[userId];
                    if (pcmFile) {
                        // Check if audio buffer contains actual data (not all zeros)
                        bool hasAudioData = false;
                        int16_t* samples = (int16_t*)audioFrame.buffer;
                        int sampleCount = bytesToCopy / sizeof(int16_t);
                        for (int i = 0; i < sampleCount; i++) {
                            if (samples[i] != 0) {
                                hasAudioData = true;
                                break;
                            }
                        }

                        if (frameCounts[userId] % 200 == 0) {
                            printf(
                                "[RtcClient] User %s audio frame %d: %d samples, hasData=%s, "
                                "first_sample=%d\n",
                                userId.c_str(), frameCounts[userId], sampleCount,
                                hasAudioData ? "YES" : "NO", sampleCount > 0 ? samples[0] : 0);
                        }

                        fwrite(audioFrame.buffer, 1, bytesToCopy, pcmFile);
                        fflush(pcmFile);
                        frameCounts[userId]++;
                        if (frameCounts[userId] % 100 == 0) {
                            printf("[RtcClient] Dumped %d audio frames to PCM file for user %s\n",
                                   frameCounts[userId], userId.c_str());
                        }
                    }

                    // Always forward the audio frame - let the encoder handle format conversion
                    // printf("[RtcClient] Forwarding audio frame: %dHz, %dch, %d samples, user:
                    // %s\n",
                    //        audioFrame.samplesPerSec, audioFrame.channels,
                    //        audioFrame.samplesPerChannel, userId.c_str());
                    callback_(pcmFrame, userId);
                }
            }
            return true;
        }

        int getObservedAudioFramePosition() override {
            return AUDIO_FRAME_POSITION_BEFORE_MIXING;
        }

        AudioParams getPlaybackAudioParams() override {
            AudioParams params;
            params.sample_rate = audioPlaybackSampleRate_;
            params.channels = audioPlaybackChannels_;
            params.mode = agora::rtc::RAW_AUDIO_FRAME_OP_MODE_READ_ONLY;
            params.samples_per_call = audioPlaybackSamplesPerCall_;
            return params;
        }

        AudioParams getRecordAudioParams() override {
            AudioParams params;
            params.sample_rate = audioPlaybackSampleRate_;
            params.channels = audioPlaybackChannels_;
            params.mode = agora::rtc::RAW_AUDIO_FRAME_OP_MODE_READ_ONLY;
            params.samples_per_call = audioPlaybackSamplesPerCall_;
            return params;
        }

        AudioParams getMixedAudioParams() override {
            AudioParams params;
            params.sample_rate = audioPlaybackSampleRate_;
            params.channels = audioPlaybackChannels_;
            params.mode = agora::rtc::RAW_AUDIO_FRAME_OP_MODE_READ_ONLY;
            params.samples_per_call = audioPlaybackSamplesPerCall_;
            return params;
        }

        AudioParams getEarMonitoringAudioParams() override {
            AudioParams params;
            params.sample_rate = audioPlaybackSampleRate_;
            params.channels = audioPlaybackChannels_;
            params.mode = agora::rtc::RAW_AUDIO_FRAME_OP_MODE_READ_ONLY;
            params.samples_per_call = audioPlaybackSamplesPerCall_;
            return params;
        }

       private:
        AudioFrameCallback callback_;
        int audioPlaybackSampleRate_;
        int audioPlaybackChannels_;
        int audioPlaybackSamplesPerCall_;
    };

   private:
    Config config_;
    bool initialized_ = false;
    std::atomic<bool> connected_{false};
    FrameCallback frameCallback_;
    AudioFrameCallback audioFrameCallback_;

    // Agora SDK objects
    std::unique_ptr<agora::base::IAgoraService, AgoraServiceDeleter> service_;
    agora::agora_refptr<agora::rtc::IRtcConnection> connection_;
    agora::agora_refptr<agora::rtc::IMediaNodeFactory> factory_;
    agora::agora_refptr<agora::rtc::IVideoMixerSource> videoMixer_;
    agora::agora_refptr<agora::rtc::ILocalVideoTrack> videoTrack_;
    agora::agora_refptr<YuvFrameObserver> frameObserver_;
    agora::agora_refptr<AudioFrameObserver> audioObserver_;

    std::mutex mutex_;
};

}  // namespace rtc
}  // namespace agora