#include "compress/compress.h"
#include "compress/types.h"

#include <cxxopts.hpp>
#include <fmt/color.h>
#include <fmt/format.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

// -----------------------------------------------------------------------
// Format helpers
// -----------------------------------------------------------------------
const char* format_desc(compress::Format fmt) {
    switch (fmt) {
        case compress::Format::ZIP:       return "ZIP archive";
        case compress::Format::TAR:       return "TAR archive";
        case compress::Format::GZIP:      return "GZIP compressed";
        case compress::Format::BZIP2:     return "BZIP2 compressed";
        case compress::Format::XZ:        return "XZ compressed";
        case compress::Format::ZSTD:      return "Zstandard compressed";
        case compress::Format::SEVEN_ZIP:  return "7z archive";
        case compress::Format::RAR:        return "RAR archive";
        case compress::Format::AUTO:       return "detect automatically";
    }
    return "unknown";
}

int compression_level_int(const std::string& s) {
    try { return std::stoi(s); } catch (...) { return -1; }
}

uint64_t parse_volume(const std::string& s) {
    return compress::detail::parse_size_suffix(s);
}

// -----------------------------------------------------------------------
// Command: add / create
// -----------------------------------------------------------------------
int cmd_add(const std::vector<std::string>& files,
            const std::string& archive,
            compress::ArchiveOptions& opts) {
    if (files.empty()) {
        fmt::print(stderr, "error: no input files specified\n");
        return 1;
    }

    if (opts.format == compress::Format::AUTO) {
        opts.format = compress::detect_format_from_extension(archive);
        if (opts.format == compress::Format::AUTO) {
            fmt::print(stderr, "error: could not determine archive format from extension: {}\n", archive);
            fmt::print(stderr, "       use --format to specify explicitly\n");
            return 1;
        }
    }

    fmt::print("Creating {} with {} file(s)...\n", format_desc(opts.format), files.size());
    if (opts.verbose) {
        for (auto& f : files) fmt::print("  + {}\n", f);
    }

    compress::Error err = compress::compress_files(files, archive, opts);
    if (err) {
        fmt::print(stderr, "error: {}\n", err.message.empty() ? "unknown error" : err.message);
        return 1;
    }
    fmt::print("Successfully created: {}\n", archive);
    return 0;
}

// -----------------------------------------------------------------------
// Command: extract
// -----------------------------------------------------------------------
int cmd_extract(const std::string& archive, compress::ArchiveOptions& opts) {
    if (!fs::exists(archive)) {
        fmt::print(stderr, "error: archive not found: {}\n", archive);
        return 1;
    }

    fmt::print("Extracting {}...\n", archive);
    compress::Error err = compress::extract_archive(archive, opts);
    if (err) {
        if (err.code == compress::Error::WRONG_PASSWORD) {
            fmt::print(stderr, "error: wrong password or encrypted entry requires password (-p)\n");
        } else {
            fmt::print(stderr, "error: {}\n", err.message.empty() ? "extraction failed" : err.message);
        }
        return 1;
    }
    fmt::print("Successfully extracted to: {}\n",
               opts.output_dir.empty() ? fs::current_path().string() : opts.output_dir);
    return 0;
}

// -----------------------------------------------------------------------
// Command: list
// -----------------------------------------------------------------------
int cmd_list(const std::string& archive, compress::ArchiveOptions& opts) {
    if (!fs::exists(archive)) {
        fmt::print(stderr, "error: archive not found: {}\n", archive);
        return 1;
    }

    auto result = compress::inspect_archive(archive, opts);
    if (!result) {
        fmt::print(stderr, "error: {}\n", result.error.message);
        return 1;
    }

    auto& info = *result;
    fmt::print("Archive: {}\n", info.path);
    fmt::print("Format:  {}\n", format_desc(info.format));
    fmt::print("Entries: {}\n", info.total_entries);
    fmt::print("Size:    {} bytes\n", info.total_size);
    if (info.is_encrypted) fmt::print("Encrypted: yes\n");
    if (info.is_multipart) fmt::print("Multipart: yes ({} part(s))\n", info.parts.size());

    // Read entries
    auto reader = compress::create_reader(archive, opts);
    if (!reader) {
        fmt::print(stderr, "warning: cannot open archive for listing entries\n");
        return 0;
    }

    auto entries_result = reader->entries();
    reader->close();

    if (!entries_result) {
        fmt::print(stderr, "warning: cannot read entries\n");
        return 0;
    }

    fmt::print("\n{:<6} {:>12}  {}\n", "TYPE", "SIZE", "NAME");
    fmt::print("{:-<6} {:->12}  {:-}\n", "", "", "");

    for (auto& e : *entries_result) {
        std::string type = e.is_directory ? "DIR" : (e.is_symlink ? "LNK" : "FILE");
        fmt::print("{:<6} {:>12}  {}{}{}\n",
                   type,
                   e.is_directory ? 0UL : e.size,
                   e.path,
                   e.is_symlink ? (" -> " + e.symlink_target) : "",
                   e.is_encrypted ? " [encrypted]" : "");
    }

    return 0;
}

// -----------------------------------------------------------------------
// Command: test
// -----------------------------------------------------------------------
int cmd_test(const std::string& archive, compress::ArchiveOptions& opts) {
    if (!fs::exists(archive)) {
        fmt::print(stderr, "error: archive not found: {}\n", archive);
        return 1;
    }

    fmt::print("Testing: {}\n", archive);

    auto reader = compress::create_reader(archive, opts);
    if (!reader) {
        fmt::print(stderr, "error: unsupported or corrupted format\n");
        return 1;
    }

    auto entries_result = reader->entries();
    if (!entries_result) {
        reader->close();
        fmt::print(stderr, "error: {}\n", entries_result.error.message);
        return 1;
    }

    bool ok = true;
    for (auto& e : *entries_result) {
        auto data = reader->read_entry(e.path);
        if (!data) {
            fmt::print(stderr, "  FAIL: {} ({})\n", e.path, data.error.message);
            ok = false;
        } else if (opts.verbose) {
            fmt::print("  OK:   {} ({} bytes)\n", e.path, data->size());
        }
    }

    reader->close();

    if (ok) {
        fmt::print("All {} entries verified successfully.\n", entries_result->size());
        return 0;
    }
    return 1;
}

// -----------------------------------------------------------------------
// Main
// -----------------------------------------------------------------------
int main(int argc, char* argv[]) {
    cxxopts::Options cli("cpct", "Cross-Platform Compressed Tool (cpct)");
    cli.custom_help("[options] command [archive] [files...]");
    cli.positional_help("command archive [files...]");

    // clang-format off
    cli.add_options()
        ("command", "Command: a|add, x|extract, l|list, t|test", cxxopts::value<std::string>())
        ("archive", "Archive file path", cxxopts::value<std::string>())
        ("files",   "Input files/directories", cxxopts::value<std::vector<std::string>>())

        ("p,password",   "Encryption/decryption password", cxxopts::value<std::string>())
        ("m,method",     "Encryption method: AES-128, AES-256, ZipCrypto", cxxopts::value<std::string>())
        ("f,format",     "Archive format: zip, tar, 7z, gz, bz2, xz, zst, rar", cxxopts::value<std::string>())
        ("v,volume-size","Split into volumes of given size (e.g. 10M, 100K, 1G)", cxxopts::value<std::string>())
        ("o,output-dir", "Output directory for extraction", cxxopts::value<std::string>())
        ("0..9",         "Compression level 0-9 (0=store, 9=max)", cxxopts::value<std::string>())
        ("overwrite",    "Overwrite existing files on extract")
        ("skip",         "Skip existing files on extract (default)")
        ("verbose",      "Verbose output")
        ("q,quiet",      "Suppress output")
        ("h,help",       "Print usage")
        ;

    cli.parse_positional({"command", "archive", "files"});
    // clang-format on

    auto args = cli.parse(argc, argv);

    if (args.count("help") || argc == 1) {
        fmt::print("{}\n", cli.help());
        fmt::print("\nCommands:\n");
        fmt::print("  a / add       Create or update archive\n");
        fmt::print("  x / extract   Extract archive\n");
        fmt::print("  l / list      List archive contents\n");
        fmt::print("  t / test      Verify archive integrity\n");
        fmt::print("\nExamples:\n");
        fmt::print("  cpct a archive.zip file1.txt file2.txt\n");
        fmt::print("  cpct a archive.7z dir/ -p secret\n");
        fmt::print("  cpct a archive.zip files/ -v 10M\n");
        fmt::print("  cpct x archive.zip -o output/\n");
        fmt::print("  cpct l archive.zip\n");
        fmt::print("  cpct t archive.7z\n");
        return 0;
    }

    // Parse command
    std::string cmd;
    if (args.count("command")) {
        cmd = args["command"].as<std::string>();
    } else {
        fmt::print(stderr, "error: no command specified\n");
        return 1;
    }

    std::string archive;
    if (args.count("archive")) {
        archive = args["archive"].as<std::string>();
    } else if (cmd != "a" && cmd != "add") {
        fmt::print(stderr, "error: archive path required\n");
        return 1;
    }

    // Build options
    compress::ArchiveOptions opts;
    opts.verbose = args.count("verbose") > 0;

    if (args.count("password")) opts.password = args["password"].as<std::string>();
    if (args.count("output-dir")) opts.output_dir = args["output-dir"].as<std::string>();
    if (args.count("overwrite")) opts.overwrite = compress::OverwriteMode::OVERWRITE;

    if (args.count("format")) {
        std::string fmt = args["format"].as<std::string>();
        std::transform(fmt.begin(), fmt.end(), fmt.begin(),
                       [](char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); });
        if (fmt == "zip")       opts.format = compress::Format::ZIP;
        else if (fmt == "tar")  opts.format = compress::Format::TAR;
        else if (fmt == "gz" || fmt == "gzip")   opts.format = compress::Format::GZIP;
        else if (fmt == "bz2" || fmt == "bzip2") opts.format = compress::Format::BZIP2;
        else if (fmt == "xz")   opts.format = compress::Format::XZ;
        else if (fmt == "zst" || fmt == "zstd")  opts.format = compress::Format::ZSTD;
        else if (fmt == "7z")   opts.format = compress::Format::SEVEN_ZIP;
        else if (fmt == "rar")  opts.format = compress::Format::RAR;
        else fmt::print(stderr, "warning: unknown format '{}', using auto-detect\n", fmt);
    }

    if (args.count("method")) {
        std::string m = args["method"].as<std::string>();
        std::transform(m.begin(), m.end(), m.begin(),
                       [](char c) { return static_cast<char>(std::toupper(static_cast<unsigned char>(c))); });
        std::replace(m.begin(), m.end(), '-', '_');
        if (m == "AES_128") opts.encryption = compress::EncryptionMethod::AES_128;
        else if (m == "AES_256") opts.encryption = compress::EncryptionMethod::AES_256;
        else if (m == "ZIP_CRYPTO" || m == "ZIPCRYPTO") opts.encryption = compress::EncryptionMethod::ZIP_CRYPTO;
        else fmt::print(stderr, "warning: unknown encryption method '{}', using AES-256\n", m);
    }

    if (args.count("volume-size")) {
        opts.multipart_mode = compress::MultipartMode::SPLIT;
        opts.volume_size = parse_volume(args["volume-size"].as<std::string>());
        if (opts.volume_size == 0) {
            fmt::print(stderr, "warning: invalid volume size, ignoring\n");
            opts.multipart_mode = compress::MultipartMode::NONE;
        }
    }

    if (args.count("0..9")) {
        int lvl = compression_level_int(args["0..9"].as<std::string>());
        if (lvl >= 0 && lvl <= 9) opts.compression_level = lvl;
    }

    // Parse file list
    std::vector<std::string> files;
    if (args.count("files")) {
        files = args["files"].as<std::vector<std::string>>();
    }

    // Dispatch
    if (cmd == "a" || cmd == "add") {
        if (archive.empty()) {
            fmt::print(stderr, "error: archive path required\n");
            return 1;
        }
        return cmd_add(files, archive, opts);
    } else if (cmd == "x" || cmd == "extract") {
        return cmd_extract(archive, opts);
    } else if (cmd == "l" || cmd == "list") {
        return cmd_list(archive, opts);
    } else if (cmd == "t" || cmd == "test") {
        return cmd_test(archive, opts);
    } else {
        fmt::print(stderr, "error: unknown command '{}'\n", cmd);
        fmt::print(stderr, "Use 'cpct --help' for usage.\n");
        return 1;
    }
}
