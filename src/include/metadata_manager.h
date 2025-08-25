#pragma once

#include <atomic>
#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../nlohmann/json.hpp"

namespace agora {
namespace rtc {

class MetadataManager {
   public:
    enum class FileType { JPEG, MP4, TS, M3U8 };
    enum class CompositionMode { Individual, Composite };
    enum class Layout { Flat, Spotlight, Freestyle, Customized };

    struct FileInfo {
        std::string filename;
        std::string fullPath;
        FileType type;
        uint64_t sizeBytes = 0;
        std::chrono::system_clock::time_point createdAt;
        std::chrono::system_clock::time_point completedAt;
        double durationSeconds = 0.0;  // For video/audio files
        bool isComplete = false;

        // Image-specific
        int width = 0;
        int height = 0;

        // TS segment-specific
        int segmentNumber = 0;
        bool isKeyframeAligned = false;

        // Integrity tracking
        std::string checksum;  // SHA-256 hash of file contents
        std::string checksumAlgorithm = "SHA-256";
        bool integrityVerified = false;
        std::string integrityStatus = "pending";  // pending, verified, failed, corrupted
        std::chrono::system_clock::time_point lastIntegrityCheck;

        // File recovery information
        std::string backupPath;    // Path to backup copy if available
        std::string recoveryInfo;  // Recovery instructions or status

        // Audit trail
        std::vector<std::string> auditLog;  // Timestamped operations on this file
    };

    struct TaskSession {
        std::string taskId;
        std::string description;
        std::string channel;
        std::vector<std::string> users;

        // Composition settings
        CompositionMode compositionMode = CompositionMode::Composite;
        Layout layout = Layout::Flat;

        // Technical specs
        int width = 0;
        int height = 0;
        int fps = 0;
        std::string videoCodec;
        int audioSampleRate = 0;
        int audioChannels = 0;
        std::string audioCodec;
        std::string outputFormat;

        // Session timeline
        std::chrono::system_clock::time_point sessionStartTime;
        std::chrono::system_clock::time_point sessionEndTime;
        double durationSeconds = 0.0;

        // TS-specific
        int tsSegmentDuration = 0;
        bool tsGeneratePlaylist = false;
        std::string playlistPath;

        // File tracking (thread-safe)
        std::vector<FileInfo> files;
        mutable std::mutex filesMutex;

        // Session metadata file path (set once during startSession)
        std::string outputFilePrefix;

        // Statistics
        std::atomic<int> totalFramesProcessed{0};
        std::atomic<int> droppedFrames{0};
        std::atomic<uint64_t> totalBytesGenerated{0};
        double averageFrameRate = 0.0;

        // Session state
        bool sessionCompleted = false;
        std::string terminationReason = "active";

        // Session integrity and audit
        std::string sessionIntegrityHash;          // Hash of all session data
        std::vector<std::string> sessionAuditLog;  // Timestamped session events

        // File validation summary
        int totalFilesExpected = 0;               // Total files expected in session
        int filesWithIntegrityIssues = 0;         // Count of files with integrity problems
        int missingFiles = 0;                     // Count of missing files
        std::vector<std::string> criticalErrors;  // List of critical errors
    };

    MetadataManager();
    ~MetadataManager();

    // Session lifecycle
    bool startSession(const std::string& taskId, const TaskSession& session,
                      const std::string& outputDir = "");
    bool endSession(const std::string& taskId, const std::string& terminationReason = "normal",
                    const std::string& outputDir = "");

    // File management - thread-safe operations
    bool addNewFileWithoutLock(TaskSession* session, const std::string& taskId,
                               const FileInfo& fileInfo);
    bool markFileCompleteWithoutLock(TaskSession* session, const std::string& taskId,
                                     const std::string& filename, uint64_t finalSize = 0,
                                     double duration = 0.0);

    // Persistent metadata with atomic operations
    bool saveSessionMetadataWithoutLock(TaskSession* session, const std::string& taskId,
                                        const std::string& outputFilePrefix);

    // Incremental updates for real-time file generation
    bool appendFileToMetadata(const std::string& taskId, const FileInfo& fileInfo,
                              const std::string& outputFilePrefix);

    // File naming utilities
    std::string generateOutputFilePrefixWithoutLock(const std::string& taskId) const;
    std::string generateMetadataPathWithoutLock(const std::string& outputFilePrefix) const;

    // Integrity and validation - enhanced
    bool validateFileIntegrityWithoutLock(TaskSession* session, const std::string& taskId,
                                          const std::string& filename);
    bool validateAllFilesIntegrityWithoutLock(TaskSession* session, const std::string& taskId);

    // SHA-256 checksum generation
    std::string calculateSHA256(const std::string& filepath);
    bool verifyFileChecksum(const std::string& filepath, const std::string& expectedChecksum);

    // Audit trail management
    void addAuditEntryWithoutLock(TaskSession* session, const std::string& taskId,
                                  const std::string& operation, const std::string& details = "");
    void addFileAuditEntryWithoutLock(TaskSession* session, const std::string& taskId,
                                      const std::string& filename, const std::string& operation,
                                      const std::string& details = "");
    std::vector<std::string> getAuditTrail(TaskSession* session, const std::string& taskId);

    // Internal serialization helpers

    // Cleanup operations
    void cleanupIncompleteFiles(const std::string& taskId);
    void removeOrphanedMetadata(const std::string& outputDir);

    // Utility conversions
    static std::string compositionModeToString(CompositionMode mode);
    static CompositionMode stringToCompositionMode(const std::string& str);
    static std::string layoutToString(Layout layout);
    static Layout stringToLayout(const std::string& str);
    static std::string fileTypeToString(FileType type);
    static FileType stringToFileType(const std::string& str);

   private:
    // Session storage
    std::map<std::string, std::unique_ptr<TaskSession>> activeSessions_;
    mutable std::mutex sessionsMutex_;

    // Atomic file operations
    bool atomicWriteMetadata(const std::string& filepath, const nlohmann::json& data);

    // JSON serialization
    nlohmann::json serializeSession(const TaskSession& session) const;
    void deserializeSessionInto(TaskSession* session, const nlohmann::json& json) const;
    nlohmann::json serializeFileInfo(const FileInfo& fileInfo) const;
    FileInfo deserializeFileInfo(const nlohmann::json& json) const;

    // Utilities
    std::string formatTimestamp(const std::chrono::system_clock::time_point& tp) const;
    std::string formatDateTimePrefix() const;
    FileType detectFileType(const std::string& filename) const;
    uint64_t getFileSize(const std::string& filepath) const;
    bool fileExists(const std::string& filepath) const;

    // Directory operations
    bool ensureDirectoryExists(const std::string& path);
    std::string getDirectoryFromPath(const std::string& filepath) const;

    // Concurrent access control
    std::mutex metadataWriteMutex_;  // Global mutex for metadata file writes
};

}  // namespace rtc
}  // namespace agora