// Win32Input.cpp - see Win32Input.h.
// FB_TESTS_EXCLUDE: the unit tests link tests/FakeWin32Input.cpp instead (no test sends real input).
#include "Win32Input.h"

namespace fb::win {

POINT ClientToDesktop(HWND game, int x, int y) {
    POINT p{x, y};
    if (game) ::ClientToScreen(game, &p);
    return p;
}

bool DesktopToClient(HWND game, POINT desk, POINT& client) {
    client = desk;
    return game && ::ScreenToClient(game, &client) != FALSE;
}

void NormalizeAbsolute(POINT desk, LONG& nx, LONG& ny) {
    const int vsX = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vsY = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vsW = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int vsH = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (vsW <= 1) vsW = ::GetSystemMetrics(SM_CXSCREEN);
    if (vsH <= 1) vsH = ::GetSystemMetrics(SM_CYSCREEN);
    const long long dw = vsW > 1 ? vsW - 1 : 1;
    const long long dh = vsH > 1 ? vsH - 1 : 1;
    nx = static_cast<LONG>((static_cast<long long>(desk.x - vsX) * 65535 + dw / 2) / dw);
    ny = static_cast<LONG>((static_cast<long long>(desk.y - vsY) * 65535 + dh / 2) / dh);
}

bool MoveCursorDesktop(POINT desk) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    NormalizeAbsolute(desk, in.mi.dx, in.mi.dy);
    in.mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    return ::SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool MoveCursorClient(HWND game, int x, int y) { return MoveCursorDesktop(ClientToDesktop(game, x, y)); }

bool GetCursorDesktop(POINT& out) { return ::GetCursorPos(&out) != FALSE; }

bool IsMouseVk(int vk) {
    return vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2;
}

bool IsExtendedKey(int vk) {
    switch (vk) {
        case VK_INSERT: case VK_DELETE: case VK_HOME: case VK_END: case VK_PRIOR: case VK_NEXT:
        case VK_LEFT: case VK_RIGHT: case VK_UP: case VK_DOWN: case VK_NUMLOCK: case VK_RCONTROL:
        case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_DIVIDE: case VK_SNAPSHOT:
            return true;
        default:
            return false;
    }
}

namespace {
DWORD ButtonFlag(int vk, bool down, DWORD& mouseData) {
    mouseData = 0;
    switch (vk) {
        case VK_LBUTTON: return down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        case VK_RBUTTON: return down ? MOUSEEVENTF_RIGHTDOWN : MOUSEEVENTF_RIGHTUP;
        case VK_MBUTTON: return down ? MOUSEEVENTF_MIDDLEDOWN : MOUSEEVENTF_MIDDLEUP;
        case VK_XBUTTON1: mouseData = XBUTTON1; return down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        case VK_XBUTTON2: mouseData = XBUTTON2; return down ? MOUSEEVENTF_XDOWN : MOUSEEVENTF_XUP;
        default: return 0;
    }
}
}  // namespace

bool MouseButton(int vk, bool down, HWND game, const POINT* client) {
    DWORD data = 0;
    const DWORD flag = ButtonFlag(vk, down, data);
    if (!flag) return false;
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.mouseData = data;
    in.mi.dwFlags = flag;
    if (client) {
        NormalizeAbsolute(ClientToDesktop(game, client->x, client->y), in.mi.dx, in.mi.dy);
        in.mi.dwFlags |= MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    }
    return ::SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool MouseClick(int vk, HWND game, int x, int y) {
    DWORD dataDown = 0, dataUp = 0;
    const DWORD down = ButtonFlag(vk, true, dataDown);
    const DWORD up = ButtonFlag(vk, false, dataUp);
    if (!down || !up) return false;
    INPUT in[2]{};
    LONG nx = 0, ny = 0;
    NormalizeAbsolute(ClientToDesktop(game, x, y), nx, ny);
    const DWORD move = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    for (INPUT& i : in) {
        i.type = INPUT_MOUSE;
        i.mi.dx = nx;
        i.mi.dy = ny;
    }
    in[0].mi.dwFlags = move | down;
    in[0].mi.mouseData = dataDown;
    in[1].mi.dwFlags = move | up;
    in[1].mi.mouseData = dataUp;
    return ::SendInput(2, in, sizeof(INPUT)) == 2;
}

bool KeyEvent(int vk, bool down) {
    if (vk <= 0 || vk > 0xFE || IsMouseVk(vk)) return false;
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(vk);
    in.ki.wScan = static_cast<WORD>(::MapVirtualKeyW(static_cast<UINT>(vk), MAPVK_VK_TO_VSC));
    in.ki.dwFlags = (down ? 0u : KEYEVENTF_KEYUP) | (IsExtendedKey(vk) ? KEYEVENTF_EXTENDEDKEY : 0u);
    return ::SendInput(1, &in, sizeof(INPUT)) == 1;
}

bool VkEvent(int vk, bool down, HWND game, const POINT* client) {
    if (IsMouseVk(vk)) return MouseButton(vk, down, game, client);
    return KeyEvent(vk, down);
}

bool SendUnicodeText(const std::wstring& text) {
    if (text.empty()) return true;
    bool ok = true;
    for (wchar_t ch : text) {
        INPUT in[2]{};
        in[0].type = INPUT_KEYBOARD;
        in[0].ki.wScan = static_cast<WORD>(ch);
        in[0].ki.dwFlags = KEYEVENTF_UNICODE;
        in[1] = in[0];
        in[1].ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
        ok = ::SendInput(2, in, sizeof(INPUT)) == 2 && ok;
    }
    return ok;
}

bool IsKeyDown(int vk) {
    if (vk <= 0 || vk > 0xFE) return false;
    return (::GetAsyncKeyState(vk) & 0x8000) != 0;
}

void ReleaseHeldModifiers() {
    static const int kMods[] = {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU};
    for (int vk : kMods)
        if (IsKeyDown(vk)) KeyEvent(vk, false);
}

bool IsForeground(HWND game) { return game && ::GetForegroundWindow() == game; }

}  // namespace fb::win
