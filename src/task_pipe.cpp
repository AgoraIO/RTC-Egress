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

#include "include/recording_sink.h"
#include "include/uds_message.h"
#include "rtc_client.h"
#include "snapshot_sink.h"

namespace {

constexpr const char* kLogTag = "[TaskPipe]";

void logInfo(const std::string& message, const std::string& instance_id = "") {
    if (!instance_id.empty()) {
        std::cout << kLogTag << "[" << instance_id << "] " << message << std::endl;
    } else {
        std::cout << kLogTag << " " << message << std::endl;
    }
}

void logError(const std::string& message, const std::string& instance_id = "") {
    if (!instance_id.empty()) {
        std::cerr << kLogTag << "[" << instance_id << "] [ERROR] " << message << std::endl;
    } else {
        std::cerr << kLogTag << " [ERROR] " << message << std::endl;
    }
}

void logDebug(const std::string& message, const std::string& instance_id = "") {
    if (!instance_id.empty()) {
        std::cout << kLogTag << "[" << instance_id << "] [DEBUG] " << message << std::endl;
    } else {
        std::cout << kLogTag << " [DEBUG] " << message << std::endl;
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

    // Setup frame callbacks
    rtc_client_->setVideoFrameCallback([this](const agora::media::base::VideoFrame& frame,
                                              const std::string& userId) {
        // Check which tasks are active
        bool has_active_snapshot = false;
        bool has_active_recording = false;
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            for (const auto& [channel, state] : channel_states_) {
                if (state.is_snapshot_active) {
                    has_active_snapshot = true;
                }
                if (state.is_recording_active) {
                    has_active_recording = true;
                }
                if (has_active_snapshot && has_active_recording) {
                    break;  // Found both, no need to continue
                }
            }
        }

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
        [this](const agora::media::base::AudioPcmFrame& frame, const std::string& userId) {
            // Only forward audio frames if there are active recording tasks
            bool has_active_recording = false;
            {
                std::lock_guard<std::mutex> lock(state_mutex_);
                for (const auto& [channel, state] : channel_states_) {
                    if (state.is_recording_active) {
                        has_active_recording = true;
                        break;
                    }
                }
            }

            if (has_active_recording && recording_sink_) {
                recording_sink_->onAudioFrame(reinterpret_cast<const uint8_t*>(frame.data_),
                                              frame.samples_per_channel_, frame.sample_rate_hz_,
                                              frame.num_channels_, frame.capture_timestamp, userId);
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
            if (snapshot_sink_ && state.is_snapshot_active) {
                snapshot_sink_->stop();
            }
            if (recording_sink_ && state.is_recording_active) {
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

int TaskPipe::getChannelRefCount(const std::string& channel) const {
    std::lock_guard<std::mutex> lock(state_mutex_);
    auto it = channel_states_.find(channel);
    return it != channel_states_.end() ? it->second.ref_count : 0;
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

        // Update channel state
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto& state = channel_states_[msg.channel];
            state.is_snapshot_active = true;
            if (msg.interval_in_ms > 0) {
                state.snapshot_interval = msg.interval_in_ms;
                snapshot_sink_->setIntervalInMs(msg.interval_in_ms);
                logInfo("Set snapshot interval to: " + std::to_string(msg.interval_in_ms) + "ms",
                        instance_id_);
            }
        }

        if (snapshot_sink_ && !snapshot_sink_->isCapturing()) {
            if (!snapshot_sink_->start()) {
                logError("Failed to start snapshot sink", instance_id_);
                releaseConnection(msg.channel);
                return;
            }
        }

        logInfo("Started snapshot for channel: " + msg.channel, instance_id_);
    } else if (action == "release") {
        // Stop the specific snapshot task but keep channel if other tasks exist
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = channel_states_.find(msg.channel);
            if (it != channel_states_.end()) {
                auto& state = it->second;
                if (state.is_snapshot_active) {
                    logInfo("Stopping snapshot for channel: " + msg.channel, instance_id_);
                    if (snapshot_sink_ && snapshot_sink_->isCapturing()) {
                        snapshot_sink_->stop();
                    }
                    state.is_snapshot_active = false;
                }
            }
        }
        // Use ref counting to determine if we should disconnect from channel
        releaseConnection(msg.channel);
        logInfo("Released snapshot for channel: " + msg.channel, instance_id_);
    } else if (action == "status") {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channel_states_.find(msg.channel);
        if (it != channel_states_.end()) {
            const auto& state = it->second;
            logInfo("Status - Channel: " + msg.channel +
                        ", Refs: " + std::to_string(state.ref_count) +
                        ", Snapshots: " + (state.is_snapshot_active ? "active" : "inactive") +
                        ", Interval: " + std::to_string(state.snapshot_interval) + "ms",
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

        std::lock_guard<std::mutex> lock(state_mutex_);
        auto& state = channel_states_[msg.channel];

        if (state.is_recording_active) {
            logInfo("Recording is already active for channel: " + msg.channel, instance_id_);
            return;
        }

        // Configure recording
        state.recording_file_path = msg.recording_file;
        state.recording_width = msg.recording_width;
        state.recording_height = msg.recording_height;
        state.recording_fps = msg.recording_fps > 0 ? msg.recording_fps : 30;

        // Start recording with proper config
        if (recording_sink_) {
            agora::rtc::RecordingSink::Config config = recording_config_;
            config.outputDir = state.recording_file_path;
            config.videoWidth = state.recording_width;
            config.videoHeight = state.recording_height;
            config.videoFps = state.recording_fps;

            if (recording_sink_->initialize(config) && recording_sink_->start()) {
                state.is_recording_active = true;
                logInfo("Started recording for channel: " + msg.channel + " to " +
                            state.recording_file_path,
                        instance_id_);
            } else {
                logError("Failed to start recording for channel: " + msg.channel, instance_id_);
                releaseConnection(msg.channel);
            }
        }
    } else if (action == "stop") {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channel_states_.find(msg.channel);
        if (it == channel_states_.end()) {
            logError("No active recording found for channel: " + msg.channel, instance_id_);
            return;
        }

        auto& state = it->second;
        if (!state.is_recording_active) {
            logInfo("No active recording to stop for channel: " + msg.channel, instance_id_);
            return;
        }

        // Stop recording
        if (recording_sink_) {
            recording_sink_->stop();
        }

        state.is_recording_active = false;
        logInfo(
            "Stopped recording for channel: " + msg.channel + " at " + state.recording_file_path,
            instance_id_);

        // Release the connection if no other tasks are using it
        releaseConnection(msg.channel);
    } else if (action == "release") {
        // Stop the specific recording task but keep channel if other tasks exist
        {
            std::lock_guard<std::mutex> lock(state_mutex_);
            auto it = channel_states_.find(msg.channel);
            if (it != channel_states_.end()) {
                auto& state = it->second;
                if (state.is_recording_active) {
                    logInfo("Stopping recording for channel: " + msg.channel, instance_id_);
                    if (recording_sink_) {
                        recording_sink_->stop();
                    }
                    state.is_recording_active = false;
                }
            }
        }
        // Use ref counting to determine if we should disconnect from channel
        releaseConnection(msg.channel);
        logInfo("Released recording for channel: " + msg.channel, instance_id_);
    } else if (action == "status") {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channel_states_.find(msg.channel);
        if (it != channel_states_.end()) {
            const auto& state = it->second;
            logInfo("Recording Status - Channel: " + msg.channel +
                        ", Active: " + (state.is_recording_active ? "yes" : "no") + ", File: " +
                        (state.is_recording_active ? state.recording_file_path : "none"),
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
    state.ref_count++;

    if (!state.is_connected) {
        logInfo("Connecting to channel: " + channel, instance_id_);

        rtc_client_->setChannel(channel);
        if (!token.empty()) {
            rtc_client_->setAccessToken(token);
        }

        if (!rtc_client_->connect()) {
            logError("Failed to connect to channel: " + channel, instance_id_);
            if (--state.ref_count <= 0) {
                channel_states_.erase(channel);
            }
            return false;
        }

        state.is_connected = true;
    }

    logDebug("Channel " + channel + " reference count: " + std::to_string(state.ref_count),
             instance_id_);
    return true;
}

void TaskPipe::releaseConnection(const std::string& channel) {
    bool should_cleanup = false;
    {
        std::lock_guard<std::mutex> lock(state_mutex_);
        auto it = channel_states_.find(channel);
        if (it == channel_states_.end()) {
            logError("No active connection found for channel: " + channel, instance_id_);
            return;
        }

        auto& state = it->second;
        if (--state.ref_count <= 0) {
            should_cleanup = true;
            // Don't erase the state yet - cleanupConnection needs it
        } else {
            logDebug("Channel " + channel + " reference count: " + std::to_string(state.ref_count),
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

    // Stop any active snapshot
    if (state.is_snapshot_active && snapshot_sink_ && snapshot_sink_->isCapturing()) {
        logInfo("Stopping snapshot for channel: " + channel, instance_id_);
        snapshot_sink_->stop();
        state.is_snapshot_active = false;
    }

    // Stop any active recording
    if (state.is_recording_active && recording_sink_) {
        logInfo("Stopping recording for channel: " + channel, instance_id_);
        recording_sink_->stop();
        state.is_recording_active = false;
    }

    // Disconnect from the channel if we're still connected
    if (rtc_client_ && state.is_connected) {
        logInfo("Disconnecting from channel: " + channel, instance_id_);
        rtc_client_->disconnect();
        state.is_connected = false;
    }

    // Remove the channel state if no active tasks
    if (state.ref_count <= 0) {
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

                        UDSMessage msg;
                        msg.cmd = json.value("cmd", "");
                        msg.action = json.value("action", "");
                        msg.channel = json.value("channel", "");
                        msg.access_token = json.value("access_token", "");
                        msg.interval_in_ms = json.value("interval_in_ms", 0);

                        // Parse recording configuration if present
                        if (json.contains("recording_file")) {
                            msg.recording_file = json.value("recording_file", "");
                        }
                        if (json.contains("recording_width")) {
                            msg.recording_width = json.value("recording_width", 0);
                        }
                        if (json.contains("recording_height")) {
                            msg.recording_height = json.value("recording_height", 0);
                        }
                        if (json.contains("recording_fps")) {
                            msg.recording_fps = json.value("recording_fps", 30);
                        }

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

}  // namespace egress
}  // namespace agora