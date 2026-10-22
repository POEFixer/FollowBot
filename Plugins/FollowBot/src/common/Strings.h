// Strings.h - UTF-8 / wide conversion and the case-insensitive matching the
// bot uses for names (leader, items, buffs, skills).
#pragma once

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace fb {

inline std::string WideToUtf8(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), out.data(), n, nullptr, nullptr);
    return out;
}

inline std::wstring Utf8ToWide(std::string_view s) {
    if (s.empty()) return {};
    const int n = ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    if (n <= 0) return {};
    std::wstring out(static_cast<size_t>(n), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), out.data(), n);
    return out;
}

inline char AsciiLower(char c) { return (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c; }

inline std::string ToLowerAscii(std::string_view s) {
    std::string out(s);
    for (char& c : out) c = AsciiLower(c);
    return out;
}

// Case-insensitive lower-casing for any script: ASCII directly, everything else
// through the Unicode-aware LCMapStringEx (Cyrillic, Greek, accented Latin...).
inline std::string FoldCase(std::string_view s) {
    bool ascii = true;
    for (char c : s)
        if (static_cast<unsigned char>(c) >= 0x80) { ascii = false; break; }
    if (ascii) return ToLowerAscii(s);
    std::wstring w = Utf8ToWide(s);
    if (w.empty()) return std::string(s);
    const int n = ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.data(), static_cast<int>(w.size()),
                                  nullptr, 0, nullptr, nullptr, 0);
    if (n <= 0) return ToLowerAscii(s);
    std::wstring lw(static_cast<size_t>(n), L'\0');
    ::LCMapStringEx(LOCALE_NAME_INVARIANT, LCMAP_LOWERCASE, w.data(), static_cast<int>(w.size()), lw.data(), n,
                    nullptr, nullptr, 0);
    return WideToUtf8(lw);
}

inline bool EqualsCI(std::string_view a, std::string_view b) { return FoldCase(a) == FoldCase(b); }

inline bool ContainsCI(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    return FoldCase(hay).find(FoldCase(needle)) != std::string::npos;
}

inline bool StartsWith(std::string_view s, std::string_view prefix) {
    return s.size() >= prefix.size() && s.compare(0, prefix.size(), prefix) == 0;
}

inline bool EndsWith(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Metadata paths may carry an "@<n>" variant suffix: "…/Stash@2" -> "…/Stash".
inline std::string_view StripAtSuffix(std::string_view path) {
    const size_t at = path.find('@');
    return at == std::string_view::npos ? path : path.substr(0, at);
}

inline std::string Trim(std::string_view s) {
    size_t b = 0, e = s.size();
    while (b < e && (s[b] == ' ' || s[b] == '\t' || s[b] == '\r' || s[b] == '\n')) ++b;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t' || s[e - 1] == '\r' || s[e - 1] == '\n')) --e;
    return std::string(s.substr(b, e - b));
}

// Splits on newlines and commas, trims, drops empty entries (name list fields).
inline std::vector<std::string> SplitList(std::string_view s) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == '\n' || s[i] == ',') {
            std::string t = Trim(s.substr(start, i - start));
            if (!t.empty()) out.push_back(std::move(t));
            start = i + 1;
        }
    }
    return out;
}

inline std::string JoinLines(const std::vector<std::string>& v) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += '\n';
        out += v[i];
    }
    return out;
}

}  // namespace fb
