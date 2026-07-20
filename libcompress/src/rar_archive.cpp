#include "compress/types.h"
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// For reading RAR archives we use libarchive (supports RAR5/RAR4).
// If the unrar SDK is available, we could also use it directly.
#include <archive.h>
#include <archive_entry.h>

#include "compress/detail/compat.h"

namespace fs = std::filesystem;

namespace compress {

namespace {

constexpr size_t BUFFER_SIZE = 65536;

// -----------------------------------------------------------------------
// RAR Reader (uses libarchive — reads .rar natively, including encrypted)
// -----------------------------------------------------------------------
class RarReader : public ArchiveReader {
public:
    RarReader(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~RarReader() override { close(); }

    Result<std::vector<ArchiveEntry>> entries() override {
        if (!open_handle())
            return Result<std::vector<ArchiveEntry>>{{}, Error::make(Error::IO_ERROR, "Cannot open RAR")};

        close();
        open_handle();

        std::vector<ArchiveEntry> result;
        struct archive_entry* ae = nullptr;
        while (archive_read_next_header(handle_, &ae) == ARCHIVE_OK) {
            ArchiveEntry e;
            const char* name_utf8 = archive_entry_pathname_utf8(ae);
            e.path = name_utf8 ? name_utf8 : archive_entry_pathname(ae);
            e.size = static_cast<uint64_t>(archive_entry_size(ae));
            e.compressed_size = 0; // libarchive may not populate this
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
            return Error::make(Error::IO_ERROR, "Cannot open RAR");

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

            if (opts_.overwrite == OverwriteMode::SKIP && fs::exists(fpath))
                continue;

            // Check for wrong password
            if (archive_entry_is_encrypted(ae) && opts_.password.empty()) {
                archive_read_data_skip(handle_);
                continue;
            }

            FILE* fp = nullptr;
            if (fopen_s(&fp, fpath.string().c_str(), "wb") != 0 || !fp) {
                archive_read_data_skip(handle_);
                continue;
            }

            char buf[BUFFER_SIZE];
            while (true) {
                auto nread = archive_read_data(handle_, buf, sizeof(buf));
                if (nread < 0) {
                    // Archive error (likely wrong password)
                    fclose(fp);
                    fs::remove(fpath);
                    archive_read_data_skip(handle_);
                    return Error::make(Error::WRONG_PASSWORD,
                        "Incorrect password for encrypted RAR entry: " + safe);
                }
                if (nread == 0) break;
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
        if (!open_handle())
            return Error::make(Error::IO_ERROR, "Cannot open RAR");

        std::string target = archive_path;
        std::replace(target.begin(), target.end(), '\\', '/');

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
        return Error::make(Error::FILE_NOT_FOUND, "Entry not found: " + archive_path);
    }

    Result<std::vector<char>> read_entry(const std::string& archive_path) override {
        if (!open_handle())
            return Result<std::vector<char>>{{}, Error::make(Error::IO_ERROR, "Cannot open RAR")};

        std::string target = archive_path;
        std::replace(target.begin(), target.end(), '\\', '/');

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

        // RAR format support
        archive_read_support_format_rar(handle_);
        archive_read_support_format_rar5(handle_);

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
        info_.format = Format::RAR;
        info_.is_encrypted = !opts_.password.empty();
        return true;
    }

    std::string path_;
    ArchiveOptions opts_;
    ArchiveInfo info_;
    struct archive* handle_ = nullptr;
    bool opened_ = false;
};

} // anonymous namespace

// RAR: read-only (no writer — proprietary format)
std::unique_ptr<ArchiveReader> create_rar_reader(const std::string& path,
                                                  const ArchiveOptions& opts) {
    return std::make_unique<RarReader>(path, opts);
}

// No writer for RAR
std::unique_ptr<ArchiveWriter> create_rar_writer(const std::string& path,
                                                  const ArchiveOptions& opts) {
    return nullptr;
}

} // namespace compress
