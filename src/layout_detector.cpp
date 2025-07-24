#include "include/layout_detector.h"

#include <algorithm>
#include <iostream>

#define MUTEX_DEBUG

// Debug macro for mutex locking
#ifdef MUTEX_DEBUG
#define LOCK_GUARD(mutex_var)                                                         \
    std::cout << "[" << std::this_thread::get_id() << "] [" << __FUNCTION__ << "] ["  \
              << std::chrono::duration_cast<std::chrono::milliseconds>(               \
                     std::chrono::steady_clock::now().time_since_epoch())             \
                     .count()                                                         \
              << "ms] LOCKING mutex at " << __FILE__ << ":" << __LINE__ << std::endl; \
    std::lock_guard<std::mutex> lock(mutex_var);                                      \
    std::cout << "[" << std::this_thread::get_id() << "] [" << __FUNCTION__ << "] ["  \
              << std::chrono::duration_cast<std::chrono::milliseconds>(               \
                     std::chrono::steady_clock::now().time_since_epoch())             \
                     .count()                                                         \
              << "ms] LOCKED mutex" << std::endl
#else
#define LOCK_GUARD(mutex_var) std::lock_guard<std::mutex> lock(mutex_var)
#endif

namespace agora {
namespace rtc {

LayoutDetector::LayoutDetector() {}

LayoutDetector::~LayoutDetector() {
    stop();
}

bool LayoutDetector::initialize(const Config& config) {
    if (running_.load()) {
        std::cerr << "[LayoutDetector] Cannot initialize while running" << std::endl;
        return false;
    }

    LOCK_GUARD(mutex_);

    config_ = config;

    // Pre-populate expected users if provided
    if (!config_.expectedUsers.empty()) {
        uint64_t currentTime = getCurrentTimeMs();
        for (const auto& userId : config_.expectedUsers) {
            users_[userId] = UserInfo(userId, currentTime);
            users_[userId].state = UserState::JOINING;
        }
        std::cout << "[LayoutDetector] Pre-populated " << config_.expectedUsers.size()
                  << " expected users (only these will be considered)" << std::endl;
    } else {
        std::cout << "[LayoutDetector] Auto-detect mode: will consider any user that sends frames"
                  << std::endl;
    }

    std::cout << "[LayoutDetector] Initialized with config:"
              << " userTimeout=" << config_.userTimeoutMs << "ms"
              << " layoutStability=" << config_.layoutStabilityMs << "ms"
              << " maxUsers=" << config_.maxUsers << std::endl;

    return true;
}

void LayoutDetector::start() {
    LOCK_GUARD(mutex_);

    if (running_.load()) {
        std::cout << "[LayoutDetector] Already running" << std::endl;
        return;
    }

    running_ = true;
    detectionThread_ = std::make_unique<std::thread>(&LayoutDetector::detectionThread, this);

    std::cout << "[LayoutDetector] Started detection thread" << std::endl;
}

void LayoutDetector::stop() {
    {
        LOCK_GUARD(mutex_);
        if (!running_.load()) {
            return;
        }
        running_ = false;
    }

    // Wake up the detection thread immediately
    detectionCv_.notify_all();

    if (detectionThread_ && detectionThread_->joinable()) {
        detectionThread_->join();
        detectionThread_.reset();
    }

    std::cout << "[LayoutDetector] Stopped. Stats: " << totalFramesProcessed_
              << " frames processed, " << layoutChanges_ << " layout changes" << std::endl;
}

std::vector<std::string> LayoutDetector::onUserFrame(const std::string& userId,
                                                     uint64_t timestamp) {
    // Early exit if not running to prevent deadlocks during shutdown
    if (!running_.load()) {
        return {};
    }

    std::cout << "[LayoutDetector] onUserFrame() ENTRY, userId=" << userId << std::endl;
    std::flush(std::cout);

    if (userId.empty()) {
        std::cout << "[LayoutDetector] onUserFrame() early return - empty userId" << std::endl;
        std::flush(std::cout);
        return {};
    }

    std::cout << "[LayoutDetector] onUserFrame() about to acquire mutex_" << std::endl;
    std::flush(std::cout);

    LOCK_GUARD(mutex_);

    std::cout << "[LayoutDetector] onUserFrame() acquired mutex_" << std::endl;
    std::flush(std::cout);

    totalFramesProcessed_++;

    auto it = users_.find(userId);
    if (it == users_.end()) {
        // New user detected
        if (!config_.expectedUsers.empty()) {
            // Expected users mode: only accept expected users
            bool isExpected = std::find(config_.expectedUsers.begin(), config_.expectedUsers.end(),
                                        userId) != config_.expectedUsers.end();
            if (!isExpected) {
                return {};
            }
        } else {
            // Auto-detect mode: accept any user up to limit
            if (users_.size() >= config_.maxUsers) {
                return {};
            }
        }

        users_[userId] = UserInfo(userId, timestamp);
        std::cout << "[LayoutDetector] New user: " << userId << std::endl;
    } else {
        // Update existing user
        it->second.lastFrameMs = timestamp;
        it->second.frameCount++;
        it->second.hasRecentFrames = true;
    }

    std::cout << "[LayoutDetector] onUserFrame() EXIT, userId=" << userId << std::endl;
    std::flush(std::cout);

    return getActiveUsersWithoutLock();
}

inline std::vector<std::string> LayoutDetector::getActiveUsersWithoutLock() const {
    std::vector<std::string> activeUsers;
    for (const auto& pair : users_) {
        if (pair.second.state == UserState::ACTIVE || pair.second.state == UserState::JOINING) {
            activeUsers.push_back(pair.first);
        }
    }

    return activeUsers;
}

std::vector<std::string> LayoutDetector::getActiveUsers() const {
    LOCK_GUARD(mutex_);

    return getActiveUsersWithoutLock();
}

std::vector<std::string> LayoutDetector::getStableUsers() const {
    LOCK_GUARD(mutex_);
    return currentLayout_;
}

LayoutDetector::UserState LayoutDetector::getUserState(const std::string& userId) const {
    LOCK_GUARD(mutex_);

    auto it = users_.find(userId);
    if (it == users_.end()) {
        return UserState::LEFT;
    }

    return it->second.state;
}

size_t LayoutDetector::getUserCount() const {
    LOCK_GUARD(mutex_);

    size_t count = 0;
    for (const auto& pair : users_) {
        if (pair.second.state != UserState::LEFT) {
            count++;
        }
    }

    return count;
}

bool LayoutDetector::shouldUpdateLayout() const {
    LOCK_GUARD(mutex_);

    uint64_t currentTime = getCurrentTimeMs();
    return (currentTime - lastLayoutChangeMs_) >= config_.layoutStabilityMs;
}

void LayoutDetector::setLayoutChangeCallback(LayoutChangeCallback callback) {
    LOCK_GUARD(mutex_);
    layoutCallback_ = std::move(callback);
}

void LayoutDetector::setExpectedUsers(const std::vector<std::string>& users) {
    LOCK_GUARD(mutex_);

    config_.expectedUsers = users;

    // Add new expected users that aren't already tracked
    uint64_t currentTime = getCurrentTimeMs();
    for (const auto& userId : users) {
        if (users_.find(userId) == users_.end()) {
            users_[userId] = UserInfo(userId, currentTime);
            users_[userId].state = UserState::JOINING;
        }
    }
}

void LayoutDetector::addExpectedUser(const std::string& userId) {
    LOCK_GUARD(mutex_);

    if (std::find(config_.expectedUsers.begin(), config_.expectedUsers.end(), userId) ==
        config_.expectedUsers.end()) {
        config_.expectedUsers.push_back(userId);

        if (users_.find(userId) == users_.end()) {
            uint64_t currentTime = getCurrentTimeMs();
            users_[userId] = UserInfo(userId, currentTime);
            users_[userId].state = UserState::JOINING;
        }
    }
}

void LayoutDetector::removeExpectedUser(const std::string& userId) {
    LOCK_GUARD(mutex_);

    auto it = std::find(config_.expectedUsers.begin(), config_.expectedUsers.end(), userId);
    if (it != config_.expectedUsers.end()) {
        config_.expectedUsers.erase(it);
    }

    // Mark user as leaving if not already left
    auto userIt = users_.find(userId);
    if (userIt != users_.end() && userIt->second.state != UserState::LEFT) {
        userIt->second.state = UserState::LEAVING;
    }
}

void LayoutDetector::detectionThread() {
    std::cout << "[LayoutDetector] Detection thread started" << std::endl;

    while (running_.load()) {
        updateUserStates();
        checkLayoutChange();

        // Use interruptible wait instead of uninterruptible sleep
        std::unique_lock<std::mutex> lock(detectionMutex_);
        detectionCv_.wait_for(lock, std::chrono::milliseconds(500),
                              [this] { return !running_.load(); });

        if (!running_.load()) {
            break;
        }
    }

    std::cout << "[LayoutDetector] Detection thread stopped" << std::endl;
}

void LayoutDetector::updateUserStates() {
    // Exit immediately if we're shutting down to avoid blocking other threads
    if (!running_.load()) {
        return;
    }

    LOCK_GUARD(mutex_);

    uint64_t currentTime = getCurrentTimeMs();

    for (auto& pair : users_) {
        UserInfo& user = pair.second;

        // Check for frame timeout
        bool hasRecentFrames = (currentTime - user.lastFrameMs) < config_.framePresenceTimeoutMs;

        switch (user.state) {
            case UserState::JOINING:
                if (hasRecentFrames && isUserStable(user)) {
                    user.state = UserState::ACTIVE;
                    std::cout << "[LayoutDetector] User " << user.userId << " is now ACTIVE"
                              << std::endl;
                } else if (!hasRecentFrames &&
                           (currentTime - user.firstSeenMs) > config_.userTimeoutMs) {
                    user.state = UserState::LEFT;
                    std::cout << "[LayoutDetector] User " << user.userId
                              << " LEFT (timeout during joining)" << std::endl;
                }
                break;

            case UserState::ACTIVE:
                if (!hasRecentFrames) {
                    user.state = UserState::LEAVING;
                    std::cout << "[LayoutDetector] User " << user.userId
                              << " is LEAVING (no recent frames)" << std::endl;
                }
                break;

            case UserState::LEAVING:
                if (hasRecentFrames) {
                    user.state = UserState::ACTIVE;
                    std::cout << "[LayoutDetector] User " << user.userId << " returned to ACTIVE"
                              << std::endl;
                } else if ((currentTime - user.lastFrameMs) > config_.userTimeoutMs) {
                    user.state = UserState::LEFT;
                    std::cout << "[LayoutDetector] User " << user.userId << " LEFT (timeout)"
                              << std::endl;
                }
                break;

            case UserState::LEFT:
                // User is gone, no state changes
                break;
        }

        // Reset frame tracking flag
        user.hasRecentFrames = false;
    }

    // Clean up users that have been LEFT for too long
    auto it = users_.begin();
    while (it != users_.end()) {
        if (it->second.state == UserState::LEFT &&
            (currentTime - it->second.lastFrameMs) > (config_.userTimeoutMs * 2)) {
            std::cout << "[LayoutDetector] Removing user " << it->first << " from tracking"
                      << std::endl;
            it = users_.erase(it);
        } else {
            ++it;
        }
    }
}

void LayoutDetector::checkLayoutChange() {
    // Exit immediately if we're shutting down to avoid blocking other threads
    if (!running_.load()) {
        return;
    }

    LOCK_GUARD(mutex_);
    uint64_t currentTime = getCurrentTimeMs();

    // Don't change layout too frequently
    if ((currentTime - lastLayoutChangeMs_) < config_.layoutStabilityMs) {
        return;
    }

    // Build new layout from stable users
    std::vector<std::string> newLayout;
    for (const auto& pair : users_) {
        if (shouldIncludeInLayout(pair.second)) {
            newLayout.push_back(pair.first);
        }
    }

    // Sort for consistent ordering
    std::sort(newLayout.begin(), newLayout.end());

    // Check if layout has changed
    if (newLayout != currentLayout_) {
        currentLayout_ = newLayout;
        lastLayoutChangeMs_ = currentTime;
        layoutChanges_++;

        std::cout << "[LayoutDetector] Layout changed to " << newLayout.size() << " users: ";
        for (const auto& userId : newLayout) {
            std::cout << userId << " ";
        }
        std::cout << std::endl;

        // Notify callback
        if (layoutCallback_) {
            layoutCallback_(currentLayout_);
        }

        std::cout << "[LayoutDetector] Layout callback done";
    }
}

uint64_t LayoutDetector::getCurrentTimeMs() const {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool LayoutDetector::isUserStable(const UserInfo& user) const {
    uint64_t currentTime = getCurrentTimeMs();

    // User is stable if:
    // 1. They've been present for minimum time
    // 2. They have sufficient frame count
    // 3. They have recent frames

    uint64_t presenceTime = currentTime - user.firstSeenMs;
    bool hasMinPresence =
        presenceTime >= (config_.layoutStabilityMs / 3);  // 1/3 of layout stability time
    bool hasMinFrames = user.frameCount >= 5;             // At least 5 frames
    bool hasRecentActivity = (currentTime - user.lastFrameMs) < config_.framePresenceTimeoutMs;

    return hasMinPresence && hasMinFrames && hasRecentActivity;
}

bool LayoutDetector::shouldIncludeInLayout(const UserInfo& user) const {
    // Simple logic: include users that are ACTIVE or stable JOINING users
    return user.state == UserState::ACTIVE ||
           (user.state == UserState::JOINING && isUserStable(user));
}

}  // namespace rtc
}  // namespace agora