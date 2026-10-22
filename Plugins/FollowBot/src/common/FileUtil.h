// FileUtil.h - exception-free file helpers (the PickMyLoot / AutoMyAim pattern).
//
// - Every path is a std::filesystem::path, so a Unicode user folder works.
// - WriteTextFileAtomic writes "<name>.tmp" and swaps it in with
//   MoveFileExW(REPLACE_EXISTING | WRITE_THROUGH): a crash mid-write keeps the
//   previous file.
// - ReadTextFileChecked tells a missing file from one that exists but cannot be
//   read; KeepCorruptCopy and SaveGuard keep a settings file that could not be
//   read or parsed from being replaced by the defaults.
#pragma once

#include <Windows.h>

#include <cstdio>
#include <filesystem>
#include <string>
#include <system_error>

namespace fb::io {

enum class ReadStatus { Ok, Missing, Unreadable };

inline ReadStatus ReadTextFileChecked(const std::filesystem::path& path, std::string& out) {
    out.clear();
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) {
        const DWORD attrs = ::GetFileAttributesW(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            const DWORD err = ::GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) return ReadStatus::Missing;
        }
        return ReadStatus::Unreadable;
    }
    char buf[16384];
    size_t n = 0;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) {
        out.append(buf, n);
        if (out.size() > (16u << 20)) {  // a config is never this large: refuse garbage
            std::fclose(f);
            out.clear();
            return ReadStatus::Unreadable;
        }
    }
    const bool failed = std::ferror(f) != 0;
    std::fclose(f);
    if (failed) {
        out.clear();
        return ReadStatus::Unreadable;
    }
    return ReadStatus::Ok;
}

// Copies a file that could not be parsed to "<name>.corrupt" (an older copy is
// replaced) so the user can recover it by hand.
inline bool KeepCorruptCopy(const std::filesystem::path& path) {
    std::filesystem::path copy = path;
    copy += L".corrupt";
    return ::CopyFileW(path.c_str(), copy.c_str(), FALSE) != FALSE;
}

enum class LoadStatus {
    Missing,     // no file yet: the defaults are used and may be written
    Loaded,      // read and parsed (a single bad field falls back on its own)
    Unreadable,  // the file exists but could not be read: kept untouched
    Corrupt,     // read but not valid JSON: copied to .corrupt, kept untouched
};

// Write gate for a settings file: after an Unreadable or Corrupt load the plugin
// runs on the defaults but never writes them over the user's file (the host
// saves every plugin every minute, on disable and on exit) until the user
// changes a setting in this session. An Unreadable file is retried meanwhile.
class SaveGuard {
public:
    void OnLoad(LoadStatus s) {
        m_reason = s;
        m_hold = s == LoadStatus::Unreadable || s == LoadStatus::Corrupt;
    }
    void OnUserChange() { m_hold = false; }
    bool MayWrite() const { return !m_hold; }
    bool Holding() const { return m_hold; }
    LoadStatus Reason() const { return m_reason; }
    bool ShouldRetryLoad() const { return m_hold && m_reason == LoadStatus::Unreadable; }

private:
    LoadStatus m_reason = LoadStatus::Missing;
    bool m_hold = false;
};

inline bool WriteTextFileAtomic(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    std::filesystem::path tmp = path;
    tmp += L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return false;
    const bool written = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    const bool flushed = std::fflush(f) == 0;
    std::fclose(f);
    if (!written || !flushed) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    if (!::MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        ::DeleteFileW(tmp.c_str());
        return false;
    }
    return true;
}

// Appends text to a file (decision-log sink). Creates the folder.
inline bool AppendTextFile(const std::filesystem::path& path, const std::string& text) {
    std::error_code ec;
    if (path.has_parent_path()) std::filesystem::create_directories(path.parent_path(), ec);
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"ab") != 0 || !f) return false;
    const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
    std::fclose(f);
    return ok;
}

}  // namespace fb::io
