// UiFollow.cpp - the Follow & Movement tab: following distances and goal,
// steering, sprint, navigation and stuck recovery.
// FB_TESTS_EXCLUDE: UI (ImGui + PluginSDK); not built into tests/.
#include "../FollowBotPlugin.h"

#include "Tabs.h"

#include <cstdio>

using namespace fb;
using PluginUi::Check;
using PluginUi::Heading;
using PluginUi::Hint;
using PluginUi::LabelColumn;
using PluginUi::Tip;

void FollowBotPlugin::DrawTabFollow() {
    Profile& p = EditProfile();
    FollowSettings& f = p.follow;
    bool changed = false;

    // --- Following ---------------------------------------------------------
    PluginUi::BeginCard("##following");
    Heading(ICON_FA_PERSON_WALKING, "Following",
            "Distances are in grid cells (one cell is about a tenth of a character's height).\n"
            "The bot starts walking when the leader is farther than the follow distance and stops\n"
            "once it is closer than the stop distance, so small moves of the leader do not cause jitter.");
    {
        const LabelColumn col({"Follow distance", "Stop distance", "Predict ahead", "Formation slot", "Slot spacing",
                               "Wait after zoning", "Keep last goal"});
        changed |= ui::FloatRow(col, "Follow distance", "##followDist", &f.followDistance, 6.f, 150.f, "%.0f cells",
                                "Start walking when the leader is farther than this.");
        changed |= ui::FloatRow(col, "Stop distance", "##stopDist", &f.stopDistance, 2.f, 140.f, "%.0f cells",
                                "Stop walking once closer than this (always below the follow distance).");
        ImGui::BeginDisabled(!f.predict);
        changed |= ui::IntRow(col, "Predict ahead", "##predictMs", &f.predictMs, 0, 2000, "%d ms",
                              "Aim where the leader will be this far in the future (from his speed),\n"
                              "kept on walkable ground.");
        ImGui::EndDisabled();
        changed |= ui::IntRow(col, "Formation slot", "##formSlot", &f.formationSlot, 0, 5, "%d",
                              "0 = straight at the leader. 1..5 give several followers their own place behind him\n"
                              "(each follower a different slot).");
        ImGui::BeginDisabled(f.formationSlot == 0);
        changed |= ui::FloatRow(col, "Slot spacing", "##formSpacing", &f.formationSpacing, 2.f, 30.f, "%.0f cells",
                                "Distance between formation places.");
        ImGui::EndDisabled();
        changed |= ui::IntRow(col, "Wait after zoning", "##reacquire", &f.reacquireMs, 1000, 60000, "%d ms",
                              "After you change area, wait this long for the leader to appear before reporting\n"
                              "\"leader not in this area\".");
        changed |= ui::IntRow(col, "Keep last goal", "##lostHold", &f.lostHoldMs, 0, 10000, "%d ms",
                              "When the leader just left sight, keep walking to where he was for this long.");
    }
    changed |= Check("Predict the leader's movement", "predict", &f.predict, "Aims ahead of a moving leader.");
    changed |= Check("Aim at the end of his route while he moves", "followDest", &f.followDestination,
                     "Uses the leader's own route (where he clicked) instead of his position.");
    changed |= Check("Follow in town and hideout", "followTown", &f.followInTownHideout,
                     "Off: the bot stands still in towns and hideouts.");
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Movement ------------------------------------------------------------
    PluginUi::BeginCard("##movement");
    Heading(ICON_FA_ROUTE, "Steering",
            "The bot holds the Move key and keeps the cursor a fixed distance from the character in the\n"
            "walking direction (never on the skill bar or flask belt).");
    {
        const LabelColumn col({"Look ahead", "Cursor distance", "Re-aim every", "Re-aim when moved"});
        changed |= ui::FloatRow(col, "Look ahead", "##lookahead", &f.lookaheadCells, 4.f, 40.f, "%.0f cells",
                                "How far along the path the cursor points (in sight of the character).");
        changed |= ui::IntRow(col, "Cursor distance", "##cursorDist", &f.cursorDistancePx, 60, 600, "%d px",
                              "Distance of the cursor from the character on screen.");
        changed |= ui::IntRow(col, "Re-aim every", "##reaimMs", &f.reaimMs, 16, 500, "%d ms", "Cursor update interval while walking.");
        changed |= ui::IntRow(col, "Re-aim when moved", "##reaimPx", &f.reaimPx, 1, 60, "%d px",
                              "The cursor is moved at once when its target shifts more than this.");
    }
    changed |= Check("Activation click after focus / zoning", "activation", &f.activationClick,
                     "The game ignores a held key until a real click lands after it regains focus or after a\n"
                     "zone change: one click just below the character first.");
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Sprint ----------------------------------------------------------------
    PluginUi::BeginCard("##sprint");
    Heading(ICON_FA_PERSON_RUNNING, "Sprint", "Holds the Dodge / Sprint key on long, straight stretches without enemies.");
    changed |= Check("Sprint to catch up", "sprint", &f.sprint, "Needs the Dodge / Sprint key (General tab).");
    ImGui::BeginDisabled(!f.sprint || f.sprintKey == 0);
    {
        const LabelColumn col({"Farther than", "No enemy within", "Hold at most", "Pause between"});
        changed |= ui::FloatRow(col, "Farther than", "##sprintDist", &f.sprintDistance, 10.f, 200.f, "%.0f cells",
                                "Sprint while the goal is farther than this; stop below 60 % of it.");
        changed |= ui::FloatRow(col, "No enemy within", "##sprintEnemy", &f.sprintNoEnemyRadius, 0.f, 120.f, "%.0f cells",
                                "Never sprint with an enemy this close.");
        changed |= ui::IntRow(col, "Hold at most", "##sprintHold", &f.sprintMaxHoldMs, 500, 60000, "%d ms", "Longest single sprint.");
        changed |= ui::IntRow(col, "Pause between", "##sprintCd", &f.sprintCooldownMs, 0, 30000, "%d ms", "Rest between two sprints.");
    }
    ImGui::EndDisabled();
    if (f.sprint && f.sprintKey == 0) Hint("Set the Dodge / Sprint key in the General tab to sprint.");
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Navigation ------------------------------------------------------------
    PluginUi::BeginCard("##nav");
    Heading(ICON_FA_DIAGRAM_PROJECT, "Path finding",
            "Paths are planned on the game's walkable grid and keep some distance from walls when they can.");
    {
        const LabelColumn col({"Plan again every", "Keep off walls", "Search limit", "Snap radius"});
        changed |= ui::IntRow(col, "Plan again every", "##replan", &f.replanMs, 200, 10000, "%d ms",
                              "Also plans again when the leader moved or the character left the path.");
        changed |= ui::FloatRow(col, "Keep off walls", "##wallPen", &f.wallPenalty, 0.f, 5.f, "%.1f",
                                "0 = the shortest path (hugs walls); higher keeps more distance from walls.");
        changed |= ui::IntRow(col, "Search limit", "##astarIter", &f.astarMaxIterations, 10000, 4000000, "%d",
                              "Upper bound for one path search (cells examined).");
        changed |= ui::IntRow(col, "Snap radius", "##snap", &f.snapRadius, 1, 100, "%d cells",
                              "A goal standing on an unwalkable spot moves to the nearest walkable cell within this.");
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Stuck ------------------------------------------------------------------
    PluginUi::BeginCard("##stuck");
    Heading(ICON_FA_ARROWS_ROTATE, "Getting unstuck",
            "Stuck = the character hardly moved for a moment, or made no progress toward the goal.\n"
            "Then, in order: plan around the spot, side-step, open a door, walk back a little, dodge,\n"
            "try escape directions, and finally give up the goal for a while.");
    {
        StuckSettings& k = f.stuck;
        const LabelColumn col({"Moved less than", "Within", "No progress for", "Calm again after", "Block radius",
                               "Block for", "Side-step", "Door search", "Walk back at least", "After zoning wait",
                               "Retry following after"});
        changed |= ui::FloatRow(col, "Moved less than", "##stMin", &k.minDisplacement, 0.2f, 10.f, "%.1f cells", "Stuck threshold.");
        changed |= ui::IntRow(col, "Within", "##stWin", &k.windowMs, 300, 10000, "%d ms", "Time window of the threshold above.");
        changed |= ui::IntRow(col, "No progress for", "##stNoProg", &k.noProgressMs, 500, 20000, "%d ms",
                              "No progress toward the goal for this long also counts as stuck.");
        changed |= ui::IntRow(col, "Calm again after", "##stReset", &k.ladderResetMs, 500, 30000, "%d ms",
                              "Walking freely this long starts the recovery steps from the first one again.");
        changed |= ui::IntRow(col, "Block radius", "##stBlockR", &k.blockRadius, 0, 8, "%d cells",
                              "The spot ahead is avoided in this radius when planning around it.");
        changed |= ui::IntRow(col, "Block for", "##stBlockMs", &k.blockMs, 1000, 120000, "%d ms", "How long that spot stays avoided.");
        changed |= ui::IntRow(col, "Side-step", "##stSide", &k.sideStepMs, 100, 3000, "%d ms", "Length of a side-step.");
        changed |= ui::FloatRow(col, "Door search", "##stDoor", &k.doorSearchCells, 2.f, 40.f, "%.0f cells",
                                "A closed door this close to the path is clicked.");
        changed |= ui::FloatRow(col, "Walk back at least", "##stBack", &k.backtrackMinCells, 2.f, 60.f, "%.0f cells",
                                "Distance of the earlier position the bot walks back to.");
        changed |= ui::IntRow(col, "After zoning wait", "##stIgn", &k.ignitionTimeoutMs, 0, 30000, "%d ms",
                              "After a zone change the game may keep the character still for a while: stuck checks wait\n"
                              "until it moved once or this long.");
        changed |= ui::IntRow(col, "Retry following after", "##stRetry", &k.followRetryMs, 500, 30000, "%d ms",
                              "When every step failed, following pauses this long and tries again.");
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    if (PluginUi::BeginDiagnostics("What the follow logic sees right now.", "followDiag")) {
        const BotView* v = m_uiView.get();
        if (!v) {
            Hint("No data yet.");
        } else {
            char line[256];
            std::snprintf(line, sizeof(line), "Mode: %s   |   moving: %s%s   |   plan: %s (%d points)", v->mode.c_str(),
                          v->moving ? "yes" : "no", v->sprinting ? " (sprint)" : "", v->planStatus.c_str(),
                          static_cast<int>(v->path.size()));
            Hint(line);
            std::snprintf(line, sizeof(line), "Leader: %s, %s, %.0f cells, speed %.1f cells/s, route %d nodes",
                          v->leaderName.empty() ? "-" : v->leaderName.c_str(), v->leaderVisible ? "visible" : "not visible",
                          v->leaderDistance, v->leaderSpeed, static_cast<int>(v->leaderRoute.size()));
            Hint(line);
            std::snprintf(line, sizeof(line), "Stuck episodes: %d   |   current step: %s   |   avoided spots: %d",
                          v->stuckEpisodes, v->stuckStep.c_str(), static_cast<int>(v->blocks.size()));
            Hint(line);
        }
        PluginUi::EndDiagnostics();
    }
    if (changed) MarkDirty();
}
