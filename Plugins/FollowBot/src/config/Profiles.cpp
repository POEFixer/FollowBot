// Profiles.cpp - see Profiles.h.
#include "Profiles.h"

#include "../common/Strings.h"

#include <set>

namespace fb {

int FindProfile(const Settings& s, std::string_view name) {
    for (size_t i = 0; i < s.profiles.size(); ++i)
        if (s.profiles[i].name == name) return static_cast<int>(i);
    return -1;
}

namespace {
int CharacterMatch(const Settings& s, std::string_view character) {
    if (!s.autoSelectByCharacter || character.empty()) return -1;
    for (size_t i = 0; i < s.profiles.size(); ++i) {
        const std::string& c = s.profiles[i].character;
        if (!c.empty() && EqualsCI(c, character)) return static_cast<int>(i);
    }
    return -1;
}
}  // namespace

int ResolveProfileIndex(const Settings& s, std::string_view character) {
    if (s.profiles.empty()) return -1;
    const int byChar = CharacterMatch(s, character);
    if (byChar >= 0) return byChar;
    const int byName = FindProfile(s, s.activeProfile);
    return byName >= 0 ? byName : 0;
}

bool IsAutoSelected(const Settings& s, std::string_view character, int idx) {
    return idx >= 0 && CharacterMatch(s, character) == idx;
}

std::string UniqueProfileName(const Settings& s, std::string_view base) {
    std::string b = Trim(base);
    if (b.empty()) b = "Profile";
    // At most 40 bytes, cut between characters (a Cyrillic or Thai name never ends in half a one).
    if (b.size() > 40) {
        TruncateUtf8(b, 40);
        b = Trim(b);
    }
    if (FindProfile(s, b) < 0) return b;
    // "Witch 2" taken -> "Witch 3", not "Witch 2 2": number from the bare name.
    const size_t sp = b.find_last_of(' ');
    if (sp != std::string::npos && sp + 1 < b.size() &&
        b.find_first_not_of("0123456789", sp + 1) == std::string::npos)
        b.resize(sp);
    for (int n = 2; n < 1000; ++n) {
        std::string candidate = b + " " + std::to_string(n);
        if (FindProfile(s, candidate) < 0) return candidate;
    }
    return b + " " + std::to_string(s.profiles.size() + 1);
}

int AddProfile(Settings& s, std::string_view baseName) {
    if (s.profiles.size() >= 32) return -1;
    Profile p;
    p.name = UniqueProfileName(s, baseName);
    s.profiles.push_back(std::move(p));
    return static_cast<int>(s.profiles.size()) - 1;
}

int DuplicateProfile(Settings& s, int idx) {
    if (idx < 0 || idx >= static_cast<int>(s.profiles.size()) || s.profiles.size() >= 32) return -1;
    Profile copy = s.profiles[static_cast<size_t>(idx)];
    copy.name = UniqueProfileName(s, copy.name);
    copy.character.clear();
    s.profiles.push_back(std::move(copy));
    return static_cast<int>(s.profiles.size()) - 1;
}

bool RenameProfile(Settings& s, int idx, std::string_view newName) {
    if (idx < 0 || idx >= static_cast<int>(s.profiles.size())) return false;
    Profile& p = s.profiles[static_cast<size_t>(idx)];
    const std::string trimmed = Trim(newName);
    if (trimmed.empty() || trimmed == p.name) return false;
    const bool wasActive = s.activeProfile == p.name;
    const std::string old = p.name;
    p.name.clear();  // so the unique-name search does not count the old name
    p.name = UniqueProfileName(s, trimmed);
    if (wasActive) s.activeProfile = p.name;
    return p.name != old;
}

bool DeleteProfile(Settings& s, int idx) {
    if (s.profiles.size() <= 1 || idx < 0 || idx >= static_cast<int>(s.profiles.size())) return false;
    const bool wasActive = s.activeProfile == s.profiles[static_cast<size_t>(idx)].name;
    s.profiles.erase(s.profiles.begin() + idx);
    if (wasActive) s.activeProfile = s.profiles.front().name;
    return true;
}

void EnsureProfiles(Settings& s) {
    if (s.profiles.empty()) s.profiles.push_back(Profile{});
    std::set<std::string> seen;
    for (auto& p : s.profiles) {
        std::string name = Trim(p.name);
        if (name.empty()) name = "Profile";
        if (seen.count(name)) {
            for (int n = 2;; ++n) {
                std::string candidate = name + " " + std::to_string(n);
                if (!seen.count(candidate)) { name = candidate; break; }
            }
        }
        p.name = name;
        seen.insert(name);
    }
    if (FindProfile(s, s.activeProfile) < 0) s.activeProfile = s.profiles.front().name;
}

PluginProfileName ParsePluginProfileName(std::string_view name) {
    PluginProfileName out;
    std::string_view base = name;
    int number = 0;
    const size_t sp = name.find_last_of(' ');
    if (sp != std::string_view::npos && sp + 1 < name.size() && name.size() - sp - 1 <= 4 && name[sp + 1] != '0' &&
        name.find_first_not_of("0123456789", sp + 1) == std::string_view::npos) {
        base = name.substr(0, sp);
        for (size_t i = sp + 1; i < name.size(); ++i) number = number * 10 + (name[i] - '0');
    }
    if (base == "Default") out.base = PluginProfileBase::Default;
    else if (base == "Profile") out.base = PluginProfileBase::Profile;
    else return out;
    out.number = number;
    return out;
}

}  // namespace fb
