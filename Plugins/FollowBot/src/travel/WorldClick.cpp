// WorldClick.cpp - see WorldClick.h.
#include "WorldClick.h"

#include "../common/BotLog.h"
#include "../input/Actuator.h"
#include "../move/Mover.h"
#include "../move/StuckRecovery.h"
#include "../nav/PathPlanner.h"
#include "../world/Projection.h"

namespace fb {

void WorldClick::Start(const WorldClickTarget& target, const WorldClickParams& params, uint64_t now) {
    m_target = target;
    m_params = params;
    m_phase = Phase::Approach;
    m_startMs = now;
    m_settleMs = 0;
    m_fail.clear();
}

void WorldClick::Abort(BotContext& ctx) {
    if (Active()) ctx.mover.Stop(ctx.act);
    m_phase = Phase::Idle;
}

WorldClickStatus WorldClick::Fail(BotContext& ctx, const char* why) {
    ctx.mover.Stop(ctx.act);
    m_phase = Phase::Failed;
    m_fail = why;
    ctx.log.Add(LogCat::Task, "Click on object %u failed: %s", m_target.id, why);
    return WorldClickStatus::Failed;
}

WorldClickStatus WorldClick::Tick(BotContext& ctx) {
    const World& w = ctx.w;
    switch (m_phase) {
        case Phase::Idle:
            return WorldClickStatus::Failed;
        case Phase::Done:
            return WorldClickStatus::Clicked;
        case Phase::Failed:
            return WorldClickStatus::Failed;
        case Phase::Approach: {
            if (ctx.now - m_startMs > static_cast<uint64_t>(m_params.approachTimeoutMs)) return Fail(ctx, "approach timed out");
            const float dist = Dist(w.self.grid, m_target.grid);
            ctx.report.hasTarget = true;
            ctx.report.target = m_target.grid;
            if (dist <= m_params.interactRange) {
                ctx.mover.Stop(ctx.act);
                ctx.stuck.NoteIdle(w, ctx.now);
                m_phase = Phase::Settle;
                m_settleMs = ctx.now;
                m_settlePos = w.self.grid;
                return WorldClickStatus::Running;
            }
            if (ctx.cursorBusy) {
                ctx.mover.Stop(ctx.act);
                ctx.stuck.NoteIdle(w, ctx.now);
                return WorldClickStatus::Running;
            }
            const PlanStatus ps = ctx.planner.Update(w.self.grid, m_target.grid, ctx.now);
            if (ps == PlanStatus::Unreachable) return Fail(ctx, "object unreachable");
            const RecoveryState rs = ctx.stuck.Tick(w, ctx.mover, ctx.planner, ctx.act, ctx.log, m_target.grid,
                                                    m_params.interactRange, true, ctx.now);
            if (rs == RecoveryState::GaveUp) {
                ctx.stuck.Reset(ctx.now);
                return Fail(ctx, "stuck on the way");
            }
            if (rs == RecoveryState::Recovering) return WorldClickStatus::Running;
            const MoveStatus ms = ctx.mover.Steer(w, ctx.planner.Path(), m_target.grid, ctx.act, &ctx.planner, ctx.now);
            if (ms == MoveStatus::NoKey) return Fail(ctx, "no move key set");
            return WorldClickStatus::Running;
        }
        case Phase::Settle: {
            // Wait until the character stands still (or 250 ms).
            const bool still = Dist(w.self.grid, m_settlePos) < 0.3f && ctx.now - m_settleMs >= 60;
            m_settlePos = w.self.grid;
            if (!still && ctx.now - m_settleMs < 250) return WorldClickStatus::Running;
            if (Dist(w.self.grid, m_target.grid) > m_params.interactRange + 3.f) {
                // Slid away (pushed, or the object moved): approach again.
                m_phase = Phase::Approach;
                return WorldClickStatus::Running;
            }
            m_phase = Phase::Click;
            [[fallthrough]];
        }
        case Phase::Click: {
            if (ctx.cursorBusy) return WorldClickStatus::Running;
            Vec2 screen;
            bool ok = false;
            if (m_target.screenOffsetY > 0.f) {
                ok = ProjectWorld(w.proj, m_target.world, screen);
                screen.y -= m_target.screenOffsetY;
            } else {
                ok = ProjectLifted(w.proj, m_target.world, m_target.modelHeight * m_params.heightFactor, screen);
            }
            if (!ok || !OnScreen(w.proj, screen, 4.f)) return Fail(ctx, "object not on screen");
            std::function<bool()> verify;
            if (ctx.hooks.isTargeted && m_target.targetableAddr != 0) {
                const uintptr_t addr = m_target.targetableAddr;
                const auto hook = ctx.hooks.isTargeted;
                verify = [hook, addr] { return hook(addr); };
            }
            const ActResult r = ctx.act.WorldClick(screen, m_params.dwellMs, m_params.holdMs, m_params.button, verify,
                                                   m_params.reHovers);
            if (r == ActResult::Ok) {
                m_phase = Phase::Done;
                ctx.log.Add(LogCat::Task, "Clicked object %u at (%.0f, %.0f)", m_target.id, m_target.grid.x, m_target.grid.y);
                return WorldClickStatus::Clicked;
            }
            if (r == ActResult::NotReady || r == ActResult::CursorBusy) return WorldClickStatus::Running;
            return Fail(ctx, ActResultName(r));
        }
    }
    return WorldClickStatus::Failed;
}

}  // namespace fb
