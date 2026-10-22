// DeathSettings.h - death detection and respawn (design §5.8): the settings
// struct and its JSON (per-field fallback + clamp).
#pragma once

#include "../common/JsonUtil.h"

namespace fb {

struct DeathSettings {
    bool autoRespawn = true;          // click a respawn button on the death screen
    bool preferCheckpoint = true;     // the checkpoint buttons before the generic respawn button
    bool allowGenericRespawn = true;  // the generic "respawn_button" may be pressed (its meaning depends on
                                      // the game state: town / hideout respawn, ...)
    int  deadConfirmMs = 300;         // HP 0 (with max HP > 0) this long = dead
    int  respawnDelayMs = 2500;       // wait before the first click
    int  respawnJitterMs = 800;       // + uniform 0..jitter
    int  retryIntervalMs = 2000;
    int  maxRetries = 5;              // death-screen lookups / clicks before "respawn manually"
    int  maxDeathsPerMap = 3;         // stop the bot at this many deaths in one map (0 = unlimited)
    int  maxDeathsPerSession = 0;     // stop the bot at this many deaths since Start (0 = unlimited)
};

inline void Clamp(DeathSettings& s) {
    s.deadConfirmMs = std::clamp(s.deadConfirmMs, 100, 3000);
    s.respawnDelayMs = std::clamp(s.respawnDelayMs, 500, 15000);
    s.respawnJitterMs = std::clamp(s.respawnJitterMs, 0, 5000);
    s.retryIntervalMs = std::clamp(s.retryIntervalMs, 500, 10000);
    s.maxRetries = std::clamp(s.maxRetries, 1, 20);
    s.maxDeathsPerMap = std::clamp(s.maxDeathsPerMap, 0, 50);
    s.maxDeathsPerSession = std::clamp(s.maxDeathsPerSession, 0, 500);
}

inline void ToJson(json::Json& j, const DeathSettings& s) {
    j = json::Json::object();
    j["auto_respawn"] = s.autoRespawn;
    j["prefer_checkpoint"] = s.preferCheckpoint;
    j["allow_generic_respawn"] = s.allowGenericRespawn;
    j["dead_confirm_ms"] = s.deadConfirmMs;
    j["respawn_delay_ms"] = s.respawnDelayMs;
    j["respawn_jitter_ms"] = s.respawnJitterMs;
    j["retry_interval_ms"] = s.retryIntervalMs;
    j["max_retries"] = s.maxRetries;
    j["max_deaths_per_map"] = s.maxDeathsPerMap;
    j["max_deaths_per_session"] = s.maxDeathsPerSession;
}

inline void FromJson(const json::Json& j, DeathSettings& s) {
    using json::Read;
    Read(j, "auto_respawn", s.autoRespawn);
    Read(j, "prefer_checkpoint", s.preferCheckpoint);
    Read(j, "allow_generic_respawn", s.allowGenericRespawn);
    Read(j, "dead_confirm_ms", s.deadConfirmMs);
    Read(j, "respawn_delay_ms", s.respawnDelayMs);
    Read(j, "respawn_jitter_ms", s.respawnJitterMs);
    Read(j, "retry_interval_ms", s.retryIntervalMs);
    Read(j, "max_retries", s.maxRetries);
    Read(j, "max_deaths_per_map", s.maxDeathsPerMap);
    Read(j, "max_deaths_per_session", s.maxDeathsPerSession);
    Clamp(s);
}

}  // namespace fb
