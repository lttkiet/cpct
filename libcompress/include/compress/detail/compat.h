#pragma once

#include <algorithm>
#include <filesystem>
#include <string>

#ifndef _WIN32
#include <cerrno>
#include <cstdio>
#define fopen_s(pfp, path, mode) ((*(pfp) = fopen(path, mode)) == nullptr ? errno : 0)
#endif

namespace fs = std::filesystem;

inline bool safe_extract_path(const fs::path& out_dir, const fs::path& entry_path,
                               fs::path& result) {
    result = out_dir / entry_path;
    auto norm = fs::weakly_canonical(result);
    auto base = fs::weakly_canonical(out_dir);
    auto base_str = base.string();
    auto norm_str = norm.string();
    if (norm_str.size() < base_str.size()) return false;
    if (norm_str.compare(0, base_str.size(), base_str) != 0) return false;
    return true;
}

inline std::string safe_entry_name(const char* name) {
    if (!name || !*name) return "";
    std::string safe = name;
    std::replace(safe.begin(), safe.end(), '\\', '/');
    return safe;
}
