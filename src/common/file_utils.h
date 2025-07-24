#pragma once

#include <string>

namespace agora {
namespace common {

/**
 * Create directory and all parent directories if they don't exist
 * @param path Directory path to create
 * @return true if directory exists or was created successfully, false otherwise
 */
bool createDirectoriesIfNotExists(const std::string& path);

/**
 * Generate timestamped filename with user ID support
 * @param prefix Filename prefix (e.g., "recording", "snapshot")
 * @param suffix File extension (e.g., ".mp4", ".jpg")
 * @param userId Optional user ID to include in filename
 * @param includeMilliseconds Whether to include milliseconds in timestamp
 * @return Generated filename in format: prefix_YYYYMMDD_HHMMSS[_SSS][_userID].suffix
 */
std::string generateTimestampedFilename(const std::string& prefix,
                                       const std::string& suffix,
                                       const std::string& userId = "",
                                       bool includeMilliseconds = true);

/**
 * Get file extension from format string
 * @param format Format string (e.g., "mp4", "avi", "mkv")
 * @return Extension with dot (e.g., ".mp4")
 */
std::string getFileExtension(const std::string& format);

/**
 * Sanitize filename by removing invalid characters
 * @param filename Input filename
 * @return Sanitized filename safe for filesystem
 */
std::string sanitizeFilename(const std::string& filename);

} // namespace common
} // namespace agora