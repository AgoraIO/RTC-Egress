#pragma once
#include <string>
#include <vector>

#include "../nlohmann/json.hpp"

// UDSMessage defines the protocol for communication between Go (egress) and C++ (eg_worker)
struct UDSMessage {
    std::string cmd;                 // "snapshot", "record", "rtmp", or "whip"
    std::string action;              // "start", "release", "status"
    std::string layout;              // "grid", "flat", "spotlight", or "freestyle"
    std::string freestyleCanvasUrl;  // URL for custom canvas, used if layout is "freestyle"
    std::vector<std::string> uid;    // User IDs, if empty, all users will be included
    std::string channel;             // Channel Name
    std::string access_token;        // Access token for authentication
    int workerUid = 0;               // Worker UID
    int interval_in_ms = 0;          // Interval in milliseconds
};

inline void to_json(nlohmann::json& j, const UDSMessage& m) {
    j = nlohmann::json{{"cmd", m.cmd},
                       {"action", m.action},
                       {"layout", m.layout},
                       {"freestyleCanvasUrl", m.freestyleCanvasUrl},
                       {"uid", m.uid},
                       {"channel", m.channel},
                       {"access_token", m.access_token},
                       {"workerUid", m.workerUid},
                       {"interval_in_ms", m.interval_in_ms}};
}

inline void from_json(const nlohmann::json& j, UDSMessage& m) {
    j.at("cmd").get_to(m.cmd);
    if (j.contains("action"))
        j.at("action").get_to(m.action);
    else
        m.action = "start";
    if (j.contains("layout")) j.at("layout").get_to(m.layout);
    if (j.contains("freestyleCanvasUrl")) j.at("freestyleCanvasUrl").get_to(m.freestyleCanvasUrl);
    if (j.contains("uid")) j.at("uid").get_to(m.uid);
    j.at("channel").get_to(m.channel);
    j.at("access_token").get_to(m.access_token);
    if (j.contains("workerUid")) j.at("workerUid").get_to(m.workerUid);
    if (j.contains("interval_in_ms")) j.at("interval_in_ms").get_to(m.interval_in_ms);
}