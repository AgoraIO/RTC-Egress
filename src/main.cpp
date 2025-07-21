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
#include <iostream>
#include <map>
#include <mutex>
#include <sstream>
#include <thread>

#include "common/log.h"
#include "common/opt_parser.h"
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
        return;
    }

    std::cout << "\nReceived signal " << signal << ", shutting down..." << std::endl;
    g_signal_status = signal;
    g_running = false;
    g_shutdown_cv.notify_all();  // Wake up any waiting threads
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
            std::cout << "Socket mode cleanup starting..." << std::endl;
            if (g_recordingSink) {
                std::cout << "Stopping recording sink in socket mode..." << std::endl;
                g_recordingSink->stop();
                std::cout << "Recording sink stopped in socket mode" << std::endl;
                g_recordingSink.reset();
                std::cout << "Recording sink reset in socket mode" << std::endl;
            }

            if (g_snapshotSink) {
                std::cout << "Stopping snapshot sink in socket mode..." << std::endl;
                g_snapshotSink->stop();
                std::cout << "Snapshot sink stopped in socket mode" << std::endl;
                g_snapshotSink.reset();
                std::cout << "Snapshot sink reset in socket mode" << std::endl;
            }

            // Cleanup
            std::cout << "Stopping task pipe..." << std::endl;
            task_pipe->stop();
            std::cout << "Task pipe stopped" << std::endl;
            task_pipe.reset();
            std::cout << "Task pipe reset" << std::endl;

            if (g_rtcClient) {
                std::cout << "Disconnecting RTC client in socket mode..." << std::endl;
                g_rtcClient->disconnect();
                std::cout << "RTC client disconnected in socket mode" << std::endl;
                g_rtcClient.reset();
                std::cout << "RTC client reset in socket mode" << std::endl;
            }

            return 0;
        }
        // Otherwise, run as before: auto-connect and start snapshot
        // Load configuration
        YAML::Node config = YAML::LoadFile(config_file);

        // Set up signal handlers with proper initialization
        struct sigaction sa;
        sa.sa_handler = signal_handler;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;

        if (sigaction(SIGINT, &sa, NULL) == -1) {
            std::cerr << "Failed to set up SIGINT handler" << std::endl;
            return 1;
        }
        if (sigaction(SIGTERM, &sa, NULL) == -1) {
            std::cerr << "Failed to set up SIGTERM handler" << std::endl;
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

        // Set output directory
        if (config["egress_home"]) {
            rtc_config.EGRESS_HOME = config["egress_home"].as<std::string>();
        }

        std::string default_output_dir =
            rtc_config.EGRESS_HOME.empty() ? "." : rtc_config.EGRESS_HOME;

        // Configure snapshot sink
        agora::rtc::SnapshotSink::Config snapshots_config;

        // Apply snapshots settings if provided
        if (config["snapshots"]) {
            const auto& snapshots_node = config["snapshots"];

            // Set output directory
            std::string output_dir = default_output_dir + "/snapshots";
            if (snapshots_node["output_dir"]) {
                output_dir = snapshots_node["output_dir"].as<std::string>();
            }
            snapshots_config.outputDir = output_dir;

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

        if (config["recording"]) {
            const auto& recording_node = config["recording"];

            std::string output_dir;
            if (recording_node["output_dir"]) {
                output_dir = recording_node["output_dir"].as<std::string>();
            }
            recording_config.outputDir =
                output_dir.empty() ? default_output_dir + "/recordings" : output_dir;
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
                recording_config.mode = agora::rtc::RecordingSink::RecordingMode::INDIVIDUAL;
                std::cout << "[Config] Single user specified (" << userList[0]
                          << ") - individual recording mode (only this user will be recorded)"
                          << std::endl;
            } else {
                // Multiple users or empty list - composite recording with dynamic layout
                recording_config.mode = agora::rtc::RecordingSink::RecordingMode::COMPOSITE;
                if (userList.empty()) {
                    std::cout
                        << "[Config] No users specified - composite recording with dynamic layout "
                        << "(all users, adaptive grid)" << std::endl;
                } else {
                    std::cout << "[Config] Multiple users specified (" << userList.size()
                              << " users) - composite recording with grid layout" << std::endl;
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
        }

        // Initialize RTC client
        if (!g_rtcClient->initialize(rtc_config)) {
            std::cerr << "Failed to initialize RTC client" << std::endl;
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
            [&user_snapshot_sinks, &user_sinks_mutex, &snapshots_config](
                const agora::media::base::VideoFrame& frame, const std::string& userId) {
                std::lock_guard<std::mutex> lock(user_sinks_mutex);

                // Check if this user should be captured for snapshots
                if (!snapshots_config.targetUsers.empty()) {
                    auto it = std::find(snapshots_config.targetUsers.begin(),
                                        snapshots_config.targetUsers.end(), userId);
                    if (it == snapshots_config.targetUsers.end()) {
                        // User not in target list, skip snapshot but continue with recording
                        if (g_recordingSink->isRecording()) {
                            g_recordingSink->onVideoFrame(frame.yBuffer, frame.uBuffer,
                                                          frame.vBuffer, frame.yStride,
                                                          frame.uStride, frame.vStride, frame.width,
                                                          frame.height, frame.renderTimeMs, userId);
                        }
                        return;
                    }
                }

                // Get or create snapshot sink for this user
                auto& user_sink = user_snapshot_sinks[userId];
                if (!user_sink) {
                    user_sink = std::make_unique<agora::rtc::SnapshotSink>();
                    agora::rtc::SnapshotSink::Config user_config = snapshots_config;
                    user_config.outputDir = snapshots_config.outputDir + "/user_" + userId;
                    if (!user_sink->initialize(user_config)) {
                        std::cerr << "Failed to initialize snapshot sink for user " << userId
                                  << std::endl;
                        return;
                    }
                    if (!user_sink->start()) {
                        std::cerr << "Failed to start snapshot sink for user " << userId
                                  << std::endl;
                        return;
                    }
                    std::cout << "Started capturing snapshots for user " << userId
                              << " to: " << user_config.outputDir << std::endl;
                }

                // Send frame to user-specific snapshot sink
                user_sink->onVideoFrame(frame.yBuffer, frame.uBuffer, frame.vBuffer, frame.yStride,
                                        frame.uStride, frame.vStride, frame.width, frame.height,
                                        frame.renderTimeMs);

                // Send frame to recording sink if enabled
                if (g_recordingSink->isRecording()) {
                    g_recordingSink->onVideoFrame(
                        frame.yBuffer, frame.uBuffer, frame.vBuffer, frame.yStride, frame.uStride,
                        frame.vStride, frame.width, frame.height, frame.renderTimeMs, userId);
                }
            });

        // Set up audio frame callback
        g_rtcClient->setAudioFrameCallback([](const agora::media::base::AudioPcmFrame& frame,
                                              const std::string& userId) {
            // Send audio frame to recording sink if enabled
            if (g_recordingSink->isRecording()) {
                g_recordingSink->onAudioFrame(reinterpret_cast<const uint8_t*>(frame.data_),
                                              frame.samples_per_channel_, frame.sample_rate_hz_,
                                              frame.num_channels_, frame.capture_timestamp, userId);
            }
        });

        // Connect to the channel
        if (!g_rtcClient->connect()) {
            std::cerr << "Failed to connect to channel" << std::endl;
            return 1;
        }

        // Start snapshot capture
        if (!g_snapshotSink->start()) {
            std::cerr << "Failed to start snapshot capture" << std::endl;
            return 1;
        }

        // Start recording
        std::cout << "About to start recording sink..." << std::endl;
        if (!g_recordingSink->start()) {
            std::cerr << "Failed to start recording" << std::endl;
            return 1;
        }
        std::cout << "Recording sink started successfully" << std::endl;

        std::cout << "Snapshot capture & recording active. Press Ctrl+C to stop..." << std::endl;

        // Main loop
        try {
            auto last_status = std::chrono::steady_clock::now();

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
                    std::cout << "Capturing in progress... (" << num_users << " users)"
                              << std::endl;
                    last_status = now;
                }

                // Wait for shutdown signal or timeout
                std::unique_lock<std::mutex> lock(g_shutdown_mutex);
                if (g_shutdown_cv.wait_for(lock, std::chrono::milliseconds(100),
                                           [] { return !g_running.load(); })) {
                    break;
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "Error in main loop: " << e.what() << std::endl;
            g_running = false;
        }

        std::cout << "Shutting down..." << std::endl;

        // Stop capturing and disconnect
        std::cout << "Stopping all captures..." << std::endl;
        {
            std::lock_guard<std::mutex> lock(user_sinks_mutex);
            for (auto& pair : user_snapshot_sinks) {
                if (pair.second) {
                    try {
                        std::cout << "Stopping capture for user " << pair.first << "..."
                                  << std::endl;
                        pair.second->stop();
                        std::cout << "Successfully stopped capture for user " << pair.first
                                  << std::endl;
                    } catch (const std::exception& e) {
                        std::cerr << "Error stopping capture for user " << pair.first << ": "
                                  << e.what() << std::endl;
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
                std::cout << "Stopping recording..." << std::endl;
                std::flush(std::cout);

                std::cout << "About to call g_recordingSink->stop()..." << std::endl;
                std::flush(std::cout);

                g_recordingSink->stop();

                std::cout << "g_recordingSink->stop() returned successfully" << std::endl;
                std::flush(std::cout);
                std::cout << "Recording stopped successfully" << std::endl;
            } catch (const std::exception& e) {
                std::cerr << "Error stopping recording: " << e.what() << std::endl;
                std::flush(std::cerr);
            }
            std::cout << "Resetting recording sink..." << std::endl;
            std::flush(std::cout);
            g_recordingSink.reset();
            std::cout << "Recording sink reset complete" << std::endl;
        }

        if (g_snapshotSink) {
            try {
                std::cout << "Stopping snapshot capture..." << std::endl;
                g_snapshotSink->stop();
            } catch (const std::exception& e) {
                std::cerr << "Error stopping snapshot capture: " << e.what() << std::endl;
            }
            g_snapshotSink.reset();
        }

        if (g_rtcClient) {
            try {
                std::cout << "Disconnecting from channel..." << std::endl;
                g_rtcClient->disconnect();
            } catch (const std::exception& e) {
                std::cerr << "Error disconnecting from channel: " << e.what() << std::endl;
            }
            g_rtcClient.reset();
        }

        // Final cleanup
        std::cout << "Cleanup complete. Exiting..." << std::endl;

        std::cout << "Capture&Recording stopped" << std::endl;

    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << std::endl;
        return 1;
    }

    return 0;
}
