// InputHook.h - user-activity detection for the "pause while I play" gate
// (design §5, input hook thread).
//
// A thread with WH_MOUSE_LL + WH_KEYBOARD_LL hooks and a message loop. It
// counts only NON-injected events (LLMHF_INJECTED / LLKHF_INJECTED clear: the
// bot's own SendInput never counts) while the game window is in front. It
// never blocks or swallows input: the callback stamps an atomic and passes the
// event on. Only one monitor may run per process (the hook procedures are
// global); a second Start() fails.
#pragma once

#include <Windows.h>

#include <atomic>
#include <cstdint>
#include <thread>

namespace fb {

class UserActivityMonitor {
public:
    UserActivityMonitor() = default;
    ~UserActivityMonitor() { Stop(); }
    UserActivityMonitor(const UserActivityMonitor&) = delete;
    UserActivityMonitor& operator=(const UserActivityMonitor&) = delete;

    bool Start();
    void Stop();
    bool Running() const { return m_running.load(); }

    // The game window whose foreground state gates counting (any thread).
    void SetGameWindow(HWND game) { m_game.store(reinterpret_cast<uintptr_t>(game)); }

    // steady ms (fb::NowMs) of the last user event while the game was in front; 0 = none.
    uint64_t LastActivityMs() const { return m_lastActivityMs.load(); }
    // Ignore mouse movement below this many pixels between events (jitter of a
    // resting mouse); clicks, wheel and keys always count.
    void SetMoveThresholdPx(int px) { m_moveThreshold.store(px); }

private:
    static LRESULT CALLBACK MouseProc(int code, WPARAM wp, LPARAM lp);
    static LRESULT CALLBACK KeyboardProc(int code, WPARAM wp, LPARAM lp);
    void ThreadMain();
    void Stamp();
    bool GameInFront() const;

    std::thread           m_thread;
    std::atomic<bool>     m_running{false};
    std::atomic<bool>     m_exited{false};
    std::atomic<DWORD>    m_threadId{0};
    std::atomic<uintptr_t> m_game{0};
    std::atomic<uint64_t> m_lastActivityMs{0};
    std::atomic<int>      m_moveThreshold{3};
    POINT                 m_lastMove{};   // hook thread only
    bool                  m_haveMove = false;
};

}  // namespace fb
