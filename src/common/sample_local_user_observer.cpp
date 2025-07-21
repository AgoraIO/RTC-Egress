//  Agora RTC/MEDIA SDK
//
//  Created by Pengfei Han in 2020-03.
//  Copyright (c) 2020 Agora.io. All rights reserved.
//

#include "sample_local_user_observer.h"

#include "log.h"

SampleLocalUserObserver::SampleLocalUserObserver(agora::rtc::IRtcConnection *connection)
		: connection_(connection)
{
	local_user_ = connection_->getLocalUser();
	local_user_->registerLocalUserObserver(this);
}

SampleLocalUserObserver::SampleLocalUserObserver(agora::rtc::ILocalUser *user) : local_user_(user)
{
	local_user_->registerLocalUserObserver(this);
}

SampleLocalUserObserver::~SampleLocalUserObserver()
{
	local_user_->unregisterLocalUserObserver(this);
}

agora::rtc::ILocalUser *SampleLocalUserObserver::GetLocalUser()
{
	return local_user_;
}

void SampleLocalUserObserver::PublishAudioTrack(
		agora::agora_refptr<agora::rtc::ILocalAudioTrack> audioTrack)
{
	local_user_->publishAudio(audioTrack);
}

void SampleLocalUserObserver::PublishVideoTrack(
		agora::agora_refptr<agora::rtc::ILocalVideoTrack> videoTrack)
{
	local_user_->publishVideo(videoTrack);
}

void SampleLocalUserObserver::UnpublishAudioTrack(
		agora::agora_refptr<agora::rtc::ILocalAudioTrack> audioTrack)
{
	local_user_->unpublishAudio(audioTrack);
}

void SampleLocalUserObserver::UnpublishVideoTrack(
		agora::agora_refptr<agora::rtc::ILocalVideoTrack> videoTrack)
{
	local_user_->unpublishVideo(videoTrack);
}



void SampleLocalUserObserver::onUserAudioTrackSubscribed(
		agora::user_id_t userId, agora::agora_refptr<agora::rtc::IRemoteAudioTrack> audioTrack)
{
	std::lock_guard<std::mutex> _(observer_lock_);
	remote_audio_track_ = audioTrack;
	if (remote_audio_track_ && audio_frame_observer_) {
		local_user_->registerAudioFrameObserver(audio_frame_observer_);
	}
}

void SampleLocalUserObserver::onUserVideoTrackSubscribed(
		agora::user_id_t userId, agora::rtc::VideoTrackInfo trackInfo,
		agora::agora_refptr<agora::rtc::IRemoteVideoTrack> videoTrack)
{
	AG_LOG(INFO, "onUserVideoTrackSubscribed: userId %s, codecType %d, encodedFrameOnly %d", userId,
		   trackInfo.codecType, trackInfo.encodedFrameOnly);
	std::lock_guard<std::mutex> _(observer_lock_);
	remote_video_track_ = videoTrack;
	if (remote_video_track_ && video_encoded_receiver_) {
		local_user_->registerVideoEncodedFrameObserver(video_encoded_receiver_);
	}
	if (remote_video_track_ && video_frame_observer_) {
		local_user_->registerVideoFrameObserver(video_frame_observer_);
	}
	if (remote_video_track_ && video_mixer_ && enable_video_mix_) {
		video_mixer_->addVideoTrack(userId, remote_video_track_);
		remote_video_track_map_[std::string(userId)] = remote_video_track_;
	}
}

void SampleLocalUserObserver::onFirstRemoteVideoDecoded(agora::user_id_t userId, int width,
														int height, int elapsed)
{
	/*  Note:   
	    before refresh(),we must set all-StreamLayout again 
	*/
	// agora::rtc::MixerLayoutConfig mixConfig;
	// mixConfig.height = height;
	// mixConfig.width = width;
	// mixConfig.zOrder = 1;
	// mixConfig.x = x_offset_;
	// mixConfig.y = y_offset_;
	// x_offset_ += 300;
	// y_offset_ += 300;
	if (video_mixer_ && enable_video_mix_) {
		remote_source_map_[std::string(userId)] = videoInfo(width, height,false);
		x_offset_ = 0;
		y_offset_ = 0;
		for (auto it = remote_source_map_.begin(); it != remote_source_map_.end(); it++) {
			video_mixer_->setStreamLayout(it->first.c_str(), calculate_layout(it->second.width,it->second.height));
		}
		video_mixer_->refresh();
	}
}

agora::rtc::MixerLayoutConfig SampleLocalUserObserver::calculate_layout(int width, int height)
{
	agora::rtc::MixerLayoutConfig mixConfig;
	mixConfig.height = height;
	mixConfig.width = width;
	mixConfig.zOrder = 1;
	mixConfig.x = x_offset_;
	mixConfig.y = y_offset_;
	// x_offset_ += 300;
	// y_offset_ += 300;
	if (x_offset_ + width < 1920) {
		x_offset_ += width;
		y_offset_ = y_offset_;
	} else {
		x_offset_ = width;
		y_offset_ += height;
	}
	AG_LOG(INFO, "the x_offset id %d y_offset is %d ", x_offset_, y_offset_);
	return mixConfig;
}

void SampleLocalUserObserver::onUserVideoTrackStateChanged(
    agora::user_id_t userId,
    agora::agora_refptr<agora::rtc::IRemoteVideoTrack> videoTrack,
    agora::rtc::REMOTE_VIDEO_STATE state,
    agora::rtc::REMOTE_VIDEO_STATE_REASON reason, int elapsed) {
   if(!(video_mixer_ && enable_video_mix_)) return;
  if (state == 0) {
	  if(remote_source_map_.find(std::string(userId))!=remote_source_map_.end())
    remote_source_map_[std::string(userId)].muted = true;
    x_offset_ = 0;
    y_offset_ = 0;
    for (auto it = remote_source_map_.begin(); it != remote_source_map_.end();
         it++) {
	if(!it->second.muted)
      video_mixer_->setStreamLayout(it->first.c_str(), calculate_layout(it->second.width,it->second.height));
    }
    video_mixer_->refresh();
   }else if(state == 1){
	  if(remote_source_map_.find(std::string(userId))!=remote_source_map_.end())
	   	remote_source_map_[std::string(userId)].muted = false;
    x_offset_ = 0;
    y_offset_ = 0;
    for (auto it = remote_source_map_.begin(); it != remote_source_map_.end();
         it++) {
	if(!it->second.muted)
      video_mixer_->setStreamLayout(it->first.c_str(), calculate_layout(it->second.width,it->second.height));
    }
    video_mixer_->refresh();
   }
}

void SampleLocalUserObserver::onUserInfoUpdated(agora::user_id_t userId,
												ILocalUserObserver::USER_MEDIA_INFO msg, bool val)
{
	AG_LOG(INFO, "onUserInfoUpdated: userId %s, msg %d, val %d", userId, msg, val);
	if (use_string_uid_ && !user_string_.empty()) {
		if (std::string(userId) == user_string_) {
			int r = local_user_->subscribeAudio(user_string_.c_str());
			agora::rtc::VideoSubscriptionOptions subscriptionOptions;
			subscriptionOptions.encodedFrameOnly = true;
			local_user_->subscribeVideo(user_string_.c_str(), subscriptionOptions);
			AG_LOG(INFO, "subscribe audio & video from %s \n", user_string_.c_str());
		}
	}
}

void SampleLocalUserObserver::onUserAudioTrackStateChanged(
		agora::user_id_t userId, agora::agora_refptr<agora::rtc::IRemoteAudioTrack> audioTrack,
		agora::rtc::REMOTE_AUDIO_STATE state, agora::rtc::REMOTE_AUDIO_STATE_REASON reason,
		int elapsed)
{
	AG_LOG(INFO, "onUserAudioTrackStateChanged: userId %s, state %d, reason %d", userId, state,
		   reason);
}

void SampleLocalUserObserver::onIntraRequestReceived()
{
	AG_LOG(INFO, "onIntraRequestReceived");
}
