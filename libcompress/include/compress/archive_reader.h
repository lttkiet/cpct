#pragma once

#include "types.h"
#include <memory>
#include <string>
#include <vector>

namespace compress {

class ArchiveReader {
public:
    virtual ~ArchiveReader() = default;

    virtual Result<std::vector<ArchiveEntry>> entries() = 0;

    virtual Error extract(const std::string& output_dir = "") = 0;

    virtual Error extract_entry(const std::string& archive_path,
                                const std::string& output_path = "") = 0;

    virtual Result<std::vector<char>> read_entry(const std::string& archive_path) = 0;

    virtual Error close() = 0;

    virtual ArchiveInfo info() const = 0;

    virtual bool is_open() const = 0;
};

std::unique_ptr<ArchiveReader> create_reader(const std::string& archive_path,
                                             const ArchiveOptions& options = {});

} // namespace compress
