// Coordination.h - the cross-plugin cursor busy channel (Pick My Loot / Auto My
// Aim protocol, unchanged): a pagefile mapping "Local\PoeFixer_PickMyLoot_Busy",
// { uint32 magic 0x504D4C31; 4 bytes pad; uint64 busyUntilTick } where
// busyUntilTick is a GetTickCount64() deadline (capped at 2000 ms ahead).
//
// FollowBot writes it while it clicks or aims (Auto My Aim then backs off) and
// reads it before cursor actions: a deadline it did not write itself means
// another plugin owns the cursor right now, and the bot defers its cursor work
// for that tick.
#pragma once

#include <Windows.h>

#include <cstddef>
#include <cstdint>

namespace fb {

class CursorBusyChannel {
public:
    static constexpr uint32_t kMagic = 0x504D4C31;
    static constexpr uint64_t kMaxBusyMs = 2000;

    CursorBusyChannel() {
        m_map = ::CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                     static_cast<DWORD>(sizeof(Shared)), L"Local\\PoeFixer_PickMyLoot_Busy");
        if (!m_map) return;
        m_p = static_cast<Shared*>(::MapViewOfFile(m_map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(Shared)));
        if (!m_p) return;
        const LONG prev = ::InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&m_p->magic),
                                                       static_cast<LONG>(kMagic), 0);
        m_ok = (prev == 0 || static_cast<uint32_t>(prev) == kMagic);
    }
    ~CursorBusyChannel() {
        if (m_p) ::UnmapViewOfFile(m_p);
        if (m_map) ::CloseHandle(m_map);
    }
    CursorBusyChannel(const CursorBusyChannel&) = delete;
    CursorBusyChannel& operator=(const CursorBusyChannel&) = delete;

    bool Valid() const { return m_ok; }

    // "FollowBot owns the cursor for the next ms milliseconds".
    void MarkBusy(uint32_t ms) {
        if (!m_ok) return;
        const uint64_t until = ::GetTickCount64() + (ms > kMaxBusyMs ? kMaxBusyMs : ms);
        m_ownUntil = until;
        ::InterlockedExchange64(reinterpret_cast<volatile LONG64*>(&m_p->busyUntilTick), static_cast<LONG64>(until));
    }

    // Releases the cursor now, only when the deadline is still ours.
    void MarkIdle() {
        if (!m_ok || m_ownUntil == 0) return;
        ::InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&m_p->busyUntilTick), 0,
                                       static_cast<LONG64>(m_ownUntil));
        m_ownUntil = 0;
    }

    // Another plugin (Pick My Loot) owns the cursor right now.
    bool BusyByOther() const {
        if (!m_ok) return false;
        const uint64_t until = Read();
        const uint64_t now = ::GetTickCount64();
        if (!(now < until && until - now <= kMaxBusyMs)) return false;
        return until != m_ownUntil;
    }

private:
    struct Shared {
        uint32_t          magic;
        volatile uint64_t busyUntilTick;
    };
    static_assert(sizeof(Shared) == 16, "busy channel layout is a cross-plugin contract");
    static_assert(offsetof(Shared, busyUntilTick) == 8, "busy channel layout is a cross-plugin contract");

    uint64_t Read() const {
        return static_cast<uint64_t>(
            ::InterlockedCompareExchange64(reinterpret_cast<volatile LONG64*>(&m_p->busyUntilTick), 0, 0));
    }

    HANDLE   m_map = nullptr;
    Shared*  m_p = nullptr;
    bool     m_ok = false;
    uint64_t m_ownUntil = 0;
};

}  // namespace fb
