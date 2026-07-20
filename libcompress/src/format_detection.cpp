#include "compress/types.h"
#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <fstream>
#include <string>

namespace compress {
namespace {

struct Magic {
    Format fmt;
    const char* bytes;
    size_t len;
    size_t offset;
};

// Known magic byte signatures
constexpr Magic MAGIC_TABLE[] = {
    { Format::SEVEN_ZIP,  "7z\xBC\xAF\x27\x1C", 6, 0 },
    { Format::GZIP,       "\x1F\x8B",            2, 0 },
    { Format::BZIP2,      "BZ",                  2, 0 },
    { Format::XZ,         "\xFD\x37\x7A\x58\x5A",5, 0 },
    { Format::ZSTD,       "\x28\xB5\x2F\xFD",    4, 0 },
    { Format::RAR,        "Rar!\x1A\x07\x00",    7, 0 },
    { Format::RAR,        "Rar!\x1A\x07\x01",    7, 0 },
    { Format::ZIP,        "PK\x03\x04",          4, 0 },
    { Format::ZIP,        "PK\x05\x06",          4, 0 },  // empty archive
    { Format::ZIP,        "PK\x07\x08",          4, 0 },  // spanned
    { Format::TAR,        "ustar\x00\x00",       8, 257 },
    { Format::TAR,        "ustar ",              7, 257 },
};

std::string lower_ascii(const std::string& s) {
    std::string r;
    r.reserve(s.size());
    for (char c : s) r.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return r;
}

} // anonymous namespace

Format detect_format_from_extension(const std::string& path) {
    auto pos = path.find_last_of('.');
    if (pos == std::string::npos) return Format::AUTO;

    std::string ext = lower_ascii(path.substr(pos));

    // Compound extensions first
    auto ends_with = [&](const std::string& suffix) -> bool {
        if (path.size() < suffix.size()) return false;
        return lower_ascii(path.substr(path.size() - suffix.size())) == suffix;
    };

    if (ends_with(".tar.gz") || ends_with(".tgz")) return Format::TAR;
    if (ends_with(".tar.bz2") || ends_with(".tbz2")) return Format::TAR;
    if (ends_with(".tar.bz") || ends_with(".tbz")) return Format::TAR;
    if (ends_with(".tar.xz") || ends_with(".txz")) return Format::TAR;
    if (ends_with(".tar.zst") || ends_with(".tzst")) return Format::TAR;
    if (ends_with(".tar.zstd")) return Format::TAR;
    if (path.size() >= 4) {
        std::string last4 = lower_ascii(path.substr(path.size() - 4));
        if (last4 == ".zip"    ) return Format::ZIP;
        if (last4 == ".7z"     ) return Format::SEVEN_ZIP;
        if (last4 == ".rar"    ) return Format::RAR;
        if (last4 == ".tar"    ) return Format::TAR;
        if (last4 == ".bz2"    ) return Format::BZIP2;
        if (last4 == ".xz"     ) return Format::XZ;
        if (last4 == ".zst"    ) return Format::ZSTD;
        if (last4 == ".zstd"   ) return Format::ZSTD;
        if (last4 == ".gz"     ) return Format::GZIP;
        if (last4 == ".gzip"   ) return Format::GZIP;
    }
    if (path.size() >= 3) {
        std::string last3 = lower_ascii(path.substr(path.size() - 3));
        if (last3 == ".gz"     ) return Format::GZIP;
        if (last3 == ".bz"     ) return Format::BZIP2;
        if (last3 == ".xz"     ) return Format::XZ;
        if (last3 == ".7z"     ) return Format::SEVEN_ZIP;
        if (last3 == ".tgz"    ) return Format::TAR;
        if (last3 == ".tbz"    ) return Format::TAR;
        if (last3 == ".txz"    ) return Format::TAR;
    }

    return Format::AUTO;
}

Format detect_format_from_magic(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Format::AUTO;

    // Read enough bytes for the longest signature + offset
    std::array<char, 512> buf{};
    f.read(buf.data(), static_cast<std::streamsize>(buf.size()));
    auto got = f.gcount();
    if (got <= 0) return Format::AUTO;

    auto matches = [&](const Magic& m) -> bool {
        if (static_cast<size_t>(got) < m.offset + m.len) return false;
        return std::memcmp(buf.data() + m.offset, m.bytes, m.len) == 0;
    };

    for (auto& m : MAGIC_TABLE) {
        if (matches(m)) return m.fmt;
    }
    return Format::AUTO;
}

Format detect_format(const std::string& path) {
    Format fmt = detect_format_from_magic(path);
    if (fmt == Format::AUTO) {
        fmt = detect_format_from_extension(path);
    }
    return fmt;
}

const char* format_name(Format fmt) {
    switch (fmt) {
        case Format::ZIP:       return "ZIP";
        case Format::TAR:       return "TAR";
        case Format::GZIP:      return "GZIP";
        case Format::BZIP2:     return "BZIP2";
        case Format::XZ:        return "XZ";
        case Format::ZSTD:      return "ZSTD";
        case Format::SEVEN_ZIP:  return "7z";
        case Format::RAR:        return "RAR";
        case Format::AUTO:       return "AUTO";
    }
    return "UNKNOWN";
}

bool is_multientry_format(Format fmt) {
    switch (fmt) {
        case Format::ZIP:
        case Format::TAR:
        case Format::SEVEN_ZIP:
        case Format::RAR:
            return true;
        default:
            return false;
    }
}

bool is_single_stream_format(Format fmt) {
    switch (fmt) {
        case Format::GZIP:
        case Format::BZIP2:
        case Format::XZ:
        case Format::ZSTD:
            return true;
        default:
            return false;
    }
}

} // namespace compress
