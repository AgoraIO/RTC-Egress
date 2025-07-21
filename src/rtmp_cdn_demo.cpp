//  Agora RTC/MEDIA SDK
//
//  Created by ZZ in 2024-01.
//  Copyright (c) 2020 Agora.io. All rights reserved.
//

#include <atomic>
#include <condition_variable>
#include <csignal>
#include <cstring>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>

#include "AgoraRefCountedObject.h"
#include "IAgoraService.h"
#include "NGIAgoraAudioTrack.h"
#include "NGIAgoraLocalUser.h"
#include "NGIAgoraMediaNode.h"
#include "NGIAgoraMediaNodeFactory.h"
#include "NGIAgoraRtcConnection.h"
#include "NGIAgoraRtmpConnection.h"
#include "NGIAgoraRtmpLocalUser.h"
#include "NGIAgoraVideoTrack.h"
#include "common/helper.h"
#include "common/log.h"
#include "common/opt_parser.h"
#include "common/sample_common.h"
#include "common/sample_connection_observer.h"
#include "common/sample_local_user_observer.h"

#define DEFAULT_CONNECT_TIMEOUT_MS (3000)
#define DEFAULT_SAMPLE_RATE (16000)
#define DEFAULT_NUM_OF_CHANNELS (1)
#define DEFAULT_AUDIO_FILE "received_audio.pcm"
#define DEFAULT_VIDEO_FILE "received_video.yuv"
#define DEFAULT_FILE_LIMIT (100 * 1024 * 1024)
#define STREAM_TYPE_HIGH "high"
#define STREAM_TYPE_LOW "low"
#define DEFAULT_RTMP_URL "rtmp://10.82.0.193/live/test"
#define DEFAULT_TARGET_BITRATE (1 * 1000 * 1000)

struct audioFrame {
    std::atomic<int> i;
    uint8_t *buffer[2];
};

struct videoFrame {
    int width;
    int height;
    int rotation;
    std::atomic<int> i;
    uint8_t *buffer[2];
};

audioFrame common_audio;
videoFrame common_video;

struct SampleOptions {
    std::string appId;
    std::string channelId;
    std::string userId;
    std::string remoteUserId;
    std::string local_vos;
    std::string local_ap;
    std::string streamType = STREAM_TYPE_HIGH;
    std::string rtmpUrl = DEFAULT_RTMP_URL;
    int targetBitrate = DEFAULT_TARGET_BITRATE;

    struct {
        int sampleRate = DEFAULT_SAMPLE_RATE;
        int numOfChannels = DEFAULT_NUM_OF_CHANNELS;
    } audio;
};

class PcmFrameObserver : public agora::media::IAudioFrameObserverBase {
   public:
    bool onPlaybackAudioFrame(const char *channelId, AudioFrame &audioFrame) override {
        return true;
    };

    bool onRecordAudioFrame(const char *channelId, AudioFrame &audioFrame) override {
        return true;
    };

    bool onMixedAudioFrame(const char *channelId, AudioFrame &audioFrame) override {
        return true;
    };

    bool onPlaybackAudioFrameBeforeMixing(const char *channelId,
                                          agora::media::base::user_id_t userId,
                                          AudioFrame &audioFrame) override;

    bool onEarMonitoringAudioFrame(AudioFrame &audioFrame) override {
        return true;
    };

    AudioParams getEarMonitoringAudioParams() override {
        return AudioParams();
    };

    int getObservedAudioFramePosition() override {
        return 0;
    };

    AudioParams getPlaybackAudioParams() override {
        return AudioParams();
    };

    AudioParams getRecordAudioParams() override {
        return AudioParams();
    };

    AudioParams getMixedAudioParams() override {
        return AudioParams();
    };
};

class YuvFrameObserver : public agora::rtc::IVideoFrameObserver2 {
   public:
    void onFrame(const char *channelId, agora::user_id_t remoteUid,
                 const agora::media::base::VideoFrame *frame) override;

    virtual ~YuvFrameObserver() = default;
};

bool PcmFrameObserver::onPlaybackAudioFrameBeforeMixing(const char *channelId,
                                                        agora::media::base::user_id_t userId,
                                                        AudioFrame &audioFrame) {
    // Write PCM samples
    size_t writeBytes = audioFrame.samplesPerChannel * audioFrame.channels * sizeof(int16_t);

    if (!common_audio.buffer[0]) {
        common_audio.buffer[0] = (uint8_t *)malloc(writeBytes);
        common_audio.buffer[1] = (uint8_t *)malloc(writeBytes);
    }

    if (common_audio.i % 2 == 0) {
        memcpy(common_audio.buffer[0], audioFrame.buffer, writeBytes);
        common_audio.i++;
    } else {
        memcpy(common_audio.buffer[1], audioFrame.buffer, writeBytes);
        common_audio.i++;
    }
    return true;
}

void YuvFrameObserver::onFrame(const char *channelId, agora::user_id_t remoteUid,
                               const agora::media::base::VideoFrame *videoFrame) {
    // Write Y planar
    size_t one_frame_size = videoFrame->yStride * videoFrame->height +
                            videoFrame->uStride * videoFrame->height / 2 +
                            videoFrame->vStride * videoFrame->height / 2;

    if (!common_video.buffer[0]) {
        common_video.buffer[0] = (uint8_t *)malloc(one_frame_size);
        common_video.buffer[1] = (uint8_t *)malloc(one_frame_size);
        common_video.height = videoFrame->height;
        common_video.width = videoFrame->yStride;
        common_video.rotation = videoFrame->rotation;
    }

    if (common_video.i % 2 == 0) {
        size_t writeBytes = videoFrame->yStride * videoFrame->height;
        auto ptr = common_video.buffer[0];
        memcpy(ptr, videoFrame->yBuffer, writeBytes);
        ptr += writeBytes;
        writeBytes = videoFrame->uStride * videoFrame->height / 2;
        memcpy(ptr, videoFrame->uBuffer, writeBytes);
        ptr += writeBytes;
        writeBytes = videoFrame->vStride * videoFrame->height / 2;
        memcpy(ptr, videoFrame->vBuffer, writeBytes);
        common_video.i++;
    } else {
        size_t writeBytes = videoFrame->yStride * videoFrame->height;
        auto ptr = common_video.buffer[1];
        memcpy(ptr, videoFrame->yBuffer, writeBytes);
        ptr += writeBytes;
        writeBytes = videoFrame->uStride * videoFrame->height / 2;
        memcpy(ptr, videoFrame->uBuffer, writeBytes);
        ptr += writeBytes;
        writeBytes = videoFrame->vStride * videoFrame->height / 2;
        memcpy(ptr, videoFrame->vBuffer, writeBytes);
        common_video.i++;
    }

    return;
};

static void sendOnePcmFrame(
    const SampleOptions &options,
    agora::agora_refptr<agora::rtc::IAudioPcmDataSender> audioPcmDataSender) {
    // Calculate byte size for 10ms audio samples
    int sampleSize = sizeof(int16_t) * options.audio.numOfChannels;
    int samplesPer10ms = options.audio.sampleRate / 100;
    int sendBytes = sampleSize * samplesPer10ms;

    uint8_t frameBuf[sendBytes];

    if (!common_audio.buffer[0]) {
        return;
    }

    if (common_audio.i % 2 == 0) {
        memcpy(frameBuf, common_audio.buffer[1], sendBytes);
        common_audio.i++;
    } else {
        memcpy(frameBuf, common_audio.buffer[0], sendBytes);

        common_audio.i++;
    }

    if (audioPcmDataSender->sendAudioPcmData(
            frameBuf, 0, samplesPer10ms, agora::rtc::TWO_BYTES_PER_SAMPLE,
            options.audio.numOfChannels, options.audio.sampleRate) < 0) {
        AG_LOG(ERROR, "Failed to send audio frame!");
    }
}

static void sendOneYuvFrame(const SampleOptions &options,
                            agora::agora_refptr<agora::rtc::IVideoFrameSender> videoFrameSender) {
    if (!common_video.buffer[0]) {
        return;
    }

    // Calculate byte size for YUV420 image
    int sendBytes = common_video.width * common_video.height * 3 / 2;

    agora::media::base::ExternalVideoFrame videoFrame;
    videoFrame.type = agora::media::base::ExternalVideoFrame::VIDEO_BUFFER_RAW_DATA;
    videoFrame.format = agora::media::base::VIDEO_PIXEL_I420;
    //  videoFrame.buffer = frameBuf;
    videoFrame.stride = common_video.width;
    videoFrame.height = common_video.height;
    videoFrame.cropLeft = 0;
    videoFrame.cropTop = 0;
    videoFrame.cropRight = 0;
    videoFrame.cropBottom = 0;
    videoFrame.rotation = common_video.rotation;
    videoFrame.timestamp = 0;

    if (common_video.i % 2 == 0) {
        videoFrame.buffer = common_video.buffer[1];
    } else {
        videoFrame.buffer = common_video.buffer[0];
    }

    if (videoFrameSender->sendVideoFrame(videoFrame) < 0) {
        AG_LOG(ERROR, "Failed to send video frame!");
    }
}

static void SampleSendAudioTask(
    const SampleOptions &options,
    agora::agora_refptr<agora::rtc::IAudioPcmDataSender> audioPcmDataSender, bool &exitFlag) {
    // Currently only 10 ms PCM frame is supported. So PCM frames are sent at 10
    // ms interval
    PacerInfo pacer = {0, 10, 0, std::chrono::steady_clock::now()};

    while (!exitFlag) {
        sendOnePcmFrame(options, audioPcmDataSender);
        waitBeforeNextSend(pacer);  // sleep for a while before sending next frame
    }
}

static void SampleSendVideoTask(const SampleOptions &options,
                                agora::agora_refptr<agora::rtc::IVideoFrameSender> videoFrameSender,
                                bool &exitFlag) {
    // Calculate send interval based on frame rate. H264 frames are sent at this
    // interval
    PacerInfo pacer = {0, 25, 0, std::chrono::steady_clock::now()};

    while (!exitFlag) {
        sendOneYuvFrame(options, videoFrameSender);
        waitBeforeNextSend(pacer);  // sleep for a while before sending next frame
    }
}

static bool exitFlag = false;
static void SignalHandler(int sigNo) {
    exitFlag = true;
}

int join_rtc_and_receive(SampleOptions options, agora::base::IAgoraService *service) {
    // Create Agora connection
    agora::rtc::RtcConnectionConfiguration ccfg;
    ccfg.clientRoleType = agora::rtc::CLIENT_ROLE_AUDIENCE;
    ccfg.autoSubscribeAudio = false;
    ccfg.autoSubscribeVideo = false;
    ccfg.enableAudioRecordingOrPlayout = false;  // Subscribe audio but without playback
    agora::agora_refptr<agora::rtc::IRtcConnection> connection = service->createRtcConnection(ccfg);
    if (!connection) {
        AG_LOG(ERROR, "Failed to creating Agora connection!");
        return -1;
    }

    auto p = connection->getAgoraParameter();
    if (!options.local_vos.empty()) {
        p->setParameters("{\"rtc.force_local\":true}");
        p->setBool("rtc.enable_nasa2", false);
        p->setString("rtc.local_domain", options.local_ap.c_str());
        AG_LOG(INFO, "rtc.local_domain is %s", options.local_ap.c_str());

        //  p->setString("rtc.local_ap_list",options.local_vos_ip.c_str());

        p->setParameters(options.local_vos.c_str());
        AG_LOG(INFO, "rtc.local_ap_list is  %s", options.local_vos.c_str());
    }
    //  p->setParameters();

    // Subcribe streams from all remote users or specific remote user
    agora::rtc::VideoSubscriptionOptions subscriptionOptions;
    if (options.streamType == STREAM_TYPE_HIGH) {
        subscriptionOptions.type = agora::rtc::VIDEO_STREAM_HIGH;
    } else if (options.streamType == STREAM_TYPE_LOW) {
        subscriptionOptions.type = agora::rtc::VIDEO_STREAM_LOW;
    } else {
        AG_LOG(ERROR, "It is a error stream type");
        return -1;
    }
    if (options.remoteUserId.empty()) {
        AG_LOG(INFO, "Subscribe streams from all remote users");
        connection->getLocalUser()->subscribeAllAudio();
        connection->getLocalUser()->subscribeAllVideo(subscriptionOptions);
    } else {
        connection->getLocalUser()->subscribeAudio(options.remoteUserId.c_str());
        connection->getLocalUser()->subscribeVideo(options.remoteUserId.c_str(),
                                                   subscriptionOptions);
    }
    // Register connection observer to monitor connection event
    auto connObserver = std::make_shared<SampleConnectionObserver>();
    connection->registerObserver(connObserver.get());

    // Create local user observer
    auto localUserObserver = std::make_shared<SampleLocalUserObserver>(connection->getLocalUser());

    // Register audio frame observer to receive audio stream
    auto pcmFrameObserver = std::make_shared<PcmFrameObserver>();
    if (connection->getLocalUser()->setPlaybackAudioFrameBeforeMixingParameters(
            options.audio.numOfChannels, options.audio.sampleRate)) {
        AG_LOG(ERROR, "Failed to set audio frame parameters!");
        return -1;
    }
    localUserObserver->setAudioFrameObserver(pcmFrameObserver.get());

    // Register video frame observer to receive video stream
    std::shared_ptr<YuvFrameObserver> yuvFrameObserver = std::make_shared<YuvFrameObserver>();
    localUserObserver->setVideoFrameObserver(yuvFrameObserver.get());

    // Connect to Agora channel
    if (connection->connect(options.appId.c_str(), options.channelId.c_str(),
                            options.userId.c_str())) {
        AG_LOG(ERROR, "Failed to connect to Agora channel!");
        return -1;
    }

    // Start receiving incoming media data
    AG_LOG(INFO, "Start receiving audio & video data ...");

    // Periodically check exit flag
    while (!exitFlag) {
        usleep(10000);
    }

    // Unregister audio & video frame observers
    localUserObserver->unsetAudioFrameObserver();
    localUserObserver->unsetVideoFrameObserver();

    // Unregister connection observer
    connection->unregisterObserver(connObserver.get());

    // Disconnect from Agora channel
    if (connection->disconnect()) {
        AG_LOG(ERROR, "Failed to disconnect from Agora channel!");
        return -1;
    }
    AG_LOG(INFO, "Disconnected from Agora channel successfully");

    // Destroy Agora connection and related resources
    localUserObserver.reset();
    pcmFrameObserver.reset();
    yuvFrameObserver.reset();
    connection = nullptr;
}

int send_rtmp_to_cdn(SampleOptions options, agora::base::IAgoraService *service) {
    agora::rtc::RtmpConnectionConfiguration ccfg;
    ccfg.videoConfig.height = common_video.height;
    ccfg.videoConfig.width = common_video.width;

    agora::agora_refptr<agora::rtc::IRtmpConnection> connection =
        service->createRtmpConnection(ccfg);
    if (!connection) {
        AG_LOG(ERROR, "Failed to creating Agora connection!");
        return -1;
    }

    connection->getRtmpLocalUser()->setVideoEnabled(1);

    // Register connection observer to monitor connection event
    auto rtmpObserver = std::make_shared<RtmpConnectionObserver>();
    connection->registerObserver(rtmpObserver.get());

    // Connect to Agora channel
    if (connection->connect(options.rtmpUrl.c_str())) {
        AG_LOG(ERROR, "Failed to connect to Agora channel!");
        return -1;
    }

    // Wait until connected before sending media stream
    if (rtmpObserver->waitUntilConnected(DEFAULT_CONNECT_TIMEOUT_MS) < 0) {
        AG_LOG(INFO, "connection failed");
        return -1;
    }

    // Create media node factory
    agora::agora_refptr<agora::rtc::IMediaNodeFactory> factory = service->createMediaNodeFactory();
    if (!factory) {
        AG_LOG(ERROR, "Failed to create media node factory!");
    }

    // Create audio data sender
    agora::agora_refptr<agora::rtc::IAudioPcmDataSender> audioPcmDataSender =
        factory->createAudioPcmDataSender();
    if (!audioPcmDataSender) {
        AG_LOG(ERROR, "Failed to create audio data sender!");
        return -1;
    }

    // Create audio track
    agora::agora_refptr<agora::rtc::ILocalAudioTrack> customAudioTrack =
        service->createCustomAudioTrack(audioPcmDataSender);
    if (!customAudioTrack) {
        AG_LOG(ERROR, "Failed to create audio track!");
        return -1;
    }

    // Create video frame sender
    agora::agora_refptr<agora::rtc::IVideoFrameSender> videoFrameSender =
        factory->createVideoFrameSender();
    if (!videoFrameSender) {
        AG_LOG(ERROR, "Failed to create video frame sender!");
        return -1;
    }

    // Create video track
    agora::agora_refptr<agora::rtc::ILocalVideoTrack> customVideoTrack =
        service->createCustomVideoTrack(videoFrameSender);
    if (!customVideoTrack) {
        AG_LOG(ERROR, "Failed to create video track!");
        return -1;
    }

    // Configure video encoder
    agora::rtc::VideoEncoderConfiguration encoderConfig;
    encoderConfig.codecType = agora::rtc::VIDEO_CODEC_H264;
    encoderConfig.dimensions.width = common_video.width;
    encoderConfig.dimensions.height = common_video.height;
    encoderConfig.frameRate = 25;
    encoderConfig.bitrate = options.targetBitrate;

    customVideoTrack->setVideoEncoderConfiguration(encoderConfig);

    // Publish audio & video track
    customAudioTrack->setEnabled(true);
    connection->getRtmpLocalUser()->publishAudio(customAudioTrack);
    customVideoTrack->setEnabled(true);
    connection->getRtmpLocalUser()->publishVideo(customVideoTrack);

    // Start sending media data
    AG_LOG(INFO, "Start sending audio & video data ...");
    std::thread sendAudioThread(SampleSendAudioTask, options, audioPcmDataSender,
                                std::ref(exitFlag));
    std::thread sendVideoThread(SampleSendVideoTask, options, videoFrameSender, std::ref(exitFlag));

    sendAudioThread.join();
    sendVideoThread.join();

    // Unpublish audio & video track
    connection->getRtmpLocalUser()->unpublishAudio(customAudioTrack);
    connection->getRtmpLocalUser()->unpublishVideo(customVideoTrack);

    // Unregister connection observer
    connection->unregisterObserver(rtmpObserver.get());

    // Disconnect from Agora channel
    if (connection->disconnect()) {
        AG_LOG(ERROR, "Failed to disconnect from Agora channel!");
        return -1;
    }
    AG_LOG(INFO, "Disconnected from Agora channel successfully");

    // Destroy Agora connection and related resources
    rtmpObserver.reset();
    audioPcmDataSender = nullptr;
    videoFrameSender = nullptr;
    customAudioTrack = nullptr;
    customVideoTrack = nullptr;
    factory = nullptr;
    connection = nullptr;
}

int main(int argc, char *argv[]) {
    common_audio.buffer[0] = nullptr;
    common_audio.buffer[1] = nullptr;
    common_video.buffer[0] = nullptr;
    common_video.buffer[1] = nullptr;

    SampleOptions options;
    opt_parser optParser;

    optParser.add_long_opt("token", &options.appId, "The token for authentication");
    optParser.add_long_opt("channelId", &options.channelId, "Channel Id");
    optParser.add_long_opt("userId", &options.userId, "User Id / default is 0");
    optParser.add_long_opt("remoteUserId", &options.remoteUserId,
                           "The remote user to receive stream from");
    optParser.add_long_opt("sampleRate", &options.audio.sampleRate,
                           "Sample rate for received audio");
    optParser.add_long_opt("numOfChannels", &options.audio.numOfChannels,
                           "Number of channels for received audio");
    optParser.add_long_opt("streamtype", &options.streamType, "the stream type");
    optParser.add_long_opt("rtmpUrl", &options.rtmpUrl, "The rtmp cdn url");
    optParser.add_long_opt("localvos", &options.local_vos, "The local_vos_ip  ");
    optParser.add_long_opt("localap", &options.local_ap, "The local_ap  ");

    if ((argc <= 1) || !optParser.parse_opts(argc, argv)) {
        std::ostringstream strStream;
        optParser.print_usage(argv[0], strStream);
        std::cout << strStream.str() << std::endl;
        return -1;
    }

    if (options.appId.empty()) {
        AG_LOG(ERROR, "Must provide appId!");
        return -1;
    }

    if (options.channelId.empty()) {
        AG_LOG(ERROR, "Must provide channelId!");
        return -1;
    }

    std::signal(SIGQUIT, SignalHandler);
    std::signal(SIGABRT, SignalHandler);
    std::signal(SIGINT, SignalHandler);

    // Create Agora service
    auto service = createAndInitAgoraService(false, true, true);
    if (!service) {
        AG_LOG(ERROR, "Failed to creating Agora service!");
    }

    std::thread t1(join_rtc_and_receive, options, service);

    // join_rtc_and_receive(options,service);
    while (!common_video.buffer[0] && !common_audio.buffer[0] && !exitFlag) {
        usleep(500 * 1000);
    }
    AG_LOG(INFO, "Start send to CDN");
    std::thread t2(send_rtmp_to_cdn, options, service);

    // send_rtmp_to_cdn(options,service);
    t1.join();
    t2.join();

    // Destroy Agora Service
    service->release();
    service = nullptr;

    return 0;
}
