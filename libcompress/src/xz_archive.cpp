#include "compress/types.h"
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

#include <lzma.h>

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
// XZ Reader
// -----------------------------------------------------------------------
class XzReader : public ArchiveReader {
public:
    XzReader(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~XzReader() override { close(); }

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
        auto dot = name.rfind('.');
        if (dot != std::string::npos) {
            if (name.substr(dot) == ".xz" || name.substr(dot) == ".lzma")
                name = name.substr(0, dot);
        }
        return decompress(path_, name);
    }

    Error extract_entry(const std::string& archive_path, const std::string& output_path) override {
        std::string out = output_path.empty() ? fs::path(path_).stem().string() : output_path;
        return decompress(path_, out);
    }

    Result<std::vector<char>> read_entry(const std::string& archive_path) override {
        std::vector<char> data;
        lzma_stream strm = LZMA_STREAM_INIT;
        if (lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK)
            return Result<std::vector<char>>{{}, Error::make(Error::UNSUPPORTED_FORMAT)};

        FILE* fp = nullptr;
        if (fopen_s(&fp, path_.c_str(), "rb") != 0 || !fp) {
            lzma_end(&strm);
            return Result<std::vector<char>>{{}, Error::make(Error::FILE_NOT_FOUND, path_)};
        }

        char inbuf[BUFFER_SIZE];
        char outbuf[BUFFER_SIZE];

        lzma_action action = LZMA_RUN;
        while (true) {
            size_t nread = fread(inbuf, 1, sizeof(inbuf), fp);
            if (nread < sizeof(inbuf)) action = LZMA_FINISH;
            strm.next_in = reinterpret_cast<const uint8_t*>(inbuf);
            strm.avail_in = nread;

            lzma_ret ret;
            do {
                strm.next_out = reinterpret_cast<uint8_t*>(outbuf);
                strm.avail_out = sizeof(outbuf);
                ret = lzma_code(&strm, action);
                size_t written = sizeof(outbuf) - strm.avail_out;
                if (written > 0)
                    data.insert(data.end(), outbuf, outbuf + written);
            } while (strm.avail_out == 0);

            if (ret != LZMA_OK && ret != LZMA_STREAM_END) break;
            if (ret == LZMA_STREAM_END || (action == LZMA_FINISH && nread == 0)) break;
        }

        lzma_end(&strm);
        fclose(fp);
        return Result<std::vector<char>>{data, Error::ok()};
    }

    Error close() override { return Error::ok(); }

    ArchiveInfo info() const override {
        ArchiveInfo info;
        info.path = path_;
        info.format = Format::XZ;
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

        lzma_stream strm = LZMA_STREAM_INIT;
        if (lzma_stream_decoder(&strm, UINT64_MAX, LZMA_CONCATENATED) != LZMA_OK) {
            fclose(in); fclose(out);
            return Error::make(Error::UNSUPPORTED_FORMAT);
        }

        char inbuf[BUFFER_SIZE];
        char outbuf[BUFFER_SIZE];
        lzma_action action = LZMA_RUN;
        lzma_ret ret;

        while (true) {
            size_t nread = fread(inbuf, 1, sizeof(inbuf), in);
            if (nread < sizeof(inbuf)) action = LZMA_FINISH;
            strm.next_in = reinterpret_cast<const uint8_t*>(inbuf);
            strm.avail_in = nread;

            do {
                strm.next_out = reinterpret_cast<uint8_t*>(outbuf);
                strm.avail_out = sizeof(outbuf);
                ret = lzma_code(&strm, action);
                size_t written = sizeof(outbuf) - strm.avail_out;
                if (written > 0) fwrite(outbuf, 1, written, out);
            } while (strm.avail_out == 0);

            if (ret == LZMA_STREAM_END) break;
            if (ret != LZMA_OK && ret != LZMA_STREAM_END) break;
            if (action == LZMA_FINISH && nread == 0) break;
        }

        lzma_end(&strm);
        fclose(in); fclose(out);
        return ret == LZMA_OK || ret == LZMA_STREAM_END
            ? Error::ok()
            : Error::make(Error::CORRUPTED_ARCHIVE);
    }

    std::string path_;
    ArchiveOptions opts_;
};

// -----------------------------------------------------------------------
// XZ Writer
// -----------------------------------------------------------------------
class XzWriter : public ArchiveWriter {
public:
    XzWriter(const std::string& path, const ArchiveOptions& opts)
        : path_(path), opts_(opts) {}

    ~XzWriter() override { close(); }

    Error add_file(const std::string& disk_path, const std::string& archive_path) override {
        FILE* fp = nullptr;
        if (fopen_s(&fp, disk_path.c_str(), "rb") != 0 || !fp)
            return Error::make(Error::FILE_NOT_FOUND, disk_path);

        std::vector<char> file_data;
        char fbuf[BUFFER_SIZE];
        size_t nread = 0;
        while ((nread = fread(fbuf, 1, sizeof(fbuf), fp)) > 0)
            file_data.insert(file_data.end(), fbuf, fbuf + nread);
        fclose(fp);

        return add_from_memory(archive_path, file_data.data(), file_data.size());
    }

    Error add_directory(const std::string& disk_path) override {
        return add_file(disk_path, "");
    }

    Error add_from_memory(const std::string& archive_path, const void* data, size_t size) override {
        if (!init_writer()) return Error::make(Error::IO_ERROR, "Cannot create XZ");

        lzma_action action = (size == 0) ? LZMA_FINISH : LZMA_RUN;
        strm_.next_in = static_cast<const uint8_t*>(data);
        strm_.avail_in = size;

        uint8_t outbuf[BUFFER_SIZE];
        lzma_ret ret;

        do {
            strm_.next_out = outbuf;
            strm_.avail_out = sizeof(outbuf);
            ret = lzma_code(&strm_, action);
            size_t written = sizeof(outbuf) - strm_.avail_out;
            if (written > 0)
                fwrite(outbuf, 1, written, fp_);
        } while (strm_.avail_out == 0);

        if (ret != LZMA_OK && ret != LZMA_STREAM_END)
            return Error::make(Error::IO_ERROR, "XZ compression error");
        return Error::ok();
    }

    Error close() override {
        if (fp_) {
            // Flush remaining data with LZMA_FINISH
            uint8_t outbuf[BUFFER_SIZE];
            lzma_ret ret;
            do {
                strm_.next_out = outbuf;
                strm_.avail_out = sizeof(outbuf);
                ret = lzma_code(&strm_, LZMA_FINISH);
                size_t written = sizeof(outbuf) - strm_.avail_out;
                if (written > 0)
                    fwrite(outbuf, 1, written, fp_);
            } while (ret == LZMA_OK);

            lzma_end(&strm_);
            strm_ = LZMA_STREAM_INIT;
            fclose(fp_);
            fp_ = nullptr;
        }
        initialized_ = false;
        return Error::ok();
    }

    ArchiveInfo info() const override {
        ArchiveInfo info;
        info.path = path_;
        info.format = Format::XZ;
        return info;
    }

    bool is_open() const override { return initialized_; }

private:
    bool init_writer() {
        if (initialized_) return true;
        if (fopen_s(&fp_, path_.c_str(), "wb") != 0 || !fp_) return false;

        int level = opts_.compression_level >= 0
            ? std::clamp(opts_.compression_level, 0, 9)
            : 6;

        strm_ = LZMA_STREAM_INIT;
        if (lzma_easy_encoder(&strm_, static_cast<uint32_t>(level), LZMA_CHECK_CRC64) != LZMA_OK) {
            fclose(fp_); fp_ = nullptr;
            return false;
        }

        initialized_ = true;
        return true;
    }

    std::string path_;
    ArchiveOptions opts_;
    FILE* fp_ = nullptr;
    lzma_stream strm_ = LZMA_STREAM_INIT;
    bool initialized_ = false;
};

} // anonymous namespace

std::unique_ptr<ArchiveReader> create_xz_reader(const std::string& path,
                                                 const ArchiveOptions& opts) {
    return std::make_unique<XzReader>(path, opts);
}

std::unique_ptr<ArchiveWriter> create_xz_writer(const std::string& path,
                                                 const ArchiveOptions& opts) {
    return std::make_unique<XzWriter>(path, opts);
}

} // namespace compress
