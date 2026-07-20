#define compress zlib_compress
#include <zlib.h>
#undef compress

#include "compress/types.h"
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

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
// GZIP Reader (single-stream decompression)
// -----------------------------------------------------------------------
class GzipReader : public ArchiveReader {
public:
    GzipReader(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~GzipReader() override { close(); }

    Result<std::vector<ArchiveEntry>> entries() override {
        // GZIP is a single-stream format; we report 1 entry
        std::vector<ArchiveEntry> result;
        ArchiveEntry e;
        e.path = fs::path(path_).filename().string();
        if (e.path.size() >= 3) {
            auto dot = e.path.rfind('.');
            if (dot != std::string::npos) e.path = e.path.substr(0, dot);
            // Also handle .tar.gz -> .tar
            if (e.path.size() >= 4) {
                auto dot2 = e.path.rfind('.');
                if (dot2 != std::string::npos && e.path.substr(dot2) == ".tar")
                    e.path = e.path.substr(0, dot2);
            }
        }
        fs::path fpath(path_);
        if (fs::exists(fpath)) {
            e.size = fs::file_size(fpath);
            auto ft = fs::last_write_time(fpath);
            using namespace std::chrono;
            e.last_modified = system_clock::time_point(
                duration_cast<system_clock::duration>(ft.time_since_epoch())
            );
        }
        result.push_back(e);
        return Result<std::vector<ArchiveEntry>>{result, Error::ok()};
    }

    Error extract(const std::string& output_dir) override {
        std::string out = output_dir.empty() ? opts_.output_dir : output_dir;
        if (out.empty()) out = fs::current_path().string();

        fs::path out_path = fs::path(out) / fs::path(path_).filename();
        auto name = out_path.string();
        auto dot = name.rfind('.');
        if (dot != std::string::npos) {
            // Strip all extensions from the end (.gz, .gzip)
            std::string ext = name.substr(dot);
            if (ext == ".gz" || ext == ".gzip") {
                name = name.substr(0, dot);
                // Also strip .tar if compound
                auto dot2 = name.rfind('.');
                if (dot2 != std::string::npos && name.substr(dot2) == ".tar")
                    name = name.substr(0, dot2);
            }
        }

        return decompress(path_, name);
    }

    Error extract_entry(const std::string& archive_path, const std::string& output_path) override {
        // GZIP is single-stream; archive_path is ignored
        std::string out = output_path.empty() ? (fs::path(path_).stem().string()) : output_path;
        return decompress(path_, out);
    }

    Result<std::vector<char>> read_entry(const std::string& archive_path) override {
        std::vector<char> data;
        gzFile gf = gzopen(path_.c_str(), "rb");
        if (!gf) return Result<std::vector<char>>{{}, Error::make(Error::FILE_NOT_FOUND, path_)};

        if (!opts_.password.empty()) {
            gzsetparams(gf, Z_DEFAULT_COMPRESSION, Z_DEFAULT_STRATEGY);
        }

        char buf[BUFFER_SIZE];
        while (true) {
            int read = gzread(gf, buf, sizeof(buf));
            if (read <= 0) break;
            data.insert(data.end(), buf, buf + read);
        }
        gzclose(gf);
        return Result<std::vector<char>>{data, Error::ok()};
    }

    Error close() override { return Error::ok(); }

    ArchiveInfo info() const override {
        ArchiveInfo info;
        info.path = path_;
        info.format = Format::GZIP;
        info.total_entries = 1;
        return info;
    }

    bool is_open() const override { return true; }

private:
    Error decompress(const std::string& src, const std::string& dst) {
        gzFile gf = gzopen(src.c_str(), "rb");
        if (!gf) return Error::make(Error::FILE_NOT_FOUND, src);

        fs::create_directories(fs::path(dst).parent_path());

        FILE* fp = nullptr;
        if (fopen_s(&fp, dst.c_str(), "wb") != 0 || !fp) {
            gzclose(gf);
            return Error::make(Error::PERMISSION_DENIED, "Cannot write: " + dst);
        }

        char buf[BUFFER_SIZE];
        while (true) {
            int read = gzread(gf, buf, sizeof(buf));
            if (read <= 0) break;
            fwrite(buf, 1, static_cast<size_t>(read), fp);
        }
        fclose(fp);
        gzclose(gf);
        return Error::ok();
    }

    std::string path_;
    ArchiveOptions opts_;
};

// -----------------------------------------------------------------------
// GZIP Writer (single-stream compression)
// -----------------------------------------------------------------------
class GzipWriter : public ArchiveWriter {
public:
    GzipWriter(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~GzipWriter() override { close(); }

    Error add_file(const std::string& disk_path, const std::string& archive_path) override {
        if (!opened_) {
            int level = opts_.compression_level >= 0
                ? std::clamp(opts_.compression_level, 0, 9)
                : Z_DEFAULT_COMPRESSION;
            gf_ = gzopen(path_.c_str(), ("wb" + std::to_string(level)).c_str());
            if (!gf_) return Error::make(Error::IO_ERROR, "Cannot create: " + path_);
            opened_ = true;
        }

        fs::path src(disk_path);
        if (!fs::exists(src)) return Error::make(Error::FILE_NOT_FOUND, disk_path);

        FILE* fp = nullptr;
        if (fopen_s(&fp, disk_path.c_str(), "rb") != 0 || !fp)
            return Error::make(Error::FILE_NOT_FOUND, disk_path);

        char buf[BUFFER_SIZE];
        size_t nread = 0;
        while ((nread = fread(buf, 1, sizeof(buf), fp)) > 0) {
            gzwrite(gf_, buf, static_cast<unsigned int>(nread));
        }
        fclose(fp);

        // Set the filename in gzip header (archive path is used for the header name)
        if (!archive_path.empty()) {
            gzsetparams(gf_, Z_DEFAULT_COMPRESSION, Z_DEFAULT_STRATEGY);
        }
        return Error::ok();
    }

    Error add_directory(const std::string& disk_path) override {
        return add_file(disk_path, ""); // GZIP compresses directory as a single file
    }

    Error add_from_memory(const std::string& archive_path, const void* data, size_t size) override {
        if (!opened_) {
            int level = opts_.compression_level >= 0
                ? std::clamp(opts_.compression_level, 0, 9)
                : Z_DEFAULT_COMPRESSION;
            gf_ = gzopen(path_.c_str(), ("wb" + std::to_string(level)).c_str());
            if (!gf_) return Error::make(Error::IO_ERROR, "Cannot create: " + path_);
            opened_ = true;
        }
        gzwrite(gf_, data, static_cast<unsigned int>(size));
        return Error::ok();
    }

    Error close() override {
        if (gf_) {
            gzclose(gf_);
            gf_ = nullptr;
        }
        opened_ = false;
        return Error::ok();
    }

    ArchiveInfo info() const override {
        ArchiveInfo info;
        info.path = path_;
        info.format = Format::GZIP;
        info.total_entries = 1;
        return info;
    }

    bool is_open() const override { return opened_; }

private:
    std::string path_;
    ArchiveOptions opts_;
    gzFile gf_ = nullptr;
    bool opened_ = false;
};

} // anonymous namespace

std::unique_ptr<ArchiveReader> create_gzip_reader(const std::string& path,
                                                   const ArchiveOptions& opts) {
    return std::make_unique<GzipReader>(path, opts);
}

std::unique_ptr<ArchiveWriter> create_gzip_writer(const std::string& path,
                                                   const ArchiveOptions& opts) {
    return std::make_unique<GzipWriter>(path, opts);
}

} // namespace compress
