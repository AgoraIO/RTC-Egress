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

// Forward declarations from other namespaces
namespace egress {
class TaskConnectionObserver;
}

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

    using VideoFrameCallback =
        std::function<void(const agora::media::base::VideoFrame&, const std::string& userId)>;
    using AudioFrameCallback = std::function<void(
        const agora::media::IAudioFrameObserverBase::AudioFrame&, const std::string& userId)>;
    using SdkErrorCallback =
        std::function<void(const std::string& errorType, const std::string& errorMessage)>;

    RtcClient();
    ~RtcClient();

    bool initialize(const Config& config);
    bool connect();
    void disconnect();
    void teardown();
    bool isConnected() const {
        return connected_;
    }

    void setVideoFrameCallback(VideoFrameCallback callback) {
        videoFrameCallback_ = std::move(callback);
    }

    void setAudioFrameCallback(AudioFrameCallback callback) {
        audioFrameCallback_ = std::move(callback);
    }

    void setSdkErrorCallback(SdkErrorCallback callback) {
        sdkErrorCallback_ = std::move(callback);
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
        explicit YuvFrameObserver(VideoFrameCallback callback) : callback_(std::move(callback)) {}
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
        VideoFrameCallback callback_;
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

                // Get user ID and directly forward the AudioFrame
                std::string userId = (uid == nullptr) ? "unknown" : std::string(uid);
                callback_(audioFrame, userId);
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
    VideoFrameCallback videoFrameCallback_;
    AudioFrameCallback audioFrameCallback_;
    SdkErrorCallback sdkErrorCallback_;

    // Agora SDK objects
    std::unique_ptr<agora::base::IAgoraService, AgoraServiceDeleter> service_;
    agora::agora_refptr<agora::rtc::IRtcConnection> connection_;
    agora::agora_refptr<agora::rtc::IMediaNodeFactory> factory_;
    agora::agora_refptr<agora::rtc::IVideoMixerSource> videoMixer_;
    agora::agora_refptr<agora::rtc::ILocalVideoTrack> videoTrack_;
    agora::agora_refptr<YuvFrameObserver> frameObserver_;
    agora::agora_refptr<AudioFrameObserver> audioObserver_;

    // Forward declaration for TaskConnectionObserver from agora::egress namespace
    std::unique_ptr<agora::egress::TaskConnectionObserver> connectionObserver_;

    std::mutex mutex_;
};

}  // namespace rtc
}  // namespace agora