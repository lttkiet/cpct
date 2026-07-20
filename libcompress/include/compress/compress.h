#pragma once

#include "types.h"
#include "archive_reader.h"
#include "archive_writer.h"

#include <string>

namespace compress {

// Format detection (public API)
Format detect_format_from_extension(const std::string& path);
Format detect_format(const std::string& path);
const char* format_name(Format fmt);

// High-level operations
Error compress_file(const std::string& source_path,
                    const std::string& archive_path,
                    const ArchiveOptions& options = {});

Error compress_files(const std::vector<std::string>& source_paths,
                     const std::string& archive_path,
                     const ArchiveOptions& options = {});

Error extract_archive(const std::string& archive_path,
                      const ArchiveOptions& options = {});

Result<ArchiveInfo> inspect_archive(const std::string& archive_path,
                                     const ArchiveOptions& options = {});

} // namespace compress
