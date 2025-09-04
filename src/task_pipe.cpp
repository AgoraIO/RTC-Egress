#define AG_LOG_TAG "TaskPipe"

#include "task_pipe.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <sstream>
#include <thread>

#include "common/log.h"
#include "include/recording_sink.h"
#include "include/uds_message.h"
#include "rtc_client.h"
#include "snapshot_sink.h"

namespace {

constexpr const char* kLogTag = "[TaskPipe]";

void logInfo(const std::string& message, const std::string& instance_id = "") {
    if (!instance_id.empty()) {
        AG_LOG_FAST(INFO, "[%s] %s", instance_id.c_str(), message.c_str());
    } else {
        AG_LOG_FAST(INFO, "%s", message.c_str());
    }
}

void logError(const std::string& message, const std::string& instance_id = "") {
    if (!instance_id.empty()) {
        AG_LOG_FAST(ERROR, "[%s] %s", instance_id.c_str(), message.c_str());
    } else {
        AG_LOG_FAST(ERROR, "%s", message.c_str());
    }
}

void logDebug(const std::string& message, const std::string& instance_id = "") {
    if (!instance_id.empty()) {
        AG_LOG_FAST(INFO, "[%s] [DEBUG] %s", instance_id.c_str(), message.c_str());
    } else {
        AG_LOG_FAST(INFO, "[DEBUG] %s", message.c_str());
    }
}

}  // namespace

namespace agora {
namespace egress {

TaskPipe::TaskPipe(const std::string& socket_path, const std::string& instance_id)
    : socket_path_(socket_path), instance_id_(instance_id) {
    // Create a pipe for shutdown notification
    if (pipe(shutdown_pipe_) == -1) {
        logError("Failed to create shutdown pipe: " + std::string(strerror(errno)), instance_id_);
    }

    // Register command handlers
    command_handlers_["snapshot"] =
        CommandHandler([this](const std::string& action, const UDSMessage& msg) {
            handleSnapshotCommand(action, msg);
        });
    command_handlers_["record"] =
        CommandHandler([this](const std::string& action, const UDSMessage& msg) {
            handleRecordingCommand(action, msg);
        });
    command_handlers_["rtmp"] =
        CommandHandler([this](const std::string& action, const UDSMessage& msg) {
            handleRtmpCommand(action, msg);
        });
    command_handlers_["whip"] =
        CommandHandler([this](const std::string& action, const UDSMessage& msg) {
            handleWhipCommand(action, msg);
        });
}

TaskPipe::~TaskPipe() {
    stop();
    if (shutdown_pipe_[0] != -1) {
        close(shutdown_pipe_[0]);
        shutdown_pipe_[0] = -1;
    }
    if (shutdown_pipe_[1] != -1) {
        close(shutdown_pipe_[1]);
        shutdown_pipe_[1] = -1;
    }
}

void TaskPipe::start(agora::rtc::RtcClient& rtc_client, agora::rtc::SnapshotSink& snapshot_sink,
                     agora::rtc::RecordingSink& recording_sink) {
    rtc_client_ = &rtc_client;
    snapshot_sink_ = &snapshot_sink;
    recording_sink_ = &recording_sink;

    // Set up SDK error callback to handle connection and token errors
    rtc_client_->setSdkErrorCallback(
        [this](const std::string& errorType, const std::string& errorMessage) {
            this->handleSdkError(errorType, errorMessage);
        });

    // Setup frame callbacks
    rtc_client_->setVideoFrameCallback([this](const agora::media::base::VideoFrame& frame,
                                              const std::string& userId) {
        // Check which tasks are active
        bool has_active_snapshot = hasActiveTasksOfType("snapshot");
        bool has_active_recording = hasActiveTasksOfType("record");

        // Only forward to snapshot sink if snapshots are active
        if (has_active_snapshot && snapshot_sink_) {
            snapshot_sink_->onVideoFrame(frame.yBuffer, frame.uBuffer, frame.vBuffer, frame.yStride,
                                         frame.uStride, frame.vStride, frame.width, frame.height,
                                         frame.renderTimeMs, userId);
        }

        // Only forward to recording sink if recording is active
        if (has_active_recording && recording_sink_) {
            recording_sink_->onVideoFrame(frame.yBuffer, frame.uBuffer, frame.vBuffer,
                                          frame.yStride, frame.uStride, frame.vStride, frame.width,
                                          frame.height, frame.renderTimeMs, userId);
        }
    });

    rtc_client_->setAudioFrameCallback(
        [this](const agora::media::IAudioFrameObserverBase::AudioFrame& frame,
               const std::string& userId) {
            // Only forward audio frames if there are active recording tasks
            bool has_active_recording = hasActiveTasksOfType("record");

            if (has_active_recording && recording_sink_) {
                recording_sink_->onAudioFrame(reinterpret_cast<const uint8_t*>(frame.buffer),
                                              frame.samplesPerChannel, frame.samplesPerSec,
                                              frame.channels, frame.renderTimeMs, userId);
            }
        });

    running_ = true;
    thread_ = std::thread(&TaskPipe::thread_func, this);
    logInfo("Started task pipe thread", instance_id_);
}

void TaskPipe::stop() {
    if (!running_) return;

    running_ = false;

    // Write to the shutdown pipe to wake up the select() call
    if (shutdown_pipe_[1] != -1) {
        char buf = 'x';
        ssize_t written = write(shutdown_pipe_[1], &buf, 1);
        if (written != 1) {
            logError("Failed to write to shutdown pipe: " + std::string(strerror(errno)),
                     instance_id_);
        }
    }

    if (thread_.joinable()) {
        thread_.join();
    }

    // Clean up all connections
    std::lock_guard<std::mutex> lock(state_mutex_);
    for (auto& [channel, state] : channel_states_) {
        if (state.is_connected) {
            // Check if any snapshot tasks are active
            bool has_snapshot_tasks = false;
            bool has_recording_tasks = false;
            for (const auto& [task_id, task_type] : state.active_tasks) {
                if (task_type == "snapshot") has_snapshot_tasks = true;
                if (task_type == "record") has_recording_tasks = true;
            }

            if (snapshot_sink_ && has_snapshot_tasks) {
                snapshot_sink_->stop();
            }
            if (recording_sink_ && has_recording_tasks) {
                recording_sink_->stop();
            }
            rtc_client_->disconnect();
        }
    }
    channel_states_.clear();

    logInfo("Stopped task pipe", instance_id_);
}

void TaskPipe::setSnapshotConfig(const agora::rtc::SnapshotSink::Config& config) {
    snapshot_config_ = config;
}

void TaskPipe::setRecordingConfig(const agora::rtc::RecordingSink::Config& config) {
    recording_config_ = config;
}

// Command Handlers
void TaskPipe::handleSnapshotCommand(const std::string& action, const UDSMessage& msg) {
    if (msg.channel.empty()) {
        logError("Channel name is required for snapshot command", instance_id_);
        return;
    }

    if (action == "start") {
        if (!ensureConnected(msg.channel, msg.access_token)) {
            logError("Failed to connect to channel: " + msg.channel, instance_id_);
            return;
        }

        // Update config with channel information
        snapshot_config_.channel = msg.channel;

        // Update channel state
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto& state = channel_states_[msg.channel];
            state.active_tasks[msg.task_id] = "snapshot";
            if (msg.interval_in_ms > 0) {
                snapshot_sink_->setIntervalInMs(msg.interval_in_ms);
                logInfo("Set snapshot interval to: " + std::to_string(msg.interval_in_ms) + "ms",
                        instance_id_);
            }
        }

        if (snapshot_sink_ && !snapshot_sink_->isCapturing()) {
            if (!snapshot_sink_->start()) {
                logError("Failed to start snapshot sink", instance_id_);
                releaseConnection(msg.channel, msg.task_id);
                return;
            }
        }

        logInfo("Started snapshot for channel: " + msg.channel, instance_id_);
    } else if (action == "stop") {
        // Stop the specific snapshot task but keep channel if other tasks exist
        std::string completed_task_id;
        std::string found_channel;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);

            // Search for the specific task_id across all channels
            if (!msg.task_id.empty()) {
                for (auto& [channel, state] : channel_states_) {
                    if (state.active_tasks.count(msg.task_id)) {
                        state.active_tasks.erase(msg.task_id);
                        completed_task_id = msg.task_id;
                        found_channel = channel;
                        break;
                    }
                }
            } else {
                // Fallback: if no task_id provided, use channel-based search (legacy behavior)
                auto it = channel_states_.find(msg.channel);
                if (it != channel_states_.end()) {
                    auto& state = it->second;
                    // Find first snapshot task
                    for (const auto& [task_id, task_type] : state.active_tasks) {
                        if (task_type == "snapshot") {
                            state.active_tasks.erase(task_id);
                            completed_task_id = task_id;
                            found_channel = msg.channel;
                            break;
                        }
                    }
                }
            }

            if (completed_task_id.empty() || found_channel.empty()) {
                logInfo("No matching snapshot task to stop (task_id: " + msg.task_id +
                            ", channel: " + msg.channel + ")",
                        instance_id_);
                return;
            }

            // Stop snapshot sink if no more snapshot tasks are active
            if (!hasActiveTasksOfType("snapshot", false)) {
                logInfo("Stopping snapshot for channel: " + found_channel, instance_id_);
                if (snapshot_sink_ && snapshot_sink_->isCapturing()) {
                    snapshot_sink_->stop();
                }
            }
        }

        // Release connection for this completed snapshot task
        releaseConnection(found_channel, completed_task_id);

        // Send completion message
        sendCompletionMessage(completed_task_id, "success", "",
                              "Snapshot task completed successfully");
        logInfo("Stopped snapshot for channel: " + msg.channel, instance_id_);
    } else if (action == "status") {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channel_states_.find(msg.channel);
        if (it != channel_states_.end()) {
            const auto& state = it->second;
            // Count snapshot tasks
            int snapshot_count = 0;
            for (const auto& [task_id, task_type] : state.active_tasks) {
                if (task_type == "snapshot") snapshot_count++;
            }
            logInfo("Status - Channel: " + msg.channel +
                        ", Tasks: " + std::to_string(state.active_tasks.size()) +
                        ", Snapshots: " + std::to_string(snapshot_count) + " active",
                    instance_id_);
        } else {
            logInfo("Status - Channel: " + msg.channel + " (not active)", instance_id_);
        }
    } else {
        logError("Unknown action for snapshot: " + action, instance_id_);
    }
}

void TaskPipe::handleRecordingCommand(const std::string& action, const UDSMessage& msg) {
    if (action == "start") {
        if (msg.channel.empty()) {
            logError("Channel is required for recording", instance_id_);
            return;
        }

        // Ensure we're connected to the channel
        if (!ensureConnected(msg.channel, msg.access_token)) {
            logError("Failed to connect to channel: " + msg.channel, instance_id_);
            return;
        }

        // Update configs with channel and task information
        recording_config_.channel = msg.channel;
        recording_config_.taskId = msg.task_id;

        std::lock_guard<std::mutex> lock(state_mutex_);
        auto& state = channel_states_[msg.channel];

        // Check if recording is already active (we allow multiple recording tasks)
        bool recording_active = false;
        for (const auto& [task_id, task_type] : state.active_tasks) {
            if (task_type == "record") {
                recording_active = true;
                break;
            }
        }

        // Add this recording task
        state.active_tasks[msg.task_id] = "record";

        // Start recording sink if not already running
        if (!recording_active && recording_sink_) {
            // Set up callback for max duration completion
            recording_sink_->setCompletionCallback([this](const std::string& taskId,
                                                          const std::string& status,
                                                          const std::string& message) {
                // Find the channel for this task and release connection
                // releaseConnection will handle removing the task from active_tasks
                std::string completed_channel;
                {
                    std::lock_guard<std::mutex> lock(state_mutex_);
                    for (auto& [channel, state] : channel_states_) {
                        auto task_it = state.active_tasks.find(taskId);
                        if (task_it != state.active_tasks.end()) {
                            completed_channel = channel;
                            break;
                        }
                    }
                }

                if (!completed_channel.empty()) {
                    releaseConnection(completed_channel, taskId);
                }

                sendCompletionMessage(taskId, status, "", message);
            });

            if (recording_sink_->initialize(recording_config_) && recording_sink_->start()) {
                logInfo("Started recording for channel: " + msg.channel + " to " +
                            recording_config_.outputDir,
                        instance_id_);
            } else {
                logError("Failed to start recording for channel: " + msg.channel, instance_id_);
                releaseConnection(msg.channel, msg.task_id);
                sendCompletionMessage(msg.task_id, "failed", "Failed to start recording sink");
                return;
            }
        } else {
            logInfo("Recording already active for channel: " + msg.channel +
                        ", added task: " + msg.task_id,
                    instance_id_);
        }
    } else if (action == "stop") {
        std::string completed_task_id;
        std::string found_channel;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);

            // Search for the specific task_id across all channels (don't remove yet)
            if (!msg.task_id.empty()) {
                for (auto& [channel, state] : channel_states_) {
                    if (state.active_tasks.count(msg.task_id)) {
                        completed_task_id = msg.task_id;
                        found_channel = channel;
                        break;
                    }
                }
            } else {
                // Fallback: if no task_id provided, use channel-based search (legacy behavior)
                auto it = channel_states_.find(msg.channel);
                if (it != channel_states_.end()) {
                    auto& state = it->second;
                    // Find first recording task
                    for (const auto& [task_id, task_type] : state.active_tasks) {
                        if (task_type == "record") {
                            completed_task_id = task_id;
                            found_channel = msg.channel;
                            break;
                        }
                    }
                }
            }

            if (completed_task_id.empty() || found_channel.empty()) {
                logInfo("No matching recording task to stop (task_id: " + msg.task_id +
                            ", channel: " + msg.channel + ")",
                        instance_id_);
                return;
            }

            // Stop recording sink if no more recording tasks are active
            if (!hasActiveTasksOfType("record", false)) {
                logInfo("Stopping recording for channel: " + found_channel, instance_id_);
                if (recording_sink_) {
                    recording_sink_->stop();
                }
            }
        }

        // Release the connection for this completed task
        releaseConnection(found_channel, completed_task_id);

        // Send completion message
        sendCompletionMessage(completed_task_id, "success", "",
                              "Recording task completed successfully");

        logInfo("Stopped recording for channel: " + found_channel, instance_id_);
    } else if (action == "status") {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channel_states_.find(msg.channel);
        if (it != channel_states_.end()) {
            const auto& state = it->second;
            // Count recording tasks
            int recording_count = 0;
            for (const auto& [task_id, task_type] : state.active_tasks) {
                if (task_type == "record") recording_count++;
            }
            logInfo("Recording Status - Channel: " + msg.channel +
                        ", Active: " + std::to_string(recording_count) + " tasks",
                    instance_id_);
        } else {
            logInfo("No recording active for channel: " + msg.channel, instance_id_);
        }
    } else {
        logError("Unknown action for recording: " + action, instance_id_);
    }
}

void TaskPipe::handleRtmpCommand(const std::string& action, const UDSMessage& msg) {
    logInfo("RTMP command received (not implemented): " + action, instance_id_);
    // TODO: Implement RTMP logic
}

void TaskPipe::handleWhipCommand(const std::string& action, const UDSMessage& msg) {
    logInfo("WHIP command received (not implemented): " + action, instance_id_);
    // TODO: Implement WHIP logic
}

// Connection Management
bool TaskPipe::ensureConnected(const std::string& channel, const std::string& token) {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto& state = channel_states_[channel];

    if (!state.is_connected) {
        logInfo("Connecting to channel: " + channel, instance_id_);

        rtc_client_->setChannel(channel);
        if (!token.empty()) {
            rtc_client_->setAccessToken(token);
        }

        if (!rtc_client_->connect()) {
            logError("Failed to connect to channel: " + channel, instance_id_);
            if (state.active_tasks.empty()) {
                channel_states_.erase(channel);
            }
            return false;
        }

        state.is_connected = true;
    }

    logDebug("Channel " + channel + " task count: " + std::to_string(state.active_tasks.size()),
             instance_id_);
    return true;
}

void TaskPipe::releaseConnection(const std::string& channel, const std::string& task_id) {
    bool should_cleanup = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channel_states_.find(channel);
        if (it == channel_states_.end()) {
            logError("No active connection found for channel: " + channel, instance_id_);
            return;
        }

        auto& state = it->second;

        // Remove the specific task (if it exists)
        auto task_it = state.active_tasks.find(task_id);
        if (task_it != state.active_tasks.end()) {
            state.active_tasks.erase(task_it);
            logDebug("Removed task " + task_id + " from channel " + channel, instance_id_);
        }

        // Check if channel should be cleaned up (no tasks remaining)
        if (state.active_tasks.empty()) {
            should_cleanup = true;
            // Don't erase the state yet - cleanupConnection needs it
        } else {
            logDebug(
                "Channel " + channel + " task count: " + std::to_string(state.active_tasks.size()),
                instance_id_);
        }
    }

    if (should_cleanup) {
        cleanupConnection(channel);
    }
}

void TaskPipe::cleanupConnection(const std::string& channel) {
    logInfo("Cleaning up connection for channel: " + channel, instance_id_);

    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channel_states_.find(channel);
    if (it == channel_states_.end()) {
        return;  // No state found, nothing to clean up
    }

    auto& state = it->second;

    // Check if any snapshot or recording tasks are active
    bool has_snapshot_tasks = false;
    bool has_recording_tasks = false;
    for (const auto& [task_id, task_type] : state.active_tasks) {
        if (task_type == "snapshot") has_snapshot_tasks = true;
        if (task_type == "record") has_recording_tasks = true;
    }

    // Stop any active snapshot
    if (has_snapshot_tasks && snapshot_sink_ && snapshot_sink_->isCapturing()) {
        logInfo("Stopping snapshot for channel: " + channel, instance_id_);
        snapshot_sink_->stop();
    }

    // Stop any active recording
    if (has_recording_tasks && recording_sink_) {
        logInfo("Stopping recording for channel: " + channel, instance_id_);
        recording_sink_->stop();
    }

    // Clear all active tasks for this channel
    state.active_tasks.clear();

    // Disconnect from the channel if we're still connected
    if (rtc_client_ && state.is_connected) {
        logInfo("Disconnecting from channel: " + channel, instance_id_);
        rtc_client_->disconnect();
        state.is_connected = false;
    }

    // Remove the channel state if no active tasks
    if (state.active_tasks.empty()) {
        channel_states_.erase(it);
    }

    logInfo("Cleanup completed for channel: " + channel, instance_id_);
}

// Thread Function
void TaskPipe::thread_func() {
    // Create a Unix domain socket as client
    sockfd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (sockfd_ < 0) {
        logError("Failed to create socket: " + std::string(strerror(errno)), instance_id_);
        return;
    }

    // Connect to the server socket (created by Go manager)
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    // Retry connection with exponential backoff
    int retry_count = 0;
    const int max_retries = 10;
    while (retry_count < max_retries && running_) {
        if (connect(sockfd_, (struct sockaddr*)&addr, sizeof(addr)) == 0) {
            break;
        }

        if (errno != ECONNREFUSED && errno != ENOENT) {
            logError("Failed to connect to socket: " + std::string(strerror(errno)), instance_id_);
            close(sockfd_);
            sockfd_ = -1;
            return;
        }

        // Wait before retrying
        retry_count++;
        int delay_ms = std::min(100 * (1 << retry_count), 2000);  // Cap at 2 seconds
        logInfo("Socket not ready, retrying in " + std::to_string(delay_ms) + "ms (attempt " +
                    std::to_string(retry_count) + "/" + std::to_string(max_retries) + ")",
                instance_id_);
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
    }

    if (retry_count >= max_retries) {
        logError("Failed to connect to socket after " + std::to_string(max_retries) + " attempts",
                 instance_id_);
        close(sockfd_);
        sockfd_ = -1;
        return;
    }

    logInfo("Connected to socket: " + socket_path_, instance_id_);

    // Main loop - read from connected socket
    while (running_) {
        struct timeval tv;
        tv.tv_sec = 1;
        tv.tv_usec = 0;

        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd_, &readfds);
        FD_SET(shutdown_pipe_[0], &readfds);

        int max_fd = std::max(sockfd_, shutdown_pipe_[0]) + 1;

        int ret = select(max_fd, &readfds, nullptr, nullptr, &tv);
        if (ret < 0) {
            if (errno == EINTR) continue;
            logError("select() failed: " + std::string(strerror(errno)), instance_id_);
            break;
        }

        // Check if we got a shutdown signal
        if (ret > 0 && FD_ISSET(shutdown_pipe_[0], &readfds)) {
            char buf[32];
            ssize_t n = read(shutdown_pipe_[0], buf, sizeof(buf));
            if (n <= 0) {
                logError("Failed to read from shutdown pipe: " + std::string(strerror(errno)),
                         instance_id_);
            }
            break;
        }

        if (ret > 0 && FD_ISSET(sockfd_, &readfds)) {
            // Read message from connected socket
            char buffer[4096];
            ssize_t n = read(sockfd_, buffer, sizeof(buffer) - 1);
            if (n > 0) {
                buffer[n] = '\0';

                // Handle multiple messages separated by newlines
                std::string data(buffer, n);
                std::stringstream ss(data);
                std::string line;

                while (std::getline(ss, line) && !line.empty()) {
                    try {
                        // Parse the message
                        nlohmann::json json = nlohmann::json::parse(line);

                        // Use the from_json function to parse the message
                        UDSMessage msg = json.get<UDSMessage>();

                        // Log received message for debugging
                        logInfo("Received UDS message - cmd: " + msg.cmd +
                                    ", action: " + msg.action + ", task_id: " + msg.task_id +
                                    ", channel: " + msg.channel,
                                instance_id_);

                        // Handle the command
                        auto it = command_handlers_.find(msg.cmd);
                        if (it != command_handlers_.end()) {
                            it->second(msg.action, msg);
                        } else {
                            logError("Unknown command: " + msg.cmd, instance_id_);
                        }
                    } catch (const std::exception& e) {
                        logError("Failed to parse message: " + std::string(e.what()) +
                                     " - Message: " + line,
                                 instance_id_);
                    }
                }
            } else if (n == 0) {
                logInfo("Socket connection closed by manager", instance_id_);
                break;
            } else {
                logError("Failed to read from socket: " + std::string(strerror(errno)),
                         instance_id_);
                break;
            }
        }
    }

    // Cleanup
    if (sockfd_ >= 0) {
        close(sockfd_);
        sockfd_ = -1;
    }

    logInfo("Task pipe thread exiting", instance_id_);
}

bool TaskPipe::hasActiveTasksOfType(const std::string& task_type, const bool withLock) const {
    if (withLock) {
        std::lock_guard<std::mutex> lock(state_mutex_);
    }

    for (const auto& [channel, state] : channel_states_) {
        for (const auto& [task_id, type] : state.active_tasks) {
            if (type == task_type) {
                return true;
            }
        }
    }
    return false;
}

void TaskPipe::sendCompletionMessage(const std::string& task_id, const std::string& status,
                                     const std::string& error, const std::string& message) {
    if (sockfd_ < 0) {
        logError("Cannot send completion message: socket not connected", instance_id_);
        return;
    }

    UDSCompletionMessage completion;
    completion.task_id = task_id;
    completion.status = status;
    completion.error = error;
    completion.message = message;

    try {
        nlohmann::json json;
        to_json(json, completion);
        std::string data = json.dump() + "\n";

        ssize_t written = write(sockfd_, data.c_str(), data.length());
        if (written != static_cast<ssize_t>(data.length())) {
            logError("Failed to send completion message for task " + task_id + ": " +
                         std::string(strerror(errno)),
                     instance_id_);
        } else {
            logInfo("Sent completion message for task " + task_id + " with status: " + status,
                    instance_id_);
        }
    } catch (const std::exception& e) {
        logError("Failed to serialize completion message for task " + task_id + ": " +
                     std::string(e.what()),
                 instance_id_);
    }
}

void TaskPipe::handleSdkError(const std::string& errorType, const std::string& errorMessage) {
    logError("SDK Error detected: " + errorType + " - " + errorMessage, instance_id_);

    // Stop all active sinks before clearing state
    bool had_active_recording = hasActiveTasksOfType("record");
    bool had_active_snapshot = hasActiveTasksOfType("snapshot");

    {
        // Find all active tasks and fail them due to SDK error
        std::lock_guard<std::mutex> lock(state_mutex_);

        std::vector<std::string> tasksToFail;
        for (const auto& [channel, state] : channel_states_) {
            for (const auto& [task_id, task_type] : state.active_tasks) {
                tasksToFail.push_back(task_id);
            }
        }

        // Send failure completion messages for all active tasks
        for (const std::string& task_id : tasksToFail) {
            std::string errorMsg = "SDK Error: " + errorType + " - " + errorMessage;
            sendCompletionMessage(task_id, "failed", errorMsg,
                                  "Task failed due to unrecoverable SDK error");
            logError("Failed task " + task_id + " due to SDK error: " + errorType, instance_id_);
        }

        // Clear all active tasks since SDK connection is broken
        channel_states_.clear();
    }

    logInfo("Checking active tasks: recording=" + std::to_string(had_active_recording) +
                ", snapshot=" + std::to_string(had_active_snapshot),
            instance_id_);

    // Stop sinks if they were active
    if (had_active_recording && recording_sink_) {
        logInfo("Stopping recording sink due to SDK error", instance_id_);
        recording_sink_->stop();
        logInfo("Recording sink stopped successfully", instance_id_);
    } else {
        logInfo("Recording sink not stopped: had_active=" + std::to_string(had_active_recording) +
                    ", sink_exists=" + std::to_string(recording_sink_ != nullptr),
                instance_id_);
    }

    if (had_active_snapshot && snapshot_sink_) {
        logInfo("Stopping snapshot sink due to SDK error", instance_id_);
        snapshot_sink_->stop();
        logInfo("Snapshot sink stopped successfully", instance_id_);
    }

    logInfo("All active tasks failed due to SDK error: " + errorType, instance_id_);
}

}  // namespace egress
}  // namespace agora