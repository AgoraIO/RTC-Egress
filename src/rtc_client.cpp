#define AG_LOG_TAG "RtcClient"

// rtc_client.cpp
#include "include/rtc_client.h"

#include <chrono>
#include <csignal>
#include <iomanip>
#include <memory>
#include <sstream>
#include <vector>

// Agora SDK includes
#include "AgoraRefCountedObject.h"
#include "IAgoraService.h"
#include "NGIAgoraAudioTrack.h"
#include "NGIAgoraLocalUser.h"
#include "NGIAgoraMediaNode.h"
#include "NGIAgoraMediaNodeFactory.h"
#include "NGIAgoraRtcConnection.h"
#include "NGIAgoraVideoMixerSource.h"
#include "NGIAgoraVideoTrack.h"
#include "common/helper.h"
#include "common/log.h"
#include "common/opt_parser.h"
#include "common/sample_common.h"
#include "common/sample_connection_observer.h"

namespace agora {
namespace rtc {

RtcClient::RtcClient() {}

RtcClient::~RtcClient() {
    teardown();
}

bool RtcClient::initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    if (initialized_) {
        AG_LOG(WARN, "RtcClient already initialized");
        return true;
    }

    config_ = config;

    // Create Agora service
    agora::base::IAgoraService* rawService = createAndInitAgoraService(false, true, true);
    if (!rawService) {
        AG_LOG(ERROR, "Failed to create Agora service");
        return false;
    }
    service_.reset(rawService);

    // Create media node factory
    factory_ = service_->createMediaNodeFactory();
    if (!factory_) {
        AG_LOG(ERROR, "Failed to create media node factory");
        return false;
    }

    // Create video mixer
    videoMixer_ = factory_->createVideoMixer();
    if (!videoMixer_) {
        AG_LOG(ERROR, "Failed to create video mixer");
        return false;
    }

    // Set up video mixer
    videoMixer_->setBackground(1920, 1080, 15);

    // Create video track
    videoTrack_ = service_->createMixedVideoTrack(videoMixer_);
    if (!videoTrack_) {
        AG_LOG(ERROR, "Failed to create video track");
        return false;
    }

    // Configure video encoder
    agora::rtc::VideoEncoderConfiguration encoderConfig;
    encoderConfig.codecType = agora::rtc::VIDEO_CODEC_H264;
    encoderConfig.dimensions.width = 1920;
    encoderConfig.dimensions.height = 1080;
    encoderConfig.frameRate = 15;
    videoTrack_->setVideoEncoderConfiguration(encoderConfig);

    initialized_ = true;
    return true;
}

bool RtcClient::connect() {
    std::lock_guard<std::mutex> lock(mutex_);

    if (!initialized_) {
        AG_LOG(ERROR, "RtcClient not initialized");
        return false;
    }

    if (connected_) {
        AG_LOG(WARN, "Already connected");
        return true;
    }

    // Create RTC connection
    agora::rtc::RtcConnectionConfiguration ccfg;
    ccfg.clientRoleType = config_.role;
    ccfg.autoSubscribeAudio = config_.autoSubscribeAudio;
    ccfg.autoSubscribeVideo = config_.autoSubscribeVideo;
    ccfg.enableAudioRecordingOrPlayout = config_.enableAudio;

    connection_ = service_->createRtcConnection(ccfg);
    if (!connection_) {
        AG_LOG(ERROR, "Failed to create RTC connection");
        return false;
    }

    // Create video subscription options
    agora::rtc::VideoSubscriptionOptions subscriptionOptions;
    subscriptionOptions.type =
        agora::rtc::VIDEO_STREAM_HIGH;  // Or VIDEO_STREAM_LOW for lower quality

    // Subscribe to remote streams
    if (!config_.remoteUserId.empty()) {
        // Subscribe to specific user's video
        connection_->getLocalUser()->subscribeVideo(config_.remoteUserId.c_str(),
                                                    subscriptionOptions);

        if (config_.enableAudio) {
            connection_->getLocalUser()->subscribeAudio(config_.remoteUserId.c_str());
        }
    } else {
        // Subscribe to all videos
        if (config_.enableVideo) {
            connection_->getLocalUser()->subscribeAllVideo(subscriptionOptions);
        }
        if (config_.enableAudio) {
            connection_->getLocalUser()->subscribeAllAudio();
        }
    }

    // Create and register frame observer to receive video frames
    if (videoFrameCallback_) {
        try {
            frameObserver_ = new YuvFrameObserver(videoFrameCallback_);
            if (!frameObserver_) {
                AG_LOG(ERROR, "Failed to create frame observer");
                return false;
            }

            frameObserver_->AddRef();  // Add reference for the observer

            // Register the observer with the local user to receive frames from remote users
            int ret = connection_->getLocalUser()->registerVideoFrameObserver(frameObserver_.get());
            if (ret != 0) {
                AG_LOG(ERROR, "Failed to register video frame observer, error: %d", ret);
                frameObserver_->Release();
                frameObserver_ = nullptr;
                return false;
            }
        } catch (const std::exception& e) {
            AG_LOG(ERROR, "Exception creating frame observer: %s", e.what());
            if (frameObserver_) {
                frameObserver_->Release();
                frameObserver_ = nullptr;
            }
            return false;
        }
    }

    // Create and register audio frame observer to receive audio frames
    AG_LOG(INFO, "Checking audio frame callback: %s", audioFrameCallback_ ? "SET" : "NOT SET");
    if (audioFrameCallback_) {
        AG_LOG(INFO, "Creating audio frame observer...");
        try {
            audioObserver_ =
                new AudioFrameObserver(audioFrameCallback_, config_.audioPlaybackSampleRate,
                                       config_.audioPlaybackChannels);
            if (!audioObserver_) {
                AG_LOG(ERROR, "Failed to create audio frame observer");
                return false;
            }

            audioObserver_->AddRef();  // Add reference for the observer

            // Register the audio observer with the local user to receive playback audio frames from
            // remote users for egress recording - use configurable parameters (default 16kHz mono,
            // can be configured to 48kHz stereo)
            int ret = connection_->getLocalUser()->setPlaybackAudioFrameBeforeMixingParameters(
                config_.audioPlaybackChannels, config_.audioPlaybackSampleRate);
            if (ret == 0) {
                ret = connection_->getLocalUser()->registerAudioFrameObserver(audioObserver_.get());
            }
            if (ret != 0) {
                AG_LOG(ERROR, "Failed to register audio frame observer with %dHz %dch, error: %d",
                       config_.audioPlaybackSampleRate, config_.audioPlaybackChannels, ret);
                audioObserver_->Release();
                audioObserver_ = nullptr;
                return false;
            }

            AG_LOG(INFO, "Audio frame observer registered successfully with %dHz %dch",
                   config_.audioPlaybackSampleRate, config_.audioPlaybackChannels);
        } catch (const std::exception& e) {
            AG_LOG(ERROR, "Exception creating audio frame observer: %s", e.what());
            if (audioObserver_) {
                audioObserver_->Release();
                audioObserver_ = nullptr;
            }
            return false;
        }
    }

    // Publish local video track if we're a broadcaster
    if (config_.role == agora::rtc::CLIENT_ROLE_BROADCASTER && videoTrack_) {
        connection_->getLocalUser()->publishVideo(videoTrack_);
    }

    // Connect to Agora channel
    if (connection_->connect(config_.accessToken.c_str(), config_.channel.c_str(),
                             config_.egressUid.c_str())) {
        AG_LOG(ERROR, "Failed to connect to channel via RTC connection");
        connection_ = nullptr;
        return false;
    }

    connected_ = true;
    AG_LOG(INFO, "Connected to channel %s", config_.channel.c_str());
    return true;
}

void RtcClient::disconnect() {
    std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
    if (!lock.owns_lock()) {
        AG_LOG(WARN, "Another thread is already in disconnect()");
        return;
    }

    if (!connected_) {
        return;
    }

    // Set up a timeout for the entire disconnect operation
    const auto timeout = std::chrono::seconds(5);
    auto start = std::chrono::steady_clock::now();

    auto check_timeout = [&]() -> bool {
        return std::chrono::steady_clock::now() - start < timeout;
    };

    try {
        // Clean up frame observers FIRST before touching connection
        if (frameObserver_ && connection_) {
            try {
                connection_->getLocalUser()->unregisterVideoFrameObserver(frameObserver_.get());
            } catch (const std::exception& e) {
                AG_LOG(ERROR, "Exception unregistering frame observer: %s", e.what());
            }
            frameObserver_->Release();  // Release our reference
            frameObserver_ = nullptr;
        }

        if (audioObserver_ && connection_) {
            try {
                connection_->getLocalUser()->unregisterAudioFrameObserver(audioObserver_.get());
            } catch (const std::exception& e) {
                AG_LOG(ERROR, "Exception unregistering audio frame observer: %s", e.what());
            }
            audioObserver_->Release();  // Release our reference
            audioObserver_ = nullptr;
        }

        if (connection_) {
            // Unpublish video track if we published one
            if (config_.role == agora::rtc::CLIENT_ROLE_BROADCASTER && videoTrack_) {
                try {
                    connection_->getLocalUser()->unpublishVideo(videoTrack_);
                } catch (const std::exception& e) {
                    AG_LOG(ERROR, "Exception unpublishing video: %s", e.what());
                }
            }

            // Disconnect from channel with timeout
            try {
                int ret = connection_->disconnect();
                if (ret != 0) {
                    AG_LOG(ERROR, "Failed to disconnect from channel, error: %d", ret);
                }

                // Wait for a moment to allow disconnection to complete
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

            } catch (const std::exception& e) {
                AG_LOG(ERROR, "Exception during disconnect: %s", e.what());
            }

            connection_ = nullptr;
        }

        // Keep core resources (service, factory, videoMixer, videoTrack) for reconnection
        // Only clean up channel-specific resources
        // Note: service_, factory_, videoMixer_, videoTrack_ are preserved for next connect()

        connected_ = false;

    } catch (const std::exception& e) {
        AG_LOG(ERROR, "Exception in disconnect(): %s", e.what());
        connected_ = false;
    }
}

void RtcClient::teardown() {
    std::lock_guard<std::mutex> lock(mutex_);

    // First disconnect from channel if connected
    if (connected_) {
        // Temporarily release the lock to call disconnect, then re-acquire
        mutex_.unlock();
        disconnect();
        mutex_.lock();
    }

    // Set up a timeout for the teardown operation
    const auto timeout = std::chrono::seconds(5);

    try {
        // Clean up remaining resources in reverse order of creation
        videoTrack_ = nullptr;
        videoMixer_ = nullptr;
        factory_ = nullptr;

        // Service cleanup last - this is where we might hang
        if (service_) {
            // Release service in a separate thread with a timeout
            std::promise<void> service_release_promise;
            std::future<void> service_release_future = service_release_promise.get_future();

            std::thread([&]() {
                try {
                    service_.reset();
                    service_release_promise.set_value();
                } catch (...) {
                    service_release_promise.set_exception(std::current_exception());
                }
            }).detach();

            // Wait for service release with timeout
            if (service_release_future.wait_for(timeout) != std::future_status::ready) {
                AG_LOG(ERROR, "Timeout waiting for service release in teardown");
            }
        }

        initialized_ = false;

    } catch (const std::exception& e) {
        AG_LOG(ERROR, "Exception in teardown(): %s", e.what());
        initialized_ = false;
    }
}

}  // namespace rtc
}  // namespace agora