#include "compress/types.h"
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <archive.h>
#include <archive_entry.h>

#include "compress/detail/compat.h"

#ifdef COMPRESS_HAVE_LZMA_SDK
#  include <Lzma2Enc.h>
#  include <LzmaDec.h>
#  include <7zAlloc.h>
#  include <7zBuf.h>
#  include <7zCrc.h>
#  include <7zFile.h>
#  include <7zVersion.h>
#  include <7zTypes.h>
#  include <Sha256.h>
#  include <Aes.h>
#endif

namespace fs = std::filesystem;

namespace compress {

namespace {

constexpr size_t BUFFER_SIZE = 65536;

// ======================================================================
//  7z binary format helpers
// ======================================================================
namespace SevenzFormat {

// Property IDs
enum PropID : uint8_t {
    kEnd            = 0x00,
    kHeader         = 0x01,
    kArchiveProps   = 0x02,
    kAdditionalStreamsInfo = 0x03,
    kMainStreamsInfo = 0x04,
    kFilesInfo      = 0x05,
    kPackInfo       = 0x06,
    kUnpackInfo     = 0x07,
    kSubStreamsInfo = 0x08,
    kSize           = 0x09,
    kCRC            = 0x0A,
    kFolder         = 0x0B,
    kCodersUnpackSize = 0x0C,
    kNumUnpackStream  = 0x0D,
    kEmptyStream      = 0x0E,
    kEmptyFile        = 0x0F,
    kAnti             = 0x10,
    kName             = 0x11,
    kCTime            = 0x12,
    kATime            = 0x13,
    kMTime            = 0x14,
    kWinAttributes    = 0x15,
    kComment          = 0x16,
    kEncodedHeader    = 0x17,
    kStartPos         = 0x18,
    kDummy            = 0x19
};

// Codec IDs (stored MSB-first as varint)
// LZMA2  = 0x21
// AES256 = 0x06F10701
constexpr uint64_t CODEC_LZMA2  = 0x21;
constexpr uint64_t CODEC_AES256 = 0x06F10701;

constexpr uint8_t SIGNATURE[6] = {0x37, 0x7A, 0xBC, 0xAF, 0x27, 0x1C};

// ------------------------------------------------------------------
// Varint encoding (same as 7z format)
// ------------------------------------------------------------------
void encode_varint(std::vector<uint8_t>& buf, uint64_t v) {
    do {
        uint8_t b = static_cast<uint8_t>(v & 0x7F);
        v >>= 7;
        if (v) b |= 0x80;
        buf.push_back(b);
    } while (v);
}

void encode_uint64(std::vector<uint8_t>& buf, uint64_t v) {
    for (int i = 0; i < 8; ++i) {
        buf.push_back(static_cast<uint8_t>(v & 0xFF));
        v >>= 8;
    }
}

void encode_uint32(std::vector<uint8_t>& buf, uint32_t v) {
    buf.push_back(static_cast<uint8_t>(v & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void encode_bools(std::vector<uint8_t>& buf, const std::vector<bool>& v) {
    size_t num_def = v.size();
    encode_varint(buf, num_def);
    for (size_t i = 0; i < num_def; i += 8) {
        uint8_t mask = 0;
        for (size_t j = 0; j < 8 && i + j < num_def; ++j) {
            if (v[i + j]) mask |= static_cast<uint8_t>(1 << j);
        }
        buf.push_back(mask);
    }
}

// ------------------------------------------------------------------
// CRC-32 calculation (simple implementation)
// ------------------------------------------------------------------
uint32_t crc32_update(uint32_t crc, const uint8_t* data, size_t size) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int j = 0; j < 8; ++j) {
                c = (c >> 1) ^ (c & 1 ? 0xEDB88320 : 0);
            }
            table[i] = c;
        }
        init = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < size; ++i) {
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    }
    return ~crc;
}

uint32_t crc32(const uint8_t* data, size_t size) {
    return crc32_update(0, data, size);
}

// ======================================================================
//  AES-256 helper (bundled implementation using LZMA SDK's Aes API)
} // namespace SevenzFormat

// ======================================================================
//  7z Writer implementation
// ======================================================================

// ======================================================================
//  7z Reader (uses libarchive)
// ======================================================================
class SevenZipReader : public ArchiveReader {
public:
    SevenZipReader(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~SevenZipReader() override { close(); }

    Result<std::vector<ArchiveEntry>> entries() override {
        if (!open_handle())
            return Result<std::vector<ArchiveEntry>>{{}, Error::make(Error::IO_ERROR, "Cannot open 7z")};
        close();
        open_handle();

        std::vector<ArchiveEntry> result;
        struct archive_entry* ae = nullptr;
        while (archive_read_next_header(handle_, &ae) == ARCHIVE_OK) {
            ArchiveEntry e;
            e.path = archive_entry_pathname_utf8(ae)
                ? archive_entry_pathname_utf8(ae) : archive_entry_pathname(ae);
            e.size = static_cast<uint64_t>(archive_entry_size(ae));
            e.is_directory = archive_entry_filetype(ae) == AE_IFDIR;
            e.is_symlink = archive_entry_filetype(ae) == AE_IFLNK;
            e.is_encrypted = archive_entry_is_encrypted(ae) != 0;
            if (archive_entry_mtime_is_set(ae))
                e.last_modified = std::chrono::system_clock::from_time_t(archive_entry_mtime(ae));
            result.push_back(e);
            archive_read_data_skip(handle_);
        }
        return Result<std::vector<ArchiveEntry>>{result, Error::ok()};
    }

    Error extract(const std::string& output_dir) override {
        std::string out = output_dir.empty() ? opts_.output_dir : output_dir;
        if (out.empty()) out = fs::current_path().string();
        if (!open_handle())
            return Error::make(Error::IO_ERROR, "Cannot open 7z");

        struct archive_entry* ae = nullptr;
        while (archive_read_next_header(handle_, &ae) == ARCHIVE_OK) {
            const char* name = archive_entry_pathname_utf8(ae)
                ? archive_entry_pathname_utf8(ae) : archive_entry_pathname(ae);
            if (!name || !*name) continue;
            std::string safe = safe_entry_name(name);
            if (safe.empty()) continue;
            fs::path fpath;
            if (!safe_extract_path(out, safe, fpath)) continue;

            if (archive_entry_filetype(ae) == AE_IFDIR) {
                fs::create_directories(fpath);
                archive_read_data_skip(handle_);
                continue;
            }
            fs::create_directories(fpath.parent_path());
            if (archive_entry_filetype(ae) == AE_IFLNK) {
                const char* target = archive_entry_symlink(ae);
                if (target) {
                    fs::path tgt(target);
                    fs::path resolved = fpath.parent_path() / tgt;
                    fs::path canonical = fs::weakly_canonical(resolved);
                    fs::path base = fs::weakly_canonical(out);
                    if (tgt.is_absolute() &&
                        canonical.string().compare(0, base.string().size(), base.string()) != 0) {
                        tgt = tgt.relative_path();
                    }
                    fs::remove(fpath); fs::create_symlink(tgt.string(), fpath);
                }
                archive_read_data_skip(handle_);
                continue;
            }
            if (opts_.overwrite == OverwriteMode::SKIP && fs::exists(fpath)) continue;

            FILE* fp = nullptr;
            if (fopen_s(&fp, fpath.string().c_str(), "wb") != 0 || !fp) {
                archive_read_data_skip(handle_);
                continue;
            }
            char buf[BUFFER_SIZE];
            while (true) {
                auto nread = archive_read_data(handle_, buf, sizeof(buf));
                if (nread <= 0) break;
                fwrite(buf, 1, static_cast<size_t>(nread), fp);
            }
            fclose(fp);
            if (archive_entry_mtime_is_set(ae)) {
                time_t mtime = archive_entry_mtime(ae);
                using namespace std::chrono;
                auto sctp = system_clock::from_time_t(mtime);
                fs::last_write_time(fpath,
                    fs::file_time_type(
                        duration_cast<fs::file_time_type::duration>(sctp.time_since_epoch())
                    )
                );
            }
        }
        return Error::ok();
    }

    Error extract_entry(const std::string& archive_path, const std::string& output_path) override {
        std::string target = archive_path;
        std::replace(target.begin(), target.end(), '\\', '/');
        if (!open_handle())
            return Error::make(Error::IO_ERROR, "Cannot open 7z");

        struct archive_entry* ae = nullptr;
        while (archive_read_next_header(handle_, &ae) == ARCHIVE_OK) {
            const char* name = archive_entry_pathname_utf8(ae)
                ? archive_entry_pathname_utf8(ae) : archive_entry_pathname(ae);
            if (!name) continue;
            if (target == name) {
                std::string out = output_path.empty() ? target : output_path;
                fs::path fpath(out);
                fs::create_directories(fpath.parent_path());
                FILE* fp = nullptr;
                if (fopen_s(&fp, fpath.string().c_str(), "wb") != 0 || !fp)
                    return Error::make(Error::PERMISSION_DENIED);
                char buf[BUFFER_SIZE];
                while (true) {
                    auto nread = archive_read_data(handle_, buf, sizeof(buf));
                    if (nread <= 0) break;
                    fwrite(buf, 1, static_cast<size_t>(nread), fp);
                }
                fclose(fp);
                return Error::ok();
            }
            archive_read_data_skip(handle_);
        }
        return Error::make(Error::FILE_NOT_FOUND, "Entry not found");
    }

    Result<std::vector<char>> read_entry(const std::string& archive_path) override {
        std::string target = archive_path;
        std::replace(target.begin(), target.end(), '\\', '/');
        if (!open_handle())
            return Result<std::vector<char>>{{}, Error::make(Error::IO_ERROR, "Cannot open 7z")};

        struct archive_entry* ae = nullptr;
        while (archive_read_next_header(handle_, &ae) == ARCHIVE_OK) {
            const char* name = archive_entry_pathname_utf8(ae)
                ? archive_entry_pathname_utf8(ae) : archive_entry_pathname(ae);
            if (!name) continue;
            if (target == name) {
                std::vector<char> data;
                char buf[BUFFER_SIZE];
                while (true) {
                    auto nread = archive_read_data(handle_, buf, sizeof(buf));
                    if (nread <= 0) break;
                    data.insert(data.end(), buf, buf + nread);
                }
                return Result<std::vector<char>>{data, Error::ok()};
            }
            archive_read_data_skip(handle_);
        }
        return Result<std::vector<char>>{{}, Error::make(Error::FILE_NOT_FOUND)};
    }

    Error close() override {
        if (handle_) {
            archive_read_close(handle_);
            archive_read_free(handle_);
            handle_ = nullptr;
        }
        opened_ = false;
        return Error::ok();
    }

    ArchiveInfo info() const override { return info_; }
    bool is_open() const override { return opened_; }

private:
    bool open_handle() {
        if (opened_) return true;
        handle_ = archive_read_new();
        if (!handle_) return false;
        archive_read_support_filter_all(handle_);
        archive_read_support_format_7zip(handle_);
        if (!opts_.password.empty())
            archive_read_add_passphrase(handle_, opts_.password.c_str());
        int r = archive_read_open_filename(handle_, path_.c_str(), BUFFER_SIZE);
        if (r != ARCHIVE_OK) {
            archive_read_free(handle_);
            handle_ = nullptr;
            return false;
        }
        opened_ = true;
        info_.path = path_;
        info_.format = Format::SEVEN_ZIP;
        info_.is_encrypted = !opts_.password.empty();
        return true;
    }

    std::string path_;
    ArchiveOptions opts_;
    ArchiveInfo info_;
    struct archive* handle_ = nullptr;
    bool opened_ = false;
};

// ======================================================================
//  7z Writer
// ======================================================================
class SevenZipWriter : public ArchiveWriter {
public:
    SevenZipWriter(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~SevenZipWriter() override { close(); }

    Error add_file(const std::string& disk_path, const std::string& archive_path) override {
#ifdef COMPRESS_HAVE_LZMA_SDK
        fs::path src(disk_path);
        if (!fs::exists(src)) return Error::make(Error::FILE_NOT_FOUND, disk_path);
        if (fs::is_directory(src)) return add_directory(disk_path);

        std::string name = archive_path.empty() ? src.filename().string() : archive_path;
        std::replace(name.begin(), name.end(), '\\', '/');

        // Read file
        FILE* fp = nullptr;
        if (fopen_s(&fp, disk_path.c_str(), "rb") != 0 || !fp)
            return Error::make(Error::FILE_NOT_FOUND, disk_path);
        std::vector<uint8_t> file_data;
        char buf[BUFFER_SIZE];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), fp)) > 0)
            file_data.insert(file_data.end(), buf, buf + n);
        fclose(fp);

        FileEntry entry;
        entry.name = name;
        entry.data = std::move(file_data);
        entry.has_modified_time = true;
        entry.modified_time = fs::last_write_time(src);
        files_.push_back(std::move(entry));
        return Error::ok();
#else
        (void)disk_path; (void)archive_path;
        return Error::make(Error::UNSUPPORTED_FEATURE,
            "7z writer requires LZMA SDK (not found at build time)");
#endif
    }

    Error add_directory(const std::string& disk_path) override {
#ifdef COMPRESS_HAVE_LZMA_SDK
        fs::path dir(disk_path);
        if (!fs::is_directory(dir)) return add_file(disk_path);

        // Add directory entry
        FileEntry entry;
        entry.name = dir.filename().string() + "/";
        entry.is_dir = true;
        entry.has_modified_time = true;
        entry.modified_time = fs::last_write_time(dir);
        files_.push_back(std::move(entry));

        // Recursively add files
        for (auto& fe : fs::recursive_directory_iterator(dir)) {
            std::string ap = fs::relative(fe.path(), dir.parent_path()).string();
            std::replace(ap.begin(), ap.end(), '\\', '/');
            if (fe.is_directory()) {
                FileEntry de;
                de.name = ap + "/";
                de.is_dir = true;
                de.has_modified_time = true;
                de.modified_time = fs::last_write_time(fe.path());
                files_.push_back(std::move(de));
            } else {
                add_file(fe.path().string(), ap);
            }
        }
        return Error::ok();
#else
        (void)disk_path;
        return Error::make(Error::UNSUPPORTED_FEATURE,
            "7z writer requires LZMA SDK (not found at build time)");
#endif
    }

    Error add_from_memory(const std::string& archive_path, const void* data, size_t size) override {
#ifdef COMPRESS_HAVE_LZMA_SDK
        std::string name = archive_path;
        std::replace(name.begin(), name.end(), '\\', '/');
        FileEntry entry;
        entry.name = std::move(name);
        entry.data.resize(size);
        if (size > 0) std::memcpy(entry.data.data(), data, size);
        files_.push_back(std::move(entry));
        return Error::ok();
#else
        (void)archive_path; (void)data; (void)size;
        return Error::make(Error::UNSUPPORTED_FEATURE,
            "7z writer requires LZMA SDK (not found at build time)");
#endif
    }

    Error close() override {
#ifdef COMPRESS_HAVE_LZMA_SDK
        if (files_.empty() || closed_) return Error::ok();
        closed_ = true;

        using namespace SevenzFormat;

        // Open output file
        FILE* fp = nullptr;
        if (fopen_s(&fp, path_.c_str(), "wb") != 0 || !fp)
            return Error::make(Error::PERMISSION_DENIED, "Cannot create: " + path_);

        // 1. Write signature
        fwrite(SIGNATURE, 1, 6, fp);
        uint8_t version[2] = {0, 4};
        fwrite(version, 1, 2, fp);

        // We'll come back and fill in StartHeader CRC + data later
        // Layout: CRC(4) + NextHeaderOffset(8) + NextHeaderSize(8) + NextHeaderCRC(4) = 24 bytes
        uint64_t start_header_crc_pos = ftell(fp);
        uint64_t start_header_offset_pos = start_header_crc_pos + 4;
        uint64_t start_header_size_pos  = start_header_offset_pos + 8;
        uint64_t start_header_crc2_pos  = start_header_size_pos + 8;
        uint64_t after_start_header     = start_header_crc2_pos + 4;

        uint8_t zeros[24] = {};
        fwrite(zeros, 1, 24, fp); // StartHeader placeholder

        // 2. Compress each file with LZMA2
        struct CompressedFile {
            std::vector<uint8_t> packed;
            uint64_t unpack_size;
        };
        std::vector<CompressedFile> compressed;
        uint64_t total_unpack = 0;

        for (auto& f : files_) {
            if (f.is_dir) {
                compressed.push_back({{}, 0});
                continue;
            }

            uint8_t lzma2_prop = 0;
            auto packed = compress_lzma2_raw(f.data.data(), f.data.size(),
                                             opts_.compression_level, lzma2_prop);
            if (packed.empty()) {
                fclose(fp);
                return Error::make(Error::IO_ERROR, "LZMA2 compression failed for: " + f.name);
            }

            compressed.push_back({std::move(packed), f.data.size()});
            total_unpack += f.data.size();
        }

        // 3. Write compressed pack data
        uint64_t pack_pos = 0;
        for (auto& cf : compressed) {
            if (cf.packed.empty()) continue;
            fwrite(cf.packed.data(), 1, cf.packed.size(), fp);
            pack_pos += cf.packed.size();
        }
        (void)pack_pos;

        // 4. Build binary header (metadata)
        std::vector<uint8_t> header_buf;

        // --- PackInfo ---
        uint64_t num_files = 0;
        for (auto& cf : compressed)
            if (!cf.packed.empty()) ++num_files;

        header_buf.push_back(kPackInfo);
        encode_varint(header_buf, 0); // PackPos = 0 (relative to after start header)
        encode_varint(header_buf, num_files);

        // Sizes
        header_buf.push_back(kSize);
        for (auto& cf : compressed) {
            if (!cf.packed.empty())
                encode_varint(header_buf, cf.packed.size());
        }

        // CRCs (optional)
        header_buf.push_back(kCRC);
        {
            std::vector<bool> crc_defined;
            for (auto& cf : compressed) crc_defined.push_back(!cf.packed.empty());
            encode_bools(header_buf, crc_defined);
            for (auto& cf : compressed) {
                if (!cf.packed.empty()) {
                    uint32_t crc = crc32(cf.packed.data(), cf.packed.size());
                    encode_uint32(header_buf, crc);
                }
            }
        }

        // --- UnpackInfo ---
        header_buf.push_back(kUnpackInfo);

        // NumFolders
        header_buf.push_back(kFolder);
        encode_varint(header_buf, num_files);

        // For each non-dir file: encode folder with LZMA2 coder
        for (auto& cf : compressed) {
            if (cf.packed.empty()) continue;

            // Folder
            encode_varint(header_buf, 1); // NumCoders = 1 (LZMA2)

            // Coder flags
            {
                uint8_t flags = 0;
                flags |= (1 << 6); // Coder has properties
                flags |= (1 << 7); // Coder has ID
                flags |= 0;       // No complex coder
                header_buf.push_back(flags);
            }

            // Codec ID: LZMA2 = 0x21 (MSB-first varint)
            {
                uint8_t codec_id = 0x21;
                header_buf.push_back(codec_id);
            }

            // LZMA2 properties (1 byte: dictionary size)
            {
                uint8_t lzma_props = 20; // dictionary = 16MB
                encode_varint(header_buf, 1);
                header_buf.push_back(lzma_props);
            }

            // No bind pairs (LZMA2 doesn't need them)
            encode_varint(header_buf, 0); // NumBindPairs = 0

            // NumPackedStreams (1 by default, can be omitted)
        }

        // CodersUnpackSizes
        header_buf.push_back(kCodersUnpackSize);
        for (auto& cf : compressed) {
            if (!cf.packed.empty())
                encode_varint(header_buf, cf.unpack_size);
        }

        // CRCs
        header_buf.push_back(kCRC);
        {
            std::vector<bool> crc_defined;
            for (auto& cf : compressed) crc_defined.push_back(!cf.packed.empty());
            encode_bools(header_buf, crc_defined);
            for (auto& cf : compressed) {
                if (!cf.packed.empty()) {
                    uint32_t crc = crc32(cf.data().data(), cf.data().size());
                    encode_uint32(header_buf, crc);
                }
            }
        }

        // --- SubStreamsInfo ---
        {
            header_buf.push_back(kSubStreamsInfo);
            header_buf.push_back(kNumUnpackStream);
            for (auto& cf : compressed) {
                if (!cf.packed.empty())
                    encode_varint(header_buf, 1); // 1 unpack stream per folder
            }
            // No sizes since each folder has exactly 1 stream
            header_buf.push_back(kCRC);
            std::vector<bool> crc_defined(compressed.size(), false);
            encode_bools(header_buf, crc_defined);
        }

        // --- FilesInfo ---
        {
            header_buf.push_back(kFilesInfo);
            encode_varint(header_buf, files_.size());

            // EmptyStreams: mark directories
            std::vector<bool> is_empty;
            for (auto& f : files_) is_empty.push_back(f.is_dir);
            bool has_empty = false;
            for (auto v : is_empty) if (v) { has_empty = true; break; }

            if (has_empty) {
                header_buf.push_back(kEmptyStream);
                encode_bools(header_buf, is_empty);
            }

            // Names
            header_buf.push_back(kName);
            {
                // Strings are UTF-16LE, null-terminated, in a single block
                // First: expected size
                // We'll write the raw names
                // NumFiles + UTF-16LE strings

                // Boolean vector: all external = false
                std::vector<bool> ext(files_.size(), false);
                encode_bools(header_buf, ext);

                // Write all names as UTF-16LE
                for (auto& f : files_) {
                    std::string name_clean = f.name;
                    // Replace forward slashes with backslashes for 7z
                    // 7z stores names with backslashes
                    for (auto& c : name_clean) if (c == '/') c = '\\';
                    for (char c : name_clean) {
                        header_buf.push_back(static_cast<uint8_t>(c));
                        header_buf.push_back(0); // high byte
                    }
                    header_buf.push_back(0); // null terminator
                    header_buf.push_back(0);
                }
            }

            // MTime
            if (true) {
                header_buf.push_back(kMTime);
                // NumFiles + defined flags + timestamps
                std::vector<bool> mtime_defined;
                for (auto& f : files_) mtime_defined.push_back(f.has_modified_time);
                encode_bools(header_buf, mtime_defined);
                for (auto& f : files_) {
                    if (!f.has_modified_time) continue;
                    // FileTime in 100ns intervals since 1601-01-01
                    // Convert from system_clock
                    auto tp = std::chrono::time_point_cast<std::chrono::seconds>(f.modified_time);
                    time_t tt = std::chrono::system_clock::to_time_t(tp);
                    // Convert time_t to Windows FILETIME-like:
                    // 11644473600 seconds from 1601 to 1970
                    constexpr uint64_t EPOCH_DIFF = 11644473600ULL;
                    uint64_t ft = (static_cast<uint64_t>(tt) + EPOCH_DIFF) * 10000000ULL;
                    encode_uint64(header_buf, ft);
                }
            }

            // WinAttributes
            {
                header_buf.push_back(kWinAttributes);
                std::vector<bool> attr_defined;
                for (auto& f : files_) attr_defined.push_back(true);
                encode_bools(header_buf, attr_defined);
                for (auto& f : files_) {
                    uint32_t attr = f.is_dir ? 0x10 : 0x20; // DIRECTORY or ARCHIVE
                    encode_uint32(header_buf, attr);
                }
            }
        }

        // End marker
        header_buf.push_back(kEnd);

        // 5. Compress the header with LZMA
        uint8_t header_prop = 0;
        auto compressed_header = compress_lzma2_raw(header_buf.data(), header_buf.size(),
                                                     9, header_prop);
        if (compressed_header.empty()) {
            fclose(fp);
            return Error::make(Error::IO_ERROR, "Failed to compress 7z header");
        }

        // 6. Write compressed header
        uint64_t header_offset = ftell(fp) - after_start_header;
        fwrite(compressed_header.data(), 1, compressed_header.size(), fp);

        // 7. Go back and fill in StartHeader
        uint64_t header_end = ftell(fp);
        uint64_t header_size = compressed_header.size();
        uint32_t header_crc = crc32(header_buf.data(), header_buf.size());

        fseek(fp, static_cast<long>(start_header_offset_pos), SEEK_SET);
        encode_uint64_to_file(fp, header_offset);
        encode_uint64_to_file(fp, header_size);
        encode_uint32_to_file(fp, header_crc);

        // StartHeaderCRC covers offset + size + NextHeaderCRC (2*8 + 4 = 20 bytes at position 12)
        {
            uint8_t start_header_data[20];
            encode_uint64_to_buf(start_header_data, header_offset);
            encode_uint64_to_buf(start_header_data + 8, header_size);
            encode_uint32_to_buf(start_header_data + 16, header_crc);
            uint32_t start_header_crc = crc32(start_header_data, 20);

            fseek(fp, static_cast<long>(start_header_crc_pos), SEEK_SET);
            encode_uint32_to_file(fp, start_header_crc);
        }

        fseek(fp, static_cast<long>(header_end), SEEK_SET);
        fclose(fp);
        return Error::ok();

#else  // COMPRESS_HAVE_LZMA_SDK not defined
        return Error::make(Error::UNSUPPORTED_FEATURE,
            "7z writer requires LZMA SDK (not found at build time)");
#endif
    }

    ArchiveInfo info() const override {
        ArchiveInfo info;
        info.path = path_;
        info.format = Format::SEVEN_ZIP;
        info.total_entries = files_.size();
        info.is_encrypted = !opts_.password.empty();
        return info;
    }

    bool is_open() const override { return !closed_; }

private:
    struct FileEntry {
        std::string name;
        std::vector<uint8_t> data;
        bool is_dir = false;
        bool has_modified_time = false;
        fs::file_time_type modified_time;
    };

    static void encode_uint64_to_file(FILE* fp, uint64_t v) {
        uint8_t buf[8];
        encode_uint64_to_buf(buf, v);
        fwrite(buf, 1, 8, fp);
    }

    static void encode_uint32_to_file(FILE* fp, uint32_t v) {
        uint8_t buf[4];
        encode_uint32_to_buf(buf, v);
        fwrite(buf, 1, 4, fp);
    }

    static void encode_uint64_to_buf(uint8_t* buf, uint64_t v) {
        for (int i = 0; i < 8; ++i) {
            buf[i] = static_cast<uint8_t>(v & 0xFF);
            v >>= 8;
        }
    }

    static void encode_uint32_to_buf(uint8_t* buf, uint32_t v) {
        for (int i = 0; i < 4; ++i) {
            buf[i] = static_cast<uint8_t>(v & 0xFF);
            v >>= 8;
        }
    }

    static uint32_t crc32(const uint8_t* data, size_t size) {
        return SevenzFormat::crc32(data, size);
    }

#ifdef COMPRESS_HAVE_LZMA_SDK
    static std::vector<uint8_t> compress_lzma2_raw(const uint8_t* data, size_t size,
                                                     int level, uint8_t& prop) {
        ISzAlloc alloc = { sz_alloc, sz_free };
        CLzma2EncHandle enc = Lzma2Enc_Create(&alloc);
        if (!enc) return {};

        CLzma2EncProps props;
        Lzma2EncProps_Init(&props);
        if (level >= 0) props.lzmaProps.level = std::min(level, 9);
        props.lzmaProps.algo = 1;
        props.lzmaProps.numThreads = 1;
        props.lzmaProps.reduceSize = size;

        if (Lzma2Enc_SetProps(enc, &props) != SZ_OK) {
            Lzma2Enc_Destroy(enc);
            return {};
        }

        prop = 20; // default LZMA2 dictionary size property

        struct MemOut {
            ISeqOutStream vt;
            std::vector<uint8_t> buf;
        };
        MemOut out;
        out.vt.Write = [](const ISeqOutStream* pp, const void* d, size_t s) -> size_t {
            auto& b = const_cast<MemOut*>(static_cast<const MemOut*>(pp))->buf;
            b.insert(b.end(), static_cast<const uint8_t*>(d),
                     static_cast<const uint8_t*>(d) + s);
            return s;
        };

        struct MemIn {
            ISeqInStream vt;
            const uint8_t* data;
            size_t size;
            size_t pos;
        };
        MemIn in;
        in.data = data;
        in.size = size;
        in.pos = 0;
        in.vt.Read = [](const ISeqInStream* pp, void* buf, size_t* s) -> SRes {
            auto& mi = *static_cast<MemIn*>(const_cast<ISeqInStream*>(pp));
            size_t avail = mi.size - mi.pos;
            size_t to_copy = *s < avail ? *s : avail;
            if (to_copy > 0) std::memcpy(buf, mi.data + mi.pos, to_copy);
            mi.pos += to_copy;
            *s = to_copy;
            return SZ_OK;
        };

        SRes res = Lzma2Enc_Encode(enc, &out.vt, &in.vt, nullptr);
        Lzma2Enc_Destroy(enc);

        if (res != SZ_OK) return {};
        return std::move(out.buf);
    }

    static void* sz_alloc(ISzAllocPtr, size_t sz) { return new uint8_t[sz]; }
    static void sz_free(ISzAllocPtr, void* p) { delete[] static_cast<uint8_t*>(p); }
#endif // COMPRESS_HAVE_LZMA_SDK

    std::string path_;
    ArchiveOptions opts_;
    std::vector<FileEntry> files_;
    bool closed_ = false;
};

} // anonymous namespace

std::unique_ptr<ArchiveReader> create_sevenz_reader(const std::string& path,
                                                     const ArchiveOptions& opts) {
    return std::make_unique<SevenZipReader>(path, opts);
}

std::unique_ptr<ArchiveWriter> create_sevenz_writer(const std::string& path,
                                                     const ArchiveOptions& opts) {
#ifdef COMPRESS_HAVE_LZMA_SDK
    return std::make_unique<SevenZipWriter>(path, opts);
#else
    (void)path; (void)opts;
    return nullptr;
#endif
}

} // namespace compress
