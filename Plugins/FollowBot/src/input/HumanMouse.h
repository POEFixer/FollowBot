// HumanMouse.h - human-like cursor trajectories for clicks (design §5.3).
//
// Port of POEFixer/core/HumanMouseTrajectory.h (ours; the old FollowBot's copy
// was byte-identical): SigmaDrift model - Fitts' law timing, log-normal primary
// and corrective sub-movements, direction-dependent curvature,
// Ornstein-Uhlenbeck drift, 8-12 Hz tremor and signal-dependent noise.
// Differences: the replay is CANCELLABLE (an abort callback is polled between
// samples) and moves the cursor with SendInput absolute moves, never
// SetCursorPos. Used for clicks only, never for move steering (a blocking
// trajectory per re-aim is what made the old bot lag behind its target).
#pragma once

#include "Win32Input.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <numbers>
#include <random>
#include <vector>

namespace fb::human {

struct TrajectoryPoint {
    double x, y;   // desktop pixels
    double t;      // ms from the start
};

struct Config {
    double fitts_a = 40.0;
    double fitts_b = 120.0;
    double target_width = 12.0;
    double undershoot_min = 0.93;
    double undershoot_max = 0.98;
    double peak_time_ratio = 0.35;
    double primary_sigma_min = 0.18;
    double primary_sigma_max = 0.28;
    double overshoot_prob = 0.12;
    double overshoot_min = 1.02;
    double overshoot_max = 1.06;
    double correction_sigma_min = 0.12;
    double correction_sigma_max = 0.20;
    double second_correction_prob = 0.20;
    double curvature_scale = 0.020;
    double ou_theta = 3.5;
    double ou_sigma = 0.8;
    double tremor_freq_min = 8.0;
    double tremor_freq_max = 12.0;
    double tremor_amp_min = 0.10;
    double tremor_amp_max = 0.40;
    double sdn_k = 0.03;
    double sample_dt_mean = 7.8;
    double gamma_shape = 3.5;
    double speed_factor = 1.0;   // lower = faster
};

// The bot's profile: speed, curvature and tremor scale the defaults (the
// design's "fast profile": speed 0.45, low curvature / tremor, no overshoot).
inline Config MakeConfig(double speed, double curvature, double tremor) {
    Config c;
    c.speed_factor = std::clamp(speed, 0.2, 3.0);
    c.curvature_scale = 0.020 * std::clamp(curvature, 0.0, 3.0);
    c.tremor_amp_min = 0.10 * std::clamp(tremor, 0.0, 3.0);
    c.tremor_amp_max = 0.40 * std::clamp(tremor, 0.0, 3.0);
    c.ou_sigma = 0.8 * std::clamp(tremor, 0.0, 3.0);
    c.sdn_k = 0.03 * std::clamp(tremor, 0.0, 3.0);
    c.overshoot_prob = 0.0;
    return c;
}

namespace detail {
inline double normal_cdf(double x) { return 0.5 * (1.0 + std::erf(x / std::numbers::sqrt2)); }
inline double lognormal_cdf(double t, double t0, double mu, double sigma) {
    if (t <= t0) return 0.0;
    return normal_cdf((std::log(t - t0) - mu) / sigma);
}
inline double lognormal_pdf(double t, double t0, double mu, double sigma) {
    if (t <= t0) return 0.0;
    const double dt = t - t0;
    const double z = (std::log(dt) - mu) / sigma;
    return 1.0 / (sigma * std::sqrt(2.0 * std::numbers::pi) * dt) * std::exp(-0.5 * z * z);
}
inline double curvature_profile(double s) {
    if (s <= 0.0 || s >= 1.0) return 0.0;
    const double v = s * s * (1.0 - s) * (1.0 - s) * (1.0 - s);
    constexpr double norm = 0.4 * 0.4 * 0.6 * 0.6 * 0.6;
    return v / norm;
}
inline double direction_factor(double angle) {
    const double sa = std::abs(std::sin(angle));
    const double ca = std::abs(std::cos(angle));
    return 0.5 + 0.8 * sa - 0.15 * ca;
}
}  // namespace detail

inline std::vector<TrajectoryPoint> Generate(double x0, double y0, double x1, double y1, const Config& cfg = {},
                                             uint64_t seed = 0) {
    std::mt19937_64 rng(seed ? seed : std::random_device{}());
    auto uniform = [&](double lo, double hi) { return std::uniform_real_distribution<double>(lo, hi)(rng); };
    auto normal = [&](double m, double s) { return std::normal_distribution<double>(m, s)(rng); };
    auto gamma_dist = [&](double shape, double scale) { return std::gamma_distribution<double>(shape, scale)(rng); };

    const double dx = x1 - x0, dy = y1 - y0;
    const double distance = std::hypot(dx, dy);
    const double direction = std::atan2(dy, dx);
    if (distance < 3.0) return {{x0, y0, 0.0}, {x1, y1, 15.0}};

    const double tx = dx / distance, ty = dy / distance;
    const double nx = -ty, ny = tx;
    const double id = std::log2(distance / cfg.target_width + 1.0);
    double mt = (cfg.fitts_a + cfg.fitts_b * id) * cfg.speed_factor * std::exp(normal(0.0, 0.08));
    mt = (std::max)(mt, 50.0);

    const bool overshoot = uniform(0.0, 1.0) < cfg.overshoot_prob;
    const double reach = overshoot ? uniform(cfg.overshoot_min, cfg.overshoot_max)
                                   : uniform(cfg.undershoot_min, cfg.undershoot_max);
    const double primary_D = distance * reach;
    const double primary_sigma = uniform(cfg.primary_sigma_min, cfg.primary_sigma_max);
    const double peak_t = mt * uniform(cfg.peak_time_ratio - 0.03, cfg.peak_time_ratio + 0.03);
    const double primary_mu = std::log(peak_t) + primary_sigma * primary_sigma;

    struct Correction { double D, t0, mu, sigma, dir_x, dir_y; };
    std::vector<Correction> corrections;
    const double remaining = distance - primary_D;
    if (std::abs(remaining) > 0.5) {
        const double dir = remaining > 0.0 ? 1.0 : -1.0;
        const double cD = std::abs(remaining) * uniform(0.88, 1.02);
        const double cS = uniform(cfg.correction_sigma_min, cfg.correction_sigma_max);
        const double cPeak = mt * uniform(0.12, 0.18);
        corrections.push_back({cD, mt * uniform(0.55, 0.68), std::log(cPeak) + cS * cS, cS, tx * dir, ty * dir});
        const double left = remaining - cD * dir;
        if (std::abs(left) > 0.3 && uniform(0.0, 1.0) < cfg.second_correction_prob) {
            const double d2 = left > 0.0 ? 1.0 : -1.0;
            const double cD2 = std::abs(left) * uniform(0.85, 1.05);
            const double cS2 = uniform(0.10, 0.16);
            const double cP2 = mt * uniform(0.08, 0.12);
            corrections.push_back({cD2, mt * uniform(0.78, 0.88), std::log(cP2) + cS2 * cS2, cS2, tx * d2, ty * d2});
        }
    }

    const double curv_amp = distance * cfg.curvature_scale * detail::direction_factor(direction) * normal(0.0, 1.0);
    const double tremor_freq = uniform(cfg.tremor_freq_min, cfg.tremor_freq_max);
    const double tremor_amp = cfg.tremor_amp_max > cfg.tremor_amp_min ? uniform(cfg.tremor_amp_min, cfg.tremor_amp_max)
                                                                      : cfg.tremor_amp_min;
    const double tph_x = uniform(0.0, 2.0 * std::numbers::pi);
    const double tph_y = uniform(0.0, 2.0 * std::numbers::pi);
    double ou_x = 0.0, ou_y = 0.0;

    const double total_t = mt * 1.15;
    const double g_scale = cfg.sample_dt_mean / cfg.gamma_shape;
    std::vector<double> times = {0.0};
    for (double t = 0.0; t < total_t;) {
        const double dt = std::clamp(gamma_dist(cfg.gamma_shape, g_scale), 2.0, 25.0);
        t += dt;
        if (t <= total_t + 15.0) times.push_back(t);
    }

    std::vector<TrajectoryPoint> result;
    result.reserve(times.size() + 1);
    for (size_t i = 0; i < times.size(); ++i) {
        const double t = times[i];
        const double dt_ms = (i > 0) ? (t - times[i - 1]) : cfg.sample_dt_mean;
        const double dt_s = dt_ms / 1000.0;
        const double s = detail::lognormal_cdf(t, 0.0, primary_mu, primary_sigma);
        double bx = x0 + tx * primary_D * s;
        double by = y0 + ty * primary_D * s;
        bx += nx * curv_amp * detail::curvature_profile(s);
        by += ny * curv_amp * detail::curvature_profile(s);
        for (const auto& c : corrections) {
            const double cs = detail::lognormal_cdf(t, c.t0, c.mu, c.sigma);
            bx += c.dir_x * c.D * cs;
            by += c.dir_y * c.D * cs;
        }
        double speed = primary_D * detail::lognormal_pdf(t, 0.0, primary_mu, primary_sigma);
        for (const auto& c : corrections) speed += c.D * detail::lognormal_pdf(t, c.t0, c.mu, c.sigma);
        ou_x += -cfg.ou_theta * ou_x * dt_s + cfg.ou_sigma * std::sqrt(dt_s) * normal(0.0, 1.0);
        ou_y += -cfg.ou_theta * ou_y * dt_s + cfg.ou_sigma * std::sqrt(dt_s) * normal(0.0, 1.0);
        const double t_s = t / 1000.0;
        const double trem_mod = 1.0 / (1.0 + speed * 0.3);
        const double tr_x = tremor_amp * trem_mod * std::sin(2.0 * std::numbers::pi * tremor_freq * t_s + tph_x);
        const double tr_y = tremor_amp * trem_mod * std::sin(2.0 * std::numbers::pi * tremor_freq * t_s + tph_y);
        const double sdn_x = cfg.sdn_k * speed * normal(0.0, 1.0);
        const double sdn_y = cfg.sdn_k * speed * normal(0.0, 1.0);
        result.push_back({bx + ou_x + tr_x + sdn_x, by + ou_y + tr_y + sdn_y, t});
    }
    // End exactly on the target: the click must land where it was aimed.
    const double tEnd = result.empty() ? 15.0 : result.back().t + 2.0;
    result.push_back({x1, y1, tEnd});
    return result;
}

// Replays a trajectory with SendInput absolute moves on a QPC clock. Polls
// `abort` between samples; returns false when aborted (the cursor stays where
// the replay stopped).
inline bool Replay(const std::vector<TrajectoryPoint>& traj, const std::function<bool()>& abort) {
    if (traj.empty()) return true;
    LARGE_INTEGER freq, start, now;
    ::QueryPerformanceFrequency(&freq);
    ::QueryPerformanceCounter(&start);
    auto moveTo = [](const TrajectoryPoint& p) {
        POINT d{static_cast<LONG>(std::lround(p.x)), static_cast<LONG>(std::lround(p.y))};
        win::MoveCursorDesktop(d);
    };
    moveTo(traj.front());
    size_t idx = 1;
    while (idx < traj.size()) {
        if (abort && abort()) return false;
        ::QueryPerformanceCounter(&now);
        const double elapsed = static_cast<double>(now.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
        bool moved = false;
        while (idx < traj.size() && traj[idx].t <= elapsed) {
            ++idx;
            moved = true;
        }
        if (moved) moveTo(traj[idx - 1]);
        if (idx < traj.size() && traj[idx].t - elapsed > 2.0) ::Sleep(1);
    }
    return true;
}

}  // namespace fb::human
