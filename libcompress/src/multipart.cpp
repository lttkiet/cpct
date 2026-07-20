#include "compress/types.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <numeric>
#include <regex>
#include <string>
#include <vector>

namespace compress {
namespace detail {

// -----------------------------------------------------------------------
// Parse size suffixes like "10M", "100K", "1G"
// -----------------------------------------------------------------------
uint64_t parse_size_suffix(const std::string& s) {
    if (s.empty()) return 0;
    std::string num;
    char suffix = 0;
    for (auto it = s.rbegin(); it != s.rend(); ++it) {
        if (std::isalpha(static_cast<unsigned char>(*it))) {
            if (suffix == 0) { suffix = static_cast<char>(std::toupper(static_cast<unsigned char>(*it))); }
        } else break;
    }
    size_t end = s.find_first_not_of("0123456789");
    if (end == std::string::npos) end = s.size();
    // walk backward from end to skip suffix chars
    while (end > 0 && std::isalpha(static_cast<unsigned char>(s[end-1]))) --end;
    num = s.substr(0, end);

    uint64_t val = 0;
    try { val = std::stoull(num); } catch (...) { return 0; }

    switch (suffix) {
        case 'K': return val * 1024ULL;
        case 'M': return val * 1024ULL * 1024ULL;
        case 'G': return val * 1024ULL * 1024ULL * 1024ULL;
        default:  return val; // bytes
    }
}

} // namespace detail

// -----------------------------------------------------------------------
// Multipart helpers
// -----------------------------------------------------------------------

// Splits a filename into base + numeric part extension
// e.g. "archive.z01" -> ("archive", 1, ".zip")
//       "archive.part1.rar" -> ("archive", 1, ".rar")
//       "archive.tar.aa" -> ("archive", 0, ".tar.aa") -- not handled here
namespace {

struct PartInfo {
    std::string base;
    int number = 0;
    std::string extension;
    bool valid = false;
};

PartInfo parse_part_name(const std::string& path) {
    PartInfo info;

    // Pattern: .zNN (split ZIP) e.g. .z01, .z02, ..., .zip is last
    // We also detect .partN.ext (RAR volumes)
    std::smatch m;

    // RAR volume pattern: archive.part1.rar, archive.part2.rar
    if (std::regex_match(path, m, std::regex(R"((.+)\.part(\d+)\.(\w+)$)"))) {
        info.base = m[1].str();
        info.number = std::stoi(m[2].str());
        info.extension = "." + m[3].str();
        info.valid = true;
        return info;
    }

    // ZIP split pattern: archive.z01, archive.z02, ..., archive.zip
    if (std::regex_match(path, m, std::regex(R"((.+)\.z(\d{2})$)"))) {
        info.base = m[1].str();
        info.number = std::stoi(m[2].str());
        info.extension = ".zip";  // original format
        info.valid = true;
        return info;
    }

    // 7z split pattern: archive.7z.001, archive.7z.002
    if (std::regex_match(path, m, std::regex(R"((.+)\.(\w+)\.(\d{3})$)"))) {
        info.base = m[1].str();
        info.number = std::stoi(m[3].str());
        info.extension = "." + m[2].str();
        info.valid = true;
        return info;
    }

    // tar split: archive.tar.aa, archive.tar.ab, ...
    if (std::regex_match(path, m, std::regex(R"((.+)\.(\w+)\.([a-z]{2})$)"))) {
        info.base = m[1].str();
        // Convert aa -> 0, ab -> 1, ..., ba -> 26, ...
        const auto& letters = m[3].str();
        info.number = (letters[0] - 'a') * 26 + (letters[1] - 'a');
        info.extension = "." + m[2].str();
        info.valid = true;
        return info;
    }

    return info;
}

} // anonymous namespace

// -----------------------------------------------------------------------
// Build a sorted list of multipart file paths for reading
// -----------------------------------------------------------------------
std::vector<std::string> find_multipart_parts(const std::string& primary_path) {
    auto info = parse_part_name(primary_path);
    if (!info.valid) return { primary_path };

    // If it's already a numbered part, reconstruct the full set
    // We scan for all parts belonging to this base
    std::string pattern_base = info.base;

    // Build the "main" file extension
    std::string main_ext;
    if (info.extension == ".zip") {
        main_ext = ".zip";
    } else {
        main_ext = info.extension;
    }

    // Try to find all parts. Strategy: look for the last part first.
    std::map<int, std::string> parts;

    // Check if there's a .zip (or main extension) for ranges
    // For ZIP split: .z01, .z02, ..., .zip
    // For RAR: .part1.rar, .part2.rar, ...
    // For 7z: .7z.001, .7z.002, ...

    if (info.extension == ".zip") {
        // ZIP split: primary is .zip, find .z01, .z02, ...
        parts[999] = primary_path;  // last part
        for (int i = 0; i <= 99; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), ".z%02d", i);
            std::string part_path = info.base + buf;
            std::ifstream f(part_path, std::ios::binary);
            if (f) {
                parts[i] = part_path;
                f.close();
            }
        }
    } else if (primary_path.find(".part") != std::string::npos) {
        // RAR-style: primary is .partN.ext, find all others
        parts[info.number] = primary_path;
        // Try nearby numbers
        for (int i = 0; i <= 999; ++i) {
            if (i == info.number) continue;
            std::string part_path = info.base + ".part" + std::to_string(i) + main_ext;
            std::ifstream f(part_path, std::ios::binary);
            if (f) {
                parts[i] = part_path;
                f.close();
            }
        }
    } else {
        // 7z split or other numbered parts: .base.ext.001, .base.ext.002, ...
        parts[info.number] = primary_path;
        for (int i = 0; i <= 999; ++i) {
            if (i == info.number) continue;
            char buf[16];
            std::snprintf(buf, sizeof(buf), ".%03d", i);
            std::string part_path = info.base + main_ext + buf;
            std::ifstream f(part_path, std::ios::binary);
            if (f) {
                parts[i] = part_path;
                f.close();
            }
        }
    }

    if (parts.size() <= 1) return { primary_path };

    std::vector<std::string> result;
    for (auto& [num, path] : parts) {
        result.push_back(path);
    }
    return result;
}

// -----------------------------------------------------------------------
// Build multipart output filenames for writing
// -----------------------------------------------------------------------
std::string multipart_filename(const std::string& base_path, int part_index,
                                MultipartMode mode, const std::string& format_ext) {
    switch (mode) {
        case MultipartMode::SPLIT: {
            if (part_index == 0) return base_path; // last part = real extension
            char buf[16];
            if (format_ext == ".zip") {
                std::snprintf(buf, sizeof(buf), ".z%02d", part_index);
                // For ZIP, parts before the last use .zNN extension
                std::string p = base_path;
                // Replace .zip with .zNN
                auto dot = p.rfind(".zip");
                if (dot != std::string::npos) p.replace(dot, 4, buf);
                else p += buf;
                return p;
            } else {
                std::snprintf(buf, sizeof(buf), ".%03d", part_index);
                // Remove the extension and add .NNN
                std::string p = base_path;
                auto dot = p.rfind('.');
                if (dot != std::string::npos) p.insert(dot, buf);
                else p += buf;
                return p;
            }
        }
        case MultipartMode::RAR_VOLUME: {
            std::string p = base_path;
            auto dot = p.rfind('.');
            std::string ext = (dot != std::string::npos) ? p.substr(dot) : "";
            std::string name = (dot != std::string::npos) ? p.substr(0, dot) : p;
            return name + ".part" + std::to_string(part_index + 1) + ext;
        }
        default:
            return base_path;
    }
}

} // namespace compress
