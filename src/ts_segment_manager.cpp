#define AG_LOG_TAG "TSSegmentManager"

#include "include/ts_segment_manager.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "common/file_utils.h"
#include "common/log.h"

namespace agora {
namespace rtc {

namespace fs = std::filesystem;

TSSegmentManager::TSSegmentManager() {}

TSSegmentManager::~TSSegmentManager() {
    cleanup();
}

bool TSSegmentManager::initialize(const Config& config) {
    std::lock_guard<std::mutex> lock(mutex_);

    config_ = config;

    if (!agora::common::createDirectoriesIfNotExists(config_.outputDir)) {
        AG_LOG_FAST(ERROR, "Failed to create TS output directory: %s", config_.outputDir.c_str());
        return false;
    }

    AG_LOG_FAST(INFO, "TSSegmentManager initialized with segment duration: %ds",
                config_.segmentDurationSeconds);
    return true;
}

void TSSegmentManager::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Finalize current segment if exists
    if (!currentSegment_.filename.empty() && !currentSegment_.isComplete) {
        finalizeCurrentSegment();
    }

    // Close playlist file if open
    if (playlistFile_.is_open()) {
        playlistFile_.close();
    }

    AG_LOG_FAST(INFO, "TSSegmentManager cleanup completed");
}

bool TSSegmentManager::startNewSession() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Generate unique session ID
    currentSession_.sessionId = generateSessionId();
    currentSession_.sessionDir = config_.outputDir + "/" + currentSession_.sessionId;
    currentSession_.sessionStartTime = std::chrono::steady_clock::now();
    currentSession_.currentSegmentNumber = 1;
    currentSession_.totalDuration = 0.0;
    currentSession_.segments.clear();

    // Create session directory
    if (!createSessionDirectory()) {
        AG_LOG_FAST(ERROR, "Failed to create session directory: %s",
                    currentSession_.sessionDir.c_str());
        return false;
    }

    // Setup playlist path
    playlistPath_ = currentSession_.sessionDir + "/playlist.m3u8";

    AG_LOG_FAST(INFO, "Started new TS session: %s", currentSession_.sessionId.c_str());
    return true;
}

void TSSegmentManager::endCurrentSession() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Finalize current segment
    if (!currentSegment_.filename.empty() && !currentSegment_.isComplete) {
        finalizeCurrentSegment();
    }

    // Generate final playlist
    if (config_.generatePlaylist) {
        generateHLSPlaylist();
    }

    // Close playlist file
    if (playlistFile_.is_open()) {
        playlistFile_.close();
    }

    AG_LOG_FAST(INFO, "Ended TS session: %s (total duration: %.2fs)",
                currentSession_.sessionId.c_str(), currentSession_.totalDuration);
}

bool TSSegmentManager::shouldRotateSegment(const std::chrono::steady_clock::time_point& currentTime,
                                           bool hasKeyframe) const {
    if (currentSegment_.filename.empty()) {
        return false;  // First segment will be created during encoder initialization
    }

    auto elapsed =
        std::chrono::duration_cast<std::chrono::seconds>(currentTime - currentSegment_.startTime);

    // Only check time-based rotation - keyframe requirement is handled by caller
    return elapsed.count() >= config_.segmentDurationSeconds;
}

bool TSSegmentManager::rotateToNewSegment() {
    std::lock_guard<std::mutex> lock(mutex_);

    // Finalize current segment if exists
    if (!currentSegment_.filename.empty() && !currentSegment_.isComplete) {
        if (!finalizeCurrentSegment()) {
            AG_LOG_FAST(ERROR, "Failed to finalize current segment before rotation");
            return false;
        }
    }

    // Create new segment
    currentSegment_ = {};
    currentSegment_.segmentNumber = currentSession_.currentSegmentNumber++;
    currentSegment_.filename = generateSegmentFilename(currentSegment_.segmentNumber);
    currentSegment_.tempFilename = currentSegment_.filename + ".tmp";
    currentSegment_.startTime = std::chrono::steady_clock::now();
    currentSegment_.isComplete = false;

    AG_LOG_FAST(INFO, "Rotated to new TS segment: %s", currentSegment_.filename.c_str());
    return true;
}

std::string TSSegmentManager::getCurrentSegmentPath() const {
    if (currentSegment_.filename.empty()) {
        return "";
    }
    return currentSession_.sessionDir + "/" + currentSegment_.filename;
}

std::string TSSegmentManager::getCurrentTempSegmentPath() const {
    if (currentSegment_.tempFilename.empty()) {
        return "";
    }
    return currentSession_.sessionDir + "/" + currentSegment_.tempFilename;
}

bool TSSegmentManager::finalizeCurrentSegment() {
    if (currentSegment_.filename.empty() || currentSegment_.isComplete) {
        return true;  // Nothing to finalize
    }

    std::string tempPath = getCurrentTempSegmentPath();
    std::string finalPath = getCurrentSegmentPath();

    // Atomic rename from temp to final filename
    try {
        safeRename(tempPath, finalPath);
        currentSegment_.isComplete = true;

        // Calculate segment duration
        auto now = std::chrono::steady_clock::now();
        currentSegment_.duration =
            std::chrono::duration<double>(now - currentSegment_.startTime).count();

        // Add to session segments list
        currentSession_.segments.push_back(currentSegment_);
        currentSession_.totalDuration += currentSegment_.duration;

        // Update playlist if enabled
        if (config_.generatePlaylist) {
            updatePlaylistWithNewSegment(currentSegment_);
        }

        AG_LOG_FAST(INFO, "Finalized TS segment: %s (duration: %.2fs)",
                    currentSegment_.filename.c_str(), currentSegment_.duration);
        return true;

    } catch (const std::exception& e) {
        AG_LOG_FAST(ERROR, "Failed to finalize segment %s: %s", currentSegment_.filename.c_str(),
                    e.what());
        return false;
    }
}

void TSSegmentManager::generateHLSPlaylist() {
    try {
        std::ofstream playlist(playlistPath_);
        if (!playlist.is_open()) {
            AG_LOG_FAST(ERROR, "Failed to create playlist file: %s", playlistPath_.c_str());
            return;
        }

        // Write HLS header
        playlist << "#EXTM3U\n";
        playlist << "#EXT-X-VERSION:3\n";
        playlist << "#EXT-X-TARGETDURATION:" << config_.segmentDurationSeconds << "\n";
        playlist << "#EXT-X-MEDIA-SEQUENCE:1\n";
        playlist << "\n";

        // Write segment entries
        for (const auto& segment : currentSession_.segments) {
            if (segment.isComplete) {
                playlist << "#EXTINF:" << std::fixed << std::setprecision(3) << segment.duration
                         << ",\n";
                playlist << segment.filename << "\n";
            }
        }

        // End playlist if session is complete
        playlist << "#EXT-X-ENDLIST\n";
        playlist.close();

        AG_LOG_FAST(INFO, "Generated HLS playlist with %zu segments",
                    currentSession_.segments.size());

    } catch (const std::exception& e) {
        AG_LOG_FAST(ERROR, "Failed to generate HLS playlist: %s", e.what());
    }
}

void TSSegmentManager::updatePlaylistWithNewSegment(const SegmentInfo& segment) {
    if (!config_.generatePlaylist || !segment.isComplete) {
        return;
    }

    // For live streaming, we would append to playlist here
    // For now, regenerate entire playlist for simplicity
    generateHLSPlaylist();
}

std::string TSSegmentManager::generateSessionId() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << "ts_session_" << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S") << "_"
        << std::setfill('0') << std::setw(3) << ms.count();

    return oss.str();
}

std::string TSSegmentManager::generateSegmentFilename(int segmentNumber) const {
    std::ostringstream oss;
    oss << "segment_" << std::setfill('0') << std::setw(5) << segmentNumber << ".ts";
    return oss.str();
}

bool TSSegmentManager::createSessionDirectory() {
    return agora::common::createDirectoriesIfNotExists(currentSession_.sessionDir);
}

bool TSSegmentManager::ensureDirectoryExists(const std::string& path) {
    return agora::common::createDirectoriesIfNotExists(path);
}

void TSSegmentManager::cleanupOldSessions() {
    // Implementation for cleaning up old sessions based on retention policy
    // This would remove sessions older than a certain age
}

void TSSegmentManager::safeRename(const std::string& tempPath, const std::string& finalPath) {
    try {
        fs::rename(tempPath, finalPath);
    } catch (const fs::filesystem_error& e) {
        AG_LOG_FAST(ERROR, "Failed to rename %s to %s: %s", tempPath.c_str(), finalPath.c_str(),
                    e.what());
        throw;
    }
}

}  // namespace rtc
}  // namespace agora