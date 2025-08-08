#pragma once

#include <chrono>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace agora {
namespace rtc {

class TSSegmentManager {
   public:
    struct Config {
        std::string outputDir = "./ts_recordings";
        int segmentDurationSeconds = 10;
        int maxSegmentsPerSession = 360;
        bool generatePlaylist = true;
        bool keepIncompleteSegments = true;
        uint32_t videoWidth = 1280;
        uint32_t videoHeight = 720;
        int videoFps = 30;
    };

    struct SegmentInfo {
        std::string filename;
        std::string tempFilename;
        std::chrono::steady_clock::time_point startTime;
        double duration = 0.0;
        int segmentNumber = 0;
        bool isComplete = false;
    };

    struct SessionInfo {
        std::string sessionId;
        std::string sessionDir;
        std::vector<SegmentInfo> segments;
        std::chrono::steady_clock::time_point sessionStartTime;
        int currentSegmentNumber = 1;
        double totalDuration = 0.0;
    };

    TSSegmentManager();
    ~TSSegmentManager();

    bool initialize(const Config& config);
    void cleanup();

    // Session management
    bool startNewSession();
    void endCurrentSession();
    const SessionInfo& getCurrentSession() const {
        return currentSession_;
    }

    // Segment management
    bool shouldRotateSegment(const std::chrono::steady_clock::time_point& currentTime,
                             bool hasKeyframe = false) const;
    bool rotateToNewSegment();
    std::string getCurrentSegmentPath() const;
    std::string getCurrentTempSegmentPath() const;
    bool finalizeCurrentSegment();

    // Playlist generation
    void generateHLSPlaylist();
    void updatePlaylistWithNewSegment(const SegmentInfo& segment);

    // Utilities
    std::string generateSessionId() const;
    std::string generateSegmentFilename(int segmentNumber) const;
    bool createSessionDirectory();

    // Access methods for metadata tracking
    const std::vector<SegmentInfo>& getCompletedSegments() const {
        return currentSession_.segments;
    }

   private:
    Config config_;
    SessionInfo currentSession_;
    SegmentInfo currentSegment_;
    std::mutex mutex_;

    // Playlist management
    std::string playlistPath_;
    std::ofstream playlistFile_;

    // Helper methods
    bool ensureDirectoryExists(const std::string& path);
    void cleanupOldSessions();
    void safeRename(const std::string& tempPath, const std::string& finalPath);
};

}  // namespace rtc
}  // namespace agora