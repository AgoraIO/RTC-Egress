#define AG_LOG_TAG "FileUtils"

#include "file_utils.h"
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <vector>
#include "log.h"

namespace fs = std::filesystem;

namespace agora {
namespace common {

bool createDirectoriesIfNotExists(const std::string& path) {
    if (path.empty()) {
        AG_LOG_FAST(ERROR, "Empty path provided");
        return false;
    }

    try {
        // Check if directory already exists
        if (fs::exists(path)) {
            if (fs::is_directory(path)) {
                return true;
            } else {
                AG_LOG_FAST(ERROR, "Path exists but is not a directory: %s", path.c_str());
                return false;
            }
        }

        // Create directories recursively
        if (fs::create_directories(path)) {
            AG_LOG_FAST(INFO, "Created directory: %s", path.c_str());
            return true;
        } else {
            AG_LOG_FAST(ERROR, "Failed to create directory: %s", path.c_str());
            return false;
        }
    } catch (const fs::filesystem_error& ex) {
        AG_LOG_FAST(ERROR, "Filesystem error creating directory '%s': %s", path.c_str(), ex.what());
        return false;
    } catch (const std::exception& ex) {
        AG_LOG_FAST(ERROR, "Error creating directory '%s': %s", path.c_str(), ex.what());
        return false;
    }
}

std::string generateTimestampedFilename(const std::string& prefix,
                                       const std::string& suffix,
                                       const std::string& userId,
                                       bool includeMilliseconds) {
    // Get current time
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()) % 1000;

    // Format timestamp
    std::stringstream ss;
    ss << prefix << "_";
    ss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");

    if (includeMilliseconds) {
        ss << "_" << std::setfill('0') << std::setw(3) << ms.count();
    }

    // Add user ID if provided
    if (!userId.empty()) {
        ss << "_user_" << userId;
    }

    // Add suffix (ensure it starts with a dot)
    if (!suffix.empty()) {
        if (suffix[0] != '.') {
            ss << ".";
        }
        ss << suffix;
    }

    return ss.str();
}

std::string getFileExtension(const std::string& format) {
    if (format.empty()) {
        return ".mp4"; // Default
    }

    std::string lowerFormat = format;
    std::transform(lowerFormat.begin(), lowerFormat.end(), lowerFormat.begin(), ::tolower);

    if (lowerFormat == "mp4") return ".mp4";
    if (lowerFormat == "avi") return ".avi";
    if (lowerFormat == "mkv") return ".mkv";
    if (lowerFormat == "jpg" || lowerFormat == "jpeg") return ".jpg";
    if (lowerFormat == "png") return ".png";
    if (lowerFormat == "yuv") return ".yuv";
    if (lowerFormat == "pcm") return ".pcm";

    // If format already has a dot, use as-is
    if (format[0] == '.') {
        return format;
    }

    // Otherwise add dot and use format
    return "." + format;
}


} // namespace common
} // namespace agora