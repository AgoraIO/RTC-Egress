#define AG_LOG_TAG "TaskConnectionObserver"

#include "include/task_connection_observer.h"

#include <sstream>

#include "common/log.h"

namespace agora {
namespace egress {

void TaskConnectionObserver::onConnected(const agora::rtc::TConnectionInfo& connectionInfo,
                                         agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) {
    AG_LOG_FAST(INFO, "RTC Connected: channel=%s, uid=%s, reason=%d",
                connectionInfo.channelId.get()->c_str(), connectionInfo.localUserId.get()->c_str(),
                reason);
}

void TaskConnectionObserver::onDisconnected(const agora::rtc::TConnectionInfo& connectionInfo,
                                            agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) {
    AG_LOG_FAST(WARN, "RTC Disconnected: channel=%s, uid=%s, reason=%d (%s)",
                connectionInfo.channelId.get()->c_str(), connectionInfo.localUserId.get()->c_str(),
                reason, getReasonDescription(reason).c_str());

    // Check if this is an unrecoverable disconnection
    if (isUnrecoverableDisconnection(reason) && errorCallback_) {
        std::stringstream errorMsg;
        errorMsg << "RTC connection lost with unrecoverable reason: "
                 << getReasonDescription(reason) << " (code: " << reason << ")";

        errorCallback_("connection_failure", errorMsg.str());
    }
}

void TaskConnectionObserver::onConnecting(const agora::rtc::TConnectionInfo& connectionInfo,
                                          agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) {
    AG_LOG_FAST(INFO, "RTC Connecting: channel=%s, uid=%s, reason=%d",
                connectionInfo.channelId.get()->c_str(), connectionInfo.localUserId.get()->c_str(),
                reason);
}

void TaskConnectionObserver::onReconnecting(const agora::rtc::TConnectionInfo& connectionInfo,
                                            agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) {
    AG_LOG_FAST(INFO, "RTC Reconnecting: channel=%s, uid=%s, reason=%d",
                connectionInfo.channelId.get()->c_str(), connectionInfo.localUserId.get()->c_str(),
                reason);
}

void TaskConnectionObserver::onReconnected(const agora::rtc::TConnectionInfo& connectionInfo,
                                           agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) {
    AG_LOG_FAST(INFO, "RTC Reconnected: channel=%s, uid=%s, reason=%d",
                connectionInfo.channelId.get()->c_str(), connectionInfo.localUserId.get()->c_str(),
                reason);
}

void TaskConnectionObserver::onConnectionLost(const agora::rtc::TConnectionInfo& connectionInfo) {
    AG_LOG_FAST(ERROR, "RTC Connection Lost: channel=%s, uid=%s",
                connectionInfo.channelId.get()->c_str(), connectionInfo.localUserId.get()->c_str());

    // Connection lost is usually unrecoverable in egress scenarios
    if (errorCallback_) {
        std::stringstream errorMsg;
        errorMsg << "RTC connection lost unexpectedly for channel "
                 << connectionInfo.channelId.get()->c_str();

        errorCallback_("connection_lost", errorMsg.str());
    }
}

void TaskConnectionObserver::onConnectionFailure(
    const agora::rtc::TConnectionInfo& connectionInfo,
    agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) {
    AG_LOG_FAST(ERROR, "RTC Connection Failure: channel=%s, uid=%s, reason=%d (%s)",
                connectionInfo.channelId.get()->c_str(), connectionInfo.localUserId.get()->c_str(),
                reason, getReasonDescription(reason).c_str());

    // All connection failures are considered unrecoverable
    if (errorCallback_) {
        std::stringstream errorMsg;
        errorMsg << "RTC connection failed: " << getReasonDescription(reason)
                 << " (code: " << reason << ")";

        errorCallback_("connection_failure", errorMsg.str());
    }
}

void TaskConnectionObserver::onTokenPrivilegeWillExpire(const char* token) {
    AG_LOG_FAST(WARN, "RTC Token privilege will expire soon: token=%s", token ? token : "null");

    // Token expiry warning - this is recoverable if we refresh the token
    // For now, we don't treat this as fatal, but we could implement token refresh
}

void TaskConnectionObserver::onTokenPrivilegeDidExpire() {
    AG_LOG_FAST(ERROR, "RTC Token privilege expired");

    // Token expired is unrecoverable without a new token
    if (errorCallback_) {
        errorCallback_("access_token_expired", "Access token has expired and is no longer valid");
    }
}

std::string TaskConnectionObserver::getReasonDescription(
    agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) {
    switch (reason) {
        case agora::rtc::CONNECTION_CHANGED_CONNECTING:
            return "Connecting";
        case agora::rtc::CONNECTION_CHANGED_JOIN_SUCCESS:
            return "Join Success";
        case agora::rtc::CONNECTION_CHANGED_INTERRUPTED:
            return "Interrupted";
        case agora::rtc::CONNECTION_CHANGED_BANNED_BY_SERVER:
            return "Banned by Server";
        case agora::rtc::CONNECTION_CHANGED_JOIN_FAILED:
            return "Join Failed";
        case agora::rtc::CONNECTION_CHANGED_LEAVE_CHANNEL:
            return "Leave Channel";
        case agora::rtc::CONNECTION_CHANGED_INVALID_APP_ID:
            return "Invalid App ID";
        case agora::rtc::CONNECTION_CHANGED_INVALID_CHANNEL_NAME:
            return "Invalid Channel Name";
        case agora::rtc::CONNECTION_CHANGED_INVALID_TOKEN:
            return "Invalid Token";
        case agora::rtc::CONNECTION_CHANGED_TOKEN_EXPIRED:
            return "Token Expired";
        case agora::rtc::CONNECTION_CHANGED_REJECTED_BY_SERVER:
            return "Rejected by Server";
        case agora::rtc::CONNECTION_CHANGED_SETTING_PROXY_SERVER:
            return "Setting Proxy Server";
        case agora::rtc::CONNECTION_CHANGED_RENEW_TOKEN:
            return "Renew Token";
        case agora::rtc::CONNECTION_CHANGED_CLIENT_IP_ADDRESS_CHANGED:
            return "Client IP Address Changed";
        case agora::rtc::CONNECTION_CHANGED_KEEP_ALIVE_TIMEOUT:
            return "Keep Alive Timeout";
        case agora::rtc::CONNECTION_CHANGED_REJOIN_SUCCESS:
            return "Rejoin Success";
        case agora::rtc::CONNECTION_CHANGED_LOST:
            return "Lost";
        case agora::rtc::CONNECTION_CHANGED_ECHO_TEST:
            return "Echo Test";
        case agora::rtc::CONNECTION_CHANGED_CLIENT_IP_ADDRESS_CHANGED_BY_USER:
            return "Client IP Address Changed by User";
        case agora::rtc::CONNECTION_CHANGED_SAME_UID_LOGIN:
            return "Same UID Login";
        case agora::rtc::CONNECTION_CHANGED_TOO_MANY_BROADCASTERS:
            return "Too Many Broadcasters";
        default:
            return "Unknown Reason";
    }
}

bool TaskConnectionObserver::isUnrecoverableDisconnection(
    agora::rtc::CONNECTION_CHANGED_REASON_TYPE reason) {
    // These reasons indicate unrecoverable errors that require task failure
    switch (reason) {
        case agora::rtc::CONNECTION_CHANGED_BANNED_BY_SERVER:
        case agora::rtc::CONNECTION_CHANGED_JOIN_FAILED:
        case agora::rtc::CONNECTION_CHANGED_INVALID_APP_ID:
        case agora::rtc::CONNECTION_CHANGED_INVALID_CHANNEL_NAME:
        case agora::rtc::CONNECTION_CHANGED_INVALID_TOKEN:
        case agora::rtc::CONNECTION_CHANGED_TOKEN_EXPIRED:
        case agora::rtc::CONNECTION_CHANGED_REJECTED_BY_SERVER:
        case agora::rtc::CONNECTION_CHANGED_SAME_UID_LOGIN:
        case agora::rtc::CONNECTION_CHANGED_TOO_MANY_BROADCASTERS:
            return true;

        // These reasons might be recoverable with retry
        case agora::rtc::CONNECTION_CHANGED_INTERRUPTED:
        case agora::rtc::CONNECTION_CHANGED_KEEP_ALIVE_TIMEOUT:
        case agora::rtc::CONNECTION_CHANGED_LOST:
        case agora::rtc::CONNECTION_CHANGED_CLIENT_IP_ADDRESS_CHANGED:
        case agora::rtc::CONNECTION_CHANGED_CLIENT_IP_ADDRESS_CHANGED_BY_USER:
            return false;

        // Successful reasons
        case agora::rtc::CONNECTION_CHANGED_CONNECTING:
        case agora::rtc::CONNECTION_CHANGED_JOIN_SUCCESS:
        case agora::rtc::CONNECTION_CHANGED_LEAVE_CHANNEL:
        case agora::rtc::CONNECTION_CHANGED_REJOIN_SUCCESS:
        case agora::rtc::CONNECTION_CHANGED_RENEW_TOKEN:
        case agora::rtc::CONNECTION_CHANGED_SETTING_PROXY_SERVER:
        case agora::rtc::CONNECTION_CHANGED_ECHO_TEST:
            return false;

        default:
            // Unknown reasons are treated as potentially unrecoverable
            return true;
    }
}

}  // namespace egress
}  // namespace agora