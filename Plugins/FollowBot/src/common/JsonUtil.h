// JsonUtil.h - typed, non-throwing JSON field access with per-field fallbacks
// and clamping. One bad field never discards the rest of a settings file.
#pragma once

#include "../../lib/nlohmann/json.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <string>
#include <type_traits>
#include <vector>

namespace fb::json {

using Json = nlohmann::json;

// Parses without throwing; a parse error returns a discarded/null value.
inline Json Parse(const std::string& text) {
    Json j = Json::parse(text, nullptr, false);
    if (j.is_discarded()) return Json();
    return j;
}

inline std::string Dump(const Json& j, int indent = 2) {
    return j.dump(indent, ' ', false, Json::error_handler_t::replace);
}

// One typed field, or `fallback` on a missing key / wrong type / non-finite number.
template <typename T>
T Get(const Json& obj, const char* key, T fallback) {
    if (!obj.is_object()) return fallback;
    const auto it = obj.find(key);
    if (it == obj.end()) return fallback;
    const Json& v = *it;
    if constexpr (std::is_same_v<T, bool>) {
        if (v.is_boolean()) return v.get<bool>();
        if (v.is_number_integer()) return v.get<long long>() != 0;
        return fallback;
    } else if constexpr (std::is_integral_v<T>) {
        if (v.is_number_integer()) {
            const long long x = v.get<long long>();
            if (x < static_cast<long long>((std::numeric_limits<T>::min)()) ||
                x > static_cast<long long>((std::numeric_limits<T>::max)()))
                return fallback;
            return static_cast<T>(x);
        }
        if (v.is_number_float()) {
            const double d = v.get<double>();
            if (!std::isfinite(d)) return fallback;
            if (d < static_cast<double>((std::numeric_limits<T>::min)()) ||
                d > static_cast<double>((std::numeric_limits<T>::max)()))
                return fallback;
            return static_cast<T>(d);
        }
        return fallback;
    } else if constexpr (std::is_floating_point_v<T>) {
        if (!v.is_number()) return fallback;
        const double d = v.get<double>();
        return std::isfinite(d) ? static_cast<T>(d) : fallback;
    } else if constexpr (std::is_same_v<T, std::string>) {
        return v.is_string() ? v.get<std::string>() : fallback;
    } else {
        static_assert(sizeof(T) == 0, "fb::json::Get: unsupported type");
    }
}

// Reads into `field` (kept when missing / wrong type) and clamps it to [lo, hi].
template <typename T>
void Read(const Json& obj, const char* key, T& field, T lo, T hi) {
    field = std::clamp(Get<T>(obj, key, field), lo, hi);
}

template <typename T>
void Read(const Json& obj, const char* key, T& field) {
    field = Get<T>(obj, key, field);
}

// Enum stored as an int in [0, count): out-of-range keeps the current value.
template <typename E>
void ReadEnum(const Json& obj, const char* key, E& field, int count) {
    const int v = Get<int>(obj, key, static_cast<int>(field));
    if (v >= 0 && v < count) field = static_cast<E>(v);
}

// A list of strings: non-strings and empty entries are skipped, each entry is
// capped at `maxLen` bytes and the list at `maxCount` entries.
inline void ReadStringList(const Json& obj, const char* key, std::vector<std::string>& out, size_t maxCount = 512,
                           size_t maxLen = 256) {
    if (!obj.is_object()) return;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) return;
    out.clear();
    for (const auto& e : *it) {
        if (!e.is_string()) continue;
        std::string s = e.get<std::string>();
        if (s.empty() || s.size() > maxLen) continue;
        out.push_back(std::move(s));
        if (out.size() >= maxCount) break;
    }
}

// The child object at `key`, or an empty object.
inline const Json& Child(const Json& obj, const char* key) {
    static const Json kEmpty = Json::object();
    if (!obj.is_object()) return kEmpty;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_object()) return kEmpty;
    return *it;
}

// The child array at `key`, or an empty array.
inline const Json& ChildArray(const Json& obj, const char* key) {
    static const Json kEmpty = Json::array();
    if (!obj.is_object()) return kEmpty;
    const auto it = obj.find(key);
    if (it == obj.end() || !it->is_array()) return kEmpty;
    return *it;
}

// A uint64 bit mask stored as a JSON string of hex digits (JSON numbers lose
// precision above 2^53 in many readers).
inline uint64_t GetMask(const Json& obj, const char* key, uint64_t fallback) {
    const std::string s = Get<std::string>(obj, key, std::string());
    if (s.empty() || s.size() > 16) return fallback;
    uint64_t v = 0;
    for (char c : s) {
        int d = -1;
        if (c >= '0' && c <= '9') d = c - '0';
        else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
        else if (c >= 'A' && c <= 'F') d = c - 'A' + 10;
        if (d < 0) return fallback;
        v = (v << 4) | static_cast<uint64_t>(d);
    }
    return v;
}

inline std::string MaskToString(uint64_t v) {
    static const char* kHex = "0123456789abcdef";
    std::string s;
    if (v == 0) return "0";
    while (v) {
        s.insert(s.begin(), kHex[v & 0xF]);
        v >>= 4;
    }
    return s;
}

}  // namespace fb::json
