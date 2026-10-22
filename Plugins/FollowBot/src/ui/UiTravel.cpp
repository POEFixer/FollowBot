// UiTravel.cpp - the Travel tab: following the leader through area
// transitions, portals and boss arena entrances (design §5.7), with the last
// vanish and the last failure in its Diagnostics.
// FB_TESTS_EXCLUDE: UI (ImGui + PluginSDK); not built into tests/.
#include "../FollowBotPlugin.h"

#include "../common/Clock.h"
#include "../travel/TravelLogic.h"
#include "Tabs.h"

#include <cstdio>

using namespace fb;
using PluginUi::Check;
using PluginUi::Heading;
using PluginUi::Hint;
using PluginUi::LabelColumn;

void FollowBotPlugin::DrawTabTravel() {
    TravelSettings& t = EditProfile().travel;
    const BotView* v = m_uiView.get();
    bool changed = false;

    // --- What to follow ------------------------------------------------------
    PluginUi::BeginCard("##travelWhat");
    Heading(ICON_FA_DOOR_OPEN, "Follow through",
            "When the leader disappears right next to a transition, portal or boss arena entrance, the bot\n"
            "walks there and takes it too. It never clicks one before he has gone through.\n"
            "Waypoints and checkpoint warps cannot be followed: the bot walks to his last position and says so.");
    changed |= Check("Area transitions", "followTrans", &t.followTransitions,
                     "Doors and passages to the next area.");
    changed |= Check("Portals", "followPortals", &t.followPortals,
                     "Player and town portals, map device portals in the hideout and the map's return portal.");
    ImGui::Indent();
    ImGui::BeginDisabled(!t.followPortals);
    changed |= Check("Also camp, parent and sub-area portals", "followOtherPortals", &t.followOtherPortalKinds,
                     "Camp teleports and portals that lead back to a parent area or into a sub-area.\n"
                     "Off by default: only portal kinds a leader normally takes are followed.");
    ImGui::EndDisabled();
    ImGui::Unindent();
    changed |= Check("Boss arenas", "followArenas", &t.enterBossArenas,
                     "Boss arena entrances: the bot enters right behind the leader.\n"
                     "Off: it waits outside at his last position.");
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Recognising where he went ----------------------------------------------
    PluginUi::BeginCard("##travelWhere");
    Heading(ICON_FA_MAGNIFYING_GLASS_LOCATION, "Where he went",
            "When the leader vanishes, the object nearest his last position (within the radius below) tells\n"
            "where he went. Objects seen earlier in the area are remembered, so a transition that is out of\n"
            "sight still counts.");
    {
        const LabelColumn col({"Object near his last position", "Out of range beyond", "Another object within"});
        changed |= ui::FloatRow(col, "Object near his last position", "##vanishR", &t.vanishRadius, 3.f, 30.f, "%.0f cells",
                                "A transition or portal this close to where he vanished explains the vanish.\n"
                                "Larger catches more, but may pick a neighbouring object.");
        changed |= ui::FloatRow(col, "Out of range beyond", "##oor", &t.outOfRangeCells, 60.f, 200.f, "%.0f cells",
                                "He was at least this far from you when he vanished, with no object near him:\n"
                                "he only walked out of sight (the bot walks after him).");
        changed |= ui::FloatRow(col, "Another object within", "##fallbackR", &t.fallbackRadius, 5.f, 80.f, "%.0f cells",
                                "When the object he took is gone or closed, another of the same kind this close to his\n"
                                "last position is used instead (e.g. another map device portal).");
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Clicking ------------------------------------------------------------------
    PluginUi::BeginCard("##travelClick");
    Heading(ICON_FA_HAND_POINTER, "Clicking the object",
            "The bot walks up to the object, stops, hovers it until the game highlights it, then clicks.");
    {
        const LabelColumn col({"Click from", "Hover before click", "Hold the click", "Extra hovers", "Aim height"});
        changed |= ui::FloatRow(col, "Click from", "##interactR", &t.interactRange, 2.f, 20.f, "%.0f cells",
                                "Walk this close before clicking.");
        changed |= ui::IntRow(col, "Hover before click", "##dwell", &t.hoverDwellMs, 40, 400, "%d ms",
                              "How long the cursor rests on the object before the click\n"
                              "(the game needs a moment to register the hover).");
        changed |= ui::IntRow(col, "Hold the click", "##hold", &t.pressHoldMs, 20, 200, "%d ms", "How long the button is held down.");
        changed |= ui::IntRow(col, "Extra hovers", "##rehover", &t.reHovers, 0, 4, "%d",
                              "When the game does not highlight the object, the cursor moves a few pixels this many\n"
                              "times before it clicks anyway.");
        changed |= ui::FloatRow(col, "Aim height", "##aimH", &t.tallObjectHeightFactor, 0.f, 1.f, "%.2f",
                                "Where on a tall object to aim: 0 = its foot, 1 = its top.\n"
                                "Retries vary this a little on their own.");
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Retries -------------------------------------------------------------------
    PluginUi::BeginCard("##travelRetry");
    Heading(ICON_FA_ROTATE, "Retries and waiting",
            "After a click the bot waits for its own area to change. If it does not, it walks up again and\n"
            "clicks with a slightly different aim, up to the number of tries.");
    {
        const LabelColumn col({"Wait for the area change", "Tries", "Look for the object"});
        changed |= ui::IntRow(col, "Wait for the area change", "##areaWait", &t.areaChangeTimeoutMs, 2000, 20000, "%d ms",
                              "How long one click may take to load the new area.");
        changed |= ui::IntRow(col, "Tries", "##tries", &t.maxAttempts, 1, 6, "%d",
                              "Approach-and-click attempts before the bot gives up on following.");
        changed |= ui::IntRow(col, "Look for the object", "##findObj", &t.findObjectMs, 1000, 15000, "%d ms",
                              "At his last position, wait this long for the transition or portal to show up\n"
                              "before giving up.");
    }
    changed |= Check("Walk to his last position after a failed travel", "seekFail", &t.seekOnFailure,
                     "On: the bot walks to where the leader vanished and waits there.\n"
                     "Off: it stays where the travel failed until the leader shows up again.");
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Diagnostics -------------------------------------------------------------
    if (PluginUi::BeginDiagnostics("The last time the leader vanished and how following him went.", "travelDiag")) {
        const TravelDiag d = TravelTask::Diagnostics();
        char line[320];
        if (v) {
            const char* now = v->vanish != VanishKind::None ? VanishKindName(v->vanish) : "none";
            std::snprintf(line, sizeof(line), "Leader: %s   |   vanished: %s   |   task: %s",
                          v->leaderVisible ? "in sight" : (v->leaderConfigured ? "not in sight" : "not set"), now,
                          v->taskName.empty() ? "-" : v->taskName.c_str());
            Hint(line);
            if (v->taskName == std::string("Travel") && !v->taskDetail.empty()) Hint(v->taskDetail.c_str());
        }
        if (!d.haveVanish) {
            Hint("No vanish handled yet in this session.");
        } else {
            const uint64_t nowMs = NowMs();
            const double ago = d.vanishAtMs && nowMs >= d.vanishAtMs ? static_cast<double>(nowMs - d.vanishAtMs) / 1000.0 : 0.0;
            std::snprintf(line, sizeof(line), "Last vanish: %s, %.0f s ago   |   %s%s%s", VanishKindName(d.kind), ago,
                          d.state.c_str(), d.detail.empty() ? "" : " - ", d.detail.c_str());
            Hint(line);
            if (d.haveObject) {
                char kind[64] = "";
                if (d.objectKind == InteractKind::Portal)
                    std::snprintf(kind, sizeof(kind), ", %s", travel::PortalKindName(d.portalType));
                std::snprintf(line, sizeof(line), "Object: %s #%u (%s%s), %.0f cells away when chosen, %s",
                              InteractKindName(d.objectKind), d.objectId, d.objectName.empty() ? "?" : d.objectName.c_str(),
                              kind, d.objectDistance, d.fallbackObject ? "another of the same kind" : "the one he took");
                Hint(line);
            }
            std::snprintf(line, sizeof(line), "Attempts: %d of %d   |   followed %d, failed %d since the plugin started",
                          d.attempts, d.maxAttempts, d.followed, d.failed);
            Hint(line);
        }
        if (!d.lastFailure.empty()) {
            const uint64_t nowMs = NowMs();
            const double ago = nowMs >= d.lastFailureMs ? static_cast<double>(nowMs - d.lastFailureMs) / 1000.0 : 0.0;
            std::snprintf(line, sizeof(line), "Last failure (%.0f s ago): %s", ago, d.lastFailure.c_str());
            ui::Warning(line);
        }
        if (v) {
            int transitions = 0, portals = 0, arenas = 0;
            for (const auto& it : v->interactables) {
                if (it.kind == InteractKind::AreaTransition) ++transitions;
                else if (it.kind == InteractKind::Portal) ++portals;
                else if (it.kind == InteractKind::BossArena) ++arenas;
            }
            std::snprintf(line, sizeof(line), "Known in this area: %d transitions, %d portals, %d boss arena entrances",
                          transitions, portals, arenas);
            Hint(line);
        }
        PluginUi::EndDiagnostics();
    }
    if (changed) MarkDirty();
}
