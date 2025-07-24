#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace agora {
namespace rtc {

class LayoutDetector {
   public:
    struct Config {
        uint64_t userTimeoutMs = 5000;           // Time before considering user left
        uint64_t layoutStabilityMs = 3000;       // Minimum time between layout changes
        uint64_t framePresenceTimeoutMs = 2000;  // Time without frames before user marked inactive

        // Layout strategy: if expectedUsers is set, only consider those users
        // If expectedUsers is empty, auto-detect any user that sends frames
        std::vector<std::string> expectedUsers;  // Pre-configured user list (empty = auto-detect)
        uint32_t maxUsers = 25;                  // Maximum users to track in auto-detect mode
    };

    enum class UserState {
        JOINING,  // User detected but not stable yet
        ACTIVE,   // User confirmed and receiving frames
        LEAVING,  // User stopped sending frames but not timed out
        LEFT      // User timed out and removed
    };

    struct UserInfo {
        std::string userId;
        UserState state = UserState::JOINING;
        uint64_t firstSeenMs = 0;
        uint64_t lastFrameMs = 0;
        uint64_t frameCount = 0;
        bool hasRecentFrames = false;

        UserInfo() = default;
        UserInfo(const std::string& id, uint64_t timestamp)
            : userId(id), firstSeenMs(timestamp), lastFrameMs(timestamp) {}
    };

    using LayoutChangeCallback = std::function<void(const std::vector<std::string>& activeUsers)>;

    LayoutDetector();
    ~LayoutDetector();

    bool initialize(const Config& config);
    void start();
    void stop();

    // Frame tracking
    std::vector<std::string> onUserFrame(const std::string& userId, uint64_t timestamp);

    // User management
    inline std::vector<std::string> getActiveUsersWithoutLock() const;
    std::vector<std::string> getActiveUsers() const;
    std::vector<std::string> getStableUsers() const;  // Users that should be in layout
    UserState getUserState(const std::string& userId) const;
    size_t getUserCount() const;
    bool shouldUpdateLayout() const;

    // Callbacks
    void setLayoutChangeCallback(LayoutChangeCallback callback);

    // Configuration
    void setExpectedUsers(const std::vector<std::string>& users);
    void addExpectedUser(const std::string& userId);
    void removeExpectedUser(const std::string& userId);

   private:
    void detectionThread();
    void updateUserStates();
    void checkLayoutChange();
    uint64_t getCurrentTimeMs() const;
    bool isUserStable(const UserInfo& user) const;
    bool shouldIncludeInLayout(const UserInfo& user) const;

    Config config_;
    std::atomic<bool> running_{false};
    std::unique_ptr<std::thread> detectionThread_;

    mutable std::mutex mutex_;
    std::map<std::string, UserInfo> users_;
    std::vector<std::string> currentLayout_;  // Current stable layout
    uint64_t lastLayoutChangeMs_ = 0;

    LayoutChangeCallback layoutCallback_;

    // For interruptible detection thread sleep
    std::mutex detectionMutex_;
    std::condition_variable detectionCv_;

    // Performance tracking
    uint64_t totalFramesProcessed_ = 0;
    uint64_t layoutChanges_ = 0;
};

}  // namespace rtc
}  // namespace agora