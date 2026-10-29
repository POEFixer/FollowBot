// BotLog.cpp - see BotLog.h.
#include "BotLog.h"

#include "Clock.h"
#include "FileUtil.h"

#include <cstdio>
#include <ctime>

namespace fb {

const char* LogCatName(LogCat c) {
    switch (c) {
        case LogCat::General: return Key("log_cat_general");
        case LogCat::Gate:    return Key("log_cat_gate");
        case LogCat::Move:    return Key("log_cat_move");
        case LogCat::Nav:     return Key("log_cat_nav");
        case LogCat::Stuck:   return Key("log_cat_stuck");
        case LogCat::Leader:  return Key("log_cat_leader");
        case LogCat::Task:    return Key("log_cat_task");
        case LogCat::Travel:  return Key("log_cat_travel");
        case LogCat::Loot:    return Key("log_cat_loot");
        case LogCat::Combat:  return Key("log_cat_combat");
        case LogCat::Stash:   return Key("log_cat_stash");
        case LogCat::Death:   return Key("log_cat_death");
        case LogCat::Input:   return Key("log_cat_input");
        case LogCat::Error:   return Key("log_cat_error");
        case LogCat::Count:   break;
    }
    return Key("common_unknown");
}

BotLog::BotLog() { m_ring.reserve(kCapacity); }

void BotLog::Add(LogCat cat, LocText text) {
    if (text.empty()) return;
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

void BotLog::SetEnglish(LocLookup english) {
    std::lock_guard<std::mutex> lock(m_mu);
    m_english = english;
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
    const char* cat = LogCatName(line.cat);
    if (m_english) {
        if (const char* english = m_english(cat)) cat = english;
    }
    char prefix[96];
    std::snprintf(prefix, sizeof(prefix), "%02d:%02d:%02d [%s] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec, cat);
    io::AppendTextFile(m_fileDir / name, std::string(prefix) + Render(line.text, m_english) + "\r\n");
}

}  // namespace fb
