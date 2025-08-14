#pragma once

#include <functional>
#include <string>

#include "NGIAgoraRtcConnection.h"

namespace agora {
namespace egress {

// Error callback function type - called when unrecoverable SDK errors occur
using SdkErrorCallback =
    std::function<void(const std::string& errorType, const std::string& errorMessage)>;

class TaskConnectionObserver : public agora::rtc::IRtcConnectionObserver {
   public:
    explicit TaskConnectionObserver(SdkErrorCallback errorCallback)
        : errorCallback_(std::move(errorCallback)) {}

    virtual ~TaskConnectionObserver() = default;

    // IRtcConnectionObserver interface - detect unrecoverable errors
    void onConnected(const agora::rtc::TConnectionInfo& connectionInfo,
                     agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) override;

    void onDisconnected(const agora::rtc::TConnectionInfo& connectionInfo,
                        agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) override;

    void onConnecting(const agora::rtc::TConnectionInfo& connectionInfo,
                      agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) override;

    void onReconnecting(const agora::rtc::TConnectionInfo& connectionInfo,
                        agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) override;

    void onReconnected(const agora::rtc::TConnectionInfo& connectionInfo,
                       agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) override;

    void onConnectionLost(const agora::rtc::TConnectionInfo& connectionInfo) override;

    void onConnectionFailure(const agora::rtc::TConnectionInfo& connectionInfo,
                             agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) override;

    // Token expiry detection - these are unrecoverable errors
    void onTokenPrivilegeWillExpire(const char* token) override;
    void onTokenPrivilegeDidExpire() override;

    // Other required methods (not used for error detection)
    void onLastmileQuality(const agora::rtc::QUALITY_TYPE quality) override {}
    void onUserJoined(agora::user_id_t userId) override {}
    void onUserLeft(agora::user_id_t userId, agora::rtc::USER_OFFLINE_REASON_TYPE reason) override {
    }
    void onTransportStats(const agora::rtc::RtcStats& stats) override {}
    void onLastmileProbeResult(const agora::rtc::LastmileProbeResult& result) override {}
    void onChannelMediaRelayStateChanged(int state, int code) override {}

   private:
    SdkErrorCallback errorCallback_;

    // Helper function to get reason type description
    std::string getReasonDescription(agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason);

    // Helper function to determine if a disconnection reason is unrecoverable
    bool isUnrecoverableDisconnection(agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason);
};

}  // namespace egress
}  // namespace agora