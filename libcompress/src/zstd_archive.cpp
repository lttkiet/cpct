#include "compress/types.h"
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

#include <zstd.h>
#include <zstd_errors.h>

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
// ZSTD Reader
// -----------------------------------------------------------------------
class ZstdReader : public ArchiveReader {
public:
    ZstdReader(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~ZstdReader() override { close(); }

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
        // Strip .zst, .zstd
        auto dot = name.rfind('.');
        if (dot != std::string::npos) {
            std::string ext = name.substr(dot);
            if (ext == ".zst" || ext == ".zstd") name = name.substr(0, dot);
        }
        return decompress(path_, name);
    }

    Error extract_entry(const std::string& archive_path, const std::string& output_path) override {
        std::string out = output_path.empty() ? fs::path(path_).stem().string() : output_path;
        return decompress(path_, out);
    }

    Result<std::vector<char>> read_entry(const std::string& archive_path) override {
        FILE* fp = nullptr;
        if (fopen_s(&fp, path_.c_str(), "rb") != 0 || !fp)
            return Result<std::vector<char>>{{}, Error::make(Error::FILE_NOT_FOUND, path_)};

        auto dctx = ZSTD_createDCtx();
        std::vector<char> data;
        char inbuf[BUFFER_SIZE];
        char outbuf[BUFFER_SIZE];

        ZSTD_outBuffer output = {outbuf, sizeof(outbuf), 0};
        ZSTD_inBuffer input = {nullptr, 0, 0};

        while (true) {
            input.src = inbuf;
            input.size = fread(inbuf, 1, sizeof(inbuf), fp);
            input.pos = 0;

            if (input.size == 0) break;

            while (input.pos < input.size) {
                output.pos = 0;
                size_t ret = ZSTD_decompressStream(dctx, &output, &input);
                if (ZSTD_isError(ret)) {
                    ZSTD_freeDCtx(dctx);
                    fclose(fp);
                    return Result<std::vector<char>>{{}, Error::make(Error::CORRUPTED_ARCHIVE,
                                                                      ZSTD_getErrorName(ret))};
                }
                if (output.pos > 0)
                    data.insert(data.end(), outbuf, outbuf + output.pos);
                if (ret == 0) break;
            }
        }

        ZSTD_freeDCtx(dctx);
        fclose(fp);
        return Result<std::vector<char>>{data, Error::ok()};
    }

    Error close() override { return Error::ok(); }

    ArchiveInfo info() const override {
        ArchiveInfo info;
        info.path = path_;
        info.format = Format::ZSTD;
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

        auto dctx = ZSTD_createDCtx();
        ZSTD_inBuffer input = {nullptr, 0, 0};
        ZSTD_outBuffer output = {nullptr, 0, 0};

        char inbuf[BUFFER_SIZE];
        char outbuf[BUFFER_SIZE];
        output.dst = outbuf;
        output.size = sizeof(outbuf);

        while (true) {
            input.src = inbuf;
            input.size = fread(inbuf, 1, sizeof(inbuf), in);
            input.pos = 0;

            if (input.size == 0) break;

            while (input.pos < input.size) {
                output.pos = 0;
                size_t ret = ZSTD_decompressStream(dctx, &output, &input);
                if (ZSTD_isError(ret)) {
                    ZSTD_freeDCtx(dctx);
                    fclose(in); fclose(out);
                    return Error::make(Error::CORRUPTED_ARCHIVE, ZSTD_getErrorName(ret));
                }
                if (output.pos > 0)
                    fwrite(outbuf, 1, output.pos, out);
                if (ret == 0) break;
            }
        }

        ZSTD_freeDCtx(dctx);
        fclose(in); fclose(out);
        return Error::ok();
    }

    std::string path_;
    ArchiveOptions opts_;
};

// -----------------------------------------------------------------------
// ZSTD Writer
// -----------------------------------------------------------------------
class ZstdWriter : public ArchiveWriter {
public:
    ZstdWriter(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~ZstdWriter() override { close(); }

    Error add_file(const std::string& disk_path, const std::string& archive_path) override {
        if (!init_writer()) return Error::make(Error::IO_ERROR, "Cannot create ZSTD");

        FILE* fp = nullptr;
        if (fopen_s(&fp, disk_path.c_str(), "rb") != 0 || !fp)
            return Error::make(Error::FILE_NOT_FOUND, disk_path);

        char buf[BUFFER_SIZE];
        size_t nread = 0;
        while ((nread = fread(buf, 1, sizeof(buf), fp)) > 0) {
            ZSTD_inBuffer input = {buf, nread, 0};
            while (input.pos < input.size) {
                output_.pos = 0;
                size_t ret = ZSTD_compressStream(cctx_, &output_, &input);
                if (ZSTD_isError(ret)) break;
                if (output_.pos > 0)
                    fwrite(output_.dst, 1, output_.pos, fp_out_);
            }
        }
        fclose(fp);
        return Error::ok();
    }

    Error add_directory(const std::string& disk_path) override {
        return add_file(disk_path, "");
    }

    Error add_from_memory(const std::string& archive_path, const void* data, size_t size) override {
        if (!init_writer()) return Error::make(Error::IO_ERROR, "Cannot create ZSTD");

        ZSTD_inBuffer input = {data, size, 0};
        while (input.pos < input.size) {
            output_.pos = 0;
            size_t ret = ZSTD_compressStream(cctx_, &output_, &input);
            if (ZSTD_isError(ret)) break;
            if (output_.pos > 0)
                fwrite(output_.dst, 1, output_.pos, fp_out_);
        }
        return Error::ok();
    }

    Error close() override {
        if (cctx_) {
            output_.pos = 0;
            size_t ret = ZSTD_endStream(cctx_, &output_);
            if (output_.pos > 0)
                fwrite(output_.dst, 1, output_.pos, fp_out_);
            ZSTD_freeCCtx(cctx_);
            cctx_ = nullptr;
        }
        if (fp_out_) { fclose(fp_out_); fp_out_ = nullptr; }
        initialized_ = false;
        return Error::ok();
    }

    ArchiveInfo info() const override {
        ArchiveInfo info;
        info.path = path_;
        info.format = Format::ZSTD;
        return info;
    }

    bool is_open() const override { return initialized_; }

private:
    bool init_writer() {
        if (initialized_) return true;
        if (fopen_s(&fp_out_, path_.c_str(), "wb") != 0 || !fp_out_) return false;

        cctx_ = ZSTD_createCCtx();
        if (!cctx_) { fclose(fp_out_); fp_out_ = nullptr; return false; }

        int level = opts_.compression_level >= 0
            ? std::clamp(opts_.compression_level, 0, ZSTD_maxCLevel())
            : ZSTD_defaultCLevel();
        ZSTD_CCtx_setParameter(cctx_, ZSTD_c_compressionLevel, level);

        outbuf_.resize(ZSTD_CStreamOutSize());
        output_.dst = outbuf_.data();
        output_.size = outbuf_.size();
        output_.pos = 0;

        initialized_ = true;
        return true;
    }

    std::string path_;
    ArchiveOptions opts_;
    FILE* fp_out_ = nullptr;
    ZSTD_CCtx* cctx_ = nullptr;
    ZSTD_outBuffer output_{};
    std::vector<char> outbuf_;
    bool initialized_ = false;
};

} // anonymous namespace

std::unique_ptr<ArchiveReader> create_zstd_reader(const std::string& path,
                                                   const ArchiveOptions& opts) {
    return std::make_unique<ZstdReader>(path, opts);
}

std::unique_ptr<ArchiveWriter> create_zstd_writer(const std::string& path,
                                                   const ArchiveOptions& opts) {
    return std::make_unique<ZstdWriter>(path, opts);
}

} // namespace compress
