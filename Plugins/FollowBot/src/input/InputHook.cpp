// InputHook.cpp - see InputHook.h.
#include "InputHook.h"

#include "../common/Clock.h"

#include <cstdlib>

namespace fb {

namespace {
std::atomic<UserActivityMonitor*> g_monitor{nullptr};
}  // namespace

bool UserActivityMonitor::Start() {
    if (m_running.load()) return true;
    UserActivityMonitor* expected = nullptr;
    if (!g_monitor.compare_exchange_strong(expected, this)) return false;
    m_running.store(true);
    m_exited.store(false);
    m_thread = std::thread(&UserActivityMonitor::ThreadMain, this);
    return true;
}

void UserActivityMonitor::Stop() {
    if (!m_running.exchange(false)) return;
    // The loop may not have published its id yet: retry until it has or exited.
    for (int i = 0; i < 200 && m_thread.joinable() && !m_exited.load(); ++i) {
        const DWORD id = m_threadId.load();
        if (id != 0 && ::PostThreadMessageW(id, WM_QUIT, 0, 0)) break;
        ::Sleep(5);
    }
    if (m_thread.joinable()) m_thread.join();
    m_threadId.store(0);
    UserActivityMonitor* self = this;
    g_monitor.compare_exchange_strong(self, nullptr);
}

bool UserActivityMonitor::GameInFront() const {
    const HWND game = reinterpret_cast<HWND>(m_game.load());
    return game && ::GetForegroundWindow() == game;
}

void UserActivityMonitor::Stamp() { m_lastActivityMs.store(NowMs()); }

LRESULT CALLBACK UserActivityMonitor::MouseProc(int code, WPARAM wp, LPARAM lp) {
    UserActivityMonitor* self = g_monitor.load();
    if (code == HC_ACTION && self && lp) {
        const auto* ms = reinterpret_cast<const MSLLHOOKSTRUCT*>(lp);
        if ((ms->flags & (LLMHF_INJECTED | LLMHF_LOWER_IL_INJECTED)) == 0 && self->GameInFront()) {
            if (wp == WM_MOUSEMOVE) {
                const int th = self->m_moveThreshold.load();
                if (self->m_haveMove &&
                    (std::abs(ms->pt.x - self->m_lastMove.x) >= th || std::abs(ms->pt.y - self->m_lastMove.y) >= th))
                    self->Stamp();
                self->m_lastMove = ms->pt;
                self->m_haveMove = true;
            } else {
                self->Stamp();
            }
        }
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

LRESULT CALLBACK UserActivityMonitor::KeyboardProc(int code, WPARAM wp, LPARAM lp) {
    UserActivityMonitor* self = g_monitor.load();
    if (code == HC_ACTION && self && lp) {
        const auto* kb = reinterpret_cast<const KBDLLHOOKSTRUCT*>(lp);
        if ((kb->flags & (LLKHF_INJECTED | LLKHF_LOWER_IL_INJECTED)) == 0 && self->GameInFront() &&
            (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN))
            self->Stamp();
    }
    return ::CallNextHookEx(nullptr, code, wp, lp);
}

void UserActivityMonitor::ThreadMain() {
    // Create the message queue before publishing the id (PostThreadMessage needs it).
    MSG msg;
    ::PeekMessageW(&msg, nullptr, WM_USER, WM_USER, PM_NOREMOVE);
    m_threadId.store(::GetCurrentThreadId());
    HINSTANCE mod = nullptr;
    ::GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         reinterpret_cast<LPCWSTR>(&UserActivityMonitor::MouseProc), &mod);
    HHOOK mouse = ::SetWindowsHookExW(WH_MOUSE_LL, &UserActivityMonitor::MouseProc, mod, 0);
    HHOOK keyboard = ::SetWindowsHookExW(WH_KEYBOARD_LL, &UserActivityMonitor::KeyboardProc, mod, 0);
    while (m_running.load()) {
        const BOOL r = ::GetMessageW(&msg, nullptr, 0, 0);
        if (r == 0 || r == -1) break;
        ::TranslateMessage(&msg);
        ::DispatchMessageW(&msg);
    }
    if (mouse) ::UnhookWindowsHookEx(mouse);
    if (keyboard) ::UnhookWindowsHookEx(keyboard);
    m_exited.store(true);
}

}  // namespace fb
