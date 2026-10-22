// Profiles.h - several settings profiles, auto-selected by the local character
// name (design §5.14, goal G11). Pure: no SDK.
#pragma once

#include "Settings.h"

#include <string>
#include <string_view>

namespace fb {

// Index of the profile named `name` (exact), -1 when none.
int FindProfile(const Settings& s, std::string_view name);

// The profile the bot uses for `character`: with autoSelectByCharacter, the
// first profile whose `character` equals the name (case-insensitive) wins;
// otherwise (or without a match) the activeProfile; else the first profile.
int ResolveProfileIndex(const Settings& s, std::string_view character);

// Whether index `idx` was picked by the character match (UI shows "auto").
bool IsAutoSelected(const Settings& s, std::string_view character, int idx);

// "<base>", "<base> 2", "<base> 3", ... the first name not in use.
std::string UniqueProfileName(const Settings& s, std::string_view base);

// Adds a default profile with a unique name; returns its index.
int AddProfile(Settings& s, std::string_view baseName = "Profile");

// Copies profile `idx` under a unique name (the character binding is not
// copied: two profiles for one character would be ambiguous); returns the new
// index or -1.
int DuplicateProfile(Settings& s, int idx);

// Renames (the new name is made unique); the active name follows.
bool RenameProfile(Settings& s, int idx, std::string_view newName);

// Deletes profile `idx`, never the last one; the active name moves to a
// remaining profile when it pointed at the deleted one.
bool DeleteProfile(Settings& s, int idx);

// At least one profile, unique non-empty names, activeProfile names one.
void EnsureProfiles(Settings& s);

}  // namespace fb
