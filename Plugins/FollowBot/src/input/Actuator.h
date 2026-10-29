// Actuator.h - every input the bot sends goes through here (design §5.3).
// Worker thread only.
//
// - Held-key tracker: every key / button the bot pressed and has not released;
//   ReleaseAll() lifts them all (stop, pause, gate hold, focus loss, fault,
//   disable). A key is never left held.
// - Global action gate: every server-relevant event (key press, click, chat
//   line) waits for min interval + uniform jitter since the previous one;
//   cursor moves are free.
// - Every event first checks that the game window itself is in front (not the
//   POEFixer overlay) and that no stop / pause / override is pending; a failed
//   check aborts the action and releases every held key.
// - Clicks get a centre-biased jitter; world clicks hover with a dwell first
//   (the game must register the hover) and may verify the hover through a
//   callback (Targetable.IsTargetedByPlayer).
// - The cursor busy channel is written while the bot clicks / aims, and read
//   (CursorBusyByOther) before cursor work.
#pragma once

#include "../common/Geometry.h"
#include "../common/Random.h"
#include "Coordination.h"

#include <Windows.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace fb {

class BotLog;

// Min interval + jitter between server-relevant events. Pure (tested).
class ActionGate {
public:
    void Configure(int minIntervalMs, int jitterMs) {
        m_minMs = minIntervalMs < 0 ? 0 : minIntervalMs;
        m_jitterMs = jitterMs < 0 ? 0 : jitterMs;
    }
    int MinIntervalMs() const { return m_minMs; }
    int JitterMs() const { return m_jitterMs; }
    bool Ready(uint64_t nowMs) const { return nowMs >= m_nextMs; }
    uint64_t NextMs() const { return m_nextMs; }
    // An event happened at `nowMs`; `jitterRollMs` in [0, jitter] (rolled by the caller).
    void Mark(uint64_t nowMs, int jitterRollMs) {
        const int roll = jitterRollMs < 0 ? 0 : (jitterRollMs > m_jitterMs ? m_jitterMs : jitterRollMs);
        m_nextMs = nowMs + static_cast<uint64_t>(m_minMs) + static_cast<uint64_t>(roll);
    }
    // Pushes the next allowed time out to at least nowMs + extraMs (a cast gate).
    void Extend(uint64_t nowMs, int extraMs) {
        const uint64_t t = nowMs + static_cast<uint64_t>(extraMs < 0 ? 0 : extraMs);
        if (t > m_nextMs) m_nextMs = t;
    }
    void Reset() { m_nextMs = 0; }

private:
    int      m_minMs = 100;
    int      m_jitterMs = 60;
    uint64_t m_nextMs = 0;
};

enum class ActResult : uint8_t {
    Ok = 0,
    Aborted,     // stop / pause / override pending, or the game is not in front: keys released
    NotReady,    // the action gate is not open yet (non-blocking calls)
    CursorBusy,  // another plugin owns the cursor right now
    Refused      // not allowed now (e.g. chat while a UI task runs) or bad arguments
};

const char* ActResultName(ActResult r);   // the result's key (lang/en.json)

struct ActuatorConfig {
    int   gateMinMs = 100;
    int   gateJitterMs = 60;
    int   clickJitterPx = 4;
    bool  humanMouse = true;
    float humanSpeed = 0.45f;
    float humanCurvature = 0.25f;
    float humanTremor = 0.2f;
};

class Actuator {
public:
    Actuator();
    ~Actuator();
    Actuator(const Actuator&) = delete;
    Actuator& operator=(const Actuator&) = delete;

    void Configure(const ActuatorConfig& c);
    const ActuatorConfig& Config() const { return m_cfg; }
    void SetLog(BotLog* log) { m_log = log; }

    // The game window (each tick) and the render thread's stop / pause test.
    void SetGameWindow(HWND game) { m_game = game; }
    HWND GameWindow() const { return m_game; }
    void SetAbortCheck(std::function<bool()> abort) { m_abort = std::move(abort); }

    // The game window itself is in front and nothing asks the bot to stop.
    bool CanAct() const;

    // --- held keys -------------------------------------------------------
    ActResult KeyDown(int vk);          // tracked; a mouse VK presses the button at the cursor
    void      KeyUp(int vk);
    bool      IsHeld(int vk) const;
    bool      AnyHeld() const { return !m_held.empty(); }
    void      ReleaseAll();             // lifts every tracked key and button (works without focus)

    // The move key (or the left button in click-to-move mode); not gated:
    // holding it is continuous movement, like a person holding the key.
    ActResult SetMoveKey(int vk, bool down);
    bool      MoveKeyHeld() const { return m_moveVk != 0 && IsHeld(m_moveVk); }
    int       MoveKeyVk() const { return m_moveVk; }

    // --- cursor (free, not gated) ---------------------------------------
    ActResult MoveCursor(const Vec2& client);
    ActResult MoveCursorHuman(const Vec2& client);   // trajectory, blocks, cancellable
    bool      HasCursor() const { return m_haveCursor; }
    Vec2      LastCursor() const { return m_cursor; }

    // --- the global action gate ----------------------------------------
    ActionGate& Gate() { return m_gate; }
    bool      GateReady() const;
    ActResult WaitGate(int maxWaitMs);  // blocks (cancellable) until the gate opens
    void      MarkGate();               // an event was just sent

    // --- gated events -----------------------------------------------------
    // Instant UI click (inventory cell, button) at a client point.
    ActResult UiClick(const Vec2& client, int vk = VK_LBUTTON, bool jitter = true);
    // World object click: move (human path when on), dwell, optional hover
    // verification with up to `reHovers` small re-hovers, then press-hold.
    ActResult WorldClick(const Vec2& client, int dwellMs, int holdMs, int vk,
                         const std::function<bool()>& verifyHover, int reHovers = 2);
    ActResult TapKey(int vk, int holdMs = 30);     // a mouse VK clicks at the cursor
    ActResult HoldKeyFor(int vk, int ms);          // press, hold `ms`, release
    ActResult PressAt(int vk, const Vec2& client, int holdMs);  // aim (instant) + press (skills)
    // Enter, the text, Enter. Refused while a UI task (stash) runs.
    ActResult TypeChatCommand(const std::string& utf8);
    void      SetUiTaskActive(bool on) { m_uiTask = on; }

    // Sleeps up to `ms`, polling the abort check every few ms; false = aborted.
    bool SleepCancellable(int ms);

    // --- coordination ---------------------------------------------------
    bool CursorBusyByOther() const { return m_busy.BusyByOther(); }
    void MarkCursorBusy(uint32_t ms) { m_busy.MarkBusy(ms); }
    void ReleaseCursorBusy() { m_busy.MarkIdle(); }

    Vec2     ClickJitter();
    Rng&     Random() { return m_rng; }
    uint64_t LastEventMs() const { return m_lastEventMs; }
    uint64_t EventCount() const { return m_events; }

private:
    ActResult Abort(const char* why);   // `why`: a key (lang/en.json) for the log
    void      Track(int vk, bool down);
    POINT     ToPoint(const Vec2& v) const;

    ActuatorConfig    m_cfg;
    BotLog*           m_log = nullptr;
    HWND              m_game = nullptr;
    std::function<bool()> m_abort;
    std::vector<int>  m_held;
    int               m_moveVk = 0;
    bool              m_haveCursor = false;
    Vec2              m_cursor;
    ActionGate        m_gate;
    Rng               m_rng;
    CursorBusyChannel m_busy;
    bool              m_uiTask = false;
    uint64_t          m_lastEventMs = 0;
    uint64_t          m_events = 0;
};

}  // namespace fb
