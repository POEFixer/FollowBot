// DeathTask.cpp - see DeathTask.h. The decisions live in DeathLogic.h (pure,
// tested); this file reads the death screen through the SDK.
// FB_TESTS_EXCLUDE: adapter (PluginSDK UI reads); not built into tests/.
#include "DeathTask.h"

#include "../../../../POEFixer/plugin_sdk/PluginSDK.h"
#include "../common/BotLog.h"
#include "../input/Actuator.h"
#include "../move/Mover.h"
#include "../move/StuckRecovery.h"
#include "../nav/PathPlanner.h"

#include <algorithm>
#include <cstdio>

namespace fb {

namespace {

using PluginSDK::UiMatchKind;

bool HostCanFind(const PluginSDK::Context* sdk) { return sdk && sdk->Ui.HasFindElements(); }

// The visible death screen, 0 when none.
uintptr_t FindVisibleDeathScreen(const PluginSDK::Context& sdk) {
    const auto found = sdk.Ui.FindElements(0, death::kDeathScreen, UiMatchKind::ControlName, death::kDeathScreenDepth,
                                           true, 1);
    return found.empty() ? 0 : found.front();
}

struct ButtonHit {
    uintptr_t   addr = 0;
    const char* name = nullptr;
    float       x = 0.f, y = 0.f, w = 0.f, h = 0.f;
};

// The first visible, clickable button of `order` below `screen`; `seen` lists
// what was found but not usable (for the log).
bool FindButton(const PluginSDK::Context& sdk, uintptr_t screen, const death::ButtonOrder& order, int screenW,
                int screenH, ButtonHit& hit, std::string& seen) {
    for (int i = 0; i < order.count; ++i) {
        const auto found =
            sdk.Ui.FindElements(screen, order.names[i], UiMatchKind::ControlName, death::kButtonDepth, true, 1);
        if (found.empty()) continue;
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        if (!sdk.Ui.ComputeScreenRect(found.front(), x, y, w, h) ||
            !death::RectClickable(x, y, w, h, screenW, screenH)) {
            if (!seen.empty()) seen += ", ";
            seen += order.names[i];
            seen += " (no clickable rectangle)";
            continue;
        }
        hit.addr = found.front();
        hit.name = order.names[i];
        hit.x = x;
        hit.y = y;
        hit.w = w;
        hit.h = h;
        return true;
    }
    return false;
}

}  // namespace

// ---------------------------------------------------------------------------
// Death tracking
// ---------------------------------------------------------------------------

void DeathTask::BeginDeath(BotContext& ctx) {
    const World& w = ctx.w;
    m_active = true;
    m_session = ctx.mem.startedMs;
    m_inMap = w.gate.isMap;
    m_areaId = w.gate.areaId;
    m_nextTryMs = 0;
    m_tries = 0;
    m_clicks = 0;
    m_lastButton = nullptr;
    m_manualLogged = false;
    ctx.log.Add(LogCat::Death, "Died in %s%s", m_areaId.empty() ? "this area" : m_areaId.c_str(),
                m_inMap ? " (map)" : "");
}

void DeathTask::OnRevived(BotContext& ctx, bool byTask) {
    if (!m_active) return;
    m_active = false;
    const double secs =
        m_tracker.ConfirmedMs() != 0 && ctx.now >= m_tracker.ConfirmedMs()
            ? static_cast<double>(ctx.now - m_tracker.ConfirmedMs()) / 1000.0
            : 0.0;
    BotMemory& mem = ctx.mem;
    if (mem.startedMs == 0 || m_session != mem.startedMs) {
        ctx.log.Add(LogCat::Death, "Alive again (that death happened before this Start: not counted)");
        return;
    }
    ++mem.deathsSession;
    if (m_inMap) ++mem.deathsMap;
    mem.lastDeathMs = ctx.now;
    // Items given up as unreachable from where we were, stuck history and the
    // path all depend on the old position.
    mem.lootBlacklist.clear();
    ctx.stuck.Reset(ctx.now);
    ctx.planner.Clear();
    const bool byBot = byTask && !m_manualLogged && m_clicks > 0;
    const char* how = !byTask          ? "respawned without the bot"
                      : m_manualLogged ? "respawned manually"
                      : byBot          ? "respawned by the bot"
                                       : "revived";
    ctx.log.Add(LogCat::Death, "Alive again after %.1f s (%s%s%s); deaths: %d in this map, %d since Start", secs, how,
                byBot && m_lastButton ? ", " : "", byBot && m_lastButton ? death::ButtonLabel(m_lastButton) : "",
                mem.deathsMap, mem.deathsSession);
    const death::LimitVerdict lv = death::CheckDeathLimits(mem.deathsMap, mem.deathsSession, m_inMap, ctx.p.death);
    if (lv.stop) {
        ctx.log.Add(LogCat::Death, "Death limit reached: %s - stopping the bot", lv.reason.c_str());
        mem.RequestStop(lv.reason);
    }
}

bool DeathTask::WantsToStart(BotContext& ctx) {
    const World& w = ctx.w;
    const auto ev = m_tracker.Observe(w.self.valid, w.self.dead, ctx.now, ctx.p.death.deadConfirmMs);
    if (ev == death::DeathTracker::Event::Revived) {
        OnRevived(ctx, false);
        return false;
    }
    if (ev == death::DeathTracker::Event::Confirmed) BeginDeath(ctx);
    return w.self.dead && m_tracker.Confirmed();
}

void DeathTask::Start(BotContext& ctx) {
    const DeathSettings& s = ctx.p.death;
    ctx.mover.Stop(ctx.act);
    ctx.act.ReleaseAll();
    ctx.planner.Clear();
    if (!m_active) BeginDeath(ctx);
    if (m_tries == 0) {
        const int jitter = s.respawnJitterMs > 0 ? ctx.act.Random().UniformInt(0, s.respawnJitterMs) : 0;
        m_nextTryMs = ctx.now + static_cast<uint64_t>(s.respawnDelayMs + jitter);
        if (s.autoRespawn)
            ctx.log.Add(LogCat::Death, "Respawning in %.1f s", static_cast<double>(s.respawnDelayMs + jitter) / 1000.0);
    } else {
        // Resumed after a hold: next try after the retry interval.
        m_nextTryMs = ctx.now + static_cast<uint64_t>(s.retryIntervalMs);
    }
}

void DeathTask::Abort(BotContext& ctx) {
    // Keeps the death (tracker, tries): Start resumes it. The Brain releases input.
    (void)ctx;
}

TaskStatus DeathTask::HoldManual(BotContext& ctx, const std::string& why, bool dumpHint) {
    ctx.report.detail = "Dead - respawn manually (" + why + ")";
    if (!m_manualLogged) {
        m_manualLogged = true;
        ctx.log.Add(LogCat::Death, "Dead - respawn manually: %s%s", why.c_str(),
                    dumpHint ? ". While dead, Debug -> \"Test death screen lookup\" (or \"Find UI by control name\" "
                               "with death_screen) shows what the game offers"
                             : "");
        if (ctx.p.death.autoRespawn) ctx.mem.lastError = "Death: " + why + " - respawn manually";
    }
    return TaskStatus::Running;
}

TaskStatus DeathTask::Tick(BotContext& ctx) {
    const World& w = ctx.w;
    const DeathSettings& s = ctx.p.death;
    const auto ev = m_tracker.Observe(w.self.valid, w.self.dead, ctx.now, s.deadConfirmMs);
    if (ev == death::DeathTracker::Event::Revived) {
        OnRevived(ctx, true);
        return TaskStatus::Done;
    }
    ctx.stuck.NoteIdle(w, ctx.now);
    if (!w.self.valid) {
        ctx.report.detail = "Dead: waiting for the game";
        return TaskStatus::Running;
    }
    if (!w.self.dead) return TaskStatus::Done;   // alive without a confirmed death (a short life-0 read)

    if (!s.autoRespawn) return HoldManual(ctx, "automatic respawn is off", false);
    const PluginSDK::Context* sdk = ctx.sdk;
    if (!HostCanFind(sdk))
        return HoldManual(ctx, "this POEFixer cannot find UI elements by name; automatic respawn needs 365 or newer", false);

    char buf[256];
    if (ctx.now < m_nextTryMs) {
        const double in = static_cast<double>(m_nextTryMs - ctx.now) / 1000.0;
        if (m_clicks == 0) std::snprintf(buf, sizeof(buf), "Dead: respawning in %.1f s", in);
        else
            std::snprintf(buf, sizeof(buf), "Dead: clicked \"%s\", waiting (try %d of %d)",
                          death::ButtonLabel(m_lastButton), m_tries, s.maxRetries);
        ctx.report.detail = buf;
        return TaskStatus::Running;
    }
    if (m_tries >= s.maxRetries) {
        std::snprintf(buf, sizeof(buf), "%d %s did not bring the character back", m_tries,
                      m_tries == 1 ? "try" : "tries");
        return HoldManual(ctx, buf, true);
    }
    if (ctx.cursorBusy) {
        ctx.report.detail = "Dead: waiting (another plugin is using the cursor)";
        return TaskStatus::Running;
    }

    // One try: find the screen and the first visible allowed button, click it.
    ++m_tries;
    m_nextTryMs = ctx.now + static_cast<uint64_t>(s.retryIntervalMs);
    const uintptr_t screen = FindVisibleDeathScreen(*sdk);
    if (screen == 0) {
        ctx.log.Add(LogCat::Death, "Respawn try %d of %d: no visible death screen (%s)", m_tries, s.maxRetries,
                    death::kDeathScreen);
        if (m_tries >= s.maxRetries) return HoldManual(ctx, "the death screen was not found", true);
        std::snprintf(buf, sizeof(buf), "Dead: looking for the death screen (try %d of %d)", m_tries, s.maxRetries);
        ctx.report.detail = buf;
        return TaskStatus::Running;
    }
    const death::ButtonOrder order = death::RespawnButtonOrder(s);
    ButtonHit hit;
    std::string seen;
    if (!FindButton(*sdk, screen, order, w.gate.screenW, w.gate.screenH, hit, seen)) {
        ctx.log.Add(LogCat::Death, "Respawn try %d of %d: the death screen shows none of the allowed buttons%s%s%s",
                    m_tries, s.maxRetries, seen.empty() ? "" : " (", seen.c_str(), seen.empty() ? "" : ")");
        if (m_tries >= s.maxRetries)
            return HoldManual(ctx,
                              s.allowGenericRespawn ? "no respawn button found on the death screen"
                                                    : "no checkpoint button on the death screen (the generic respawn "
                                                      "button is not allowed)",
                              true);
        std::snprintf(buf, sizeof(buf), "Dead: no respawn button yet (try %d of %d)", m_tries, s.maxRetries);
        ctx.report.detail = buf;
        return TaskStatus::Running;
    }

    // Click inside the button: the centre plus a small jitter kept well inside it.
    Vec2 jitter = ctx.act.ClickJitter();
    jitter.x = std::clamp(jitter.x, -hit.w * 0.3f, hit.w * 0.3f);
    jitter.y = std::clamp(jitter.y, -hit.h * 0.25f, hit.h * 0.25f);
    const Vec2 at{hit.x + hit.w * 0.5f + jitter.x, hit.y + hit.h * 0.5f + jitter.y};
    const ActResult r = ctx.act.UiClick(at, VK_LBUTTON, false);
    if (r == ActResult::Ok) {
        ++m_clicks;
        m_lastButton = hit.name;
        ctx.log.Add(LogCat::Death, "Respawn try %d of %d: clicked \"%s\" at (%.0f, %.0f)", m_tries, s.maxRetries,
                    death::ButtonLabel(hit.name), at.x, at.y);
        std::snprintf(buf, sizeof(buf), "Dead: clicked \"%s\" (try %d of %d)", death::ButtonLabel(hit.name), m_tries,
                      s.maxRetries);
        ctx.report.detail = buf;
        return TaskStatus::Running;
    }
    if (r == ActResult::NotReady || r == ActResult::CursorBusy) {
        // Not sent: the same try again on the next tick.
        --m_tries;
        m_nextTryMs = ctx.now;
        ctx.report.detail = "Dead: waiting to click";
        return TaskStatus::Running;
    }
    ctx.log.Add(LogCat::Death, "Respawn try %d of %d: the click was not sent (%s)", m_tries, s.maxRetries,
                ActResultName(r));
    ctx.report.detail = "Dead: the respawn click was not sent";
    return TaskStatus::Running;
}

// ---------------------------------------------------------------------------
// Debug probe
// ---------------------------------------------------------------------------

std::string DeathTask::ProbeDeathScreen(BotContext& ctx) {
    const PluginSDK::Context* sdk = ctx.sdk;
    if (!sdk) return "no host";
    const bool names = sdk->Ui.HasControlNames();
    const bool find = sdk->Ui.HasFindElements();
    std::string out = std::string("host: UI names ") + (names ? "yes" : "no") + ", UI search " + (find ? "yes" : "no");
    if (!find) return out + " - automatic respawn needs POEFixer 365 or newer: respawn manually";
    const World& w = ctx.w;
    out += w.self.dead ? " | you are dead" : " | you are alive (the death screen is normally hidden)";

    char buf[160];
    auto rectText = [&](uintptr_t addr) {
        float x = 0.f, y = 0.f, rw = 0.f, rh = 0.f;
        if (!sdk->Ui.ComputeScreenRect(addr, x, y, rw, rh)) return std::string("no rectangle");
        std::snprintf(buf, sizeof(buf), "%.0f,%.0f %.0fx%.0f%s", x, y, rw, rh,
                      death::RectClickable(x, y, rw, rh, w.gate.screenW, w.gate.screenH) ? "" : " not clickable");
        return std::string(buf);
    };

    const auto all = sdk->Ui.FindElements(0, death::kDeathScreen, UiMatchKind::ControlName, death::kDeathScreenDepth,
                                          false, 4);
    if (all.empty())
        return out + " | death_screen: not found within " + std::to_string(death::kDeathScreenDepth) +
               " levels of the game UI (the control name may differ: Dump UI tree)";
    const auto vis = sdk->Ui.FindElements(0, death::kDeathScreen, UiMatchKind::ControlName, death::kDeathScreenDepth,
                                          true, 1);
    const uintptr_t screen = vis.empty() ? all.front() : vis.front();
    out += " | death_screen: ";
    out += vis.empty() ? "hidden" : "visible (" + rectText(screen) + ")";
    if (all.size() > 1) out += " (" + std::to_string(all.size()) + " found)";

    const char* const kButtons[] = {death::kRespawnAtCheckpoint, death::kReviveAtCheckpoint, death::kRespawn,
                                    death::kSpectate};
    for (const char* name : kButtons) {
        const auto any = sdk->Ui.FindElements(screen, name, UiMatchKind::ControlName, death::kButtonDepth, false, 1);
        out += " | ";
        out += name;
        if (any.empty()) {
            out += ": not found";
            continue;
        }
        const auto shown = sdk->Ui.FindElements(screen, name, UiMatchKind::ControlName, death::kButtonDepth, true, 1);
        out += shown.empty() ? ": hidden" : ": visible (" + rectText(shown.front()) + ")";
    }

    const DeathSettings& s = ctx.p.death;
    ButtonHit hit;
    std::string seen;
    if (!s.autoRespawn) {
        out += " | automatic respawn is off";
    } else if (!vis.empty() && FindButton(*sdk, screen, death::RespawnButtonOrder(s), w.gate.screenW, w.gate.screenH,
                                          hit, seen)) {
        out += std::string(" | would click: ") + death::ButtonLabel(hit.name);
    } else {
        out += " | would click: nothing";
    }
    if (!names) out += " | (UI names unavailable: the UI dump has no name column)";
    return out;
}

}  // namespace fb
