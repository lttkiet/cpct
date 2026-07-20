#include "compress/types.h"
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

#include <mz.h>
#include <mz_strm.h>
#include <mz_zip.h>
#include <mz_zip_rw.h>
#include <mz_crypt.h>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace compress {

namespace {

constexpr size_t BUFFER_SIZE = 65536;

static auto from_time_t(time_t t) {
    return std::chrono::system_clock::from_time_t(t);
}

static time_t file_time_to_time_t(const fs::file_time_type& ft) {
    using namespace std::chrono;
    auto sys_tp = system_clock::time_point(
        duration_cast<system_clock::duration>(ft.time_since_epoch())
    );
    return system_clock::to_time_t(sys_tp);
}

static void set_file_mtime(const fs::path& fpath, time_t mtime) {
    using namespace std::chrono;
    auto tp = system_clock::from_time_t(mtime);
    fs::last_write_time(fpath,
        fs::file_time_type(
            duration_cast<fs::file_time_type::duration>(tp.time_since_epoch())
        )
    );
}

// -----------------------------------------------------------------------
// ZIP Reader
// -----------------------------------------------------------------------
class ZipReader : public ArchiveReader {
public:
    ZipReader(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~ZipReader() override { close(); }

    Result<std::vector<ArchiveEntry>> entries() override {
        if (!open_handle()) {
            return Result<std::vector<ArchiveEntry>>{{}, Error::make(Error::IO_ERROR, "Failed to open ZIP")};
        }

        std::vector<ArchiveEntry> result;
        Error err = iterate([&](mz_zip_file* info) -> Error {
            ArchiveEntry e;
            e.path = info->filename ? info->filename : "";
            e.size = static_cast<uint64_t>(info->uncompressed_size);
            e.compressed_size = static_cast<uint64_t>(info->compressed_size);
            e.crc32 = info->crc;
            e.is_directory = mz_zip_reader_entry_is_dir(handle_) == MZ_OK;
            e.is_encrypted = (info->aes_version != 0 || info->pk_verify != 0);
            if (info->modified_date) e.last_modified = from_time_t(info->modified_date);
            result.push_back(e);
            return Error::ok();
        });

        if (err) return Result<std::vector<ArchiveEntry>>{{}, err};
        return Result<std::vector<ArchiveEntry>>{result, Error::ok()};
    }

    Error extract(const std::string& output_dir) override {
        std::string out = output_dir.empty() ? opts_.output_dir : output_dir;
        if (out.empty()) out = fs::current_path().string();

        if (!open_handle()) {
            return Error::make(Error::IO_ERROR, "Failed to open ZIP for extraction");
        }

        return iterate([&](mz_zip_file* info) -> Error {
            std::string name = info->filename ? info->filename : "";
            if (name.empty()) return Error::ok();

            std::string safe_name = name;
            std::replace(safe_name.begin(), safe_name.end(), '\\', '/');

            std::string full_path = (fs::path(out) / safe_name).string();

            if (mz_zip_reader_entry_is_dir(handle_) == MZ_OK) {
                fs::create_directories(fs::path(full_path));
                return Error::ok();
            }

            fs::create_directories(fs::path(full_path).parent_path());

            if (opts_.overwrite == OverwriteMode::SKIP && fs::exists(fs::path(full_path))) {
                return Error::ok();
            }

            int32_t err = mz_zip_reader_entry_save_file(handle_, full_path.c_str());
            if (err != MZ_OK) {
                return Error::make(Error::IO_ERROR, "Failed to extract: " + name);
            }

            if (info->modified_date) {
                set_file_mtime(fs::path(full_path), info->modified_date);
            }

            return Error::ok();
        });
    }

    Error extract_entry(const std::string& archive_path, const std::string& output_path) override {
        if (!open_handle()) {
            return Error::make(Error::IO_ERROR, "Failed to open ZIP");
        }

        std::string target = archive_path;
        std::replace(target.begin(), target.end(), '\\', '/');

        int32_t err = mz_zip_reader_locate_entry(handle_, target.c_str(), 0);
        if (err != MZ_OK) {
            return Error::make(Error::FILE_NOT_FOUND, "Entry not found: " + archive_path);
        }

        err = mz_zip_reader_entry_open(handle_);
        if (err != MZ_OK) {
            return Error::make(Error::CORRUPTED_ARCHIVE);
        }

        std::string out_path = output_path.empty() ? target : output_path;
        fs::create_directories(fs::path(out_path).parent_path());

        err = mz_zip_reader_entry_save_file(handle_, out_path.c_str());
        mz_zip_reader_entry_close(handle_);

        if (err != MZ_OK) {
            return Error::make(Error::IO_ERROR, "Failed to extract entry");
        }
        return Error::ok();
    }

    Result<std::vector<char>> read_entry(const std::string& archive_path) override {
        if (!open_handle()) {
            return Result<std::vector<char>>{{}, Error::make(Error::IO_ERROR, "Failed to open ZIP")};
        }

        std::string target = archive_path;
        std::replace(target.begin(), target.end(), '\\', '/');

        int32_t err = mz_zip_reader_locate_entry(handle_, target.c_str(), 0);
        if (err != MZ_OK) {
            return Result<std::vector<char>>{{}, Error::make(Error::FILE_NOT_FOUND, "Entry not found")};
        }

        err = mz_zip_reader_entry_open(handle_);
        if (err != MZ_OK) {
            return Result<std::vector<char>>{{}, Error::make(Error::CORRUPTED_ARCHIVE)};
        }

        std::vector<char> data;
        char buf[BUFFER_SIZE];
        while (true) {
            int32_t read = mz_zip_reader_entry_read(handle_, buf, sizeof(buf));
            if (read <= 0) break;
            data.insert(data.end(), buf, buf + read);
        }
        mz_zip_reader_entry_close(handle_);
        return Result<std::vector<char>>{data, Error::ok()};
    }

    Error close() override {
        if (handle_) {
            mz_zip_reader_close(handle_);
            mz_zip_reader_delete(&handle_);
        }
        opened_ = false;
        return Error::ok();
    }

    ArchiveInfo info() const override { return info_; }

    bool is_open() const override { return opened_; }

private:
    bool open_handle() {
        if (opened_) return true;

        handle_ = mz_zip_reader_create();
        if (!handle_) return false;

        if (!opts_.password.empty()) {
            mz_zip_reader_set_password(handle_, opts_.password.c_str());
        }

        int32_t err = mz_zip_reader_open_file(handle_, path_.c_str());
        if (err != MZ_OK) {
            mz_zip_reader_delete(&handle_);
            return false;
        }

        opened_ = true;
        info_.path = path_;
        info_.format = Format::ZIP;
        info_.is_encrypted = !opts_.password.empty();
        info_.is_multipart = opts_.multipart_mode != MultipartMode::NONE;
        return true;
    }

    Error iterate(std::function<Error(mz_zip_file*)> fn) {
        if (!open_handle()) return Error::make(Error::IO_ERROR, "Cannot open archive");

        int32_t err = mz_zip_reader_goto_first_entry(handle_);
        while (err == MZ_OK) {
            mz_zip_file* info = nullptr;
            err = mz_zip_reader_entry_get_info(handle_, &info);
            if (err == MZ_OK && info) {
                Error e = fn(info);
                if (e) return e;
            }
            err = mz_zip_reader_goto_next_entry(handle_);
        }
        return Error::ok();
    }

    std::string path_;
    ArchiveOptions opts_;
    ArchiveInfo info_;
    void* handle_ = nullptr;
    bool opened_ = false;
};

// -----------------------------------------------------------------------
// ZIP Writer
// -----------------------------------------------------------------------
class ZipWriter : public ArchiveWriter {
public:
    ZipWriter(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~ZipWriter() override { close(); }

    Error add_file(const std::string& disk_path, const std::string& archive_path) override {
        if (!open_handle()) return Error::make(Error::IO_ERROR, "Cannot open ZIP for writing");

        fs::path src(disk_path);
        if (!fs::exists(src)) return Error::make(Error::FILE_NOT_FOUND, disk_path + " not found");
        if (fs::is_directory(src)) return add_directory(disk_path);

        std::string name = archive_path.empty() ? src.filename().string() : archive_path;
        std::replace(name.begin(), name.end(), '\\', '/');

        int32_t err = mz_zip_writer_add_file(handle_, disk_path.c_str(), name.c_str());
        if (err != MZ_OK) {
            return Error::make(Error::IO_ERROR, "Cannot add file: " + name);
        }
        return Error::ok();
    }

    Error add_directory(const std::string& disk_path) override {
        if (!open_handle()) return Error::make(Error::IO_ERROR, "Cannot open ZIP for writing");

        fs::path dir(disk_path);
        if (!fs::is_directory(dir)) return add_file(disk_path, "");

        std::string name = dir.filename().string() + "/";
        std::replace(name.begin(), name.end(), '\\', '/');

        mz_zip_file info{};
        info.filename = name.c_str();
        info.filename_size = static_cast<uint16_t>(name.size());
        info.uncompressed_size = 0;
        info.compression_method = MZ_COMPRESS_METHOD_STORE;
        info.flag = MZ_ZIP_FLAG_UTF8;
        info.external_fa = 0x10;

        int32_t err = mz_zip_writer_entry_open(handle_, &info);
        if (err == MZ_OK) {
            mz_zip_writer_entry_close(handle_);
        }

        for (auto& entry : fs::recursive_directory_iterator(dir)) {
            std::string ap = fs::relative(entry.path(), dir.parent_path()).string();
            std::replace(ap.begin(), ap.end(), '\\', '/');

            if (entry.is_directory()) {
                std::string d = ap + "/";
                mz_zip_file dinfo{};
                dinfo.filename = d.c_str();
                dinfo.filename_size = static_cast<uint16_t>(d.size());
                dinfo.uncompressed_size = 0;
                dinfo.compression_method = MZ_COMPRESS_METHOD_STORE;
                dinfo.flag = MZ_ZIP_FLAG_UTF8;
                dinfo.external_fa = 0x10;

                if (mz_zip_writer_entry_open(handle_, &dinfo) == MZ_OK) {
                    mz_zip_writer_entry_close(handle_);
                }
            } else {
                Error err2 = add_file(entry.path().string(), ap);
                if (err2) return err2;
            }
        }
        return Error::ok();
    }

    Error add_from_memory(const std::string& archive_path, const void* data, size_t size) override {
        if (!open_handle()) return Error::make(Error::IO_ERROR, "Cannot open ZIP for writing");

        std::string name = archive_path;
        std::replace(name.begin(), name.end(), '\\', '/');

        mz_zip_file info{};
        info.filename = name.c_str();
        info.filename_size = static_cast<uint16_t>(name.size());
        info.uncompressed_size = static_cast<int64_t>(size);
        info.compression_method = MZ_COMPRESS_METHOD_DEFLATE;
        info.flag = MZ_ZIP_FLAG_UTF8;

        if (opts_.compression_level >= 0) {
            mz_zip_writer_set_compress_level(handle_, static_cast<int16_t>(opts_.compression_level));
        }

        int32_t err = mz_zip_writer_add_buffer(handle_, data, static_cast<int32_t>(size), &info);
        if (err != MZ_OK) {
            return Error::make(Error::IO_ERROR, "Cannot add memory entry");
        }
        return Error::ok();
    }

    Error close() override {
        if (handle_) {
            mz_zip_writer_close(handle_);
            mz_zip_writer_delete(&handle_);
        }
        opened_ = false;
        return Error::ok();
    }

    ArchiveInfo info() const override { return info_; }

    bool is_open() const override { return opened_; }

private:
    bool open_handle() {
        if (opened_) return true;

        handle_ = mz_zip_writer_create();
        if (!handle_) return false;

        if (!opts_.password.empty()) {
            mz_zip_writer_set_password(handle_, opts_.password.c_str());
            mz_zip_writer_set_aes(handle_, 1);
        }

        if (opts_.compression_level >= 0) {
            mz_zip_writer_set_compress_level(handle_, static_cast<int16_t>(opts_.compression_level));
        }

        int64_t disk_size = 0;
        if (opts_.multipart_mode != MultipartMode::NONE && opts_.volume_size > 0) {
            disk_size = static_cast<int64_t>(opts_.volume_size);
        }

        int32_t err = mz_zip_writer_open_file(handle_, path_.c_str(), disk_size,
                                               opts_.append ? 1 : 0);
        if (err != MZ_OK) {
            mz_zip_writer_delete(&handle_);
            return false;
        }

        opened_ = true;
        info_.path = path_;
        info_.format = Format::ZIP;
        info_.is_encrypted = !opts_.password.empty();
        info_.is_multipart = opts_.multipart_mode != MultipartMode::NONE;
        return true;
    }

    std::string path_;
    ArchiveOptions opts_;
    ArchiveInfo info_;
    void* handle_ = nullptr;
    bool opened_ = false;
};

} // anonymous namespace

std::unique_ptr<ArchiveReader> create_zip_reader(const std::string& path,
                                                  const ArchiveOptions& opts) {
    return std::make_unique<ZipReader>(path, opts);
}

std::unique_ptr<ArchiveWriter> create_zip_writer(const std::string& path,
                                                  const ArchiveOptions& opts) {
    return std::make_unique<ZipWriter>(path, opts);
}

} // namespace compress
