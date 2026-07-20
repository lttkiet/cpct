#include "compress/compress.h"
#include "compress/types.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace fs = std::filesystem;

void create_sample_file(const std::string& path, const std::string& content) {
    std::ofstream(path) << content;
}

int main() {
    std::string tmp = fs::temp_directory_path().string() + "/cpct_example";
    fs::create_directories(tmp);

    // Create a sample file
    std::string test_file = tmp + "/sample.txt";
    create_sample_file(test_file, "Hello, cpct - Cross-Platform Compressed Tool!\nThis is a test.\n");

    // Compress to ZIP
    {
        std::string archive = tmp + "/output.zip";
        auto err = compress::compress_file(test_file, archive);
        if (err) {
            std::cerr << "Error creating ZIP: " << err.message << "\n";
            return 1;
        }
        std::cout << "Created: " << archive << "\n";
    }

    // Compress to TAR.GZ
    {
        std::string archive = tmp + "/output.tar.gz";
        auto err = compress::compress_file(test_file, archive);
        if (err) {
            std::cerr << "Error creating TAR.GZ: " << err.message << "\n";
            return 1;
        }
        std::cout << "Created: " << archive << "\n";
    }

    // List archive contents
    {
        std::string archive = tmp + "/output.zip";
        auto result = compress::inspect_archive(archive);
        if (result) {
            std::cout << "Archive: " << result->path << "\n";
            std::cout << "Format entries: " << result->total_entries << "\n";
            std::cout << "Total size: " << result->total_size << " bytes\n";
        }
    }

    // Extract archive
    {
        std::string archive = tmp + "/output.zip";
        std::string extract_dir = tmp + "/extracted";
        compress::ArchiveOptions opts;
        opts.output_dir = extract_dir;
        auto err = compress::extract_archive(archive, opts);
        if (err) {
            std::cerr << "Error extracting: " << err.message << "\n";
            return 1;
        }
        std::cout << "Extracted to: " << extract_dir << "\n";
    }

    // Password-protected ZIP
    {
        std::string archive = tmp + "/encrypted.zip";
        compress::ArchiveOptions opts;
        opts.password = "my-secret-password";
        opts.encryption = compress::EncryptionMethod::AES_256;
        auto err = compress::compress_file(test_file, archive, opts);
        if (err) {
            std::cerr << "Error creating encrypted ZIP: " << err.message << "\n";
            return 1;
        }
        std::cout << "Created encrypted: " << archive << "\n";
    }

    // Multipart archive
    {
        std::string archive = tmp + "/multipart.zip";
        compress::ArchiveOptions opts;
        opts.multipart_mode = compress::MultipartMode::SPLIT;
        opts.volume_size = 10 * 1024; // 10KB volumes
        auto err = compress::compress_file(test_file, archive, opts);
        if (err) {
            std::cerr << "Error creating multipart: " << err.message << "\n";
        } else {
            std::cout << "Created multipart: " << archive << "\n";
        }
    }

    std::cout << "\nAll examples completed successfully.\n";
    return 0;
}
