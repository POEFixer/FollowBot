// Actuator.cpp - see Actuator.h.
#include "Actuator.h"

#include "../common/BotLog.h"
#include "../common/Clock.h"
#include "../common/Strings.h"
#include "HumanMouse.h"
#include "Win32Input.h"

#include <algorithm>
#include <cmath>

namespace fb {

const char* ActResultName(ActResult r) {
    switch (r) {
        case ActResult::Ok:         return Key("act_ok");
        case ActResult::Aborted:    return Key("act_aborted");
        case ActResult::NotReady:   return Key("act_not_ready");
        case ActResult::CursorBusy: return Key("act_cursor_busy");
        case ActResult::Refused:    return Key("act_refused");
    }
    return Key("common_unknown");
}

Actuator::Actuator() = default;

Actuator::~Actuator() { ReleaseAll(); }

void Actuator::Configure(const ActuatorConfig& c) {
    m_cfg = c;
    m_gate.Configure(c.gateMinMs, c.gateJitterMs);
}

bool Actuator::CanAct() const {
    if (m_abort && m_abort()) return false;
    return win::IsForeground(m_game);
}

ActResult Actuator::Abort(const char* why) {
    if (m_log && AnyHeld()) m_log->Add(LogCat::Input, TrF("log_input_aborted", {{"why", Tr(why)}}));
    ReleaseAll();
    return ActResult::Aborted;
}

POINT Actuator::ToPoint(const Vec2& v) const {
    return POINT{static_cast<LONG>(std::lround(v.x)), static_cast<LONG>(std::lround(v.y))};
}

void Actuator::Track(int vk, bool down) {
    const auto it = std::find(m_held.begin(), m_held.end(), vk);
    if (down) {
        if (it == m_held.end()) m_held.push_back(vk);
    } else if (it != m_held.end()) {
        m_held.erase(it);
    }
}

bool Actuator::IsHeld(int vk) const { return std::find(m_held.begin(), m_held.end(), vk) != m_held.end(); }

ActResult Actuator::KeyDown(int vk) {
    if (vk <= 0 || vk > 0xFE) return ActResult::Refused;
    if (!CanAct()) return Abort(Key("input_key_down"));
    if (IsHeld(vk)) return ActResult::Ok;
    POINT p = ToPoint(m_cursor);
    if (!win::VkEvent(vk, true, m_game, win::IsMouseVk(vk) && m_haveCursor ? &p : nullptr)) return ActResult::Refused;
    Track(vk, true);
    m_lastEventMs = NowMs();
    ++m_events;
    return ActResult::Ok;
}

void Actuator::KeyUp(int vk) {
    if (!IsHeld(vk)) return;
    win::VkEvent(vk, false, m_game, nullptr);
    Track(vk, false);
}

void Actuator::ReleaseAll() {
    const std::vector<int> held = m_held;
    for (auto it = held.rbegin(); it != held.rend(); ++it) win::VkEvent(*it, false, m_game, nullptr);
    m_held.clear();
    m_busy.MarkIdle();
}

ActResult Actuator::SetMoveKey(int vk, bool down) {
    if (!down) {
        if (m_moveVk != 0) KeyUp(m_moveVk);
        return ActResult::Ok;
    }
    if (vk <= 0) return ActResult::Refused;
    if (m_moveVk != 0 && m_moveVk != vk) KeyUp(m_moveVk);
    m_moveVk = vk;
    if (IsHeld(vk)) return CanAct() ? ActResult::Ok : Abort(Key("input_move_key"));
    return KeyDown(vk);
}

ActResult Actuator::MoveCursor(const Vec2& client) {
    if (!CanAct()) return Abort(Key("input_cursor_move"));
    if (!IsFinite(client)) return ActResult::Refused;
    win::MoveCursorClient(m_game, static_cast<int>(std::lround(client.x)), static_cast<int>(std::lround(client.y)));
    m_cursor = client;
    m_haveCursor = true;
    return ActResult::Ok;
}

ActResult Actuator::MoveCursorHuman(const Vec2& client) {
    if (!CanAct()) return Abort(Key("input_cursor_move"));
    if (!IsFinite(client)) return ActResult::Refused;
    if (!m_cfg.humanMouse) return MoveCursor(client);
    POINT from{};
    if (!win::GetCursorDesktop(from)) return MoveCursor(client);
    const POINT to = win::ClientToDesktop(m_game, static_cast<int>(std::lround(client.x)),
                                          static_cast<int>(std::lround(client.y)));
    const human::Config hc = human::MakeConfig(m_cfg.humanSpeed, m_cfg.humanCurvature, m_cfg.humanTremor);
    const auto traj = human::Generate(from.x, from.y, to.x, to.y, hc, m_rng.Engine()());
    MarkCursorBusy(1500);
    if (!human::Replay(traj, [this] { return !CanAct(); })) return Abort(Key("input_cursor_move"));
    m_cursor = client;
    m_haveCursor = true;
    return ActResult::Ok;
}

bool Actuator::GateReady() const { return m_gate.Ready(NowMs()); }

void Actuator::MarkGate() {
    const uint64_t now = NowMs();
    m_gate.Mark(now, m_rng.UniformInt(0, m_gate.JitterMs()));
    m_lastEventMs = now;
    ++m_events;
}

bool Actuator::SleepCancellable(int ms) {
    if (ms <= 0) return CanAct();
    const uint64_t end = NowMs() + static_cast<uint64_t>(ms);
    for (;;) {
        if (!CanAct()) return false;
        const uint64_t now = NowMs();
        if (now >= end) return true;
        const uint64_t left = end - now;
        ::Sleep(static_cast<DWORD>(left > 5 ? 5 : left));
    }
}

ActResult Actuator::WaitGate(int maxWaitMs) {
    const uint64_t start = NowMs();
    for (;;) {
        if (!CanAct()) return Abort(Key("input_gate_wait"));
        const uint64_t now = NowMs();
        if (m_gate.Ready(now)) return ActResult::Ok;
        if (now - start >= static_cast<uint64_t>(maxWaitMs < 0 ? 0 : maxWaitMs)) return ActResult::NotReady;
        const uint64_t left = m_gate.NextMs() - now;
        ::Sleep(static_cast<DWORD>(left > 5 ? 5 : (left == 0 ? 1 : left)));
    }
}

Vec2 Actuator::ClickJitter() { return m_rng.CenterBiased(static_cast<float>(m_cfg.clickJitterPx)); }

ActResult Actuator::UiClick(const Vec2& client, int vk, bool jitter) {
    if (!win::IsMouseVk(vk)) return ActResult::Refused;
    const ActResult g = WaitGate(400);
    if (g != ActResult::Ok) return g;
    const Vec2 at = jitter ? client + ClickJitter() : client;
    MarkCursorBusy(300);
    if (!win::MouseClick(vk, m_game, static_cast<int>(std::lround(at.x)), static_cast<int>(std::lround(at.y))))
        return ActResult::Refused;
    m_cursor = at;
    m_haveCursor = true;
    MarkGate();
    return ActResult::Ok;
}

ActResult Actuator::WorldClick(const Vec2& client, int dwellMs, int holdMs, int vk,
                               const std::function<bool()>& verifyHover, int reHovers) {
    if (!win::IsMouseVk(vk)) return ActResult::Refused;
    if (!CanAct()) return Abort(Key("input_world_click"));
    const Vec2 target = client + ClickJitter();
    MarkCursorBusy(static_cast<uint32_t>(dwellMs + holdMs + 800));
    ActResult r = m_cfg.humanMouse ? MoveCursorHuman(target) : MoveCursor(target);
    if (r != ActResult::Ok) return r;
    if (!SleepCancellable(dwellMs)) return Abort(Key("input_hover_dwell"));
    Vec2 at = target;
    if (verifyHover && !verifyHover()) {
        static const Vec2 kOffsets[] = {{0.f, -6.f}, {5.f, 3.f}, {-5.f, 3.f}, {0.f, 6.f}};
        for (int i = 0; i < reHovers && i < 4; ++i) {
            at = client + kOffsets[i];
            r = MoveCursor(at);
            if (r != ActResult::Ok) return r;
            if (!SleepCancellable((std::max)(30, dwellMs / 2))) return Abort(Key("input_re_hover"));
            if (verifyHover()) break;
        }
        // Not confirmed after the re-hovers: press anyway (the hover flag is not
        // set for every object kind).
    }
    r = WaitGate(400);
    if (r != ActResult::Ok) return r;
    const POINT p = ToPoint(at);
    if (!win::MouseButton(vk, true, m_game, &p)) return ActResult::Refused;
    Track(vk, true);
    const bool held = SleepCancellable(holdMs);
    win::MouseButton(vk, false, m_game, &p);
    Track(vk, false);
    MarkGate();
    m_cursor = at;
    m_haveCursor = true;
    return held ? ActResult::Ok : Abort(Key("input_click_hold"));
}

ActResult Actuator::TapKey(int vk, int holdMs) {
    if (vk <= 0 || vk > 0xFE) return ActResult::Refused;
    const ActResult g = WaitGate(400);
    if (g != ActResult::Ok) return g;
    const POINT p = ToPoint(m_cursor);
    const POINT* at = (win::IsMouseVk(vk) && m_haveCursor) ? &p : nullptr;
    if (!win::VkEvent(vk, true, m_game, at)) return ActResult::Refused;
    Track(vk, true);
    const bool ok = SleepCancellable(holdMs);
    win::VkEvent(vk, false, m_game, at);
    Track(vk, false);
    MarkGate();
    return ok ? ActResult::Ok : Abort(Key("input_key_tap"));
}

ActResult Actuator::HoldKeyFor(int vk, int ms) { return TapKey(vk, ms); }

ActResult Actuator::PressAt(int vk, const Vec2& client, int holdMs) {
    const ActResult m = MoveCursor(client);
    if (m != ActResult::Ok) return m;
    if (win::IsMouseVk(vk)) MarkCursorBusy(static_cast<uint32_t>(holdMs + 300));
    return TapKey(vk, holdMs > 0 ? holdMs : 30);
}

ActResult Actuator::TypeChatCommand(const std::string& utf8) {
    if (m_uiTask || utf8.empty()) return ActResult::Refused;
    const ActResult g = WaitGate(600);
    if (g != ActResult::Ok) return g;
    if (!CanAct()) return Abort(Key("input_chat"));
    win::ReleaseHeldModifiers();
    win::KeyEvent(VK_RETURN, true);
    win::KeyEvent(VK_RETURN, false);
    if (!SleepCancellable(80)) return Abort(Key("input_chat"));
    win::SendUnicodeText(Utf8ToWide(utf8));
    if (!SleepCancellable(50)) return Abort(Key("input_chat"));
    win::KeyEvent(VK_RETURN, true);
    win::KeyEvent(VK_RETURN, false);
    MarkGate();
    if (m_log) m_log->Add(LogCat::Input, TrF("log_input_chat", {{"command", utf8}}));
    return ActResult::Ok;
}

}  // namespace fb
