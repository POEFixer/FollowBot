// LocText.h - a text the worker hands to the render thread untranslated: a key of
// lang/en.json plus its named arguments, turned into words only where it is drawn
// (the program language may change at any time; PluginI18n.h translates on the
// drawing thread). STL only: the pure decision code and tests/ use it without
// PluginSDK.h, and the tests compare keys and arguments, never texts.
//
//     ctx.report.detail = TrF("travel_walking_to", { { "object", Tr(word) }, { "cells", Num("%.0f", d) } });
//     ctx.log.Add(LogCat::Stash, Tr("stash_log_panel_closed"));
//     static constexpr const char* kName = Key("task_travel");     // a key kept for later
//
// fb::Tr / fb::TrF / fb::Key are the deferred twins of PluginSDK::I18n::Tr / TrF /
// Key, named alike so `python tools/plugin_i18n/gen.py --gate` checks every literal
// key against lang/en.json and every TrF's argument names against the English
// text's {placeholders}. An argument is a value shown as is (a number formatted by
// Num, a game or user string) or another LocText, translated too ({why} filled
// with a reason). Render() turns one into text with a lookup: the program language
// in the UI (ui::Text), the embedded English for the file log, the key in tests.
#pragma once

#include <sal.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fb {

struct LocSub;

struct LocText {
    std::string key;                                         // "" = no text
    std::vector<std::pair<std::string, std::string>> args;   // {name} -> a value shown as is
    std::vector<LocSub> subs;                                // {name} -> another text, translated too

    bool empty() const { return key.empty(); }
    void clear();
    bool Is(const char* k) const { return k && key == k; }
    // The value of the argument `name`, nullptr when there is none.
    const std::string* Arg(std::string_view name) const;
    // The nested text `name`, nullptr when there is none.
    const LocText* Sub(std::string_view name) const;
};

struct LocSub {
    std::string name;
    LocText     text;
};

inline void LocText::clear() {
    key.clear();
    args.clear();
    subs.clear();
}

inline const std::string* LocText::Arg(std::string_view name) const {
    for (const auto& a : args)
        if (a.first == name) return &a.second;
    return nullptr;
}

inline const LocText* LocText::Sub(std::string_view name) const {
    for (const auto& s : subs)
        if (s.name == name) return &s.text;
    return nullptr;
}

bool operator==(const LocText& a, const LocText& b);

inline bool operator==(const LocSub& a, const LocSub& b) { return a.name == b.name && a.text == b.text; }

inline bool operator==(const LocText& a, const LocText& b) {
    return a.key == b.key && a.args == b.args && a.subs == b.subs;
}

// One argument of TrF: a value shown as is, or a nested text.
struct LocArg {
    std::string_view name;
    std::string      value;
    const LocText*   text = nullptr;

    LocArg(std::string_view n, std::string v) : name(n), value(std::move(v)) {}
    LocArg(std::string_view n, const char* v) : name(n), value(v ? v : "") {}
    LocArg(std::string_view n, std::string_view v) : name(n), value(v) {}
    LocArg(std::string_view n, const LocText& t) : name(n), text(&t) {}
};

// A key kept where it is not looked up yet (a table, a member): the key itself.
constexpr const char* Key(const char* key) { return key; }

// A text without arguments.
inline LocText Tr(const char* key) {
    LocText t;
    t.key = key ? key : "";
    return t;
}

// A text with named arguments ({name} in the English text).
inline LocText TrF(const char* key, std::initializer_list<LocArg> args) {
    LocText t;
    t.key = key ? key : "";
    for (const LocArg& a : args) {
        if (a.text) t.subs.push_back(LocSub{std::string(a.name), *a.text});
        else t.args.emplace_back(std::string(a.name), a.value);
    }
    return t;
}

// printf for an argument: Num("%.1f", seconds). The format is code, never a
// translation; MSVC's /analyze checks the arguments against it.
inline std::string Num(_In_z_ _Printf_format_string_ const char* format, ...) {
    char buf[64];
    va_list args;
    va_start(args, format);
    const int n = std::vsnprintf(buf, sizeof(buf), format, args);
    va_end(args);
    return n < 0 ? std::string() : std::string(buf);
}

// Looks a key's text up: the program language, the embedded English, ... A null
// lookup or a null answer shows the key itself.
using LocLookup = const char* (*)(const char* key);

namespace detail {

// How deep texts nest: a reason inside a reason inside ... (DescribeRejects joins
// up to seven parts as a chain of text_join, each one level deeper). Deeper
// levels are left out rather than recursing without a bound.
inline constexpr int kMaxDepth = 16;

inline void AppendRendered(std::string& out, const LocText& t, LocLookup lookup, int depth) {
    if (t.key.empty()) return;
    const char* text = lookup ? lookup(t.key.c_str()) : nullptr;
    if (!text) text = t.key.c_str();
    for (const char* p = text; *p;) {
        if (*p == '{') {
            const char* close = std::strchr(p + 1, '}');
            if (close) {
                const std::string_view name(p + 1, static_cast<size_t>(close - p - 1));
                if (const std::string* v = t.Arg(name)) {
                    out += *v;
                    p = close + 1;
                    continue;
                }
                if (const LocText* sub = t.Sub(name)) {
                    if (depth < kMaxDepth) AppendRendered(out, *sub, lookup, depth + 1);
                    p = close + 1;
                    continue;
                }
            }
        }
        out.push_back(*p++);
    }
}

}  // namespace detail

// The words of `t`: its key's text with every {name} filled (a value as is, a
// nested text rendered the same way); "" for an empty text.
inline std::string Render(const LocText& t, LocLookup lookup) {
    std::string out;
    detail::AppendRendered(out, t, lookup, 0);
    return out;
}

}  // namespace fb
