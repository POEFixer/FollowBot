// DebugTools.cpp - self-service RE tools (design §5.13): entity dump, UI-tree
// dump (text, rects and - host 365+ - locale-independent control names), the
// "Find UI by control name" probe and the death-screen probe. Run on the worker
// between ticks.
// FB_TESTS_EXCLUDE: SDK adapter; not built into tests/.
#include "../FollowBotPlugin.h"

#include "../common/FileUtil.h"
#include "../common/Strings.h"
#include "Tabs.h"

#include <cstdio>
#include <ctime>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

using namespace fb;

namespace {

std::string Stamp() {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d%02d%02d-%02d%02d%02d", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                  tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    return buf;
}

const char* TypeName(PluginSDK::EntityType t) {
    using T = PluginSDK::EntityType;
    switch (t) {
        case T::Unidentified:      return "Unidentified";
        case T::Chest:             return "Chest";
        case T::NPC:               return "NPC";
        case T::Player:            return "Player";
        case T::Shrine:            return "Shrine";
        case T::Monster:           return "Monster";
        case T::DeliriumBomb:      return "DeliriumBomb";
        case T::DeliriumSpawner:   return "DeliriumSpawner";
        case T::OtherImportant:    return "OtherImportant";
        case T::Item:              return "Item";
        case T::Renderable:        return "Renderable";
        case T::AreaTransition:    return "AreaTransition";
        case T::ExpeditionMarker:  return "ExpeditionMarker";
        case T::ExpeditionRemnant: return "ExpeditionRemnant";
        case T::AzmeriWisp:        return "AzmeriWisp";
    }
    return "?";
}

void AppendComponents(std::string& out, const PluginSDK::ComponentAddresses& c) {
    struct Item { bool has; const char* name; };
    const Item items[] = {
        {c.HasRender(), "Render"}, {c.HasPositioned(), "Positioned"}, {c.HasLife(), "Life"},
        {c.HasTargetable(), "Targetable"}, {c.HasChest(), "Chest"}, {c.HasShrine(), "Shrine"},
        {c.HasPlayer(), "Player"}, {c.HasNpc(), "Npc"}, {c.HasBuffs(), "Buffs"}, {c.HasWorldItem(), "WorldItem"},
        {c.HasAreaTransition(), "AreaTransition"}, {c.HasMinimapIcon(), "MinimapIcon"}, {c.HasStats(), "Stats"},
        {c.HasActor(), "Actor"}, {c.HasAnimated(), "Animated"}, {c.HasBase(), "Base"}, {c.HasCharges(), "Charges"},
        {c.HasMods(), "Mods"}, {c.HasStack(), "Stack"}, {c.HasTransitionable(), "Transitionable"},
        {c.HasStateMachine(), "StateMachine"}, {c.HasDiesAfterTime(), "DiesAfterTime"},
        {c.HasTriggerableBlockage(), "TriggerableBlockage"}, {c.HasOMP(), "OMP"},
    };
    bool first = true;
    for (const auto& i : items) {
        if (!i.has) continue;
        if (!first) out += ' ';
        out += i.name;
        first = false;
    }
}

// Control names from the element up to (not including) the game UI root,
// outermost first: "a > b > c" (unnamed levels as "?"). Bounded by 32 levels.
std::string ControlNamePath(const PluginSDK::Context& sdk, uintptr_t addr, uintptr_t root, int* depthOut) {
    std::vector<std::string> names;
    uintptr_t cur = addr;
    for (int i = 0; i < 32 && cur != 0 && cur != root; ++i) {
        std::string n = sdk.Ui.GetControlName(cur);
        names.push_back(n.empty() ? std::string("?") : std::move(n));
        const PluginSDK::UiElement el = sdk.Ui.Read(cur);
        if (!el.Valid) break;
        cur = el.ParentAddress;
    }
    if (depthOut) *depthOut = static_cast<int>(names.size());
    std::string out;
    for (auto it = names.rbegin(); it != names.rend(); ++it) {
        if (!out.empty()) out += " > ";
        out += *it;
    }
    return out;
}

// "Find UI by control name": up to 12 matches below the game UI root with their
// control-name path, visibility, rectangle and localized text, one line each
// (texts the Debug tab and the decision log translate).
std::vector<LocText> FindUiElements(const PluginSDK::Context* sdk, const std::string& query, int match, bool visibleOnly) {
    std::vector<LocText> lines;
    if (!sdk) {
        lines.push_back(Tr("findui_no_host"));
        return lines;
    }
    if (!sdk->Ui.HasFindElements()) {
        lines.push_back(Tr("findui_needs_365"));
        return lines;
    }
    const std::string text = Trim(query);
    if (text.empty()) {
        lines.push_back(Tr("findui_type_first"));
        return lines;
    }
    if (match < 0 || match > 2) match = 0;
    const PluginSDK::UiMatchKind kind = match == 1   ? PluginSDK::UiMatchKind::ControlNamePrefix
                                        : match == 2 ? PluginSDK::UiMatchKind::StringId
                                                     : PluginSDK::UiMatchKind::ControlName;
    constexpr const char* kKindKeys[] = {Key("findui_kind_name"), Key("findui_kind_prefix"), Key("findui_kind_text")};
    constexpr int kMaxShown = 12;
    const std::vector<uintptr_t> found = sdk->Ui.FindElements(0, text, kind, 32, visibleOnly, kMaxShown);
    const LocText kindText = Tr(kKindKeys[match]);
    const LocText visibility = visibleOnly ? Tr("findui_visible_only") : Tr("findui_visible_or_hidden");
    const std::string count = Num("%d", static_cast<int>(found.size()));
    lines.push_back(static_cast<int>(found.size()) >= kMaxShown
                        ? TrF("findui_header_capped", {{"kind", kindText}, {"query", text}, {"visibility", visibility},
                                                       {"count", count}, {"max", Num("%d", kMaxShown)}})
                        : TrF("findui_header", {{"kind", kindText}, {"query", text}, {"visibility", visibility},
                                                {"count", count}}));
    if (found.empty()) return lines;
    const uintptr_t root = sdk->Ui.GetGameUiRoot();
    const auto rects = sdk->Ui.ComputeScreenRects(found);
    for (size_t i = 0; i < found.size(); ++i) {
        const uintptr_t a = found[i];
        float x = 0.f, y = 0.f, w = 0.f, h = 0.f;
        bool ok = i < rects.size() && rects[i].ok;
        if (ok) {
            x = rects[i].x;
            y = rects[i].y;
            w = rects[i].w;
            h = rects[i].h;
        } else {
            ok = sdk->Ui.ComputeScreenRect(a, x, y, w, h);
        }
        int depth = 0;
        const std::string path = sdk->Ui.HasControlNames() ? ControlNamePath(*sdk, a, root, &depth) : std::string("-");
        std::string label = sdk->Ui.GetText(a);
        CutToChars(label, 80);   // the game's text, in its language: cut by characters
        for (char& ch : label)
            if (ch == '\r' || ch == '\n') ch = ' ';
        const LocText rect = ok ? TrF("text_verbatim", {{"text", Num("%.0f,%.0f %.0fx%.0f", x, y, w, h)}})
                                : Tr("probe_no_rect");
        const LocText state = sdk->Ui.IsVisible(a) ? Tr("findui_state_visible") : Tr("findui_state_hidden");
        const std::string number = Num("%d", static_cast<int>(i + 1));
        const std::string address = Num("0x%llX", static_cast<unsigned long long>(a));
        const std::string levels = Num("%d", depth);
        lines.push_back(label.empty()
                            ? TrF("findui_line", {{"number", number}, {"address", address}, {"state", state},
                                                  {"depth", levels}, {"rect", rect}, {"path", path}})
                            : TrF("findui_line_text", {{"number", number}, {"address", address}, {"state", state},
                                                       {"depth", levels}, {"rect", rect}, {"path", path}, {"text", label}}));
    }
    return lines;
}

}  // namespace

namespace fb::ui {

LocText DumpEntities(const PluginSDK::Context* sdk, const std::filesystem::path& dir, const Vec2& self, float radius) {
    if (!sdk) return Tr("dump_no_host");
    const PluginSDK::Snapshot snap = sdk->Game.GetSnapshot();
    std::string out;
    out.reserve(64 * 1024);
    char line[512];
    std::snprintf(line, sizeof(line), "Follow Bot entity dump  area=%s  counter=%llu  self=(%.1f, %.1f)  radius=%.0f\r\n\r\n",
                  snap.CurrentAreaName.c_str(), static_cast<unsigned long long>(snap.AreaChangeCounter), self.x, self.y,
                  radius);
    out += line;
    int count = 0;
    for (const auto& e : snap.Entities) {
        const Vec2 g{e.GridPositionX, e.GridPositionY};
        const float d = Dist(g, self);
        if (d > radius) continue;
        ++count;
        std::snprintf(line, sizeof(line), "#%u  %s  type=%s subtype=%d state=%d  grid=(%.1f, %.1f) dist=%.1f  world=(%.0f, %.0f, %.0f) h=%.0f%s\r\n",
                      e.Id, WideToUtf8(e.Path).c_str(), TypeName(e.EntityType), static_cast<int>(e.EntitySubtype),
                      static_cast<int>(e.EntityState), g.x, g.y, d, e.WorldX, e.WorldY, e.WorldZ, e.ModelBoundsZ,
                      e.IsValid ? "" : "  (invalid)");
        out += line;
        out += "    components: ";
        AppendComponents(out, e.Components);
        out += "\r\n";
        if (e.Components.StateMachine) {
            out += "    state machine:";
            for (int i = 0; i < 8; ++i) {
                uint64_t value = 0;
                if (!sdk->Sekhema.GetStateMachineValue(e.Components.StateMachine, i, value)) break;
                std::snprintf(line, sizeof(line), " [%d]=%llu", i, static_cast<unsigned long long>(value));
                out += line;
            }
            out += "\r\n";
        }
        if (e.Components.Transitionable) {
            const auto t = sdk->Components.ReadTransitionable(e.Components.Transitionable);
            std::snprintf(line, sizeof(line), "    transitionable: %d%s\r\n", t.CurrentState, t.Valid ? "" : " (unreadable)");
            out += line;
        }
        if (e.Components.Targetable) {
            const auto t = sdk->Components.ReadTargetable(e.Components.Targetable);
            std::snprintf(line, sizeof(line),
                          "    targetable: targetable=%d highlightable=%d targeted=%d hidden=%d quest=%d\r\n",
                          t.IsTargetable, t.IsHighlightable, t.IsTargetedByPlayer, t.HiddenFromPlayer, t.MeetsQuestState);
            out += line;
        }
        if (e.Components.TriggerableBlockage) {
            const auto b = sdk->Components.ReadTriggerableBlockage(e.Components.TriggerableBlockage);
            std::snprintf(line, sizeof(line), "    blockage: closed=%d blocked=%d\r\n", b.IsClosed, b.IsBlocked);
            out += line;
        }
        if (!e.PlayerName.empty()) out += "    player: " + WideToUtf8(e.PlayerName) + "\r\n";
    }
    const std::filesystem::path file = dir / ("entities-" + Stamp() + ".txt");
    if (!io::WriteTextFileAtomic(file, out)) return Tr("dump_entities_failed");
    return TrF("dump_entities_done", {{"count", std::to_string(count)}, {"file", WideToUtf8(file.wstring())}});
}

LocText DumpUiTree(const PluginSDK::Context* sdk, const std::filesystem::path& dir) {
    if (!sdk) return Tr("dump_no_host");
    const uintptr_t root = sdk->Ui.GetGameUiRoot();
    if (!root) return Tr("dump_ui_no_root");
    struct Node {
        uintptr_t addr;
        int depth;
    };
    std::vector<Node> order;
    std::deque<Node> queue;
    queue.push_back({root, 0});
    constexpr size_t kBudget = 8000;
    constexpr int kMaxDepth = 24;
    while (!queue.empty() && order.size() < kBudget) {
        const Node n = queue.front();
        queue.pop_front();
        if (!sdk->Ui.IsVisible(n.addr) && n.addr != root) continue;
        order.push_back(n);
        if (n.depth >= kMaxDepth) continue;
        for (uintptr_t c : sdk->Ui.GetChildren(n.addr)) queue.push_back({c, n.depth + 1});
    }
    std::vector<uintptr_t> addrs;
    addrs.reserve(order.size());
    for (const auto& n : order) addrs.push_back(n.addr);
    const auto rects = sdk->Ui.ComputeScreenRects(addrs);
    std::string out;
    out.reserve(order.size() * 96);
    const bool names = sdk->Ui.HasControlNames();
    out += "Follow Bot UI dump (visible elements under the game UI root, breadth first)\r\n";
    out += names ? "depth  address  type  children  rect(x y w h)  [control name]  text\r\n\r\n"
                 : "depth  address  type  children  rect(x y w h)  text   (no control names: needs POEFixer 365+)\r\n\r\n";
    char line[1024];
    std::string name;
    for (size_t i = 0; i < order.size(); ++i) {
        const auto& n = order[i];
        const PluginSDK::UiElement el = sdk->Ui.Read(n.addr);
        std::string text = sdk->Ui.GetText(n.addr);
        CutToChars(text, 200);
        for (char& ch : text)
            if (ch == '\r' || ch == '\n') ch = ' ';
        name.clear();
        if (names) {
            std::string cn = sdk->Ui.GetControlName(n.addr);
            if (cn.size() > 120) cn.resize(120);
            name = "[" + (cn.empty() ? std::string("-") : cn) + "]  ";
        }
        const bool ok = i < rects.size() && rects[i].ok;
        std::snprintf(line, sizeof(line), "%*s%d  0x%llX  t=%u  c=%d  %s(%.0f %.0f %.0f %.0f)  %s%s\r\n", n.depth * 2, "",
                      n.depth, static_cast<unsigned long long>(n.addr), static_cast<unsigned>(el.ElementType), el.ChildCount,
                      ok ? "" : "no-rect ", ok ? rects[i].x : 0.f, ok ? rects[i].y : 0.f, ok ? rects[i].w : 0.f,
                      ok ? rects[i].h : 0.f, name.c_str(), text.c_str());
        out += line;
    }
    if (order.size() >= kBudget) out += "\r\n(stopped at the node budget)\r\n";
    const std::filesystem::path file = dir / ("ui-" + Stamp() + ".txt");
    if (!io::WriteTextFileAtomic(file, out)) return Tr("dump_ui_failed");
    return TrF("dump_ui_done", {{"count", std::to_string(order.size())}, {"file", WideToUtf8(file.wstring())}});
}

}  // namespace fb::ui

void FollowBotPlugin::RunJob(Job j, BotContext& ctx) {
    std::vector<LocText> result;   // one entry per line: the Debug tab and the decision log
    const std::filesystem::path dumps = DirectoryPath() / "dumps";
    switch (j) {
        case Job::DumpEntities:
            result.push_back(ui::DumpEntities(ctx.sdk, dumps, ctx.w.self.grid, 60.f));
            break;
        case Job::DumpUi:
            result.push_back(ui::DumpUiTree(ctx.sdk, dumps));
            break;
        case Job::ProbeDeath:
            result = DeathTask::ProbeDeathScreen(ctx);
            break;
        case Job::FindUi: {
            std::string query;
            int match = 0;
            bool visibleOnly = false;
            {
                std::lock_guard<std::mutex> lock(m_findUiMu);
                query = m_findUiQuery;
                match = m_findUiQueryMatch;
                visibleOnly = m_findUiQueryVisibleOnly;
            }
            result = FindUiElements(ctx.sdk, query, match, visibleOnly);
            break;
        }
        case Job::None:
            return;
    }
    // One log line per entry; the Debug tab shows them all.
    for (const LocText& line : result) m_log.Add(LogCat::General, line);
    m_brain.SetDebugResult(std::move(result));
}
