// Clock.h - the bot's single time base: steady milliseconds.
//
// GetTickCount64 advances in ~15.6 ms steps, too coarse for a 16 ms tick, so
// everything the bot times (gates, dwell, stuck windows) uses steady_clock.
#pragma once

#include <chrono>
#include <cstdint>

namespace fb {

inline uint64_t NowMs() {
    using namespace std::chrono;
    return static_cast<uint64_t>(duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

// Milliseconds since `then`, 0 when `then` lies in the future or is unset (0).
inline uint64_t ElapsedMs(uint64_t now, uint64_t then) {
    return (then == 0 || now < then) ? 0 : now - then;
}

// True when `then` is set and at least `ms` have passed since it.
inline bool Passed(uint64_t now, uint64_t then, uint64_t ms) {
    return then != 0 && now >= then && now - then >= ms;
}

}  // namespace fb
