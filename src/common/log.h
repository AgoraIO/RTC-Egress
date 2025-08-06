#include <cstdio>
#include <chrono>
#include <ctime>
#include <cstdlib>
#include <cstring>

enum {ERROR=-1, INFO=0, WARNING, FATAL, WARN=WARNING};

// Time format configuration
enum AG_LOG_TIME_FORMAT {
    AG_TIME_NONE = 0,        // No timestamp
    AG_TIME_SHORT,           // HH:MM:SS.mmm
    AG_TIME_FULL,            // YYYY-MM-DD HH:MM:SS.mmm
    AG_TIME_UNIX,            // Unix timestamp with milliseconds
    AG_TIME_ISO8601,         // 2025-01-06T21:35:17.123Z
    AG_TIME_CUSTOM           // Custom format via AG_LOG_TIME_CUSTOM
};

// Global configuration (can be set via environment variables)
extern "C" {
    // Default values - can be overridden
    static AG_LOG_TIME_FORMAT ag_log_time_format = AG_TIME_SHORT;
    static bool ag_log_use_colors = false;
    static const char* ag_log_custom_format = "%H:%M:%S";
}

// Configuration functions
inline void ag_log_set_time_format(AG_LOG_TIME_FORMAT format) {
    ag_log_time_format = format;
}

inline void ag_log_set_colors(bool enable) {
    ag_log_use_colors = enable;
}

inline void ag_log_set_custom_format(const char* format) {
    ag_log_custom_format = format;
}

// Initialize from environment variables
inline void ag_log_init() {
    const char* time_fmt = getenv("AG_LOG_TIME_FORMAT");
    if (time_fmt) {
        if (strcmp(time_fmt, "none") == 0) ag_log_time_format = AG_TIME_NONE;
        else if (strcmp(time_fmt, "short") == 0) ag_log_time_format = AG_TIME_SHORT;
        else if (strcmp(time_fmt, "full") == 0) ag_log_time_format = AG_TIME_FULL;
        else if (strcmp(time_fmt, "unix") == 0) ag_log_time_format = AG_TIME_UNIX;
        else if (strcmp(time_fmt, "iso8601") == 0) ag_log_time_format = AG_TIME_ISO8601;
        else if (strcmp(time_fmt, "custom") == 0) ag_log_time_format = AG_TIME_CUSTOM;
    }
    
    const char* colors = getenv("AG_LOG_COLORS");
    if (colors && (strcmp(colors, "1") == 0 || strcmp(colors, "true") == 0)) {
        ag_log_use_colors = true;
    }
    
    const char* custom_fmt = getenv("AG_LOG_CUSTOM_FORMAT");
    if (custom_fmt) {
        ag_log_custom_format = custom_fmt;
    }
}

// Color codes for different log levels
#define AG_COLOR_RESET   "\033[0m"
#define AG_COLOR_RED     "\033[31m"  // ERROR
#define AG_COLOR_YELLOW  "\033[33m"  // WARNING
#define AG_COLOR_GREEN   "\033[32m"  // INFO
#define AG_COLOR_MAGENTA "\033[35m"  // FATAL

// Get color for log level
inline const char* ag_get_level_color(int level) {
    if (!ag_log_use_colors) return "";
    switch(level) {
        case ERROR: return AG_COLOR_RED;
        case WARNING: return AG_COLOR_YELLOW;
        case INFO: return AG_COLOR_GREEN;
        case FATAL: return AG_COLOR_MAGENTA;
        default: return "";
    }
}

// Format timestamp based on configuration
inline void ag_format_timestamp(char* buf, size_t buf_size) {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    struct tm* tm = std::localtime(&time_t_now);
    
    switch (ag_log_time_format) {
        case AG_TIME_NONE:
            buf[0] = '\0';
            break;
        case AG_TIME_SHORT:
            snprintf(buf, buf_size, "%02d:%02d:%02d.%03d", 
                    tm->tm_hour, tm->tm_min, tm->tm_sec, (int)ms.count());
            break;
        case AG_TIME_FULL:
            snprintf(buf, buf_size, "%04d-%02d-%02d %02d:%02d:%02d.%03d",
                    tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                    tm->tm_hour, tm->tm_min, tm->tm_sec, (int)ms.count());
            break;
        case AG_TIME_UNIX:
            snprintf(buf, buf_size, "%ld.%03d", time_t_now, (int)ms.count());
            break;
        case AG_TIME_ISO8601:
            snprintf(buf, buf_size, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                    tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                    tm->tm_hour, tm->tm_min, tm->tm_sec, (int)ms.count());
            break;
        case AG_TIME_CUSTOM:
            strftime(buf, buf_size, ag_log_custom_format, tm);
            // Add milliseconds if format contains %f placeholder
            if (strstr(ag_log_custom_format, "%f")) {
                char temp[64];
                snprintf(temp, sizeof(temp), "%03d", (int)ms.count());
                // Simple %f replacement
                char* pos = strstr(buf, "%f");
                if (pos) {
                    memmove(pos + 3, pos + 2, strlen(pos + 2) + 1);
                    memcpy(pos, temp, 3);
                }
            }
            break;
    }
}

// Fast timestamp formatting with thread-local caching
inline void ag_format_timestamp_fast(char* buf, size_t buf_size) {
    static thread_local char cached_time[64];
    static thread_local time_t last_sec = 0;
    static thread_local bool cache_valid = false;
    
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()) % 1000;
    
    if (ag_log_time_format == AG_TIME_NONE) {
        buf[0] = '\0';
        return;
    }
    
    // Cache the time string if seconds haven't changed
    if (!cache_valid || time_t_now != last_sec) {
        struct tm* tm = std::localtime(&time_t_now);
        switch (ag_log_time_format) {
            case AG_TIME_SHORT:
                snprintf(cached_time, sizeof(cached_time), "%02d:%02d:%02d", 
                        tm->tm_hour, tm->tm_min, tm->tm_sec);
                break;
            case AG_TIME_FULL:
                snprintf(cached_time, sizeof(cached_time), "%04d-%02d-%02d %02d:%02d:%02d",
                        tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
                        tm->tm_hour, tm->tm_min, tm->tm_sec);
                break;
            default:
                // For other formats, use the full formatter
                ag_format_timestamp(buf, buf_size);
                return;
        }
        last_sec = time_t_now;
        cache_valid = true;
    }
    
    // Add milliseconds
    snprintf(buf, buf_size, "%s.%03d", cached_time, (int)ms.count());
}

// Enhanced logging macros with tag support

// Set default tag for a file - define this before including log.h
#ifndef AG_LOG_TAG
#define AG_LOG_TAG ""
#endif

// Original AG_LOG (no timestamp) - now with tag support
#define AG_LOG(level, format, ...) do { \
    const char* color = ag_get_level_color(level); \
    const char* reset = ag_log_use_colors ? AG_COLOR_RESET : ""; \
    if (strlen(AG_LOG_TAG) > 0) { \
        fprintf(stderr, "%s[%s][" #level "]%s " format "\n", \
                color, AG_LOG_TAG, reset, ##__VA_ARGS__); \
    } else { \
        fprintf(stderr, "%s[EGRESS_" #level "]%s " format "\n", \
                color, reset, ##__VA_ARGS__); \
    } \
} while(0)

// Enhanced AG_LOG with timestamp and tag
#define AG_LOG_TS(level, format, ...) do { \
    char time_buf[64]; \
    ag_format_timestamp(time_buf, sizeof(time_buf)); \
    const char* color = ag_get_level_color(level); \
    const char* reset = ag_log_use_colors ? AG_COLOR_RESET : ""; \
    if (strlen(time_buf) > 0 && strlen(AG_LOG_TAG) > 0) { \
        fprintf(stderr, "%s[%s][%s][" #level "]%s " format "\n", \
                color, time_buf, AG_LOG_TAG, reset, ##__VA_ARGS__); \
    } else if (strlen(time_buf) > 0) { \
        fprintf(stderr, "%s[%s][EGRESS_" #level "]%s " format "\n", \
                color, time_buf, reset, ##__VA_ARGS__); \
    } else if (strlen(AG_LOG_TAG) > 0) { \
        fprintf(stderr, "%s[%s][" #level "]%s " format "\n", \
                color, AG_LOG_TAG, reset, ##__VA_ARGS__); \
    } else { \
        fprintf(stderr, "%s[EGRESS_" #level "]%s " format "\n", \
                color, reset, ##__VA_ARGS__); \
    } \
} while(0)

// High-performance version with tag support
#define AG_LOG_FAST(level, format, ...) do { \
    char time_buf[64]; \
    ag_format_timestamp_fast(time_buf, sizeof(time_buf)); \
    const char* color = ag_get_level_color(level); \
    const char* reset = ag_log_use_colors ? AG_COLOR_RESET : ""; \
    if (strlen(time_buf) > 0 && strlen(AG_LOG_TAG) > 0) { \
        fprintf(stderr, "%s[%s][%s][" #level "]%s " format "\n", \
                color, time_buf, AG_LOG_TAG, reset, ##__VA_ARGS__); \
    } else if (strlen(time_buf) > 0) { \
        fprintf(stderr, "%s[%s][EGRESS_" #level "]%s " format "\n", \
                color, time_buf, reset, ##__VA_ARGS__); \
    } else if (strlen(AG_LOG_TAG) > 0) { \
        fprintf(stderr, "%s[%s][" #level "]%s " format "\n", \
                color, AG_LOG_TAG, reset, ##__VA_ARGS__); \
    } else { \
        fprintf(stderr, "%s[EGRESS_" #level "]%s " format "\n", \
                color, reset, ##__VA_ARGS__); \
    } \
} while(0)

// Convenience macros for specific tags (can be used anywhere)
#define AG_LOG_WITH_TAG(tag, level, format, ...) do { \
    const char* color = ag_get_level_color(level); \
    const char* reset = ag_log_use_colors ? AG_COLOR_RESET : ""; \
    fprintf(stderr, "%s[%s][" #level "]%s " format "\n", \
            color, tag, reset, ##__VA_ARGS__); \
} while(0)

#define AG_LOG_TS_WITH_TAG(tag, level, format, ...) do { \
    char time_buf[64]; \
    ag_format_timestamp(time_buf, sizeof(time_buf)); \
    const char* color = ag_get_level_color(level); \
    const char* reset = ag_log_use_colors ? AG_COLOR_RESET : ""; \
    if (strlen(time_buf) > 0) { \
        fprintf(stderr, "%s[%s][%s][" #level "]%s " format "\n", \
                color, time_buf, tag, reset, ##__VA_ARGS__); \
    } else { \
        fprintf(stderr, "%s[%s][" #level "]%s " format "\n", \
                color, tag, reset, ##__VA_ARGS__); \
    } \
} while(0)

// Auto-initialization (called once per program)
static bool ag_log_initialized = false;
inline void ag_log_auto_init() {
    if (!ag_log_initialized) {
        ag_log_init();
        ag_log_initialized = true;
    }
}

// Call this in main() or at program start
#define AG_LOG_INIT() ag_log_auto_init() 
