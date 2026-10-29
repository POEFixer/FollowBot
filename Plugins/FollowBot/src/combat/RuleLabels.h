// RuleLabels.h - the skill rule labels the plugin writes itself: "Rule 3" (Add) and
// "<label> copy" (Duplicate). They are stored in English, like the shipped "Life flask",
// and shown in the program language while they are unchanged (ui::RuleLabel: the
// texts skills_new_rule_label and skills_copy_label); a label the user typed is his own
// text. Pure: no SDK.
#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace fb {

// A rule label holds at most this many bytes (Clamp in SkillSettings.h).
inline constexpr size_t kRuleLabelMaxBytes = 64;

// The label Add gives the rule numbered `number`: "Rule <number>".
inline std::string NewRuleLabel(size_t number) { return "Rule " + std::to_string(number); }

// The number of a label NewRuleLabel wrote ("Rule 12" -> 12); 0 for any other label.
inline int NewRuleNumber(std::string_view label) {
    constexpr std::string_view kPrefix = "Rule ";
    if (label.size() <= kPrefix.size() || label.size() > kPrefix.size() + 4) return 0;
    if (label.substr(0, kPrefix.size()) != kPrefix) return 0;
    const std::string_view digits = label.substr(kPrefix.size());
    if (digits[0] == '0') return 0;
    int n = 0;
    for (char c : digits) {
        if (c < '0' || c > '9') return 0;
        n = n * 10 + (c - '0');
    }
    return n;
}

namespace detail {
inline constexpr std::string_view kCopySuffix = " copy";
}

// The label Duplicate gives a copy of the rule labelled `label`: "<label> copy", the
// original cut on a character boundary so the whole fits kRuleLabelMaxBytes.
inline std::string CopyRuleLabel(std::string_view label) {
    const size_t room = kRuleLabelMaxBytes - detail::kCopySuffix.size();
    size_t n = label.size() > room ? room : label.size();
    if (n < label.size())
        while (n > 0 && (static_cast<unsigned char>(label[n]) & 0xC0) == 0x80) --n;
    std::string out(label.substr(0, n));
    out += detail::kCopySuffix;
    return out;
}

// The label a CopyRuleLabel label was made of ("Rule 2 copy" -> "Rule 2"); false (and
// *original untouched) for any other label.
inline bool CopiedRuleLabel(std::string_view label, std::string_view* original) {
    const std::string_view suffix = detail::kCopySuffix;
    if (label.size() <= suffix.size() || label.substr(label.size() - suffix.size()) != suffix) return false;
    if (original) *original = label.substr(0, label.size() - suffix.size());
    return true;
}

}  // namespace fb
