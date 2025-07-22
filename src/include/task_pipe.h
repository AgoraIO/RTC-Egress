#pragma once
#include <unistd.h>  // For pipe()

#include <algorithm>  // For std::max
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "recording_sink.h"
#include "rtc_client.h"  // Include RtcClient definition
#include "snapshot_sink.h"
#include "uds_message.h"

namespace agora {
namespace egress {

class TaskPipe {
   public:
    using CommandHandler = std::function<void(const std::string& action, const UDSMessage& msg)>;

    TaskPipe(const std::string& socket_path, const std::string& instance_id = "");
    ~TaskPipe();

    void start(agora::rtc::RtcClient& rtc_client, agora::rtc::SnapshotSink& snapshot_sink,
               agora::rtc::RecordingSink& recording_sink);
    void stop();

    // Set configurations
    void setSnapshotConfig(const agora::rtc::SnapshotSink::Config& config);
    void setRecordingConfig(const agora::rtc::RecordingSink::Config& config);

    // Get the current reference count for a channel
    int getChannelRefCount(const std::string& channel) const;

   private:
    // Connection state for a channel
    struct ChannelState {
        int ref_count = 0;
        bool is_connected = false;
        bool is_snapshot_active = false;
        bool is_recording_active = false;
        int64_t snapshot_interval = 0;
    };

    // Command handlers
    void handleSnapshotCommand(const std::string& action, const UDSMessage& msg);
    void handleRecordingCommand(const std::string& action, const UDSMessage& msg);
    void handleRtmpCommand(const std::string& action, const UDSMessage& msg);
    void handleWhipCommand(const std::string& action, const UDSMessage& msg);

    // Connection management
    bool ensureConnected(const std::string& channel, const std::string& token = "");
    void releaseConnection(const std::string& channel);
    void cleanupConnection(const std::string& channel);

    // Thread function
    void thread_func();

    // Member variables
    std::unordered_map<std::string, CommandHandler> command_handlers_;
    std::unordered_map<std::string, ChannelState> channel_states_;
    mutable std::mutex state_mutex_;

    std::string socket_path_;
    std::string instance_id_;
    int sockfd_ = -1;
    int shutdown_pipe_[2] = {-1, -1};  // Pipe for shutdown notification
    std::thread thread_;
    bool running_ = false;

    // External dependencies
    agora::rtc::RtcClient* rtc_client_ = nullptr;
    agora::rtc::SnapshotSink* snapshot_sink_ = nullptr;
    agora::rtc::RecordingSink* recording_sink_ = nullptr;

    // Configurations
    agora::rtc::SnapshotSink::Config snapshot_config_;
    agora::rtc::RecordingSink::Config recording_config_;
};

}  // namespace egress
}  // namespace agora