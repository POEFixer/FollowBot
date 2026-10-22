// Win32Input.h - raw SendInput primitives (design §5.3). Never PostMessage:
// PoE2 ignores posted keys and drops posted clicks.
//
// Screen points from the host are game-window CLIENT pixels; SendInput needs
// absolute desktop coordinates normalised over the virtual desktop, so every
// mouse call converts through ClientToScreen first. Mouse button events carry
// the absolute point (a click lands there even if something moved the cursor
// in between). Keys are sent with their scan code and the extended flag.
#pragma once

#include <Windows.h>

#include <string>

namespace fb::win {

POINT ClientToDesktop(HWND game, int x, int y);
bool  DesktopToClient(HWND game, POINT desk, POINT& client);
void  NormalizeAbsolute(POINT desk, LONG& nx, LONG& ny);

bool MoveCursorDesktop(POINT desk);
bool MoveCursorClient(HWND game, int x, int y);
bool GetCursorDesktop(POINT& out);

// VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2.
bool IsMouseVk(int vk);
bool IsExtendedKey(int vk);

// One mouse button event; `client` = where (game client pixels), nullptr = at
// the current cursor.
bool MouseButton(int vk, bool down, HWND game, const POINT* client);
// Down + up at one point in a single SendInput call.
bool MouseClick(int vk, HWND game, int x, int y);
// One keyboard event (scan code + extended flag).
bool KeyEvent(int vk, bool down);
// Key or mouse button, by VK.
bool VkEvent(int vk, bool down, HWND game, const POINT* client);
// Unicode text (KEYEVENTF_UNICODE), surrogate pairs included.
bool SendUnicodeText(const std::wstring& text);

// Physical/logical state (GetAsyncKeyState high bit).
bool IsKeyDown(int vk);
// Releases Shift / Ctrl / Alt when they read as held (before typing a command,
// so a stuck modifier does not turn Enter into Ctrl+Enter).
void ReleaseHeldModifiers();

bool IsForeground(HWND game);

}  // namespace fb::win
