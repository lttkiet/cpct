#include "compress/types.h"
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

#include <archive.h>
#include <archive_entry.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "compress/detail/compat.h"

namespace fs = std::filesystem;

namespace compress {

namespace {

constexpr size_t BUFFER_SIZE = 65536;

int to_archive_filter(Format fmt) {
    switch (fmt) {
        case Format::GZIP:  return ARCHIVE_FILTER_GZIP;
        case Format::BZIP2: return ARCHIVE_FILTER_BZIP2;
        case Format::XZ:    return ARCHIVE_FILTER_XZ;
        case Format::ZSTD:  return ARCHIVE_FILTER_ZSTD;
        default:             return ARCHIVE_FILTER_NONE;
    }
}

ArchiveEntry convert_entry(struct archive_entry* ae) {
    ArchiveEntry e;
    e.path = archive_entry_pathname_utf8(ae) ? archive_entry_pathname_utf8(ae)
                                             : archive_entry_pathname(ae);
    e.size = static_cast<uint64_t>(archive_entry_size(ae));
    e.is_directory = archive_entry_filetype(ae) == AE_IFDIR;
    e.is_symlink = archive_entry_filetype(ae) == AE_IFLNK;
    if (e.is_symlink && archive_entry_symlink(ae))
        e.symlink_target = archive_entry_symlink(ae);
    e.is_encrypted = archive_entry_is_encrypted(ae) != 0;
    if (archive_entry_mtime_is_set(ae))
        e.last_modified = std::chrono::system_clock::from_time_t(archive_entry_mtime(ae));
    return e;
}

// -----------------------------------------------------------------------
// TAR Reader
// -----------------------------------------------------------------------
class TarReader : public ArchiveReader {
public:
    TarReader(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~TarReader() override { close(); }

    Result<std::vector<ArchiveEntry>> entries() override {
        if (!open_handle())
            return Result<std::vector<ArchiveEntry>>{{}, Error::make(Error::IO_ERROR, "Cannot open TAR")};

        // Rewind by closing + reopening
        close();
        open_handle();

        std::vector<ArchiveEntry> result;
        struct archive_entry* ae = nullptr;
        while (archive_read_next_header(handle_, &ae) == ARCHIVE_OK) {
            result.push_back(convert_entry(ae));
            archive_read_data_skip(handle_);
        }
        return Result<std::vector<ArchiveEntry>>{result, Error::ok()};
    }

    Error extract(const std::string& output_dir) override {
        std::string out = output_dir.empty() ? opts_.output_dir : output_dir;
        if (out.empty()) out = fs::current_path().string();

        close();
        if (!open_handle())
            return Error::make(Error::IO_ERROR, "Cannot open TAR for extraction");

        struct archive_entry* ae = nullptr;
        while (archive_read_next_header(handle_, &ae) == ARCHIVE_OK) {
            const char* name = archive_entry_pathname_utf8(ae);
            if (!name) name = archive_entry_pathname(ae);
            if (!name || !*name) continue;

            std::string safe_name = safe_entry_name(name);
            if (safe_name.empty()) continue;
            fs::path fpath;
            if (!safe_extract_path(out, safe_name, fpath)) continue;

            int filetype = archive_entry_filetype(ae);

            if (filetype == AE_IFDIR) {
                fs::create_directories(fpath);
                archive_read_data_skip(handle_);
                continue;
            }

            fs::create_directories(fpath.parent_path());

            if (filetype == AE_IFLNK) {
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
                    fs::remove(fpath);
                    fs::create_symlink(tgt.string(), fpath);
                }
                archive_read_data_skip(handle_);
                continue;
            }

            if (opts_.overwrite == OverwriteMode::SKIP && fs::exists(fpath))
                continue;

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

            // Restore metadeta
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
        close();
        if (!open_handle())
            return Error::make(Error::IO_ERROR, "Cannot open TAR");

        std::string target = archive_path;
        std::replace(target.begin(), target.end(), '\\', '/');

        struct archive_entry* ae = nullptr;
        while (archive_read_next_header(handle_, &ae) == ARCHIVE_OK) {
            const char* name = archive_entry_pathname_utf8(ae);
            if (!name) name = archive_entry_pathname(ae);
            if (!name) continue;

            if (target == name) {
                std::string out_path = output_path.empty() ? target : output_path;
                fs::path fpath(out_path);
                fs::create_directories(fpath.parent_path());

                FILE* fp = nullptr;
                if (fopen_s(&fp, fpath.string().c_str(), "wb") != 0 || !fp)
                    return Error::make(Error::PERMISSION_DENIED, "Cannot write: " + out_path);

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
        close();
        if (!open_handle())
            return Result<std::vector<char>>{{}, Error::make(Error::IO_ERROR, "Cannot open TAR")};

        std::string target = archive_path;
        std::replace(target.begin(), target.end(), '\\', '/');

        struct archive_entry* ae = nullptr;
        while (archive_read_next_header(handle_, &ae) == ARCHIVE_OK) {
            const char* name = archive_entry_pathname_utf8(ae);
            if (!name) name = archive_entry_pathname(ae);
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
        return Result<std::vector<char>>{{}, Error::make(Error::FILE_NOT_FOUND, "Entry not found: " + archive_path)};
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
        archive_read_support_format_tar(handle_);
        archive_read_support_format_gnutar(handle_);

        // Also handle raw .gz/.bz2/.xz/.zst single-stream via libarchive
        archive_read_support_format_raw(handle_);

        if (!opts_.password.empty()) {
            archive_read_add_passphrase(handle_, opts_.password.c_str());
        }

        int r = archive_read_open_filename(handle_, path_.c_str(), BUFFER_SIZE);
        if (r != ARCHIVE_OK) {
            archive_read_free(handle_);
            handle_ = nullptr;
            return false;
        }

        opened_ = true;
        info_.path = path_;
        info_.format = Format::TAR;
        info_.is_encrypted = !opts_.password.empty();
        return true;
    }

    std::string path_;
    ArchiveOptions opts_;
    ArchiveInfo info_;
    struct archive* handle_ = nullptr;
    bool opened_ = false;
};

// -----------------------------------------------------------------------
// TAR Writer
// -----------------------------------------------------------------------
class TarWriter : public ArchiveWriter {
public:
    TarWriter(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~TarWriter() override { close(); }

    Error add_file(const std::string& disk_path, const std::string& archive_path) override {
        if (!open_handle())
            return Error::make(Error::IO_ERROR, "Cannot open TAR for writing");

        fs::path src(disk_path);
        if (!fs::exists(src)) return Error::make(Error::FILE_NOT_FOUND, disk_path);
        if (fs::is_directory(src)) return add_directory(disk_path);

        std::string name = archive_path.empty() ? src.filename().string() : archive_path;
        std::replace(name.begin(), name.end(), '\\', '/');

        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, name.c_str());

        std::error_code ec;
        if (fs::is_symlink(src, ec) && !ec) {
            auto target = fs::read_symlink(src, ec);
            if (!ec) {
                archive_entry_set_filetype(entry, AE_IFLNK);
                archive_entry_set_symlink(entry, target.string().c_str());
                archive_entry_set_size(entry, 0);
                archive_entry_set_perm(entry, 0777);
                int r = archive_write_header(handle_, entry);
                archive_entry_free(entry);
                return r == ARCHIVE_OK ? Error::ok()
                    : Error::make(Error::IO_ERROR, archive_error_string(handle_));
            }
        }

        archive_entry_set_size(entry, static_cast<int64_t>(fs::file_size(src)));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);

        auto ft = fs::last_write_time(src);
        using namespace std::chrono;
        auto sys_tp = system_clock::time_point(
            duration_cast<system_clock::duration>(ft.time_since_epoch())
        );
        archive_entry_set_mtime(entry, system_clock::to_time_t(sys_tp), 0);

        int r = archive_write_header(handle_, entry);
        archive_entry_free(entry);

        if (r != ARCHIVE_OK)
            return Error::make(Error::IO_ERROR, archive_error_string(handle_));

        FILE* fp = nullptr;
        if (fopen_s(&fp, disk_path.c_str(), "rb") != 0 || !fp)
            return Error::make(Error::FILE_NOT_FOUND, "Cannot open: " + disk_path);

        char buf[BUFFER_SIZE];
        size_t nread = 0;
        while ((nread = fread(buf, 1, sizeof(buf), fp)) > 0) {
            auto written = archive_write_data(handle_, buf, nread);
            if (written < 0) break;
        }
        fclose(fp);
        return Error::ok();
    }

    Error add_directory(const std::string& disk_path) override {
        if (!open_handle())
            return Error::make(Error::IO_ERROR, "Cannot open TAR for writing");

        fs::path dir(disk_path);
        if (!fs::is_directory(dir)) return add_file(disk_path, "");

        // Add directory entry
        std::string name = dir.filename().string() + "/";
        std::replace(name.begin(), name.end(), '\\', '/');

        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, name.c_str());
        archive_entry_set_size(entry, 0);
        archive_entry_set_filetype(entry, AE_IFDIR);
        archive_entry_set_perm(entry, 0755);
        archive_write_header(handle_, entry);
        archive_entry_free(entry);

        // Recursive files
        for (auto& fe : fs::recursive_directory_iterator(dir)) {
            std::string ap = fs::relative(fe.path(), dir.parent_path()).string();
            std::replace(ap.begin(), ap.end(), '\\', '/');

            if (fe.is_directory()) {
                std::string d = ap + "/";
                struct archive_entry* de = archive_entry_new();
                archive_entry_set_pathname_utf8(de, d.c_str());
                archive_entry_set_size(de, 0);
                archive_entry_set_filetype(de, AE_IFDIR);
                archive_entry_set_perm(de, 0755);
                archive_write_header(handle_, de);
                archive_entry_free(de);
            } else {
                Error err = add_file(fe.path().string(), ap);
                if (err) return err;
            }
        }
        return Error::ok();
    }

    Error add_from_memory(const std::string& archive_path, const void* data, size_t size) override {
        if (!open_handle())
            return Error::make(Error::IO_ERROR, "Cannot open TAR for writing");

        std::string name = archive_path;
        std::replace(name.begin(), name.end(), '\\', '/');

        struct archive_entry* entry = archive_entry_new();
        archive_entry_set_pathname_utf8(entry, name.c_str());
        archive_entry_set_size(entry, static_cast<int64_t>(size));
        archive_entry_set_filetype(entry, AE_IFREG);
        archive_entry_set_perm(entry, 0644);

        int r = archive_write_header(handle_, entry);
        archive_entry_free(entry);

        if (r != ARCHIVE_OK)
            return Error::make(Error::IO_ERROR, archive_error_string(handle_));

        if (size > 0) {
            archive_write_data(handle_, data, size);
        }
        return Error::ok();
    }

    Error close() override {
        if (handle_) {
            archive_write_close(handle_);
            archive_write_free(handle_);
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

        handle_ = archive_write_new();
        if (!handle_) return false;

        archive_write_set_format_pax(handle_);

        // Determine filter from extension or explicit option
        Format fmt = detect_tar_filter(path_);

        int filter = to_archive_filter(fmt);
        if (filter != ARCHIVE_FILTER_NONE) {
            archive_write_add_filter(handle_, filter);
        } else {
            archive_write_add_filter_none(handle_);
        }

        int r = archive_write_open_filename(handle_, path_.c_str());
        if (r != ARCHIVE_OK) {
            archive_write_free(handle_);
            handle_ = nullptr;
            return false;
        }

        opened_ = true;
        info_.path = path_;
        info_.format = Format::TAR;
        return true;
    }

    Format detect_tar_filter(const std::string& path) {
        auto pos = path.rfind('.');
        if (pos == std::string::npos) return Format::TAR;
        std::string ext = path.substr(pos);
        // Check compound extensions
        if (pos >= 4) {
            std::string ext4 = path.substr(pos - 4);
            if (ext4 == ".tar.gz" || ext4 == ".tgz") return Format::GZIP;
            if (ext4 == ".tar.bz" || ext4 == ".tbz") return Format::BZIP2;
            if (ext4 == ".tar.xz" || ext4 == ".txz") return Format::XZ;
        }
        if (pos >= 5) {
            std::string ext5 = path.substr(pos - 5);
            if (ext5 == ".tar.bz2" || ext5 == ".tbz2") return Format::BZIP2;
            if (ext5 == ".tar.zst") return Format::ZSTD;
        }
        return Format::TAR;
    }

    std::string path_;
    ArchiveOptions opts_;
    ArchiveInfo info_;
    struct archive* handle_ = nullptr;
    bool opened_ = false;
};

} // anonymous namespace

std::unique_ptr<ArchiveReader> create_tar_reader(const std::string& path,
                                                  const ArchiveOptions& opts) {
    return std::make_unique<TarReader>(path, opts);
}

std::unique_ptr<ArchiveWriter> create_tar_writer(const std::string& path,
                                                  const ArchiveOptions& opts) {
    return std::make_unique<TarWriter>(path, opts);
}

} // namespace compress
