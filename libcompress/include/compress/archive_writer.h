#pragma once

#include "types.h"
#include <memory>
#include <string>

namespace compress {

class ArchiveWriter {
public:
    virtual ~ArchiveWriter() = default;

    virtual Error add_file(const std::string& disk_path,
                           const std::string& archive_path = "") = 0;

    virtual Error add_directory(const std::string& disk_path) = 0;

    virtual Error add_from_memory(const std::string& archive_path,
                                  const void* data, size_t size) = 0;

    virtual Error close() = 0;

    virtual ArchiveInfo info() const = 0;

    virtual bool is_open() const = 0;
};

std::unique_ptr<ArchiveWriter> create_writer(const std::string& archive_path,
                                             const ArchiveOptions& options = {});

} // namespace compress
