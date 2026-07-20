# cpct — Cross-Platform Compressed Tool

`cpct` is a C++17 archive compression library and CLI supporting
ZIP, TAR (with gzip/bzip2/xz/zstd filters), GZIP, BZIP2, XZ, ZSTD,
7z, and RAR formats. Format selection is automatic via magic bytes
with extension fallback.

## Quick start

### CLI

```bash
# Compress
cpct a archive.zip     file1.txt file2.txt
cpct a archive.tar.gz  dir/
cpct a archive.7z      files/ -p secret
cpct a archive.zip     dir/ -v 10M         # split into 10 MB volumes

# Extract
cpct x archive.zip -o output/
cpct x encrypted.zip -p secret

# List
cpct l archive.zip

# Verify integrity
cpct t archive.7z
```

### Library

```cpp
#include <compress/compress.h>

// Compress a file
compress::compress_file("input.txt", "output.zip");

// Extract an archive
compress::ArchiveOptions opts;
opts.output_dir = "extracted/";
compress::extract_archive("archive.tar.gz", opts);

// Inspect contents
auto info = compress::inspect_archive("archive.7z");
if (info) fmt::print("{} entries, {} bytes\n", info->total_entries, info->total_size);

// Low-level API
auto writer = compress::create_writer("output.zip");
writer->add_file("document.txt");
writer->add_directory("src/");
writer->close();

auto reader = compress::create_reader("output.zip");
auto entries = reader->entries();
reader->extract("output_dir/");
reader->close();
```

See [`examples/basic_usage.cpp`](examples/basic_usage.cpp) for a runnable
example covering ZIP, TAR.GZ, password protection, and multipart archives.

## Build

**Prerequisites**: CMake ≥ 3.20, C++17 compiler, [vcpkg](https://vcpkg.io).

| Platform | Architecture | CI | Notes |
|---|---|---|---|
| Linux | x64 | ✓ | native |
| Linux | arm64 | ✓ | cross-compile + QEMU on x64 |
| Windows | x64 | ✓ | native (MSVC) |
| Windows | arm64 | ✓ | cross-compile from x64 |

```bash
export VCPKG_ROOT=/path/to/vcpkg

# Configure and build (Debug)
cmake --preset default
cmake --build --preset default

# Run tests
ctest --preset default

# Release build
cmake --preset release
cmake --build --preset release
```

| CMake option | Default | Description |
|---|---|---|
| `BUILD_CLI` | `ON` | Build the `cpct` command-line tool |
| `BUILD_TESTS` | `ON` | Build unit tests (requires GTest) |
| `BUILD_EXAMPLES` | `OFF` | Build example programs |

## Supported formats

| Format | Extensions | Read | Write | Notes |
|---|---|---|---|---|
| ZIP | `.zip` | ✓ | ✓ | AES-256 encryption, split volumes, minizip-ng |
| TAR | `.tar`, `.tar.gz`, `.tar.bz2`, `.tar.xz`, `.tar.zst`, `.tgz`, `.tbz` | ✓ | ✓ | Compound extensions auto-select compression filter |
| GZIP | `.gz` | ✓ | ✓ | Single-stream compression (zlib) |
| BZIP2 | `.bz2` | ✓ | ✓ | Single-stream (libbzip2) |
| XZ | `.xz` | ✓ | ✓ | Single-stream (liblzma) |
| ZSTD | `.zst`, `.zstd` | ✓ | ✓ | Single-stream (libzstd) |
| 7z | `.7z` | ✓ | conditional | Writer requires [lzma-sdk](https://www.7-zip.org/sdk.html); reader uses libarchive |
| RAR | `.rar` | ✓ | — | Read-only via libarchive |

Format detection: **magic bytes** first, **extension** fallback.  
TAR compound extensions (`.tar.gz`, `.tgz`, etc.) all resolve to `Format::TAR`.

## API

### Type overview

| Type | Purpose |
|---|---|
| `Format` | Enum: `ZIP`, `TAR`, `GZIP`, `BZIP2`, `XZ`, `ZSTD`, `SEVEN_ZIP`, `RAR`, `AUTO` |
| `Error` | Lightweight error with code + message; `operator bool()` is truthy on error |
| `Result<T>` | Fallible return type wrapping `T` + `Error` |
| `ArchiveEntry` | Per-file metadata: path, size, CRC32, symlink info, timestamps |
| `ArchiveOptions` | Writer/reader configuration: password, encryption, compression level, split volume, overwrite |
| `ArchiveInfo` | Aggregated archive info: format, entry count, total size |
| `ArchiveReader` | Abstract reader interface |
| `ArchiveWriter` | Abstract writer interface |

### Reader API

```cpp
auto reader = compress::create_reader("archive.zip", options);

auto entries  = reader->entries();                        // list all entries
auto err      = reader->extract("output/");               // extract all
auto err      = reader->extract_entry("path.txt", "out"); // extract single
auto data     = reader->read_entry("path.txt");           // read into memory
auto info     = reader->info();                           // archive metadata
bool open     = reader->is_open();                        // check state
reader->close();
```

### Writer API

```cpp
auto writer = compress::create_writer("archive.zip", options);

writer->add_file("path/to/file.txt");                    // from disk
writer->add_file("path/to/file.txt", "archive/name");    // with custom archive path
writer->add_directory("path/to/dir");                    // recursive
writer->add_from_memory("name.txt", data, size);         // from buffer
writer->close();
```

### High-level convenience

```cpp
compress::compress_file(source, archive, options);           // single file
compress::compress_files({src1, src2}, archive, options);    // multiple files
compress::extract_archive(archive, options);                 // extract all
compress::inspect_archive(archive, options);                 // list info
```

### ArchiveOptions

```cpp
compress::ArchiveOptions opts;
opts.format            = Format::AUTO;          // auto-detect from path/magic
opts.password          = "secret";              // encryption password
opts.encryption        = EncryptionMethod::AES_256;
opts.compression_level = 6;                     // 0-9 (0=store, 9=max)
opts.volume_size       = 10 * 1024 * 1024;      // split volume size
opts.multipart_mode    = MultipartMode::SPLIT;
opts.output_dir        = "extracted/";          // extract destination
opts.overwrite         = OverwriteMode::SKIP;   // skip existing on extract
opts.verbose           = true;
```

## Project layout

```
├── cli/main.cpp              CLI tool (cxxopts + fmt)
├── libcompress/
│   ├── include/compress/     Public headers
│   └── src/                  Per-format backends + factory
├── tests/                    80 test cases (GTest)
├── examples/                 Runnable examples
├── .github/workflows/
│   ├── ci.yml              CI (Debug + Release per PR)
│   └── release.yml         Release builds (tags)
├── vcpkg.json                Dependencies
├── CMakePresets.json         Debug / Release presets
├── LICENSE                   GPL-3.0
└── NOTICE                    Third-party license texts
```

## Dependencies

| Package | Purpose |
|---|---|
| [minizip-ng](https://github.com/zlib-ng/minizip-ng) | ZIP backend |
| [libarchive](https://libarchive.org) | TAR, 7z reading, RAR reading |
| [zlib](https://zlib.net) | GZIP compression (bundled with minizip-ng) |
| [bzip2](https://sourceware.org/bzip2/) | BZIP2 compression |
| [liblzma](https://tukaani.org/xz/) | XZ compression |
| [zstd](https://facebook.github.io/zstd/) | ZSTD compression |
| [lzma-sdk](https://www.7-zip.org/sdk.html) | 7z writing (optional) |
| [fmt](https://fmt.dev) | CLI output formatting |
| [cxxopts](https://github.com/jarro2783/cxxopts) | CLI argument parsing |
| [GTest](https://github.com/google/googletest) | Unit tests |

## License

cpct is [GPL-3.0](LICENSE). All third-party dependencies are
compatible with GPL-3.0 and their full license texts are in
[NOTICE](NOTICE).

| Dependency | License |
|---|---|
| [minizip-ng](https://github.com/zlib-ng/minizip-ng) | zlib |
| [libarchive](https://libarchive.org) | 2-clause BSD |
| [zstd](https://facebook.github.io/zstd/) | 3-clause BSD |
| [bzip2](https://sourceware.org/bzip2/) | BSD-style |
| [liblzma](https://tukaani.org/xz/) | 0BSD |
| [lzma-sdk](https://www.7-zip.org/sdk.html) | Public domain |
| [fmt](https://fmt.dev) | MIT |
| [cxxopts](https://github.com/jarro2783/cxxopts) | MIT |
| [GTest](https://github.com/google/googletest) | 3-clause BSD |
