
#include "sample_connection_observer.h"

#include "log.h"

void SampleConnectionObserver::onConnected(const agora::rtc::TConnectionInfo &connectionInfo,
										   agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason)
{
	AG_LOG(INFO, "onConnected: id %u, channelId %s, localUserId %s, reason %d\n", connectionInfo.id,
		   connectionInfo.channelId.get()->c_str(), connectionInfo.localUserId.get()->c_str(),
		   reason);

	// notify the thread which is waiting for the SDK to be connected
	connect_ready_.Set();
}

void SampleConnectionObserver::onDisconnected(const agora::rtc::TConnectionInfo &connectionInfo,
											  agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason)
{
	AG_LOG(INFO, "onDisconnected: id %u, channelId %s, localUserId %s, reason %d\n",
		   connectionInfo.id, connectionInfo.channelId.get()->c_str(),
		   connectionInfo.localUserId.get()->c_str(), reason);

	// notify the thread which is waiting for the SDK to be disconnected
	disconnect_ready_.Set();
}

void SampleConnectionObserver::onConnecting(const agora::rtc::TConnectionInfo &connectionInfo,
											agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason)
{
	AG_LOG(INFO, "onConnecting: id %u, channelId %s, localUserId %s, reason %d\n",
		   connectionInfo.id, connectionInfo.channelId.get()->c_str(),
		   connectionInfo.localUserId.get()->c_str(), reason);
}

void SampleConnectionObserver::onReconnecting(const agora::rtc::TConnectionInfo &connectionInfo,
											  agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason)
{
	AG_LOG(INFO, "onReconnecting: id %u, channelId %s, localUserId %s, reason %d\n",
		   connectionInfo.id, connectionInfo.channelId.get()->c_str(),
		   connectionInfo.localUserId.get()->c_str(), reason);
}

void SampleConnectionObserver::onReconnected(const agora::rtc::TConnectionInfo &connectionInfo,
											 agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason)
{
	AG_LOG(INFO, "onReconnected: id %u, channelId %s, localUserId %s, reason %d\n",
		   connectionInfo.id, connectionInfo.channelId.get()->c_str(),
		   connectionInfo.localUserId.get()->c_str(), reason);
}

void SampleConnectionObserver::onConnectionLost(const agora::rtc::TConnectionInfo &connectionInfo)
{
	AG_LOG(INFO, "onConnectionLost: id %u, channelId %s, localUserId %s\n", connectionInfo.id,
		   connectionInfo.channelId.get()->c_str(), connectionInfo.localUserId.get()->c_str());
}

void SampleConnectionObserver::onUplinkNetworkInfoUpdated(const agora::rtc::UplinkNetworkInfo &info)
{
	AG_LOG(INFO, "onBandwidthEstimationUpdated: video_encoder_target_bitrate_bps %d\n",
		   info.video_encoder_target_bitrate_bps);
}

void SampleConnectionObserver::onUserJoined(agora::user_id_t userId)
{
	AG_LOG(INFO, "onUserJoined: userId %s\n", userId);
}

void SampleConnectionObserver::onUserLeft(agora::user_id_t userId,
										  agora::rtc::USER_OFFLINE_REASON_TYPE reason)
{
	AG_LOG(INFO, "onUserLeft: userId %s, reason %d\n", userId, reason);
}
/************for rtmp*************************/
void RtmpConnectionObserver::onConnected(const agora::rtc::RtmpConnectionInfo &connectionInfo)
{
	AG_LOG(INFO, "onConnected: rtmp connect successfully.");
	connect_ready_.Set();
}
void RtmpConnectionObserver::onDisconnected(const agora::rtc::RtmpConnectionInfo &connectionInfo)
{
	AG_LOG(INFO, "onDisconnected:");
	disconnect_ready_.Set();
}
void RtmpConnectionObserver::onReconnecting(const agora::rtc::RtmpConnectionInfo &connectionInfo)
{
	AG_LOG(INFO, "onReconnecting:");
}
void RtmpConnectionObserver::onReconnected(const agora::rtc::RtmpConnectionInfo &connectionInfo)
{
	AG_LOG(INFO, "onReconnected:");
}
void RtmpConnectionObserver::onConnectionFailure(
		const agora::rtc::RtmpConnectionInfo &connectionInfo,
		agora::rtc::RTMP_CONNECTION_ERROR errCode)
{
	AG_LOG(INFO, "onConnectionFailure: errcode = %d", errCode);
}
void RtmpConnectionObserver::onTransferStatistics(uint64_t video_width, uint64_t video_height,
												  uint64_t video_bitrate, uint64_t audio_bitrate,
												  uint64_t video_frame_rate,
												  uint64_t push_video_frame_cnt,
												  uint64_t pop_video_frame_cnt)
{
	AG_LOG(INFO,
		   "video_bitrate: %lu, audio_bitrate: %lu, video_frame_rate: %lu, push_video_frame_cnt: %lu, pop_video_frame_cnt: %lu",
		   video_bitrate, audio_bitrate, video_frame_rate, push_video_frame_cnt,
		   pop_video_frame_cnt);
}