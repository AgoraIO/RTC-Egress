#define AG_LOG_TAG "Main"

#include <sys/socket.h>
#include <sys/stat.h>  // For mkdir
#include <sys/un.h>
#include <unistd.h>
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>  // For getenv
#include <filesystem>
#include <iomanip>  // For std::put_time
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "common/log.h"
#include "common/opt_parser.h"
#include "common/sample_common.h"
#include "include/recording_sink.h"
#include "include/rtc_client.h"
#include "include/snapshot_sink.h"
#include "include/task_pipe.h"
#include "nlohmann/json.hpp"

namespace fs = std::filesystem;

// Global pointers for cleanup
std::unique_ptr<agora::rtc::RtcClient> g_rtcClient;
std::unique_ptr<agora::rtc::SnapshotSink> g_snapshotSink;
std::unique_ptr<agora::rtc::RecordingSink> g_recordingSink;

// Global flag for controlling the main loop
std::atomic<bool> g_running{true};
std::atomic<int> g_signal_status{0};
std::mutex g_shutdown_mutex;
std::condition_variable g_shutdown_cv;

// Signal handler for graceful shutdown
void signal_handler(int signal) {
    static std::mutex signal_mutex;
    std::lock_guard<std::mutex> lock(signal_mutex);

    if (!g_running) {
        // Already shutting down, ignore additional signals
        AG_LOG_FAST(INFO, "\nSignal %d received but already shutting down, ignoring...", signal);
        return;
    }

    AG_LOG_FAST(INFO, "\nReceived signal %d, shutting down...", signal);
    g_signal_status = signal;
    g_running = false;
    AG_LOG_FAST(INFO, "Set g_running to false, notifying all waiting threads...");
    g_shutdown_cv.notify_all();  // Wake up any waiting threads
    AG_LOG_FAST(INFO, "Signal handler completed");
}

int main(int argc, char* argv[]) {
    std::string config_file;
    std::string socket_path;
    bool show_help = false;
    opt_parser parser;
    parser.add_long_opt("config", &config_file, "Path to config file", opt_parser::require_argu);
    parser.add_long_opt("sock", &socket_path, "Path to unix domain socket",
                        opt_parser::require_argu);
    parser.add_long_opt("help", &show_help, "Show this help message", opt_parser::no_argu);

    if (!parser.parse_opts(argc, argv) || show_help || config_file.empty()) {
        parser.print_usage(argv[0], std::cerr);
        return 1;
    }

    try {
        // If --sock is provided, only listen for tasks from manager
        if (!socket_path.empty()) {
            // Minimal config just for RTC client and snapshot sink construction
            YAML::Node config = YAML::LoadFile(config_file);

            // Configure Agora RTC SDK log path if specified in config
            if (config["log"] && config["log"]["path"]) {
                std::string logPath = config["log"]["path"].as<std::string>();
                if (!logPath.empty()) {
                    // Ensure log directory exists
                    if (!fs::exists(logPath)) {
                        fs::create_directories(logPath);
                    }
                    // Set RTC log file path with process ID
                    std::string rtcLogFile =
                        logPath + "/rtc_sdk_" + std::to_string(getpid()) + ".log";
                    setRTCLogPath(rtcLogFile.c_str());
                    AG_LOG_FAST(INFO, "Set Agora RTC log path to: %s", rtcLogFile.c_str());
                }
            }

            agora::rtc::RtcClient::Config rtc_config;
            if (config["app_id"]) rtc_config.appId = config["app_id"].as<std::string>();
            // Enable video subscription for socket mode
            rtc_config.enableVideo = true;         // Enable video subscription
            rtc_config.autoSubscribeVideo = true;  // Auto-subscribe to video streams
            rtc_config.autoSubscribeAudio = true;  // Auto-subscribe to audio streams

            // Configure audio playback parameters from recording.audio (defaults to 16kHz mono)
            if (config["recording"] && config["recording"]["audio"]) {
                const auto& audio_config = config["recording"]["audio"];
                if (audio_config["sample_rate"]) {
                    rtc_config.audioPlaybackSampleRate = audio_config["sample_rate"].as<int>();
                }
                if (audio_config["channels"]) {
                    rtc_config.audioPlaybackChannels = audio_config["channels"].as<int>();
                }
                // Update samples per call based on sample rate
                rtc_config.audioPlaybackSamplesPerCall = rtc_config.audioPlaybackSampleRate / 100;
            }

            g_rtcClient = std::make_unique<agora::rtc::RtcClient>();
            g_rtcClient->initialize(rtc_config);

            // Initialize snapshot sink with config
            agora::rtc::SnapshotSink::Config snapshots_config;
            g_snapshotSink = std::make_unique<agora::rtc::SnapshotSink>();

            // Apply snapshot configuration
            if (config["snapshots"]) {
                const auto& snapshots_node = config["snapshots"];
                if (snapshots_node["output_dir"]) {
                    snapshots_config.outputDir = snapshots_node["output_dir"].as<std::string>();
                }
                if (snapshots_node["width"]) {
                    snapshots_config.width = snapshots_node["width"].as<int>();
                }
                if (snapshots_node["height"]) {
                    snapshots_config.height = snapshots_node["height"].as<int>();
                }
                if (snapshots_node["interval_in_ms"]) {
                    snapshots_config.intervalInMs = snapshots_node["interval_in_ms"].as<int>();
                }
                if (snapshots_node["quality"]) {
                    snapshots_config.quality = snapshots_node["quality"].as<int>();
                }
            }

            g_snapshotSink->initialize(snapshots_config);

            // Initialize recording sink for socket mode
            g_recordingSink = std::make_unique<agora::rtc::RecordingSink>();
            agora::rtc::RecordingSink::Config recording_config;
            if (config["recording"]) {
                // Apply basic recording config for socket mode - full config will be sent via
                // socket
                if (config["recording"]["output_dir"]) {
                    recording_config.outputDir =
                        config["recording"]["output_dir"].as<std::string>();
                }
                g_recordingSink->initialize(recording_config);
            }

            // Extract worker ID from socket path for instanceId (e.g., "worker_0" from
            // "/tmp/agora/eg_worker_0.sock")
            std::string instance_id = "worker";
            size_t worker_pos = socket_path.find("eg_worker_");
            if (worker_pos != std::string::npos) {
                size_t start = worker_pos + 10;  // Length of "eg_worker_"
                size_t end = socket_path.find(".sock", start);
                if (end != std::string::npos) {
                    instance_id = "worker_" + socket_path.substr(start, end - start);
                }
            }

            // Set up signal handlers for socket mode as well
            struct sigaction sa;
            sa.sa_handler = signal_handler;
            sigemptyset(&sa.sa_mask);
            sa.sa_flags = 0;

            if (sigaction(SIGINT, &sa, NULL) == -1) {
                AG_LOG_FAST(ERROR, "Failed to set up SIGINT handler in socket mode");
                return 1;
            }
            if (sigaction(SIGTERM, &sa, NULL) == -1) {
                AG_LOG_FAST(ERROR, "Failed to set up SIGTERM handler in socket mode");
                return 1;
            }

            AG_LOG_FAST(INFO, "Signal handlers set up for socket mode");

            std::unique_ptr<agora::egress::TaskPipe> task_pipe =
                std::make_unique<agora::egress::TaskPipe>(socket_path, instance_id);
            task_pipe->setSnapshotConfig(snapshots_config);
            if (g_recordingSink) {
                task_pipe->setRecordingConfig(recording_config);
                task_pipe->start(*g_rtcClient, *g_snapshotSink, *g_recordingSink);
            } else {
                // Create a dummy recording sink if none provided
                agora::rtc::RecordingSink dummy_recording_sink;
                agora::rtc::RecordingSink::Config dummy_config;
                task_pipe->setRecordingConfig(dummy_config);
                task_pipe->start(*g_rtcClient, *g_snapshotSink, dummy_recording_sink);
            }

            // Wait until shutdown
            while (g_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            // Explicitly clean up resources
            AG_LOG_FAST(INFO, "Socket mode cleanup starting...");
            if (g_recordingSink) {
                AG_LOG_FAST(INFO, "Stopping recording sink in socket mode...");
                g_recordingSink->stop();
                AG_LOG_FAST(INFO, "Recording sink stopped in socket mode");
                g_recordingSink.reset();
                AG_LOG_FAST(INFO, "Recording sink reset in socket mode");
            }

            if (g_snapshotSink) {
                AG_LOG_FAST(INFO, "Stopping snapshot sink in socket mode...");
                g_snapshotSink->stop();
                AG_LOG_FAST(INFO, "Snapshot sink stopped in socket mode");
                g_snapshotSink.reset();
                AG_LOG_FAST(INFO, "Snapshot sink reset in socket mode");
            }

            // Cleanup
            AG_LOG_FAST(INFO, "Stopping task pipe...");
            task_pipe->stop();
            AG_LOG_FAST(INFO, "Task pipe stopped");
            task_pipe.reset();
            AG_LOG_FAST(INFO, "Task pipe reset");

            if (g_rtcClient) {
                AG_LOG_FAST(INFO, "Disconnecting RTC client in socket mode...");
                g_rtcClient->disconnect();
                AG_LOG_FAST(INFO, "RTC client disconnected in socket mode");
                g_rtcClient.reset();
                AG_LOG_FAST(INFO, "RTC client reset in socket mode");
            }

            return 0;
        }
        // Otherwise, run as before: auto-connect and start snapshot
        // Load configuration
        YAML::Node config = YAML::LoadFile(config_file);

        // Configure Agora RTC SDK log path if specified in config
        if (config["log"] && config["log"]["path"]) {
            std::string logPath = config["log"]["path"].as<std::string>();
            if (!logPath.empty()) {
                // Ensure log directory exists
                if (!fs::exists(logPath)) {
                    fs::create_directories(logPath);
                }
                // Set RTC log file path with process ID
                std::string rtcLogFile = logPath + "/rtc_sdk_" + std::to_string(getpid()) + ".log";
                setRTCLogPath(rtcLogFile.c_str());
                AG_LOG_FAST(INFO, "Set Agora RTC log path to: %s", rtcLogFile.c_str());
            }
        }

        // Set up signal handlers with proper initialization
        struct sigaction sa;
        sa.sa_handler = signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;

        if (sigaction(SIGINT, &sa, NULL) == -1) {
            AG_LOG_FAST(ERROR, "Failed to set up SIGINT handler");
            return 1;
        }
        if (sigaction(SIGTERM, &sa, NULL) == -1) {
            AG_LOG_FAST(ERROR, "Failed to set up SIGTERM handler");
            return 1;
        }

        // Initialize RTC client with global pointer
        g_rtcClient = std::make_unique<agora::rtc::RtcClient>();
        agora::rtc::RtcClient::Config rtc_config;
        rtc_config.appId = config["app_id"].as<std::string>();
        rtc_config.enableVideo = true;         // Enable video subscription
        rtc_config.autoSubscribeVideo = true;  // Auto-subscribe to video streams
        rtc_config.autoSubscribeAudio = true;  // Auto-subscribe to audio streams

        // Configure audio playback parameters from recording.audio (defaults to 16kHz mono)
        if (config["recording"] && config["recording"]["audio"]) {
            const auto& audio_config = config["recording"]["audio"];
            if (audio_config["sample_rate"]) {
                rtc_config.audioPlaybackSampleRate = audio_config["sample_rate"].as<int>();
            }
            if (audio_config["channels"]) {
                rtc_config.audioPlaybackChannels = audio_config["channels"].as<int>();
            }
            // Update samples per call based on sample rate
            rtc_config.audioPlaybackSamplesPerCall = rtc_config.audioPlaybackSampleRate / 100;
        }

        if (config["channel_name"]) {
            rtc_config.channel = config["channel_name"].as<std::string>();
        }

        if (config["access_token"]) {
            rtc_config.accessToken = config["access_token"].as<std::string>();
        }

        if (config["egress_uid"]) {
            rtc_config.egressUid = config["egress_uid"].as<std::string>();
        }

        // Handle remote user ID if provided
        if (config["remote_user_id"]) {
            rtc_config.remoteUserId = config["remote_user_id"].as<std::string>();
        }

        // Output directories will be set explicitly in snapshots and recording sections

        // Generate unique task ID for this session
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        auto ms =
            std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

        std::ostringstream task_id_stream;
        task_id_stream << "session_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
        task_id_stream << "_" << std::setfill('0') << std::setw(3) << ms.count();
        std::string session_task_id = task_id_stream.str();

        AG_LOG_FAST(INFO, "Generated session task ID: %s", session_task_id.c_str());

        // Configure snapshot sink
        agora::rtc::SnapshotSink::Config snapshots_config;
        snapshots_config.taskId = session_task_id;  // Set task ID for metadata generation

        // Apply snapshots settings if provided
        if (config["snapshots"]) {
            const auto& snapshots_node = config["snapshots"];

            // Set output directory (required in config)
            if (!snapshots_node["output_dir"]) {
                AG_LOG_FAST(ERROR, "snapshots.output_dir is required in config");
                return -1;
            }
            snapshots_config.outputDir = snapshots_node["output_dir"].as<std::string>();

            // Create output directory if it doesn't exist
            fs::create_directories(snapshots_config.outputDir);

            // Set width if provided
            if (snapshots_node["width"]) {
                snapshots_config.width = snapshots_node["width"].as<int>();
            }

            // Set height if provided
            if (snapshots_node["height"]) {
                snapshots_config.height = snapshots_node["height"].as<int>();
            }

            // Set interval if provided
            if (snapshots_node["interval_in_ms"]) {
                snapshots_config.intervalInMs = snapshots_node["interval_in_ms"].as<int>();
            }

            // Set quality if provided
            if (snapshots_node["quality"]) {
                snapshots_config.quality = snapshots_node["quality"].as<int>();
            }

            // Parse user list for snapshots
            if (snapshots_node["users"]) {
                std::string usersStr = snapshots_node["users"].as<std::string>();
                if (!usersStr.empty()) {
                    // Split comma-separated user list
                    std::stringstream ss(usersStr);
                    std::string user;
                    while (std::getline(ss, user, ',')) {
                        // Trim whitespace
                        user.erase(0, user.find_first_not_of(" \t"));
                        user.erase(user.find_last_not_of(" \t") + 1);
                        if (!user.empty()) {
                            snapshots_config.targetUsers.push_back(user);
                        }
                    }
                }
            }
        }

        // Configure recording sink
        agora::rtc::RecordingSink::Config recording_config;
        recording_config.taskId = session_task_id;  // Set task ID for metadata generation

        if (config["recording"]) {
            const auto& recording_node = config["recording"];

            // Set output directory (required in config)
            if (!recording_node["output_dir"]) {
                AG_LOG_FAST(ERROR, "recording.output_dir is required in config");
                return -1;
            }
            recording_config.outputDir = recording_node["output_dir"].as<std::string>();
            // Create output directory if it doesn't exist
            fs::create_directories(recording_config.outputDir);

            // Dynamic mode selection based on user list
            std::vector<std::string> userList;
            if (recording_node["users"]) {
                std::string usersStr = recording_node["users"].as<std::string>();
                if (!usersStr.empty()) {
                    // Split comma-separated user list
                    std::stringstream ss(usersStr);
                    std::string user;
                    while (std::getline(ss, user, ',')) {
                        // Trim whitespace
                        user.erase(0, user.find_first_not_of(" \t"));
                        user.erase(user.find_last_not_of(" \t") + 1);
                        if (!user.empty()) {
                            userList.push_back(user);
                        }
                    }
                }
            }

            // Determine recording mode based on user configuration
            if (userList.size() == 1) {
                // Single user - individual recording (only record this specific user)
                recording_config.mode = agora::rtc::VideoCompositor::Mode::Individual;
                AG_LOG_FAST(INFO,
                            "[Config] Single user specified (%s) - individual recording mode (only "
                            "this user will be recorded)",
                            userList[0].c_str());
            } else {
                // Multiple users or empty list - composite recording with dynamic layout
                recording_config.mode = agora::rtc::VideoCompositor::Mode::Composite;
                if (userList.empty()) {
                    AG_LOG_FAST(INFO,
                                "[Config] No users specified - composite recording with dynamic "
                                "layout (all users, adaptive grid)");
                } else {
                    AG_LOG_FAST(INFO,
                                "[Config] Multiple users specified (%zu users) - composite "
                                "recording with grid layout",
                                userList.size());
                }
            }

            // Set target users for recording
            recording_config.targetUsers = userList;

            if (recording_node["format"]) {
                std::string format = recording_node["format"].as<std::string>();
                if (format == "avi") {
                    recording_config.format = agora::rtc::RecordingSink::OutputFormat::AVI;
                } else if (format == "mkv") {
                    recording_config.format = agora::rtc::RecordingSink::OutputFormat::MKV;
                } else if (format == "ts") {
                    recording_config.format = agora::rtc::RecordingSink::OutputFormat::TS;
                } else {
                    recording_config.format = agora::rtc::RecordingSink::OutputFormat::MP4;
                }
            }
            if (recording_node["max_duration_seconds"]) {
                recording_config.maxDurationSeconds =
                    recording_node["max_duration_seconds"].as<int>();
            }

            // Video settings
            if (recording_node["video"]) {
                const auto& video_config = recording_node["video"];
                if (video_config["enabled"]) {
                    recording_config.recordVideo = video_config["enabled"].as<bool>();
                }
                if (video_config["width"]) {
                    recording_config.videoWidth = video_config["width"].as<uint32_t>();
                }
                if (video_config["height"]) {
                    recording_config.videoHeight = video_config["height"].as<uint32_t>();
                }
                if (video_config["fps"]) {
                    recording_config.videoFps = video_config["fps"].as<int>();
                }
                if (video_config["bitrate"]) {
                    recording_config.videoBitrate = video_config["bitrate"].as<int>();
                }
                if (video_config["codec"]) {
                    recording_config.videoCodec = video_config["codec"].as<std::string>();
                }
                if (video_config["buffer_size"]) {
                    recording_config.videoBufferSize = video_config["buffer_size"].as<int>();
                }
            }

            // Audio settings
            if (recording_node["audio"]) {
                const auto& audio_config = recording_node["audio"];
                if (audio_config["enabled"]) {
                    recording_config.recordAudio = audio_config["enabled"].as<bool>();
                }
                if (audio_config["sample_rate"]) {
                    recording_config.audioSampleRate = audio_config["sample_rate"].as<int>();
                }
                if (audio_config["channels"]) {
                    recording_config.audioChannels = audio_config["channels"].as<int>();
                }
                if (audio_config["bitrate"]) {
                    recording_config.audioBitrate = audio_config["bitrate"].as<int>();
                }
                if (audio_config["codec"]) {
                    recording_config.audioCodec = audio_config["codec"].as<std::string>();
                }
                if (audio_config["buffer_size"]) {
                    recording_config.audioBufferSize = audio_config["buffer_size"].as<int>();
                }
            }

            // TS-specific settings
            if (recording_node["ts"]) {
                const auto& ts_config = recording_node["ts"];
                if (ts_config["segment_duration"]) {
                    recording_config.tsSegmentDurationSeconds =
                        ts_config["segment_duration"].as<int>();
                }
                if (ts_config["generate_playlist"]) {
                    recording_config.tsGeneratePlaylist = ts_config["generate_playlist"].as<bool>();
                }
                if (ts_config["keep_incomplete_segments"]) {
                    recording_config.tsKeepIncompleteSegments =
                        ts_config["keep_incomplete_segments"].as<bool>();
                }
            }
        }

        // Initialize RTC client
        if (!g_rtcClient->initialize(rtc_config)) {
            AG_LOG_FAST(ERROR, "Failed to initialize RTC client");
            return 1;
        }

        // Initialize snapshot sink with global pointer
        g_snapshotSink = std::make_unique<agora::rtc::SnapshotSink>();
        g_snapshotSink->initialize(snapshots_config);

        // Initialize recording sink with global pointer
        g_recordingSink = std::make_unique<agora::rtc::RecordingSink>();
        g_recordingSink->initialize(recording_config);

        // Map to store snapshot sinks for each user
        std::map<std::string, std::unique_ptr<agora::rtc::SnapshotSink>> user_snapshot_sinks;
        std::mutex user_sinks_mutex;

        // Set up video frame callback
        g_rtcClient->setVideoFrameCallback(
            [&user_snapshot_sinks, &user_sinks_mutex, &snapshots_config, &session_task_id](
                const agora::media::base::VideoFrame& frame, const std::string& userId) {
                static int rtc_video_count = 0;
                rtc_video_count++;
                if (rtc_video_count % 30 == 0) {
                    AG_LOG_FAST(INFO,
                                "Orignal RTC Video Frame #%d from user: %s, %dx%d, "
                                "timestamp: %ld",
                                rtc_video_count, userId.c_str(), frame.width, frame.height,
                                frame.renderTimeMs);
                }
                if (!g_running) {
                    return;
                }

                std::lock_guard<std::mutex> lock(user_sinks_mutex);

                // First handle snapshot if needed
                {
                    // Check if we should process snapshots for this user
                    bool shouldProcessSnapshot =
                        snapshots_config.targetUsers.empty() ||
                        std::find(snapshots_config.targetUsers.begin(),
                                  snapshots_config.targetUsers.end(),
                                  userId) != snapshots_config.targetUsers.end();

                    if (shouldProcessSnapshot) {
                        // Get or create snapshot sink for this user
                        auto& user_snapshot_sink = user_snapshot_sinks[userId];
                        if (!user_snapshot_sink && g_running) {
                            user_snapshot_sink = std::make_unique<agora::rtc::SnapshotSink>();
                            agora::rtc::SnapshotSink::Config user_config = snapshots_config;
                            user_config.outputDir = snapshots_config.outputDir + "/user_" + userId;
                            user_config.taskId =
                                session_task_id + "_user_" + userId;  // Unique taskId per user

                            if (!user_snapshot_sink->initialize(user_config)) {
                                AG_LOG_FAST(ERROR, "Failed to initialize snapshot sink for user %s",
                                            userId.c_str());
                                return;  // Skip this frame if initialization fails
                            }
                            if (!user_snapshot_sink->start()) {
                                AG_LOG_FAST(ERROR, "Failed to start snapshot sink for user %s",
                                            userId.c_str());
                                return;  // Skip this frame if start fails
                            }
                            AG_LOG_FAST(INFO, "Started capturing snapshots for user %s to: %s",
                                        userId.c_str(), user_config.outputDir.c_str());
                        }

                        // Send frame to user-specific snapshot sink
                        user_snapshot_sink->onVideoFrame(frame.yBuffer, frame.uBuffer,
                                                         frame.vBuffer, frame.yStride,
                                                         frame.uStride, frame.vStride, frame.width,
                                                         frame.height, frame.renderTimeMs, userId);
                    }
                }  // Release the lock here

                // Then handle recording (unconditionally for all frames)
                if (g_recordingSink->isRecording()) {
                    if (!g_running) {
                        return;
                    }
                    static int recording_video_count = 0;
                    recording_video_count++;
                    if (recording_video_count % 30 == 0) {
                        AG_LOG_FAST(INFO, "Sending video frame #%d to RecordingSink from user: %s",
                                    recording_video_count, userId.c_str());
                    }
                    g_recordingSink->onVideoFrame(
                        frame.yBuffer, frame.uBuffer, frame.vBuffer, frame.yStride, frame.uStride,
                        frame.vStride, frame.width, frame.height, frame.renderTimeMs, userId);
                }
            });

        // Set up audio frame callback
        g_rtcClient->setAudioFrameCallback(
            [](const agora::media::IAudioFrameObserverBase::AudioFrame& frame,
               const std::string& userId) {
                static int rtc_audio_count = 0;
                rtc_audio_count++;
                if (rtc_audio_count % 50 == 0) {
                    AG_LOG_FAST(INFO,
                                "Orignal RTC Audio Frame #%d from user: %s, %d samples, "
                                "timestamp: %ld",
                                rtc_audio_count, userId.c_str(), frame.samplesPerChannel,
                                frame.renderTimeMs);
                }

                // Send audio frame to recording sink if enabled
                if (g_recordingSink->isRecording()) {
                    if (!g_running) {
                        return;
                    }
                    g_recordingSink->onAudioFrame(reinterpret_cast<const uint8_t*>(frame.buffer),
                                                  frame.samplesPerChannel, frame.samplesPerSec,
                                                  frame.channels, frame.renderTimeMs, userId);
                }
            });

        // Connect to the channel
        if (!g_rtcClient->connect()) {
            AG_LOG_FAST(ERROR, "Failed to connect to channel");
            return 1;
        }

        // Start snapshot capture
        if (!g_snapshotSink->start()) {
            AG_LOG_FAST(ERROR, "Failed to start snapshot capture");
            return 1;
        }

        // Start recording
        AG_LOG_FAST(INFO, "About to start recording sink...");
        if (!g_recordingSink->start()) {
            AG_LOG_FAST(ERROR, "Failed to start recording");
            return 1;
        }
        AG_LOG_FAST(INFO, "Recording sink started successfully");

        AG_LOG_FAST(INFO, "Snapshot capture & recording active. Press Ctrl+C to stop...");

        // Main loop
        try {
            auto last_status = std::chrono::steady_clock::now();
            AG_LOG_FAST(INFO, "Entering main loop, g_running = %d", g_running.load());

            while (g_running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));

                // Check if any user snapshot sinks are still capturing
                bool any_capturing = false;
                size_t num_users = 0;

                {
                    std::lock_guard<std::mutex> lock(user_sinks_mutex);
                    num_users = user_snapshot_sinks.size();
                    for (const auto& pair : user_snapshot_sinks) {
                        if (pair.second && pair.second->isCapturing()) {
                            any_capturing = true;
                            break;
                        }
                    }
                }

                // Print a status update every 5 seconds
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - last_status).count() >=
                    5) {
                    AG_LOG_FAST(INFO, "Capturing in progress... (%zu users)", num_users);
                    last_status = now;
                }

                // Wait for shutdown signal or timeout
                std::unique_lock<std::mutex> lock(g_shutdown_mutex);
                bool should_exit = g_shutdown_cv.wait_for(lock, std::chrono::milliseconds(100),
                                                          [] { return !g_running.load(); });
                if (should_exit) {
                    AG_LOG_FAST(INFO, "Condition variable signaled shutdown, exiting main loop...");
                    break;
                }
            }
        } catch (const std::exception& e) {
            AG_LOG_FAST(ERROR, "Error in main loop: %s", e.what());
            g_running = false;
        }

        AG_LOG_FAST(INFO, "Shutting down...");

        // Stop capturing and disconnect
        AG_LOG_FAST(INFO, "Stopping all snapshots...");
        {
            std::lock_guard<std::mutex> lock(user_sinks_mutex);
            for (auto& pair : user_snapshot_sinks) {
                if (pair.second) {
                    try {
                        AG_LOG_FAST(INFO, "Stopping snapshot for user %s...", pair.first.c_str());
                        pair.second->stop();
                        AG_LOG_FAST(INFO, "Successfully stopped snapshot for user %s",
                                    pair.first.c_str());
                    } catch (const std::exception& e) {
                        AG_LOG_FAST(ERROR, "Error stopping snapshot for user %s: %s",
                                    pair.first.c_str(), e.what());
                    }
                }
            }
            user_snapshot_sinks.clear();
        }

        // Give some time for cleanup, but don't block indefinitely
        std::this_thread::sleep_for(std::chrono::milliseconds(200));

        // Explicitly clean up resources in reverse order of initialization
        if (g_recordingSink) {
            try {
                AG_LOG_FAST(INFO, "Stopping recording...");

                AG_LOG_FAST(INFO, "About to call g_recordingSink->stop()...");

                g_recordingSink->stop();

                AG_LOG_FAST(INFO, "g_recordingSink->stop() returned successfully");
                AG_LOG_FAST(INFO, "Recording stopped successfully");
            } catch (const std::exception& e) {
                AG_LOG_FAST(ERROR, "Error stopping recording: %s", e.what());
            }
            AG_LOG_FAST(INFO, "Resetting recording sink...");
            g_recordingSink.reset();
            AG_LOG_FAST(INFO, "Recording sink reset complete");
        }

        if (g_snapshotSink) {
            try {
                AG_LOG_FAST(INFO, "Stopping snapshot capture...");
                g_snapshotSink->stop();
            } catch (const std::exception& e) {
                AG_LOG_FAST(ERROR, "Error stopping snapshot capture: %s", e.what());
            }
            g_snapshotSink.reset();
        }

        if (g_rtcClient) {
            try {
                AG_LOG_FAST(INFO, "Disconnecting from channel...");
                g_rtcClient->disconnect();
            } catch (const std::exception& e) {
                AG_LOG_FAST(ERROR, "Error disconnecting from channel: %s", e.what());
            }
            g_rtcClient.reset();
        }

        // Final cleanup
        AG_LOG_FAST(INFO, "Cleanup complete. Exiting...");

        AG_LOG_FAST(INFO, "Capture&Recording stopped");

    } catch (const std::exception& e) {
        AG_LOG_FAST(ERROR, "Error: %s", e.what());
        return 1;
    }

    return 0;
}
