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
// the control names found without a clickable rectangle (for the log).
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
    const LocText area = m_areaId.empty() ? Tr("death_this_area") : TrF("text_verbatim", {{"text", m_areaId}});
    ctx.log.Add(LogCat::Death, TrF(m_inMap ? Key("log_death_died_map") : Key("log_death_died"), {{"area", area}}));
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
        ctx.log.Add(LogCat::Death, Tr("log_death_not_counted"));
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
    const char* how = !byTask          ? Key("death_how_without_bot")
                      : m_manualLogged ? Key("death_how_manual")
                      : byBot          ? Key("death_how_bot")
                                       : Key("death_how_revived");
    if (byBot && m_lastButton)
        ctx.log.Add(LogCat::Death, TrF("log_death_alive_button", {{"seconds", Num("%.1f", secs)},
                                                                  {"how", Tr(how)},
                                                                  {"button", Tr(death::ButtonLabel(m_lastButton))},
                                                                  {"map", Num("%d", mem.deathsMap)},
                                                                  {"session", Num("%d", mem.deathsSession)}}));
    else
        ctx.log.Add(LogCat::Death, TrF("log_death_alive", {{"seconds", Num("%.1f", secs)},
                                                           {"how", Tr(how)},
                                                           {"map", Num("%d", mem.deathsMap)},
                                                           {"session", Num("%d", mem.deathsSession)}}));
    const death::LimitVerdict lv = death::CheckDeathLimits(mem.deathsMap, mem.deathsSession, m_inMap, ctx.p.death);
    if (lv.stop) {
        ctx.log.Add(LogCat::Death, TrF("log_death_limit", {{"why", lv.reason}}));
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
            ctx.log.Add(LogCat::Death, TrF("log_death_respawning_in",
                                           {{"seconds", Num("%.1f", static_cast<double>(s.respawnDelayMs + jitter) / 1000.0)}}));
    } else {
        // Resumed after a hold: next try after the retry interval.
        m_nextTryMs = ctx.now + static_cast<uint64_t>(s.retryIntervalMs);
    }
}

void DeathTask::Abort(BotContext& ctx) {
    // Keeps the death (tracker, tries): Start resumes it. The Brain releases input.
    (void)ctx;
}

TaskStatus DeathTask::HoldManual(BotContext& ctx, const LocText& why, bool dumpHint) {
    ctx.report.detail = TrF("detail_death_manual", {{"why", why}});
    if (!m_manualLogged) {
        m_manualLogged = true;
        ctx.log.Add(LogCat::Death, TrF(dumpHint ? Key("log_death_manual_hint") : Key("log_death_manual"), {{"why", why}}));
        if (ctx.p.death.autoRespawn) ctx.mem.lastError = TrF("error_death_manual", {{"why", why}});
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
        ctx.report.detail = Tr("detail_death_waiting_game");
        return TaskStatus::Running;
    }
    if (!w.self.dead) return TaskStatus::Done;   // alive without a confirmed death (a short life-0 read)

    if (!s.autoRespawn) return HoldManual(ctx, Tr("death_why_auto_off"), false);
    const PluginSDK::Context* sdk = ctx.sdk;
    if (!HostCanFind(sdk)) return HoldManual(ctx, Tr("death_why_old_host"), false);

    const std::string tries = Num("%d", m_tries), maxTries = Num("%d", s.maxRetries);
    if (ctx.now < m_nextTryMs) {
        const double in = static_cast<double>(m_nextTryMs - ctx.now) / 1000.0;
        if (m_clicks == 0) ctx.report.detail = TrF("detail_death_respawning_in", {{"seconds", Num("%.1f", in)}});
        else
            ctx.report.detail = TrF("detail_death_clicked_waiting", {{"button", Tr(death::ButtonLabel(m_lastButton))},
                                                                     {"try", tries},
                                                                     {"max", maxTries}});
        return TaskStatus::Running;
    }
    if (m_tries >= s.maxRetries) return HoldManual(ctx, TrF("death_why_tries", {{"tries", tries}}), true);
    if (ctx.cursorBusy) {
        ctx.report.detail = Tr("detail_death_cursor_busy");
        return TaskStatus::Running;
    }

    // One try: find the screen and the first visible allowed button, click it.
    ++m_tries;
    m_nextTryMs = ctx.now + static_cast<uint64_t>(s.retryIntervalMs);
    const uintptr_t screen = FindVisibleDeathScreen(*sdk);
    const std::string tryNow = Num("%d", m_tries);
    if (screen == 0) {
        ctx.log.Add(LogCat::Death, TrF("log_death_no_screen", {{"try", tryNow}, {"max", maxTries}, {"name", death::kDeathScreen}}));
        if (m_tries >= s.maxRetries) return HoldManual(ctx, Tr("death_why_no_screen"), true);
        ctx.report.detail = TrF("detail_death_looking", {{"try", tryNow}, {"max", maxTries}});
        return TaskStatus::Running;
    }
    const death::ButtonOrder order = death::RespawnButtonOrder(s);
    ButtonHit hit;
    std::string seen;
    if (!FindButton(*sdk, screen, order, w.gate.screenW, w.gate.screenH, hit, seen)) {
        if (seen.empty())
            ctx.log.Add(LogCat::Death, TrF("log_death_no_button", {{"try", tryNow}, {"max", maxTries}}));
        else
            ctx.log.Add(LogCat::Death, TrF("log_death_no_button_seen", {{"try", tryNow}, {"max", maxTries}, {"seen", seen}}));
        if (m_tries >= s.maxRetries)
            return HoldManual(ctx, s.allowGenericRespawn ? Tr("death_why_no_button") : Tr("death_why_no_cp_button"), true);
        ctx.report.detail = TrF("detail_death_no_button", {{"try", tryNow}, {"max", maxTries}});
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
        ctx.log.Add(LogCat::Death, TrF("log_death_clicked", {{"try", tryNow},
                                                             {"max", maxTries},
                                                             {"button", Tr(death::ButtonLabel(hit.name))},
                                                             {"x", Num("%.0f", at.x)},
                                                             {"y", Num("%.0f", at.y)}}));
        ctx.report.detail = TrF("detail_death_clicked", {{"button", Tr(death::ButtonLabel(hit.name))},
                                                         {"try", tryNow},
                                                         {"max", maxTries}});
        return TaskStatus::Running;
    }
    if (r == ActResult::NotReady || r == ActResult::CursorBusy) {
        // Not sent: the same try again on the next tick.
        --m_tries;
        m_nextTryMs = ctx.now;
        ctx.report.detail = Tr("detail_death_waiting_click");
        return TaskStatus::Running;
    }
    ctx.log.Add(LogCat::Death, TrF("log_death_click_not_sent", {{"try", tryNow}, {"max", maxTries}, {"why", Tr(ActResultName(r))}}));
    ctx.report.detail = Tr("detail_death_click_not_sent");
    return TaskStatus::Running;
}

// ---------------------------------------------------------------------------
// Debug probe
// ---------------------------------------------------------------------------

std::vector<LocText> DeathTask::ProbeDeathScreen(BotContext& ctx) {
    std::vector<LocText> out;
    out.push_back(Tr("probe_heading"));
    const PluginSDK::Context* sdk = ctx.sdk;
    if (!sdk) {
        out.push_back(Tr("probe_no_host"));
        return out;
    }
    const bool names = sdk->Ui.HasControlNames();
    const bool find = sdk->Ui.HasFindElements();
    out.push_back(TrF("probe_host", {{"names", names ? Tr("common_yes") : Tr("common_no")},
                                     {"search", find ? Tr("common_yes") : Tr("common_no")}}));
    if (!find) {
        out.push_back(Tr("probe_needs_365"));
        return out;
    }
    const World& w = ctx.w;
    out.push_back(w.self.dead ? Tr("probe_dead") : Tr("probe_alive"));

    auto rectText = [&](uintptr_t addr) {
        float x = 0.f, y = 0.f, rw = 0.f, rh = 0.f;
        if (!sdk->Ui.ComputeScreenRect(addr, x, y, rw, rh)) return Tr("probe_no_rect");
        const std::string rect = Num("%.0f,%.0f %.0fx%.0f", x, y, rw, rh);
        if (death::RectClickable(x, y, rw, rh, w.gate.screenW, w.gate.screenH))
            return TrF("text_verbatim", {{"text", rect}});
        return TrF("probe_rect_not_clickable", {{"rect", rect}});
    };

    const auto all = sdk->Ui.FindElements(0, death::kDeathScreen, UiMatchKind::ControlName, death::kDeathScreenDepth,
                                          false, 4);
    if (all.empty()) {
        out.push_back(TrF("probe_screen_not_found", {{"levels", Num("%d", death::kDeathScreenDepth)}}));
        return out;
    }
    const auto vis = sdk->Ui.FindElements(0, death::kDeathScreen, UiMatchKind::ControlName, death::kDeathScreenDepth,
                                          true, 1);
    const uintptr_t screen = vis.empty() ? all.front() : vis.front();
    out.push_back(vis.empty() ? Tr("probe_screen_hidden") : TrF("probe_screen_visible", {{"rect", rectText(screen)}}));
    if (all.size() > 1) out.push_back(TrF("probe_found_count", {{"count", Num("%d", static_cast<int>(all.size()))}}));

    const char* const kButtons[] = {death::kRespawnAtCheckpoint, death::kReviveAtCheckpoint, death::kRespawn,
                                    death::kSpectate};
    for (const char* name : kButtons) {
        const auto any = sdk->Ui.FindElements(screen, name, UiMatchKind::ControlName, death::kButtonDepth, false, 1);
        if (any.empty()) {
            out.push_back(TrF("probe_button_not_found", {{"name", name}}));
            continue;
        }
        const auto shown = sdk->Ui.FindElements(screen, name, UiMatchKind::ControlName, death::kButtonDepth, true, 1);
        out.push_back(shown.empty() ? TrF("probe_button_hidden", {{"name", name}})
                                    : TrF("probe_button_visible", {{"name", name}, {"rect", rectText(shown.front())}}));
    }

    const DeathSettings& s = ctx.p.death;
    ButtonHit hit;
    std::string seen;
    if (!s.autoRespawn) {
        out.push_back(Tr("death_why_auto_off"));
    } else if (!vis.empty() && FindButton(*sdk, screen, death::RespawnButtonOrder(s), w.gate.screenW, w.gate.screenH,
                                          hit, seen)) {
        out.push_back(TrF("probe_would_click", {{"button", Tr(death::ButtonLabel(hit.name))}}));
    } else {
        out.push_back(Tr("probe_would_click_nothing"));
    }
    if (!names) out.push_back(Tr("probe_no_names"));
    return out;
}

}  // namespace fb
