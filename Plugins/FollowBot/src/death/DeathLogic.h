// DeathLogic.h - the pure decisions of the death task (design §5.8): when a
// death is confirmed and when it ends (counted once), which death-screen
// button to press, whether a button rectangle is clickable, the death limits.
// No SDK: tests/test_death.cpp.
#pragma once

#include "DeathSettings.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace fb::death {

// Control names in metadata/ui/ingamestate/deathscreen/deathscreen.ui
// (2026-10-20 unpack). That a UI element's control name equals its .ui id is
// INFERRED: Debug -> "Test death screen lookup" / "Find UI by control name"
// verify it live.
inline constexpr const char* kDeathScreen = "death_screen";
inline constexpr const char* kRespawnAtCheckpoint = "respawn_at_checkpoint_button";
inline constexpr const char* kReviveAtCheckpoint = "revive_at_checkpoint_button";   // green, hidden by default
inline constexpr const char* kRespawn = "respawn_button";   // text and meaning depend on the game state
inline constexpr const char* kSpectate = "spectate_button";  // never pressed (reported by the probe)

inline constexpr int kDeathScreenDepth = 12;   // search depth below the game UI root
inline constexpr int kButtonDepth = 6;         // search depth below death_screen

// Readable names for logs and the probe.
inline const char* ButtonLabel(const char* controlName) {
    const std::string n = controlName ? controlName : "";
    if (n == kRespawnAtCheckpoint) return "Respawn at checkpoint";
    if (n == kReviveAtCheckpoint) return "Revive at checkpoint";
    if (n == kRespawn) return "Respawn (generic)";
    if (n == kSpectate) return "Spectate";
    return "?";
}

// The buttons to try, in order; the first VISIBLE one is pressed.
// preferCheckpoint: the checkpoint buttons first, the generic respawn button
// last; off: the generic one first. The generic button is left out entirely
// unless allowGenericRespawn.
struct ButtonOrder {
    const char* names[3] = {nullptr, nullptr, nullptr};
    int         count = 0;
};

inline ButtonOrder RespawnButtonOrder(const DeathSettings& s) {
    ButtonOrder o;
    auto add = [&o](const char* n) { o.names[o.count++] = n; };
    if (s.allowGenericRespawn && !s.preferCheckpoint) add(kRespawn);
    add(kRespawnAtCheckpoint);
    add(kReviveAtCheckpoint);
    if (s.allowGenericRespawn && s.preferCheckpoint) add(kRespawn);
    return o;
}

// A rectangle (client pixels, Ui.ComputeScreenRect) the bot may click: finite,
// at least 4 x 4 px, its centre inside the client area when the size is known.
inline bool RectClickable(float x, float y, float w, float h, int screenW, int screenH) {
    if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(w) || !std::isfinite(h)) return false;
    if (w < 4.f || h < 4.f) return false;
    const float cx = x + w * 0.5f, cy = y + h * 0.5f;
    if (cx < 0.f || cy < 0.f) return false;
    if (screenW > 0 && cx >= static_cast<float>(screenW)) return false;
    if (screenH > 0 && cy >= static_cast<float>(screenH)) return false;
    return true;
}

// After a death was counted: whether a limit stops the bot, and why.
struct LimitVerdict {
    bool        stop = false;
    std::string reason;
};

inline LimitVerdict CheckDeathLimits(int deathsMap, int deathsSession, bool diedInMap, const DeathSettings& s) {
    LimitVerdict v;
    char buf[128];
    if (diedInMap && s.maxDeathsPerMap > 0 && deathsMap >= s.maxDeathsPerMap) {
        std::snprintf(buf, sizeof(buf), "Died %d %s in this map (limit %d)", deathsMap, deathsMap == 1 ? "time" : "times",
                      s.maxDeathsPerMap);
        v.stop = true;
        v.reason = buf;
        return v;
    }
    if (s.maxDeathsPerSession > 0 && deathsSession >= s.maxDeathsPerSession) {
        std::snprintf(buf, sizeof(buf), "Died %d %s since Start (limit %d)", deathsSession,
                      deathsSession == 1 ? "time" : "times", s.maxDeathsPerSession);
        v.stop = true;
        v.reason = buf;
    }
    return v;
}

// The per-map death counter counts per map INSTANCE: its area id plus the area
// hash (GameGate::areaHash = AreaInstance::GetAreaHash as "%08X"; "" or
// "00000000" = not read). A new instance = another area id, or the same id with
// a different known hash (a new waystone of the same map); an unknown hash on
// either side never splits an instance, so coming back through a portal keeps
// the count.
inline bool KnownAreaHash(const std::string& hash) { return !hash.empty() && hash != "00000000"; }

inline bool IsNewMapInstance(const std::string& lastId, const std::string& lastHash, const std::string& id,
                             const std::string& hash) {
    if (id != lastId) return true;
    return KnownAreaHash(hash) && KnownAreaHash(lastHash) && hash != lastHash;
}

// One death from the first life-0 read to being alive again. Fed with every
// tick the brain gets to see (the death task's WantsToStart and Tick): a death
// is confirmed after confirmMs of life 0, and ends - Revived, counted exactly
// once - at the first alive read after that, whoever respawned (the bot, you,
// a party member). Unread vitals change nothing.
class DeathTracker {
public:
    enum class Event : uint8_t { None = 0, Confirmed, Revived };

    Event Observe(bool valid, bool dead, uint64_t now, int confirmMs) {
        if (!valid) return Event::None;
        if (dead) {
            if (m_deadSinceMs == 0) m_deadSinceMs = now == 0 ? 1 : now;
            if (!m_confirmed && now >= m_deadSinceMs &&
                now - m_deadSinceMs >= static_cast<uint64_t>(confirmMs < 0 ? 0 : confirmMs)) {
                m_confirmed = true;
                m_confirmedMs = now;
                return Event::Confirmed;
            }
            return Event::None;
        }
        m_deadSinceMs = 0;
        if (m_confirmed) {
            m_confirmed = false;
            return Event::Revived;
        }
        return Event::None;
    }

    bool     Confirmed() const { return m_confirmed; }
    uint64_t DeadSinceMs() const { return m_deadSinceMs; }
    uint64_t ConfirmedMs() const { return m_confirmedMs; }
    void     Reset() { *this = DeathTracker{}; }

private:
    uint64_t m_deadSinceMs = 0;   // first life-0 read of this death (0 = alive)
    uint64_t m_confirmedMs = 0;
    bool     m_confirmed = false;
};

}  // namespace fb::death
