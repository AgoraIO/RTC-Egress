//  Agora RTC/MEDIA SDK
//
//  Created by Pengfei Han in 2020-03.
//  Copyright (c) 2020 Agora.io. All rights reserved.
//

#pragma once

#include <map>
#include <mutex>

#include "AgoraBase.h"
#include "NGIAgoraLocalUser.h"
#include "NGIAgoraAudioTrack.h"

#include "NGIAgoraMediaNodeFactory.h"
#include "NGIAgoraMediaNode.h"
#include "NGIAgoraVideoTrack.h"
#include "NGIAgoraRtcConnection.h"
#include "NGIAgoraVideoMixerSource.h"


struct videoInfo{
  int width;
  int height;
  bool muted;
  videoInfo():width(0),height(0),muted(true) {};
  videoInfo(int w ,int h ,bool mute):width(w),height(h),muted(mute) {};
};

class SampleLocalUserObserver : public agora::rtc::ILocalUserObserver {
 public:
  SampleLocalUserObserver(agora::rtc::IRtcConnection* connection);
  SampleLocalUserObserver(agora::rtc::ILocalUser* user);
  virtual ~SampleLocalUserObserver();

 public:
  agora::rtc::ILocalUser* GetLocalUser();
  void PublishAudioTrack(agora::agora_refptr<agora::rtc::ILocalAudioTrack> audioTrack);
  void PublishVideoTrack(agora::agora_refptr<agora::rtc::ILocalVideoTrack> videoTrack);
  void UnpublishAudioTrack(agora::agora_refptr<agora::rtc::ILocalAudioTrack> audioTrack);
  void UnpublishVideoTrack(agora::agora_refptr<agora::rtc::ILocalVideoTrack> videoTrack);
  agora::agora_refptr<agora::rtc::IRemoteAudioTrack> GetRemoteAudioTrack() {
    return remote_audio_track_;
  }
  agora::agora_refptr<agora::rtc::IRemoteVideoTrack> GetRemoteVideoTrack() {
    return remote_video_track_;
  }

  void setVideoEncodedImageReceiver(agora::media::IVideoEncodedFrameObserver* receiver) {
    video_encoded_receiver_ = receiver;
  }

  void setAudioFrameObserver(agora::media::IAudioFrameObserverBase* observer) {
    audio_frame_observer_ = observer;
  }
  
  void setSubscribeUserString(std::string username){
      user_string_ = username;
  }
  void unsetAudioFrameObserver() {
    if (audio_frame_observer_) {
      local_user_->unregisterAudioFrameObserver(audio_frame_observer_);
    }
  }

  void setVideoFrameObserver( agora::rtc::IVideoFrameObserver2* observer) {
    video_frame_observer_ = observer;
  }

  void setUseStringUid(bool used){
     use_string_uid_ = used;
  }

  void unsetVideoFrameObserver() {
    if (remote_video_track_ && video_frame_observer_) {
      local_user_->unregisterVideoFrameObserver(video_frame_observer_);
    }
  }

  void setEnableVideoMix(bool enable){
       enable_video_mix_ = enable;
  }

void onStreamMessage(agora::user_id_t userId, int streamId, const char* data, size_t length) {
        printf("the message is %s \n",data);
    }

  void setVideoMixer( agora::agora_refptr<agora::rtc::IVideoMixerSource> video_mixer){
       video_mixer_ = video_mixer;
  }

 public:
  // inherit from agora::rtc::ILocalUserObserver
  void onAudioTrackPublishSuccess(
      agora::agora_refptr<agora::rtc::ILocalAudioTrack> audioTrack) override {}

  void onUserAudioTrackSubscribed(
      agora::user_id_t userId,
      agora::agora_refptr<agora::rtc::IRemoteAudioTrack> audioTrack) override;

  void onAudioTrackPublicationFailure(agora::agora_refptr<agora::rtc::ILocalAudioTrack> audioTrack,
                                      agora::ERROR_CODE_TYPE error) override {}

  void onUserAudioTrackStateChanged(agora::user_id_t userId,
                                    agora::agora_refptr<agora::rtc::IRemoteAudioTrack> audioTrack,
                                    agora::rtc::REMOTE_AUDIO_STATE state,
                                    agora::rtc::REMOTE_AUDIO_STATE_REASON reason,
                                    int elapsed) override;

  void onVideoTrackPublishSuccess(agora::agora_refptr<agora::rtc::ILocalVideoTrack> videoTrack) override {}

  void onVideoTrackPublicationFailure(agora::agora_refptr<agora::rtc::ILocalVideoTrack> videoTrack,
                                      agora::ERROR_CODE_TYPE error) override {}

  void onUserVideoTrackSubscribed(
      agora::user_id_t userId, agora::rtc::VideoTrackInfo trackInfo,
      agora::agora_refptr<agora::rtc::IRemoteVideoTrack> videoTrack) override;

  void onUserVideoTrackStateChanged(agora::user_id_t userId,
                                    agora::agora_refptr<agora::rtc::IRemoteVideoTrack> videoTrack,
                                    agora::rtc::REMOTE_VIDEO_STATE state,
                                    agora::rtc::REMOTE_VIDEO_STATE_REASON reason,
                                    int elapsed) ;

  void onRemoteVideoTrackStatistics(agora::agora_refptr<agora::rtc::IRemoteVideoTrack> videoTrack,
                                    const agora::rtc::RemoteVideoTrackStats& stats) override {}

  void onLocalVideoTrackStateChanged(agora::agora_refptr<agora::rtc::ILocalVideoTrack> videoTrack,
                                     agora::rtc::LOCAL_VIDEO_STREAM_STATE state,
                                     agora::rtc::LOCAL_VIDEO_STREAM_ERROR errorCode) override {}

  void onLocalVideoTrackStatistics(agora::agora_refptr<agora::rtc::ILocalVideoTrack> videoTrack,
                                   const agora::rtc::LocalVideoTrackStats& stats) override {}

  void onAudioVolumeIndication(const agora::rtc::AudioVolumeInformation* speakers,
                               unsigned int speakerNumber, int totalVolume) override {}

  void onLocalAudioTrackStatistics(const agora::rtc::LocalAudioStats& stats) override {}

  void onRemoteAudioTrackStatistics(agora::agora_refptr<agora::rtc::IRemoteAudioTrack> audioTrack,
                                    const agora::rtc::RemoteAudioTrackStats& stats) override {}
                                  
  //void onRemoteVideoStreamInfoUpdated(const agora::rtc::RemoteVideoStreamInfo& info) override {}
  

  void onUserInfoUpdated(agora::user_id_t userId, USER_MEDIA_INFO msg, bool val) override;

  void onIntraRequestReceived() override;

  void onAudioSubscribeStateChanged(const char* channel, agora::user_id_t uid,
                                    agora::rtc::STREAM_SUBSCRIBE_STATE oldState,
                                    agora::rtc::STREAM_SUBSCRIBE_STATE newState,
                                    int elapseSinceLastState) override {}

  void onVideoSubscribeStateChanged(const char* channel, agora::user_id_t uid,
                                    agora::rtc::STREAM_SUBSCRIBE_STATE oldState,
                                    agora::rtc::STREAM_SUBSCRIBE_STATE newState,
                                    int elapseSinceLastState) override {}

  void onAudioPublishStateChanged(const char* channel, agora::rtc::STREAM_PUBLISH_STATE oldState,
                                  agora::rtc::STREAM_PUBLISH_STATE newState,
                                  int elapseSinceLastState) override {}

  void onVideoPublishStateChanged(const char* channel, agora::rtc::STREAM_PUBLISH_STATE oldState,
                                  agora::rtc::STREAM_PUBLISH_STATE newState,
                                  int elapseSinceLastState) override {}
  void onFirstRemoteVideoFrameRendered(agora::user_id_t userId, int width,
                                               int height, int elapsed){}
  void onFirstRemoteVideoFrame(agora::user_id_t userId, int width, int height, int elapsed){}
  void onFirstRemoteAudioFrame(agora::user_id_t userId, int elapsed) {}
  void onFirstRemoteAudioDecoded(agora::user_id_t userId, int elapsed) {}
  void onFirstRemoteVideoDecoded(agora::user_id_t userId, int width, int height, int elapsed) override;

  // virtual void onFirstRemoteAudioDecoded(agora::user_id_t userId, int elapsed){}
  void onAudioTrackPublishStart(agora::agora_refptr<agora::rtc::ILocalAudioTrack> audioTrack) override {}

  void onAudioTrackUnpublished(agora::agora_refptr<agora::rtc::ILocalAudioTrack> audioTrack) override {}

  void onVideoTrackPublishStart(agora::agora_refptr<agora::rtc::ILocalVideoTrack> videoTrack) override {}

  void onVideoTrackUnpublished(agora::agora_refptr<agora::rtc::ILocalVideoTrack> videoTrack)override {}

  void onVideoSizeChanged(agora::user_id_t userId, int width, int height, int rotation) {}
  void onActiveSpeaker(agora::user_id_t userId){}
 private:

  agora::rtc::MixerLayoutConfig calculate_layout(int width,int height);

  agora::rtc::IRtcConnection* connection_{nullptr};
  agora::rtc::ILocalUser* local_user_{nullptr};

  agora::agora_refptr<agora::rtc::IRemoteAudioTrack> remote_audio_track_;
  agora::agora_refptr<agora::rtc::IRemoteVideoTrack> remote_video_track_;

  agora::agora_refptr<agora::rtc::IVideoMixerSource> video_mixer_{nullptr};

  agora::media::IVideoEncodedFrameObserver* video_encoded_receiver_{nullptr};
  agora::media::IAudioFrameObserverBase* audio_frame_observer_{nullptr};
  agora::rtc::IVideoFrameObserver2* video_frame_observer_{nullptr};

  std::map<std::string ,agora::agora_refptr<agora::rtc::IRemoteVideoTrack>> remote_video_track_map_;
  std::map<std::string ,videoInfo> remote_source_map_;


  std::mutex observer_lock_;
  bool use_string_uid_{false};
  std::string  user_string_; 

  bool enable_video_mix_{false};
  int x_offset_{0};
  int y_offset_{0};
};
