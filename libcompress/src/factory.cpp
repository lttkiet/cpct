#include "compress/types.h"
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

#include <memory>
#include <string>

// Forward declarations of backend constructors
namespace compress {
    extern std::unique_ptr<ArchiveReader> create_zip_reader(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveWriter> create_zip_writer(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveReader> create_tar_reader(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveWriter> create_tar_writer(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveReader> create_gzip_reader(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveWriter> create_gzip_writer(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveReader> create_bzip2_reader(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveWriter> create_bzip2_writer(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveReader> create_xz_reader(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveWriter> create_xz_writer(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveReader> create_zstd_reader(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveWriter> create_zstd_writer(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveReader> create_sevenz_reader(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveWriter> create_sevenz_writer(const std::string&, const ArchiveOptions&);
    extern std::unique_ptr<ArchiveReader> create_rar_reader(const std::string&, const ArchiveOptions&);
}

namespace compress {

// -----------------------------------------------------------------------
// Internal: detect format and resolve
// -----------------------------------------------------------------------
Format detect_format(const std::string& path);
Format detect_format_from_extension(const std::string& path);
const char* format_name(Format fmt);

namespace {

Format resolve_format(const std::string& path, Format hint) {
    if (hint != Format::AUTO) return hint;
    Format detected = detect_format(path);
    if (detected != Format::AUTO) return detected;
    // Last resort: try extension-based detection
    return detect_format_from_extension(path);
}

struct ReaderFactoryEntry {
    Format fmt;
    std::unique_ptr<ArchiveReader> (*create)(const std::string&, const ArchiveOptions&);
};

struct WriterFactoryEntry {
    Format fmt;
    std::unique_ptr<ArchiveWriter> (*create)(const std::string&, const ArchiveOptions&);
};

} // anonymous namespace

// -----------------------------------------------------------------------
// create_reader
// -----------------------------------------------------------------------
std::unique_ptr<ArchiveReader> create_reader(const std::string& archive_path,
                                              const ArchiveOptions& options) {
    Format fmt = resolve_format(archive_path, options.format);
    if (fmt == Format::AUTO) return nullptr;

    static const ReaderFactoryEntry readers[] = {
        { Format::ZIP,       create_zip_reader },
        { Format::TAR,       create_tar_reader },
        { Format::GZIP,      create_gzip_reader },
        { Format::BZIP2,     create_bzip2_reader },
        { Format::XZ,        create_xz_reader },
        { Format::ZSTD,      create_zstd_reader },
        { Format::SEVEN_ZIP, create_sevenz_reader },
        { Format::RAR,       create_rar_reader },
    };

    for (auto& entry : readers) {
        if (entry.fmt == fmt) return entry.create(archive_path, options);
    }
    return nullptr;
}

// -----------------------------------------------------------------------
// create_writer
// -----------------------------------------------------------------------
std::unique_ptr<ArchiveWriter> create_writer(const std::string& archive_path,
                                              const ArchiveOptions& options) {
    Format fmt = resolve_format(archive_path, options.format);
    if (fmt == Format::AUTO) return nullptr;

    static const WriterFactoryEntry writers[] = {
        { Format::ZIP,       create_zip_writer },
        { Format::TAR,       create_tar_writer },
        { Format::GZIP,      create_gzip_writer },
        { Format::BZIP2,     create_bzip2_writer },
        { Format::XZ,        create_xz_writer },
        { Format::ZSTD,      create_zstd_writer },
        { Format::SEVEN_ZIP, create_sevenz_writer },
        // RAR: no writer (proprietary)
    };

    for (auto& entry : writers) {
        if (entry.fmt == fmt) return entry.create(archive_path, options);
    }
    return nullptr;
}

// -----------------------------------------------------------------------
// High-level convenience functions
// -----------------------------------------------------------------------
Error compress_file(const std::string& source_path,
                    const std::string& archive_path,
                    const ArchiveOptions& options) {
    auto writer = create_writer(archive_path, options);
    if (!writer) return Error::make(Error::UNSUPPORTED_FORMAT, "Cannot determine format from: " + archive_path);

    Error err = writer->add_file(source_path);
    if (err) { writer->close(); return err; }
    return writer->close();
}

Error compress_files(const std::vector<std::string>& source_paths,
                     const std::string& archive_path,
                     const ArchiveOptions& options) {
    auto writer = create_writer(archive_path, options);
    if (!writer) return Error::make(Error::UNSUPPORTED_FORMAT, "Cannot determine format from: " + archive_path);

    for (auto& src : source_paths) {
        Error err = writer->add_file(src);
        if (err) { writer->close(); return err; }
    }
    return writer->close();
}

Error extract_archive(const std::string& archive_path,
                      const ArchiveOptions& options) {
    auto reader = create_reader(archive_path, options);
    if (!reader) return Error::make(Error::UNSUPPORTED_FORMAT, "Cannot determine format from: " + archive_path);

    Error err = reader->extract(options.output_dir);
    reader->close();
    return err;
}

Result<ArchiveInfo> inspect_archive(const std::string& archive_path,
                                    const ArchiveOptions& options) {
    auto reader = create_reader(archive_path, options);
    if (!reader) {
        ArchiveInfo info;
        info.path = archive_path;
        return Result<ArchiveInfo>{info, Error::make(Error::UNSUPPORTED_FORMAT, "Cannot determine format")};
    }

    auto entries_result = reader->entries();
    reader->close();

    if (!entries_result) {
        return Result<ArchiveInfo>{reader->info(), entries_result.error};
    }

    // Aggregate info
    auto info = reader->info();
    info.total_entries = entries_result->size();
    info.total_size = 0;
    for (auto& e : *entries_result) info.total_size += e.size;
    return Result<ArchiveInfo>{info, Error::ok()};
}

} // namespace compress
