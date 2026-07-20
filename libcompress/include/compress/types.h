#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace compress {

enum class Format {
    ZIP,
    TAR,
    GZIP,
    BZIP2,
    XZ,
    ZSTD,
    SEVEN_ZIP,
    RAR,
    AUTO
};

enum class EncryptionMethod {
    NONE,
    ZIP_CRYPTO,
    AES_128,
    AES_256,
    SEVEN_ZIP_AES_256,
    RAR_AES_256
};

enum class MultipartMode {
    NONE,
    SPLIT,
    RAR_VOLUME
};

enum class OverwriteMode {
    PROMPT,
    SKIP,
    OVERWRITE,
    RENAME
};

struct ArchiveEntry {
    std::string  path;
    uint64_t     size             = 0;
    uint64_t     compressed_size  = 0;
    uint32_t     crc32            = 0;
    bool         is_directory     = false;
    bool         is_encrypted     = false;
    bool         is_symlink       = false;
    std::string  symlink_target;
    std::chrono::system_clock::time_point last_modified;
};

struct ArchiveOptions {
    Format           format           = Format::AUTO;
    std::string      password;
    EncryptionMethod encryption       = EncryptionMethod::NONE;
    int              compression_level = -1;   // -1 = default for the format
    bool             append           = false;
    bool             verbose          = false;

    MultipartMode    multipart_mode   = MultipartMode::NONE;
    uint64_t         volume_size      = 0;

    std::string      output_dir;
    OverwriteMode    overwrite        = OverwriteMode::SKIP;
    bool             extract_with_paths = true;
    int              num_threads      = 1;
};

struct ArchiveInfo {
    Format               format;
    std::string          path;
    uint64_t             total_entries       = 0;
    uint64_t             total_size          = 0;
    uint64_t             total_compressed_size = 0;
    bool                 is_encrypted        = false;
    bool                 is_multipart        = false;
    std::vector<std::string> parts;
};

struct Error {
    enum Code : int {
        OK                 = 0,
        INVALID_ARGUMENT   = 1,
        FILE_NOT_FOUND     = 2,
        PERMISSION_DENIED  = 3,
        WRONG_PASSWORD     = 4,
        CORRUPTED_ARCHIVE  = 5,
        UNSUPPORTED_FORMAT = 6,
        UNSUPPORTED_FEATURE= 7,
        MULTIPART_MISSING  = 8,
        OUT_OF_MEMORY      = 9,
        IO_ERROR           = 10,
        NOT_IMPLEMENTED    = 11,
        UNKNOWN            = 99
    };

    Code    code    = OK;
    std::string message;

    explicit operator bool() const { return code != OK; }

    static Error ok() { return Error{}; }
    static Error make(Code c, std::string m = "") { return Error{c, std::move(m)}; }
};

template <typename T>
struct Result {
    T     value;
    Error error;

    bool  ok()      const { return !error; }
    explicit operator bool() const { return ok(); }
    const T& operator*()  const { return value; }
    T&       operator*()        { return value; }
    const T* operator->() const { return &value; }
    T*       operator->()       { return &value; }
};

template <>
struct Result<void> {
    Error error;
    bool  ok() const { return !error; }
    explicit operator bool() const { return ok(); }
};

namespace detail {
    uint64_t parse_size_suffix(const std::string& s);
}

} // namespace compress
