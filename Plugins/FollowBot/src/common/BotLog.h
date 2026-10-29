// BotLog.h - the decision log: a thread-safe ring buffer (400 lines) of
// timestamped, categorised lines the Debug tab shows with a filter, plus an
// optional daily file under <plugin>/logs/. The host log (ctx->Log) only gets
// start/stop, area changes, task outcomes and errors; everything else is here.
//
// A line is a LocText (a key and its arguments): the Debug tab translates it when
// it draws, the file gets it in English (SetEnglish; without it the key and its
// arguments). Category names are keys too (LogCatName).
#pragma once

#include "../core/LocText.h"

#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace fb {

enum class LogCat : uint8_t {
    General = 0,
    Gate,
    Move,
    Nav,
    Stuck,
    Leader,
    Task,
    Travel,
    Loot,
    Combat,
    Stash,
    Death,
    Input,
    Error,
    Count
};

// The category's key (lang/en.json), e.g. "log_cat_general".
const char* LogCatName(LogCat c);

struct LogLine {
    uint64_t    timeMs = 0;   // steady clock (fb::NowMs)
    uint64_t    seq = 0;      // increasing, never reused
    LogCat      cat = LogCat::General;
    LocText     text;
};

class BotLog {
public:
    static constexpr size_t kCapacity = 400;

    BotLog();

    void Add(LogCat cat, LocText text);

    // Copy of the buffer, oldest first.
    std::vector<LogLine> Snapshot() const;
    uint64_t LastSeq() const;
    void Clear();

    // File sink: `dir` = <plugin>/logs; empty dir disables it.
    void SetFileSink(const std::filesystem::path& dir, bool enabled);
    // How the file sink turns a line into English (the embedded English texts).
    void SetEnglish(LocLookup english);

    // Repeated identical text in the same category within `windowMs` is dropped
    // (a per-tick decision must not flood the log). 0 disables the filter.
    void SetRepeatWindow(uint64_t windowMs);

private:
    void WriteFileLine(const LogLine& line);

    mutable std::mutex     m_mu;
    std::vector<LogLine>   m_ring;
    size_t                 m_head = 0;   // next write position once full
    uint64_t               m_seq = 0;
    uint64_t               m_repeatWindowMs = 1500;
    std::filesystem::path  m_fileDir;
    bool                   m_fileEnabled = false;
    LocLookup              m_english = nullptr;
};

}  // namespace fb
