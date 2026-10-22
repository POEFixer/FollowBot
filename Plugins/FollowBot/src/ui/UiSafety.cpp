// UiSafety.cpp - the Death & Safety tab: respawn, death limits, safety stops,
// humanisation and the worker rate.
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

void FollowBotPlugin::DrawTabSafety() {
    Profile& p = EditProfile();
    DeathSettings& d = p.death;
    SafetySettings& sf = m_settings.global.safety;
    HumanSettings& hu = m_settings.global.human;
    const BotView* v = m_uiView.get();
    bool changed = false;

    // --- Death -----------------------------------------------------------
    PluginUi::BeginCard("##death");
    Heading(ICON_FA_SKULL, "Death",
            "When the character dies the bot releases every key, waits a moment and clicks the respawn button\n"
            "of the death screen (found by its locale-independent name).");
    changed |= Check("Respawn automatically", "autoRespawn", &d.autoRespawn, "Off: the bot holds still until you respawn.");
    ImGui::BeginDisabled(!d.autoRespawn);
    changed |= Check("Prefer \"Respawn at checkpoint\"", "preferCp", &d.preferCheckpoint,
                     "Uses the checkpoint button when the death screen offers it.");
    changed |= Check("Allow the generic respawn button", "genericResp", &d.allowGenericRespawn,
                     "Lets the bot press the death screen's general respawn button: after the checkpoint buttons,\n"
                     "or before them when \"Prefer Respawn at checkpoint\" is off.\n"
                     "Warning: what that button does depends on the game state - it may respawn you in town or\n"
                     "the hideout (leaving the map), or log you out where respawning is not possible.\n"
                     "Off: the bot only presses checkpoint buttons and otherwise waits for you.");
    {
        const LabelColumn col({"Wait before clicking", "Random extra", "Retry every", "Tries"});
        changed |= ui::IntRow(col, "Wait before clicking", "##respDelay", &d.respawnDelayMs, 500, 15000, "%d ms",
                              "Time between death and the first click.");
        changed |= ui::IntRow(col, "Random extra", "##respJitter", &d.respawnJitterMs, 0, 5000, "%d ms", "Adds 0..this to the wait.");
        changed |= ui::IntRow(col, "Retry every", "##respRetry", &d.retryIntervalMs, 500, 10000, "%d ms", "When a click did not respawn.");
        changed |= ui::IntRow(col, "Tries", "##respTries", &d.maxRetries, 1, 20, "%d", "Clicks before giving up (then respawn manually).");
    }
    ImGui::EndDisabled();
    {
        const LabelColumn col({"Stop after deaths in a map", "Stop after deaths in total", "Dead after"});
        changed |= ui::IntRow(col, "Stop after deaths in a map", "##maxDeathMap", &d.maxDeathsPerMap, 0, 50, "%d",
                              "The bot stops at this many deaths in one map (0 = never).");
        changed |= ui::IntRow(col, "Stop after deaths in total", "##maxDeathSess", &d.maxDeathsPerSession, 0, 500, "%d",
                              "The bot stops at this many deaths since Start (0 = never).");
        changed |= ui::IntRow(col, "Dead after", "##deadConfirm", &d.deadConfirmMs, 100, 3000, "%d ms",
                              "Life at 0 for this long counts as dead.");
    }
    if (!ctx()->Ui.HasFindElements())
        ui::HostNote("Automatic respawn needs POEFixer 365 or newer (locale-independent UI names). Until then the bot "
                     "holds still when dead and you respawn yourself.");
    if (v) {
        char line[128];
        std::snprintf(line, sizeof(line), "Deaths: %d in this map, %d since Start.", v->deathsMap, v->deathsSession);
        Hint(line);
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Safety ----------------------------------------------------------
    PluginUi::BeginCard("##safety");
    Heading(ICON_FA_SHIELD_HALVED, "Safety",
            "The bot never acts while another window (or the POEFixer menu) is in front, during loading, in the\n"
            "game menu, while POEFixer's own automation runs, or while a panel it did not open is visible.");
    changed |= Check("Pause while I use mouse or keyboard", "userPause", &sf.userActivityPause,
                     "Your own input in the game (not the bot's) pauses it for a moment, so you can take over\n"
                     "any time just by playing.");
    {
        const LabelColumn col({"Pause for", "Resume after override", "Stop after", "Close it after"});
        ImGui::BeginDisabled(!sf.userActivityPause);
        changed |= ui::IntRow(col, "Pause for", "##userPauseMs", &sf.userActivityPauseMs, 200, 30000, "%d ms",
                              "How long your input pauses the bot.");
        ImGui::EndDisabled();
        changed |= ui::IntRow(col, "Resume after override", "##ovResume", &sf.overrideResumeMs, 0, 5000, "%d ms",
                              "After you release the hold-to-take-over key.");
        changed |= ui::IntRow(col, "Stop after", "##maxRun", &sf.maxRuntimeMinutes, 0, 1440, "%d min",
                              "The bot stops by itself after this long (0 = never).");
        ImGui::BeginDisabled(!sf.closeForeignPanels);
        changed |= ui::IntRow(col, "Close it after", "##closePanelMs", &sf.closeForeignPanelsAfterMs, 1000, 30000, "%d ms",
                              "A panel the bot did not open is closed with Esc after it stayed open this long.");
        ImGui::EndDisabled();
    }
    changed |= Check("Close panels the bot did not open", "closePanels", &sf.closeForeignPanels,
                     "Off: the bot waits until you close an inventory or stash panel yourself.");
    changed |= Check("Emergency exit at low life in maps", "emergency", &sf.emergencyExit,
                     "Types /exit once (back to character selection) and stops the bot when life falls below the\n"
                     "threshold inside a map.");
    if (sf.emergencyExit) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
        changed |= ImGui::SliderInt("##emergencyHp", &sf.emergencyExitHpPct, 1, 90, "below %d %%", ImGuiSliderFlags_AlwaysClamp);
    }
    PluginUi::EndCard();
    ImGui::Spacing();

    // --- Humanisation ----------------------------------------------------
    PluginUi::BeginCard("##human");
    Heading(ICON_FA_HAND, "Input timing",
            "Every key press and click waits for a minimum gap plus a random extra; clicks land a little off\n"
            "centre; cursor paths for clicks look like a hand's.");
    {
        const LabelColumn col({"Min gap between actions", "Random extra", "Click spread", "Cursor speed",
                               "Cursor curve", "Cursor tremor", "Re-aim randomness", "Bot tick"});
        changed |= ui::IntRow(col, "Min gap between actions", "##gateMin", &hu.actionMinIntervalMs, 30, 1000, "%d ms",
                              "No two presses or clicks closer than this.");
        changed |= ui::IntRow(col, "Random extra", "##gateJit", &hu.actionJitterMs, 0, 500, "%d ms", "Adds 0..this to the gap.");
        changed |= ui::IntRow(col, "Click spread", "##clickJit", &hu.clickJitterPx, 0, 20, "%d px",
                              "Clicks land up to this far from the centre (mostly near it).");
        ImGui::BeginDisabled(!hu.humanMouse);
        changed |= ui::FloatRow(col, "Cursor speed", "##hmSpeed", &hu.mouseSpeed, 0.2f, 3.f, "%.2f", "Lower = faster cursor paths.");
        changed |= ui::FloatRow(col, "Cursor curve", "##hmCurve", &hu.mouseCurvature, 0.f, 3.f, "%.2f", "How much the path bends.");
        changed |= ui::FloatRow(col, "Cursor tremor", "##hmTremor", &hu.mouseTremor, 0.f, 3.f, "%.2f", "Small hand tremor along the path.");
        ImGui::EndDisabled();
        changed |= ui::IntRow(col, "Re-aim randomness", "##reaimJit", &hu.reaimJitterMs, 0, 200, "%d ms",
                              "Random extra to the steering update interval.");
        changed |= ui::IntRow(col, "Bot tick", "##tickMs", &m_settings.global.tickMs, 10, 50, "%d ms",
                              "How often the bot reads the game and decides (lower = more responsive, more CPU).");
    }
    changed |= Check("Human-like cursor paths for clicks", "humanMouse", &hu.humanMouse,
                     "Only for clicks on objects; steering while walking always moves the cursor directly.");
    PluginUi::EndCard();
    if (changed) MarkDirty();
}
