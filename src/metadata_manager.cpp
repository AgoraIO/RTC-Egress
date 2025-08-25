#define AG_LOG_TAG "MetadataManager"

#include "include/metadata_manager.h"

// Enhanced with OpenSSL for SHA-256 checksums and file integrity validation

#include <openssl/evp.h>
#include <openssl/sha.h>

#include <algorithm>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include "common/log.h"

namespace agora {
namespace rtc {

namespace fs = std::filesystem;

MetadataManager::MetadataManager() {
    AG_LOG_FAST(INFO, "MetadataManager initialized");
}

MetadataManager::~MetadataManager() {
    std::lock_guard<std::mutex> lock(sessionsMutex_);

    // Save all active sessions before shutdown
    for (const auto& session : activeSessions_) {
        std::string prefix = generateOutputFilePrefixWithoutLock(session.first);

        // Lock the files mutex for serialization
        std::lock_guard<std::mutex> fileLock(session.second->filesMutex);
        saveSessionMetadataWithoutLock(session.second.get(), session.first, prefix);
    }

    AG_LOG_FAST(INFO, "MetadataManager shutdown complete");
}

bool MetadataManager::startSession(const std::string& taskId, const TaskSession& session,
                                   const std::string& outputDir) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);

    if (activeSessions_.find(taskId) != activeSessions_.end()) {
        AG_LOG_FAST(WARN, "Task %s already exists, replacing session", taskId.c_str());
    }

    auto newSession = std::make_unique<TaskSession>();
    // Copy fields manually due to atomic/mutex members
    newSession->taskId = taskId;
    newSession->description = session.description;
    newSession->channel = session.channel;
    newSession->users = session.users;
    newSession->compositionMode = session.compositionMode;
    newSession->layout = session.layout;
    newSession->width = session.width;
    newSession->height = session.height;
    newSession->fps = session.fps;
    newSession->videoCodec = session.videoCodec;
    newSession->audioSampleRate = session.audioSampleRate;
    newSession->audioChannels = session.audioChannels;
    newSession->audioCodec = session.audioCodec;
    newSession->outputFormat = session.outputFormat;
    newSession->sessionStartTime = std::chrono::system_clock::now();
    newSession->durationSeconds = session.durationSeconds;
    newSession->tsSegmentDuration = session.tsSegmentDuration;
    newSession->tsGeneratePlaylist = session.tsGeneratePlaylist;
    newSession->playlistPath = session.playlistPath;
    newSession->files = session.files;
    newSession->totalFramesProcessed.store(session.totalFramesProcessed.load());
    newSession->droppedFrames.store(session.droppedFrames.load());
    newSession->totalBytesGenerated.store(session.totalBytesGenerated.load());
    newSession->averageFrameRate = session.averageFrameRate;
    newSession->sessionCompleted = false;
    newSession->terminationReason = "active";
    newSession->sessionIntegrityHash = session.sessionIntegrityHash;
    newSession->sessionAuditLog = session.sessionAuditLog;
    newSession->totalFilesExpected = session.totalFilesExpected;
    newSession->filesWithIntegrityIssues = session.filesWithIntegrityIssues;
    newSession->missingFiles = session.missingFiles;
    newSession->criticalErrors = session.criticalErrors;

    // Add initial audit entry before moving
    addAuditEntryWithoutLock(newSession.get(), taskId, "session_started",
                             "Metadata tracking initialized");

    activeSessions_[taskId] = std::move(newSession);

    // Create initial JSON file if output directory is provided
    if (!outputDir.empty()) {
        std::string prefix = generateOutputFilePrefixWithoutLock(taskId);
        std::string outputPrefix = outputDir + "/" + prefix;

        // Get pointer to the session we just moved and store the output prefix
        auto* sessionPtr = activeSessions_[taskId].get();
        sessionPtr->outputFilePrefix = outputPrefix;  // Store for reuse in endSession

        if (!saveSessionMetadataWithoutLock(sessionPtr, taskId, outputPrefix)) {
            AG_LOG_FAST(WARN, "Failed to create initial metadata file for session %s",
                        taskId.c_str());
        } else {
            AG_LOG_FAST(INFO, "Created initial metadata file for session %s", taskId.c_str());
        }
    }

    AG_LOG_FAST(INFO, "Started metadata session for task: %s", taskId.c_str());
    return true;
}

bool MetadataManager::endSession(const std::string& taskId, const std::string& terminationReason,
                                 const std::string& outputDir) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);

    auto it = activeSessions_.find(taskId);
    if (it == activeSessions_.end()) {
        AG_LOG_FAST(ERROR, "Task %s not found for ending session", taskId.c_str());
        return false;
    }

    auto& session = it->second;
    session->sessionEndTime = std::chrono::system_clock::now();
    session->sessionCompleted = true;
    session->terminationReason = terminationReason;

    // Calculate session duration
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        session->sessionEndTime - session->sessionStartTime);
    session->durationSeconds = duration.count() / 1000.0;

    // Calculate average frame rate
    if (session->durationSeconds > 0) {
        session->averageFrameRate = session->totalFramesProcessed.load() / session->durationSeconds;
    }

    // Add final audit entry before saving
    std::ostringstream summary;
    summary << "Session completed: " << session->totalFramesProcessed.load() << " frames, "
            << session->totalBytesGenerated.load() << " bytes, " << session->durationSeconds << "s";
    addAuditEntryWithoutLock(session.get(), taskId, "session_ended", summary.str());

    bool saved = false;

    // Validate all files integrity before final save
    {
        std::lock_guard<std::mutex> fileLock(session->filesMutex);
        validateAllFilesIntegrityWithoutLock(session.get(), taskId);

        // Use stored output prefix from startSession
        std::string outputPrefix = session->outputFilePrefix;

        saved = saveSessionMetadataWithoutLock(session.get(), taskId, outputPrefix);

        AG_LOG_FAST(INFO, "Ended session %s after %.2fs (%s) - %d frames, %lu bytes",
                    taskId.c_str(), session->durationSeconds, terminationReason.c_str(),
                    session->totalFramesProcessed.load(), session->totalBytesGenerated.load());
    }

    // Remove from active sessions
    activeSessions_.erase(it);

    return saved;
}

bool MetadataManager::addNewFileWithoutLock(TaskSession* session, const std::string& taskId,
                                            const FileInfo& fileInfo) {
    // NO LOCK - assume caller already holds both sessionsMutex_ and filesMutex

    // Check if file already exists
    auto fileIt =
        std::find_if(session->files.begin(), session->files.end(),
                     [&fileInfo](const FileInfo& f) { return f.filename == fileInfo.filename; });

    if (fileIt != session->files.end()) {
        // Update existing file
        *fileIt = fileInfo;
        addFileAuditEntryWithoutLock(session, taskId, fileInfo.filename, "file_updated",
                                     "Size: " + std::to_string(fileInfo.sizeBytes) + " bytes");
        AG_LOG_FAST(INFO, "Updated file %s in session %s", fileInfo.filename.c_str(),
                    taskId.c_str());
    } else {
        // Add new file
        session->files.push_back(fileInfo);
        session->totalBytesGenerated.fetch_add(fileInfo.sizeBytes);
        addFileAuditEntryWithoutLock(session, taskId, fileInfo.filename, "file_created",
                                     "Size: " + std::to_string(fileInfo.sizeBytes) + " bytes");
        AG_LOG_FAST(INFO, "Added file %s to session %s (size: %lu bytes)",
                    fileInfo.filename.c_str(), taskId.c_str(), fileInfo.sizeBytes);
    }

    return true;
}

bool MetadataManager::markFileCompleteWithoutLock(TaskSession* session, const std::string& taskId,
                                                  const std::string& filename, uint64_t finalSize,
                                                  double duration) {
    // NO LOCK - assume caller already holds filesMutex
    auto fileIt = std::find_if(session->files.begin(), session->files.end(),
                               [&filename](const FileInfo& f) { return f.filename == filename; });

    if (fileIt == session->files.end()) {
        AG_LOG_FAST(ERROR, "File %s not found in session %s for completion", filename.c_str(),
                    taskId.c_str());
        return false;
    }

    fileIt->isComplete = true;
    fileIt->completedAt = std::chrono::system_clock::now();

    if (finalSize > 0) {
        // Update size difference
        session->totalBytesGenerated.fetch_add(finalSize - fileIt->sizeBytes);
        fileIt->sizeBytes = finalSize;
    }

    if (duration > 0) {
        fileIt->durationSeconds = duration;
    }

    // Enhanced integrity check - calculate checksum for completed files
    if (!fileIt->fullPath.empty() && fileExists(fileIt->fullPath)) {
        // Calculate SHA-256 checksum for completed file
        fileIt->checksum = calculateSHA256(fileIt->fullPath);
        if (!fileIt->checksum.empty()) {
            fileIt->integrityStatus = "verified";
            fileIt->integrityVerified = true;
            fileIt->lastIntegrityCheck = std::chrono::system_clock::now();
            addFileAuditEntryWithoutLock(session, taskId, filename, "checksum_generated",
                                         "SHA-256: " + fileIt->checksum);
        } else {
            fileIt->integrityStatus = "checksum_failed";
            addFileAuditEntryWithoutLock(session, taskId, filename, "checksum_failed",
                                         "Failed to calculate SHA-256 checksum");
        }
    } else {
        fileIt->integrityStatus = "missing";
        addFileAuditEntryWithoutLock(session, taskId, filename, "file_missing",
                                     "File not found at expected path");
    }

    addFileAuditEntryWithoutLock(session, taskId, filename, "file_completed",
                                 "Final size: " + std::to_string(fileIt->sizeBytes) + " bytes");

    AG_LOG_FAST(INFO, "Marked file %s complete in session %s (final size: %lu)", filename.c_str(),
                taskId.c_str(), fileIt->sizeBytes);

    return true;
}

bool MetadataManager::saveSessionMetadataWithoutLock(TaskSession* session,
                                                     const std::string& taskId,
                                                     const std::string& outputFilePrefix) {
    // NO LOCK - assume caller already holds filesMutex if needed
    std::string metadataPath = generateMetadataPathWithoutLock(outputFilePrefix);

    // Ensure output directory exists
    std::string dir = getDirectoryFromPath(metadataPath);
    if (!ensureDirectoryExists(dir)) {
        AG_LOG_FAST(ERROR, "Failed to create directory for metadata: %s", dir.c_str());
        return false;
    }

    // Serialize session (assuming caller holds filesMutex)
    nlohmann::json jsonData = serializeSession(*session);

    bool success = atomicWriteMetadata(metadataPath, jsonData);
    if (success) {
        AG_LOG_FAST(INFO, "Saved metadata for session %s to %s", taskId.c_str(),
                    metadataPath.c_str());
    } else {
        AG_LOG_FAST(ERROR, "Failed to save metadata for session %s", taskId.c_str());
    }

    return success;
}

bool MetadataManager::appendFileToMetadata(const std::string& taskId, const FileInfo& fileInfo,
                                           const std::string& outputFilePrefix) {
    std::lock_guard<std::mutex> lock(sessionsMutex_);

    auto it = activeSessions_.find(taskId);
    if (it == activeSessions_.end()) {
        return false;
    }

    // First add to memory with proper session parameter
    bool added;
    {
        std::lock_guard<std::mutex> fileLock(it->second->filesMutex);
        added = addNewFileWithoutLock(it->second.get(), taskId, fileInfo);
    }

    if (!added) {
        return false;
    }

    bool saved;
    {
        std::lock_guard<std::mutex> fileLock(it->second->filesMutex);
        saved = saveSessionMetadataWithoutLock(it->second.get(), taskId, outputFilePrefix);

        if (saved && fileInfo.type == FileType::TS) {
            // Mark TS segment as complete with integrity validation
            markFileCompleteWithoutLock(it->second.get(), taskId, fileInfo.filename,
                                        fileInfo.sizeBytes, fileInfo.durationSeconds);

            // Validate TS segment integrity
            if (!validateFileIntegrityWithoutLock(it->second.get(), taskId, fileInfo.filename)) {
                AG_LOG_FAST(WARN, "TS segment integrity validation failed: %s",
                            fileInfo.filename.c_str());
            }
        }
    }

    if (saved) {
        AG_LOG_FAST(INFO, "Saved metadata for session %s to %s", taskId.c_str(),
                    outputFilePrefix.c_str());
    }

    return saved;
}

std::string MetadataManager::generateOutputFilePrefixWithoutLock(const std::string& taskId) const {
    return formatDateTimePrefix() + "_" + taskId;
}

std::string MetadataManager::generateMetadataPathWithoutLock(
    const std::string& outputFilePrefix) const {
    return outputFilePrefix + ".json";
}

// Static utility functions
std::string MetadataManager::compositionModeToString(CompositionMode mode) {
    switch (mode) {
        case CompositionMode::Individual:
            return "Individual";
        case CompositionMode::Composite:
            return "Composite";
        default:
            return "Unknown";
    }
}

MetadataManager::CompositionMode MetadataManager::stringToCompositionMode(const std::string& str) {
    if (str == "Individual") return CompositionMode::Individual;
    if (str == "Composite") return CompositionMode::Composite;
    return CompositionMode::Composite;  // Default
}

std::string MetadataManager::layoutToString(Layout layout) {
    switch (layout) {
        case Layout::Flat:
            return "flat";
        case Layout::Spotlight:
            return "spotlight";
        case Layout::Freestyle:
            return "freestyle";
        case Layout::Customized:
            return "customized";
        default:
            return "flat";
    }
}

MetadataManager::Layout MetadataManager::stringToLayout(const std::string& str) {
    if (str == "flat" || str == "Flat") return Layout::Flat;
    if (str == "spotlight" || str == "Spotlight") return Layout::Spotlight;
    if (str == "freestyle" || str == "Freestyle") return Layout::Freestyle;
    if (str == "customized" || str == "Customized") return Layout::Customized;
    return Layout::Flat;  // Default
}

std::string MetadataManager::fileTypeToString(FileType type) {
    switch (type) {
        case FileType::JPEG:
            return "JPEG";
        case FileType::MP4:
            return "MP4";
        case FileType::TS:
            return "TS";
        case FileType::M3U8:
            return "M3U8";
        default:
            return "Unknown";
    }
}

MetadataManager::FileType MetadataManager::stringToFileType(const std::string& str) {
    if (str == "JPEG" || str == "jpeg" || str == "jpg") return FileType::JPEG;
    if (str == "MP4" || str == "mp4") return FileType::MP4;
    if (str == "TS" || str == "ts") return FileType::TS;
    if (str == "M3U8" || str == "m3u8") return FileType::M3U8;
    return FileType::MP4;  // Default
}

// Private implementation methods
std::string MetadataManager::formatTimestamp(
    const std::chrono::system_clock::time_point& tp) const {
    auto time_t = std::chrono::system_clock::to_time_t(tp);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(tp.time_since_epoch()) % 1000;

    std::ostringstream oss;
    oss << std::put_time(std::gmtime(&time_t), "%Y-%m-%dT%H:%M:%S");
    oss << "." << std::setfill('0') << std::setw(3) << ms.count() << "Z";
    return oss.str();
}

std::string MetadataManager::formatDateTimePrefix() const {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);

    std::ostringstream oss;
    oss << std::put_time(std::localtime(&time_t), "%Y%m%d_%H%M%S");
    return oss.str();
}

MetadataManager::FileType MetadataManager::detectFileType(const std::string& filename) const {
    size_t pos = filename.find_last_of('.');
    if (pos == std::string::npos) return FileType::MP4;

    std::string ext = filename.substr(pos + 1);
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);

    if (ext == "jpg" || ext == "jpeg") return FileType::JPEG;
    if (ext == "mp4") return FileType::MP4;
    if (ext == "ts") return FileType::TS;
    if (ext == "m3u8") return FileType::M3U8;
    return FileType::MP4;
}

uint64_t MetadataManager::getFileSize(const std::string& filepath) const {
    try {
        if (fs::exists(filepath)) {
            return fs::file_size(filepath);
        }
    } catch (const fs::filesystem_error&) {
        // File doesn't exist or access error
    }
    return 0;
}

bool MetadataManager::fileExists(const std::string& filepath) const {
    return fs::exists(filepath);
}

bool MetadataManager::ensureDirectoryExists(const std::string& path) {
    try {
        return fs::create_directories(path) || fs::exists(path);
    } catch (const fs::filesystem_error& e) {
        AG_LOG_FAST(ERROR, "Failed to create directory %s: %s", path.c_str(), e.what());
        return false;
    }
}

std::string MetadataManager::getDirectoryFromPath(const std::string& filepath) const {
    size_t pos = filepath.find_last_of("/\\");
    if (pos != std::string::npos) {
        return filepath.substr(0, pos);
    }
    return ".";
}

bool MetadataManager::atomicWriteMetadata(const std::string& filepath, const nlohmann::json& data) {
    std::lock_guard<std::mutex> writeLock(metadataWriteMutex_);

    std::string tempPath = filepath + ".tmp";

    try {
        std::ofstream file(tempPath, std::ios::binary);
        if (!file.is_open()) {
            AG_LOG_FAST(ERROR, "Failed to open temp metadata file: %s", tempPath.c_str());
            return false;
        }

        file << data.dump(4);  // Pretty print with 4 spaces
        file.close();

        // Atomic rename
        fs::rename(tempPath, filepath);
        return true;

    } catch (const std::exception& e) {
        AG_LOG_FAST(ERROR, "Failed to write metadata file %s: %s", filepath.c_str(), e.what());
        try {
            fs::remove(tempPath);
        } catch (...) {
        }
        return false;
    }
}

nlohmann::json MetadataManager::serializeSession(const TaskSession& session) const {
    nlohmann::json j;

    // Basic info
    j["taskId"] = session.taskId;
    j["description"] = session.description;
    j["channel"] = session.channel;
    j["users"] = session.users;

    // Composition settings
    j["compositionMode"] = compositionModeToString(session.compositionMode);
    j["layout"] = layoutToString(session.layout);

    // Technical specs
    j["width"] = session.width;
    j["height"] = session.height;
    j["fps"] = session.fps;
    j["videoCodec"] = session.videoCodec;
    j["audioSampleRate"] = session.audioSampleRate;
    j["audioChannels"] = session.audioChannels;
    j["audioCodec"] = session.audioCodec;
    j["outputFormat"] = session.outputFormat;

    // Timeline
    j["sessionStartTime"] = formatTimestamp(session.sessionStartTime);
    if (session.sessionCompleted) {
        j["sessionEndTime"] = formatTimestamp(session.sessionEndTime);
    }
    j["durationSeconds"] = session.durationSeconds;

    // TS-specific
    j["tsSegmentDuration"] = session.tsSegmentDuration;
    j["tsGeneratePlaylist"] = session.tsGeneratePlaylist;
    j["playlistPath"] = session.playlistPath;

    // Statistics
    j["totalFramesProcessed"] = session.totalFramesProcessed.load();
    j["droppedFrames"] = session.droppedFrames.load();
    j["totalBytesGenerated"] = session.totalBytesGenerated.load();
    j["averageFrameRate"] = session.averageFrameRate;

    // Session state
    j["sessionCompleted"] = session.sessionCompleted;
    j["terminationReason"] = session.terminationReason;

    // Session integrity and audit
    j["sessionIntegrityHash"] = session.sessionIntegrityHash;
    j["sessionAuditLog"] = session.sessionAuditLog;

    // File validation summary
    j["totalFilesExpected"] = session.totalFilesExpected;
    j["filesWithIntegrityIssues"] = session.filesWithIntegrityIssues;
    j["missingFiles"] = session.missingFiles;
    j["criticalErrors"] = session.criticalErrors;

    // Files (already under files mutex lock)
    j["files"] = nlohmann::json::array();
    for (const auto& file : session.files) {
        j["files"].push_back(serializeFileInfo(file));
    }

    return j;
}

nlohmann::json MetadataManager::serializeFileInfo(const FileInfo& fileInfo) const {
    nlohmann::json j;

    j["filename"] = fileInfo.filename;
    j["fullPath"] = fileInfo.fullPath;
    j["type"] = fileTypeToString(fileInfo.type);
    j["sizeBytes"] = fileInfo.sizeBytes;
    j["createdAt"] = formatTimestamp(fileInfo.createdAt);

    if (fileInfo.isComplete) {
        j["completedAt"] = formatTimestamp(fileInfo.completedAt);
    }

    j["durationSeconds"] = fileInfo.durationSeconds;
    j["isComplete"] = fileInfo.isComplete;
    j["width"] = fileInfo.width;
    j["height"] = fileInfo.height;
    j["segmentNumber"] = fileInfo.segmentNumber;
    j["isKeyframeAligned"] = fileInfo.isKeyframeAligned;
    j["checksum"] = fileInfo.checksum;
    j["integrityVerified"] = fileInfo.integrityVerified;
    j["checksumAlgorithm"] = fileInfo.checksumAlgorithm;
    j["integrityStatus"] = fileInfo.integrityStatus;
    if (fileInfo.lastIntegrityCheck != std::chrono::system_clock::time_point{}) {
        j["lastIntegrityCheck"] = formatTimestamp(fileInfo.lastIntegrityCheck);
    }
    j["backupPath"] = fileInfo.backupPath;
    j["recoveryInfo"] = fileInfo.recoveryInfo;
    j["auditLog"] = fileInfo.auditLog;

    return j;
}

// Enhanced integrity validation methods
std::string MetadataManager::calculateSHA256(const std::string& filepath) {
    if (!fileExists(filepath)) {
        AG_LOG_FAST(ERROR, "File does not exist: %s", filepath.c_str());
        return "";
    }

    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        AG_LOG_FAST(ERROR, "Failed to open file for checksum: %s", filepath.c_str());
        return "";
    }

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (!context) {
        AG_LOG_FAST(ERROR, "Failed to create hash context");
        return "";
    }

    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(context);
        AG_LOG_FAST(ERROR, "Failed to initialize hash");
        return "";
    }

    constexpr size_t bufferSize = 8192;
    char buffer[bufferSize];

    while (file.read(buffer, bufferSize) || file.gcount() > 0) {
        if (EVP_DigestUpdate(context, buffer, file.gcount()) != 1) {
            EVP_MD_CTX_free(context);
            AG_LOG_FAST(ERROR, "Failed to update hash");
            return "";
        }
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hashLength = 0;

    if (EVP_DigestFinal_ex(context, hash, &hashLength) != 1) {
        EVP_MD_CTX_free(context);
        AG_LOG_FAST(ERROR, "Failed to finalize hash");
        return "";
    }

    EVP_MD_CTX_free(context);

    std::ostringstream oss;
    for (unsigned int i = 0; i < hashLength; ++i) {
        oss << std::hex << std::setfill('0') << std::setw(2) << static_cast<int>(hash[i]);
    }

    return oss.str();
}

bool MetadataManager::verifyFileChecksum(const std::string& filepath,
                                         const std::string& expectedChecksum) {
    if (expectedChecksum.empty()) {
        AG_LOG_FAST(WARN, "No checksum to verify for file: %s", filepath.c_str());
        return false;
    }

    std::string actualChecksum = calculateSHA256(filepath);
    if (actualChecksum.empty()) {
        return false;
    }

    bool matches = (actualChecksum == expectedChecksum);
    if (!matches) {
        AG_LOG_FAST(ERROR, "Checksum mismatch for %s - expected: %s, actual: %s", filepath.c_str(),
                    expectedChecksum.c_str(), actualChecksum.c_str());
    }

    return matches;
}

bool MetadataManager::validateFileIntegrityWithoutLock(TaskSession* session,
                                                       const std::string& taskId,
                                                       const std::string& filename) {
    // NO LOCK - assume caller already holds filesMutex

    auto fileIt = std::find_if(session->files.begin(), session->files.end(),
                               [&filename](const FileInfo& f) { return f.filename == filename; });

    if (fileIt == session->files.end()) {
        AG_LOG_FAST(ERROR, "File %s not found in session %s", filename.c_str(), taskId.c_str());
        return false;
    }

    // Check if file exists
    if (!fileExists(fileIt->fullPath)) {
        fileIt->integrityStatus = "missing";
        addFileAuditEntryWithoutLock(session, taskId, filename, "integrity_check",
                                     "File missing from filesystem");
        return false;
    }

    // Verify file size
    uint64_t currentSize = getFileSize(fileIt->fullPath);
    if (currentSize != fileIt->sizeBytes) {
        fileIt->integrityStatus = "corrupted";
        std::ostringstream oss;
        oss << "Size mismatch: expected " << fileIt->sizeBytes << ", actual " << currentSize;
        addFileAuditEntryWithoutLock(session, taskId, filename, "integrity_check", oss.str());
        AG_LOG_FAST(ERROR, "File size mismatch for %s: %s", filename.c_str(), oss.str().c_str());
        return false;
    }

    // Generate and verify checksum if not present
    if (fileIt->checksum.empty()) {
        fileIt->checksum = calculateSHA256(fileIt->fullPath);
        if (fileIt->checksum.empty()) {
            fileIt->integrityStatus = "checksum_failed";
            addFileAuditEntryWithoutLock(session, taskId, filename, "integrity_check",
                                         "Failed to calculate checksum");
            return false;
        }
        addFileAuditEntryWithoutLock(session, taskId, filename, "checksum_generated",
                                     "SHA-256: " + fileIt->checksum);
    } else {
        // Verify existing checksum
        if (!verifyFileChecksum(fileIt->fullPath, fileIt->checksum)) {
            fileIt->integrityStatus = "corrupted";
            addFileAuditEntryWithoutLock(session, taskId, filename, "integrity_check",
                                         "Checksum verification failed");
            return false;
        }
    }

    // All checks passed
    fileIt->integrityStatus = "verified";
    fileIt->integrityVerified = true;
    fileIt->lastIntegrityCheck = std::chrono::system_clock::now();
    addFileAuditEntryWithoutLock(session, taskId, filename, "integrity_check",
                                 "File integrity verified");

    return true;
}

bool MetadataManager::validateAllFilesIntegrityWithoutLock(TaskSession* session,
                                                           const std::string& taskId) {
    // NO LOCK - assume caller already holds filesMutex
    bool allValid = true;
    int totalFiles = 0;
    int validFiles = 0;
    int corruptedFiles = 0;
    int missingFiles = 0;

    totalFiles = session->files.size();

    for (auto& fileInfo : session->files) {
        if (validateFileIntegrityWithoutLock(session, taskId, fileInfo.filename)) {
            validFiles++;
        } else {
            allValid = false;
            if (fileInfo.integrityStatus == "missing") {
                missingFiles++;
            } else if (fileInfo.integrityStatus == "corrupted") {
                corruptedFiles++;
            }
        }
    }

    // Update session integrity summary
    session->totalFilesExpected = totalFiles;
    session->missingFiles = missingFiles;
    session->filesWithIntegrityIssues = corruptedFiles + missingFiles;

    std::ostringstream summary;
    summary << "Integrity validation: " << validFiles << "/" << totalFiles << " files valid";
    if (missingFiles > 0) summary << ", " << missingFiles << " missing";
    if (corruptedFiles > 0) summary << ", " << corruptedFiles << " corrupted";

    addAuditEntryWithoutLock(session, taskId, "full_integrity_check", summary.str());

    AG_LOG_FAST(INFO, "Session %s integrity check: %s", taskId.c_str(), summary.str().c_str());

    return allValid;
}

void MetadataManager::addAuditEntryWithoutLock(TaskSession* session, const std::string& taskId,
                                               const std::string& operation,
                                               const std::string& details) {
    // NO LOCK - assume caller already holds sessionsMutex_

    auto now = std::chrono::system_clock::now();
    std::ostringstream oss;
    oss << formatTimestamp(now) << " [" << operation << "]";
    if (!details.empty()) {
        oss << " " << details;
    }

    session->sessionAuditLog.push_back(oss.str());

    // Keep audit log size manageable (last 1000 entries)
    if (session->sessionAuditLog.size() > 1000) {
        session->sessionAuditLog.erase(session->sessionAuditLog.begin());
    }
}

void MetadataManager::addFileAuditEntryWithoutLock(TaskSession* session, const std::string& taskId,
                                                   const std::string& filename,
                                                   const std::string& operation,
                                                   const std::string& details) {
    // NO LOCK - assume caller already holds both sessionsMutex_ and filesMutex

    auto fileIt = std::find_if(session->files.begin(), session->files.end(),
                               [&filename](const FileInfo& f) { return f.filename == filename; });

    if (fileIt == session->files.end()) {
        return;
    }

    auto now = std::chrono::system_clock::now();
    std::ostringstream oss;
    oss << formatTimestamp(now) << " [" << operation << "]";
    if (!details.empty()) {
        oss << " " << details;
    }

    fileIt->auditLog.push_back(oss.str());

    // Keep file audit log size manageable (last 100 entries per file)
    if (fileIt->auditLog.size() > 100) {
        fileIt->auditLog.erase(fileIt->auditLog.begin());
    }

    // Also add to session audit log for global tracking
    std::ostringstream sessionEntry;
    sessionEntry << formatTimestamp(now) << " [file_" << operation << "] " << filename;
    if (!details.empty()) {
        sessionEntry << " - " << details;
    }
    session->sessionAuditLog.push_back(sessionEntry.str());

    // Keep session audit log size manageable
    if (session->sessionAuditLog.size() > 1000) {
        session->sessionAuditLog.erase(session->sessionAuditLog.begin());
    }
}

std::vector<std::string> MetadataManager::getAuditTrail(TaskSession* session,
                                                        const std::string& taskId) {
    return session->sessionAuditLog;
}

void MetadataManager::deserializeSessionInto(TaskSession* session,
                                             const nlohmann::json& json) const {
    // Basic info
    if (json.contains("taskId")) session->taskId = json["taskId"];
    if (json.contains("description")) session->description = json["description"];
    if (json.contains("channel")) session->channel = json["channel"];
    if (json.contains("users")) session->users = json["users"];

    // Composition settings
    if (json.contains("compositionMode")) {
        session->compositionMode = stringToCompositionMode(json["compositionMode"]);
    }
    if (json.contains("layout")) {
        session->layout = stringToLayout(json["layout"]);
    }

    // Technical specs
    if (json.contains("width")) session->width = json["width"];
    if (json.contains("height")) session->height = json["height"];
    if (json.contains("fps")) session->fps = json["fps"];
    if (json.contains("videoCodec")) session->videoCodec = json["videoCodec"];
    if (json.contains("audioSampleRate")) session->audioSampleRate = json["audioSampleRate"];
    if (json.contains("audioChannels")) session->audioChannels = json["audioChannels"];
    if (json.contains("audioCodec")) session->audioCodec = json["audioCodec"];
    if (json.contains("outputFormat")) session->outputFormat = json["outputFormat"];

    // Timeline - parse timestamp strings
    if (json.contains("sessionStartTime")) {
        // For simplicity, using current time - in production this should parse the ISO timestamp
        session->sessionStartTime = std::chrono::system_clock::now();
    }
    if (json.contains("sessionEndTime")) {
        session->sessionEndTime = std::chrono::system_clock::now();
    }
    if (json.contains("durationSeconds")) session->durationSeconds = json["durationSeconds"];

    // TS-specific
    if (json.contains("tsSegmentDuration")) session->tsSegmentDuration = json["tsSegmentDuration"];
    if (json.contains("tsGeneratePlaylist"))
        session->tsGeneratePlaylist = json["tsGeneratePlaylist"];
    if (json.contains("playlistPath")) session->playlistPath = json["playlistPath"];

    // Statistics
    if (json.contains("totalFramesProcessed")) {
        session->totalFramesProcessed.store(json["totalFramesProcessed"]);
    }
    if (json.contains("droppedFrames")) {
        session->droppedFrames.store(json["droppedFrames"]);
    }
    if (json.contains("totalBytesGenerated")) {
        session->totalBytesGenerated.store(json["totalBytesGenerated"]);
    }
    if (json.contains("averageFrameRate")) session->averageFrameRate = json["averageFrameRate"];

    // Session state
    if (json.contains("sessionCompleted")) session->sessionCompleted = json["sessionCompleted"];
    if (json.contains("terminationReason")) session->terminationReason = json["terminationReason"];

    // Session integrity and audit
    if (json.contains("sessionIntegrityHash"))
        session->sessionIntegrityHash = json["sessionIntegrityHash"];
    if (json.contains("sessionAuditLog")) session->sessionAuditLog = json["sessionAuditLog"];

    // File validation summary
    if (json.contains("totalFilesExpected"))
        session->totalFilesExpected = json["totalFilesExpected"];
    if (json.contains("filesWithIntegrityIssues"))
        session->filesWithIntegrityIssues = json["filesWithIntegrityIssues"];
    if (json.contains("missingFiles")) session->missingFiles = json["missingFiles"];
    if (json.contains("criticalErrors")) session->criticalErrors = json["criticalErrors"];

    // Files
    if (json.contains("files") && json["files"].is_array()) {
        for (const auto& fileJson : json["files"]) {
            session->files.push_back(deserializeFileInfo(fileJson));
        }
    }
}

MetadataManager::FileInfo MetadataManager::deserializeFileInfo(const nlohmann::json& json) const {
    FileInfo fileInfo;

    if (json.contains("filename")) fileInfo.filename = json["filename"];
    if (json.contains("fullPath")) fileInfo.fullPath = json["fullPath"];
    if (json.contains("type")) fileInfo.type = stringToFileType(json["type"]);
    if (json.contains("sizeBytes")) fileInfo.sizeBytes = json["sizeBytes"];

    // Parse timestamps - for simplicity using current time, in production should parse ISO format
    if (json.contains("createdAt")) {
        fileInfo.createdAt = std::chrono::system_clock::now();
    }
    if (json.contains("completedAt")) {
        fileInfo.completedAt = std::chrono::system_clock::now();
    }
    if (json.contains("lastIntegrityCheck")) {
        fileInfo.lastIntegrityCheck = std::chrono::system_clock::now();
    }

    if (json.contains("durationSeconds")) fileInfo.durationSeconds = json["durationSeconds"];
    if (json.contains("isComplete")) fileInfo.isComplete = json["isComplete"];
    if (json.contains("width")) fileInfo.width = json["width"];
    if (json.contains("height")) fileInfo.height = json["height"];
    if (json.contains("segmentNumber")) fileInfo.segmentNumber = json["segmentNumber"];
    if (json.contains("isKeyframeAligned")) fileInfo.isKeyframeAligned = json["isKeyframeAligned"];
    if (json.contains("checksum")) fileInfo.checksum = json["checksum"];
    if (json.contains("integrityVerified")) fileInfo.integrityVerified = json["integrityVerified"];
    if (json.contains("checksumAlgorithm")) fileInfo.checksumAlgorithm = json["checksumAlgorithm"];
    if (json.contains("integrityStatus")) fileInfo.integrityStatus = json["integrityStatus"];
    if (json.contains("backupPath")) fileInfo.backupPath = json["backupPath"];
    if (json.contains("recoveryInfo")) fileInfo.recoveryInfo = json["recoveryInfo"];
    if (json.contains("auditLog")) fileInfo.auditLog = json["auditLog"];

    return fileInfo;
}

}  // namespace rtc
}  // namespace agora