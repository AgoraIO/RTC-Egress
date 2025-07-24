#include "file_utils.h"
#include <filesystem>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <vector>

namespace fs = std::filesystem;

namespace agora {
namespace common {

bool createDirectoriesIfNotExists(const std::string& path) {
    if (path.empty()) {
        std::cerr << "[FileUtils] Empty path provided" << std::endl;
        return false;
    }

    try {
        // Check if directory already exists
        if (fs::exists(path)) {
            if (fs::is_directory(path)) {
                return true;
            } else {
                std::cerr << "[FileUtils] Path exists but is not a directory: " << path << std::endl;
                return false;
            }
        }

        // Create directories recursively
        if (fs::create_directories(path)) {
            std::cout << "[FileUtils] Created directory: " << path << std::endl;
            return true;
        } else {
            std::cerr << "[FileUtils] Failed to create directory: " << path << std::endl;
            return false;
        }
    } catch (const fs::filesystem_error& ex) {
        std::cerr << "[FileUtils] Filesystem error creating directory '" << path
                  << "': " << ex.what() << std::endl;
        return false;
    } catch (const std::exception& ex) {
        std::cerr << "[FileUtils] Error creating directory '" << path
                  << "': " << ex.what() << std::endl;
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
        std::string sanitizedUserId = sanitizeFilename(userId);
        ss << "_user_" << sanitizedUserId;
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

std::string sanitizeFilename(const std::string& filename) {
    std::string sanitized = filename;

    // Replace invalid characters with underscores
    const std::string invalidChars = "<>:\"/\\|?*";
    for (char& c : sanitized) {
        if (invalidChars.find(c) != std::string::npos || c < 32) {
            c = '_';
        }
    }

    // Remove leading/trailing dots and spaces
    while (!sanitized.empty() && (sanitized.front() == '.' || sanitized.front() == ' ')) {
        sanitized.erase(0, 1);
    }
    while (!sanitized.empty() && (sanitized.back() == '.' || sanitized.back() == ' ')) {
        sanitized.pop_back();
    }

    // Ensure filename is not empty and not reserved names
    if (sanitized.empty()) {
        sanitized = "unnamed";
    }

    // Check for Windows reserved names
    const std::vector<std::string> reservedNames = {
        "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
        "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
        "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"
    };

    std::string upperSanitized = sanitized;
    std::transform(upperSanitized.begin(), upperSanitized.end(), upperSanitized.begin(), ::toupper);

    for (const auto& reserved : reservedNames) {
        if (upperSanitized == reserved) {
            sanitized = "file_" + sanitized;
            break;
        }
    }

    return sanitized;
}

} // namespace common
} // namespace agora