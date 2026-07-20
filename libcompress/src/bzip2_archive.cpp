#include "compress/types.h"
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

#include <bzlib.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

#include "compress/detail/compat.h"

namespace fs = std::filesystem;

namespace compress {

namespace {

constexpr size_t BUFFER_SIZE = 65536;

// -----------------------------------------------------------------------
// BZIP2 Reader
// -----------------------------------------------------------------------
class Bzip2Reader : public ArchiveReader {
public:
    Bzip2Reader(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~Bzip2Reader() override { close(); }

    Result<std::vector<ArchiveEntry>> entries() override {
        std::vector<ArchiveEntry> result;
        ArchiveEntry e;
        e.path = fs::path(path_).filename().string();
        auto dot = e.path.rfind('.');
        if (dot != std::string::npos) e.path = e.path.substr(0, dot);
        if (fs::exists(path_)) e.size = fs::file_size(path_);
        result.push_back(e);
        return Result<std::vector<ArchiveEntry>>{result, Error::ok()};
    }

    Error extract(const std::string& output_dir) override {
        std::string out = output_dir.empty() ? opts_.output_dir : output_dir;
        if (out.empty()) out = fs::current_path().string();
        std::string name = out + "/" + fs::path(path_).stem().string();
        // Strip .bz2, .bz
        auto dot = name.rfind('.');
        if (dot != std::string::npos) {
            std::string ext = name.substr(dot);
            if (ext == ".bz2" || ext == ".bz") name = name.substr(0, dot);
        }
        return decompress(path_, name);
    }

    Error extract_entry(const std::string& archive_path, const std::string& output_path) override {
        std::string out = output_path.empty() ? fs::path(path_).stem().string() : output_path;
        return decompress(path_, out);
    }

    Result<std::vector<char>> read_entry(const std::string& archive_path) override {
        std::vector<char> data;
        FILE* fp = nullptr;
        if (fopen_s(&fp, path_.c_str(), "rb") != 0 || !fp)
            return Result<std::vector<char>>{{}, Error::make(Error::FILE_NOT_FOUND, path_)};

        BZFILE* bzf = BZ2_bzReadOpen(&bzerr_, fp, 0, 0, nullptr, 0);
        if (!bzf || bzerr_ != BZ_OK) {
            fclose(fp);
            return Result<std::vector<char>>{{}, Error::make(Error::CORRUPTED_ARCHIVE)};
        }

        char buf[BUFFER_SIZE];
        while (bzerr_ == BZ_OK) {
            int nread = BZ2_bzRead(&bzerr_, bzf, buf, sizeof(buf));
            if (nread > 0) data.insert(data.end(), buf, buf + nread);
        }
        BZ2_bzReadClose(&bzerr_, bzf);
        fclose(fp);
        return Result<std::vector<char>>{data, Error::ok()};
    }

    Error close() override { return Error::ok(); }

    ArchiveInfo info() const override {
        ArchiveInfo info;
        info.path = path_;
        info.format = Format::BZIP2;
        info.total_entries = 1;
        return info;
    }

    bool is_open() const override { return true; }

private:
    Error decompress(const std::string& src, const std::string& dst) {
        FILE* in = nullptr;
        if (fopen_s(&in, src.c_str(), "rb") != 0 || !in)
            return Error::make(Error::FILE_NOT_FOUND, src);

        fs::create_directories(fs::path(dst).parent_path());
        FILE* out = nullptr;
        if (fopen_s(&out, dst.c_str(), "wb") != 0 || !out) {
            fclose(in);
            return Error::make(Error::PERMISSION_DENIED, "Cannot write: " + dst);
        }

        int bzerr = BZ_OK;
        BZFILE* bzf = BZ2_bzReadOpen(&bzerr, in, 0, 0, nullptr, 0);
        if (!bzf || bzerr != BZ_OK) {
            fclose(in); fclose(out);
            return Error::make(Error::CORRUPTED_ARCHIVE);
        }

        char buf[BUFFER_SIZE];
        while (bzerr == BZ_OK) {
            int nread = BZ2_bzRead(&bzerr, bzf, buf, sizeof(buf));
            if (nread > 0) fwrite(buf, 1, static_cast<size_t>(nread), out);
        }
        BZ2_bzReadClose(&bzerr, bzf);
        fclose(in); fclose(out);
        return Error::ok();
    }

    std::string path_;
    ArchiveOptions opts_;
    int bzerr_ = BZ_OK;
};

// -----------------------------------------------------------------------
// BZIP2 Writer
// -----------------------------------------------------------------------
class Bzip2Writer : public ArchiveWriter {
public:
    Bzip2Writer(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~Bzip2Writer() override { close(); }

    Error add_file(const std::string& disk_path, const std::string& archive_path) override {
        if (!init_writer()) return Error::make(Error::IO_ERROR, "Cannot create BZ2");

        FILE* fp = nullptr;
        if (fopen_s(&fp, disk_path.c_str(), "rb") != 0 || !fp)
            return Error::make(Error::FILE_NOT_FOUND, disk_path);

        char buf[BUFFER_SIZE];
        size_t nread = 0;
        while ((nread = fread(buf, 1, sizeof(buf), fp)) > 0) {
            BZ2_bzWrite(&bzerr_, bzf_, buf, static_cast<int>(nread));
        }
        fclose(fp);
        return Error::ok();
    }

    Error add_directory(const std::string& disk_path) override {
        return add_file(disk_path, "");
    }

    Error add_from_memory(const std::string& archive_path, const void* data, size_t size) override {
        if (!init_writer()) return Error::make(Error::IO_ERROR, "Cannot create BZ2");
        BZ2_bzWrite(&bzerr_, bzf_, const_cast<void*>(data), static_cast<int>(size));
        return Error::ok();
    }

    Error close() override {
        if (bzf_) {
            BZ2_bzWriteClose(&bzerr_, bzf_, 0, nullptr, nullptr);
            bzf_ = nullptr;
        }
        if (fp_) {
            fclose(fp_);
            fp_ = nullptr;
        }
        opened_ = false;
        return Error::ok();
    }

    ArchiveInfo info() const override {
        ArchiveInfo info;
        info.path = path_;
        info.format = Format::BZIP2;
        return info;
    }

    bool is_open() const override { return opened_; }

private:
    bool init_writer() {
        if (opened_) return true;
        if (fopen_s(&fp_, path_.c_str(), "wb") != 0 || !fp_) return false;

        int level = opts_.compression_level >= 0
            ? std::clamp(opts_.compression_level, 1, 9)
            : 6;

        bzf_ = BZ2_bzWriteOpen(&bzerr_, fp_, level, 0, 30);
        if (!bzf_ || bzerr_ != BZ_OK) { fclose(fp_); fp_ = nullptr; return false; }
        opened_ = true;
        return true;
    }

    std::string path_;
    ArchiveOptions opts_;
    FILE* fp_ = nullptr;
    BZFILE* bzf_ = nullptr;
    int bzerr_ = BZ_OK;
    bool opened_ = false;
};

} // anonymous namespace

std::unique_ptr<ArchiveReader> create_bzip2_reader(const std::string& path,
                                                    const ArchiveOptions& opts) {
    return std::make_unique<Bzip2Reader>(path, opts);
}

std::unique_ptr<ArchiveWriter> create_bzip2_writer(const std::string& path,
                                                    const ArchiveOptions& opts) {
    return std::make_unique<Bzip2Writer>(path, opts);
}

} // namespace compress
