// BotLog.cpp - see BotLog.h.
#include "BotLog.h"

#include "Clock.h"
#include "FileUtil.h"

#include <cstdio>
#include <ctime>

namespace fb {

const char* LogCatName(LogCat c) {
    switch (c) {
        case LogCat::General: return "General";
        case LogCat::Gate:    return "Gate";
        case LogCat::Move:    return "Move";
        case LogCat::Nav:     return "Nav";
        case LogCat::Stuck:   return "Stuck";
        case LogCat::Leader:  return "Leader";
        case LogCat::Task:    return "Task";
        case LogCat::Travel:  return "Travel";
        case LogCat::Loot:    return "Loot";
        case LogCat::Combat:  return "Combat";
        case LogCat::Stash:   return "Stash";
        case LogCat::Death:   return "Death";
        case LogCat::Input:   return "Input";
        case LogCat::Error:   return "Error";
        case LogCat::Count:   break;
    }
    return "?";
}

BotLog::BotLog() { m_ring.reserve(kCapacity); }

void BotLog::Add(LogCat cat, const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    AddV(cat, fmt, args);
    va_end(args);
}

void BotLog::AddV(LogCat cat, const char* fmt, va_list args) {
    if (!fmt) return;
    char buf[1024];
    const int n = std::vsnprintf(buf, sizeof(buf), fmt, args);
    if (n < 0) return;
    AddText(cat, std::string(buf));
}

void BotLog::AddText(LogCat cat, std::string text) {
    std::lock_guard<std::mutex> lock(m_mu);
    const uint64_t now = NowMs();
    if (m_repeatWindowMs > 0 && !m_ring.empty()) {
        // The same line again inside the window (among the 8 newest) is dropped.
        const size_t n = m_ring.size();
        const size_t look = n < 8 ? n : 8;
        for (size_t k = 0; k < look; ++k) {
            const size_t idx = (n < kCapacity) ? n - 1 - k : (m_head + kCapacity - 1 - k) % kCapacity;
            const LogLine& l = m_ring[idx];
            if (now - l.timeMs > m_repeatWindowMs) break;
            if (l.cat == cat && l.text == text) return;
        }
    }
    LogLine line;
    line.timeMs = now;
    line.seq = ++m_seq;
    line.cat = cat;
    line.text = std::move(text);
    if (m_fileEnabled) WriteFileLine(line);
    if (m_ring.size() < kCapacity) {
        m_ring.push_back(std::move(line));
    } else {
        m_ring[m_head] = std::move(line);
        m_head = (m_head + 1) % kCapacity;
    }
}

std::vector<LogLine> BotLog::Snapshot() const {
    std::lock_guard<std::mutex> lock(m_mu);
    std::vector<LogLine> out;
    out.reserve(m_ring.size());
    if (m_ring.size() < kCapacity) {
        out = m_ring;
    } else {
        for (size_t i = 0; i < kCapacity; ++i) out.push_back(m_ring[(m_head + i) % kCapacity]);
    }
    return out;
}

uint64_t BotLog::LastSeq() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_seq;
}

void BotLog::Clear() {
    std::lock_guard<std::mutex> lock(m_mu);
    m_ring.clear();
    m_head = 0;
}

void BotLog::SetFileSink(const std::filesystem::path& dir, bool enabled) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_fileDir = dir;
    m_fileEnabled = enabled && !dir.empty();
}

void BotLog::SetRepeatWindow(uint64_t windowMs) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_repeatWindowMs = windowMs;
}

void BotLog::WriteFileLine(const LogLine& line) {
    std::time_t t = std::time(nullptr);
    std::tm tmv{};
    localtime_s(&tmv, &t);
    char name[64];
    std::snprintf(name, sizeof(name), "followbot-%04d-%02d-%02d.log", tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday);
    char prefix[64];
    std::snprintf(prefix, sizeof(prefix), "%02d:%02d:%02d [%s] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec,
                  LogCatName(line.cat));
    io::AppendTextFile(m_fileDir / name, std::string(prefix) + line.text + "\r\n");
}

}  // namespace fb
