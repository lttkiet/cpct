#include <gtest/gtest.h>
#include "compress/compress.h"
#include "test_helpers.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using namespace compress;

// ====================================================================
// Test fixture — creates a temp dir, copies test data files into it,
// builds a small directory tree for multi-file / recursive tests.
// ====================================================================
class AdvancedTest : public ::testing::Test {
protected:
    std::string tmp_dir_;
    std::string hello_;
    std::string lorem_;
    std::string empty_;
    std::string binary_;
    std::string large_;
    std::string tree_dir_;   // dir with subdirs and files

    void SetUp() override {
        tmp_dir_ = (fs::temp_directory_path() / "cpct_advanced_test").string();
        fs::create_directories(tmp_dir_);

        hello_  = tmp_dir_ + "/hello.txt";
        lorem_  = tmp_dir_ + "/lorem.txt";
        empty_  = tmp_dir_ + "/empty.txt";
        binary_ = tmp_dir_ + "/binary.bin";
        large_  = tmp_dir_ + "/large.txt";
        tree_dir_ = tmp_dir_ + "/tree";

        fs::copy_file(test_data_dir() / "hello.txt",  hello_);
        fs::copy_file(test_data_dir() / "lorem.txt",  lorem_);
        fs::copy_file(test_data_dir() / "empty.txt",  empty_);
        fs::copy_file(test_data_dir() / "binary.bin", binary_);
        fs::copy_file(test_data_dir() / "large.txt",  large_);

        fs::create_directories(tree_dir_ + "/sub/deep");
        std::ofstream(tree_dir_ + "/root.txt")    << "root-level";
        std::ofstream(tree_dir_ + "/sub/a.txt")    << "a-content";
        std::ofstream(tree_dir_ + "/sub/deep/b.txt") << "deep-b-content";
        std::ofstream(tree_dir_ + "/sub/deep/c.txt") << "deep-c-content";
    }

    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }

    static std::string read_content(const std::string& path) {
        std::ifstream in(path, std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(in),
                           std::istreambuf_iterator<char>());
    }
};

// ====================================================================
// Multi-file archive roundtrip (ZIP + TAR)
// ====================================================================
TEST_F(AdvancedTest, ZipMultiFile) {
    std::string arc = tmp_dir_ + "/multi.zip";
    auto writer = create_writer(arc);
    ASSERT_NE(writer, nullptr);

    EXPECT_FALSE(writer->add_file(hello_)) << "add_file hello";
    EXPECT_FALSE(writer->add_file(lorem_)) << "add_file lorem";
    EXPECT_FALSE(writer->add_file(binary_)) << "add_file binary";
    writer->close();
    EXPECT_TRUE(fs::exists(arc));

    auto reader = create_reader(arc);
    ASSERT_NE(reader, nullptr);
    auto entries = reader->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_EQ(entries->size(), 3U);

    std::string out = tmp_dir_ + "/multi_out";
    EXPECT_FALSE(reader->extract(out));
    reader->close();

    EXPECT_TRUE(fs::exists(out + "/hello.txt"));
    EXPECT_TRUE(fs::exists(out + "/lorem.txt"));
    EXPECT_TRUE(fs::exists(out + "/binary.bin"));

    EXPECT_EQ(read_content(out + "/hello.txt"), "Hello, world!\n");
    EXPECT_EQ(read_content(out + "/lorem.txt"),
              read_content((test_data_dir() / "lorem.txt").string()));
}

TEST_F(AdvancedTest, TarMultiFile) {
    std::string arc = tmp_dir_ + "/multi.tar";
    auto writer = create_writer(arc);
    ASSERT_NE(writer, nullptr);

    EXPECT_FALSE(writer->add_file(hello_));
    EXPECT_FALSE(writer->add_file(lorem_));
    EXPECT_FALSE(writer->add_file(empty_));
    writer->close();

    auto reader = create_reader(arc);
    ASSERT_NE(reader, nullptr);
    auto entries = reader->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_EQ(entries->size(), 3U);

    // Verify entry paths
    bool found_hello = false, found_lorem = false, found_empty = false;
    for (auto& e : *entries) {
        if (e.path == "hello.txt") found_hello = true;
        if (e.path == "lorem.txt") found_lorem = true;
        if (e.path == "empty.txt") found_empty = true;
    }
    EXPECT_TRUE(found_hello);
    EXPECT_TRUE(found_lorem);
    EXPECT_TRUE(found_empty);

    std::string out = tmp_dir_ + "/multi_out";
    EXPECT_FALSE(reader->extract(out));
    reader->close();

    EXPECT_TRUE(fs::exists(out + "/hello.txt"));
    EXPECT_TRUE(fs::exists(out + "/lorem.txt"));
    EXPECT_TRUE(fs::exists(out + "/empty.txt"));
    EXPECT_EQ(fs::file_size(out + "/empty.txt"), 0);
}

// ====================================================================
// Directory recursion (add_directory)
// ====================================================================
TEST_F(AdvancedTest, ZipAddDirectory) {
    std::string arc = tmp_dir_ + "/dir.zip";
    auto writer = create_writer(arc);
    ASSERT_NE(writer, nullptr);

    EXPECT_FALSE(writer->add_directory(tree_dir_));
    writer->close();
    EXPECT_TRUE(fs::exists(arc));

    auto reader = create_reader(arc);
    ASSERT_NE(reader, nullptr);
    auto entries = reader->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_GE(entries->size(), 4U);

    std::string out = tmp_dir_ + "/dir_out";
    EXPECT_FALSE(reader->extract(out));
    reader->close();

    // files are stored relative to the parent of tree_dir_
    std::string pref = fs::path(tree_dir_).filename().string() + "/";
    EXPECT_TRUE(fs::exists(out + "/" + pref + "root.txt"));
    EXPECT_TRUE(fs::exists(out + "/" + pref + "sub/a.txt"));
    EXPECT_TRUE(fs::exists(out + "/" + pref + "sub/deep/b.txt"));
    EXPECT_TRUE(fs::exists(out + "/" + pref + "sub/deep/c.txt"));
}

TEST_F(AdvancedTest, TarAddDirectory) {
    std::string arc = tmp_dir_ + "/dir.tar";
    auto writer = create_writer(arc);
    ASSERT_NE(writer, nullptr);

    EXPECT_FALSE(writer->add_directory(tree_dir_));
    writer->close();
    EXPECT_TRUE(fs::exists(arc));

    auto reader = create_reader(arc);
    ASSERT_NE(reader, nullptr);
    auto entries = reader->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_GE(entries->size(), 4U);

    std::string out = tmp_dir_ + "/dir_out";
    EXPECT_FALSE(reader->extract(out));
    reader->close();

    // files are stored relative to the parent of tree_dir_
    std::string pref = fs::path(tree_dir_).filename().string() + "/";
    EXPECT_TRUE(fs::exists(out + "/" + pref + "root.txt"));
    EXPECT_TRUE(fs::exists(out + "/" + pref + "sub/a.txt"));
    EXPECT_TRUE(fs::exists(out + "/" + pref + "sub/deep/b.txt"));
    EXPECT_TRUE(fs::exists(out + "/" + pref + "sub/deep/c.txt"));
}

// ====================================================================
// Single-stream multi-file error / add_file replaces
// ====================================================================
TEST_F(AdvancedTest, GzipMultiFileConcatenates) {
    std::string arc = tmp_dir_ + "/multi.gz";
    auto writer = create_writer(arc);
    ASSERT_NE(writer, nullptr);
    writer->add_file(hello_);
    writer->add_file(lorem_);
    writer->close();

    auto reader = create_reader(arc);
    ASSERT_NE(reader, nullptr);
    auto data = reader->read_entry("");
    ASSERT_TRUE(data.ok());
    // gzip writes all data to a single stream; decompress gives concatenation
    std::string expected = read_content(hello_) + read_content(lorem_);
    std::string content(data->data(), data->size());
    EXPECT_EQ(content, expected);
    reader->close();
}

// ====================================================================
// Entry metadata verification
// ====================================================================
TEST_F(AdvancedTest, ZipEntryMetadata) {
    std::string arc = tmp_dir_ + "/meta.zip";
    auto writer = create_writer(arc);
    ASSERT_NE(writer, nullptr);
    writer->add_file(hello_);
    writer->close();

    auto reader = create_reader(arc);
    ASSERT_NE(reader, nullptr);
    auto entries = reader->entries();
    ASSERT_TRUE(entries.ok());
    ASSERT_GE(entries->size(), 1U);

    auto& e = (*entries)[0];
    EXPECT_EQ(e.path, "hello.txt");
    EXPECT_EQ(e.size, fs::file_size(hello_));
    EXPECT_FALSE(e.is_directory);

    auto data = reader->read_entry("hello.txt");
    ASSERT_TRUE(data.ok());
    reader->close();
}

TEST_F(AdvancedTest, TarEntryMetadata) {
    std::string arc = tmp_dir_ + "/meta.tar";
    auto writer = create_writer(arc);
    ASSERT_NE(writer, nullptr);
    writer->add_file(hello_);
    writer->add_file(empty_);
    writer->close();

    auto reader = create_reader(arc);
    ASSERT_NE(reader, nullptr);
    auto entries = reader->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_GE(entries->size(), 2U);

    for (auto& e : *entries) {
        if (e.path == "hello.txt") {
            EXPECT_EQ(e.size, fs::file_size(hello_));
            EXPECT_FALSE(e.is_directory);
        }
        if (e.path == "empty.txt") {
            EXPECT_EQ(e.size, 0);
            EXPECT_FALSE(e.is_directory);
        }
    }
    reader->close();
}

// ====================================================================
// Error handling — nonexistent source file
// ====================================================================
TEST_F(AdvancedTest, AddFileNotFound) {
    auto writer = create_writer(tmp_dir_ + "/nf.zip");
    ASSERT_NE(writer, nullptr);
    Error err = writer->add_file(tmp_dir_ + "/does_not_exist.txt");
    EXPECT_TRUE(err);
    EXPECT_EQ(err.code, Error::FILE_NOT_FOUND);
    writer->close();
}

TEST_F(AdvancedTest, ReadNonexistentEntry) {
    std::string arc = tmp_dir_ + "/ne.zip";
    auto writer = create_writer(arc);
    ASSERT_NE(writer, nullptr);
    writer->add_file(hello_);
    writer->close();

    auto reader = create_reader(arc);
    ASSERT_NE(reader, nullptr);
    auto data = reader->read_entry("nope.txt");
    EXPECT_FALSE(data.ok());
    reader->close();
}

TEST_F(AdvancedTest, ExtractNonexistentEntry) {
    std::string arc = tmp_dir_ + "/nee.zip";
    auto writer = create_writer(arc);
    ASSERT_NE(writer, nullptr);
    writer->add_file(hello_);
    writer->close();

    auto reader = create_reader(arc);
    ASSERT_NE(reader, nullptr);
    Error err = reader->extract_entry("nope.txt", tmp_dir_ + "/nope_out.txt");
    EXPECT_TRUE(err);
    reader->close();
}

// ====================================================================
// Corrupted / truncated archive detection
// ====================================================================
TEST_F(AdvancedTest, CorruptedZip) {
    std::string arc = tmp_dir_ + "/corrupt.zip";
    {
        std::ofstream f(arc, std::ios::binary);
        f << "not a valid zip file!";
    }
    auto reader = create_reader(arc);
    // minizip-ng may reject the file on creation or fail at entries()
    if (reader) {
        auto entries = reader->entries();
        // entries() should fail for corrupted input
        EXPECT_FALSE(entries.ok());
        reader->close();
    }
}

// ====================================================================
// High-level API — compress_file / extract_archive / inspect_archive
// ====================================================================
TEST_F(AdvancedTest, CompressFileExtractRoundtrip) {
    std::string arc = tmp_dir_ + "/hl.zip";
    EXPECT_FALSE(compress_file(hello_, arc));

    std::string out = tmp_dir_ + "/hl_out";
    ArchiveOptions opts;
    opts.output_dir = out;
    EXPECT_FALSE(extract_archive(arc, opts));

    EXPECT_TRUE(fs::exists(out + "/hello.txt"));
    EXPECT_EQ(read_content(out + "/hello.txt"), "Hello, world!\n");
}

TEST_F(AdvancedTest, CompressFileTarGz) {
    std::string arc = tmp_dir_ + "/hl.tar.gz";
    EXPECT_FALSE(compress_file(hello_, arc));
    EXPECT_TRUE(fs::exists(arc));
    EXPECT_GT(fs::file_size(arc), 0);

    auto info = inspect_archive(arc);
    EXPECT_TRUE(info.ok());
    EXPECT_GE(info->total_entries, 1);
}

TEST_F(AdvancedTest, InspectArchive) {
    std::string arc = tmp_dir_ + "/ins.zip";
    auto writer = create_writer(arc);
    ASSERT_NE(writer, nullptr);
    writer->add_file(hello_);
    writer->add_file(lorem_);
    writer->add_file(empty_);
    writer->close();

    auto info = inspect_archive(arc);
    ASSERT_TRUE(info.ok());
    EXPECT_EQ(info->path, arc);
    EXPECT_EQ(info->format, Format::ZIP);
    EXPECT_EQ(info->total_entries, 3);
    EXPECT_GT(info->total_size, 0);
}

// ====================================================================
// Compression level tests
// ====================================================================
TEST_F(AdvancedTest, ZipCompressionLevel) {
    std::string l0 = tmp_dir_ + "/level0.zip";
    std::string l9 = tmp_dir_ + "/level9.zip";

    ArchiveOptions opts0; opts0.compression_level = 0;
    ArchiveOptions opts9; opts9.compression_level = 9;

    auto w0 = create_writer(l0, opts0);
    ASSERT_NE(w0, nullptr);
    w0->add_file(large_);
    w0->close();

    auto w9 = create_writer(l9, opts9);
    ASSERT_NE(w9, nullptr);
    w9->add_file(large_);
    w9->close();

    EXPECT_TRUE(fs::exists(l0));
    EXPECT_TRUE(fs::exists(l9));
    EXPECT_GT(fs::file_size(l0), 0);
    EXPECT_GT(fs::file_size(l9), 0);

    // level 9 should generally be <= level 0
    EXPECT_LE(fs::file_size(l9), fs::file_size(l0));

    // verify both extract correctly
    for (const auto& arc : {l0, l9}) {
        auto r = create_reader(arc);
        ASSERT_NE(r, nullptr);
        std::string out = tmp_dir_ + "/level_out";
        EXPECT_FALSE(r->extract(out));
        r->close();
        EXPECT_TRUE(fs::exists(out + "/large.txt"));
        fs::remove_all(out);
    }
}

TEST_F(AdvancedTest, GzipCompressionLevel) {
    std::string l1 = tmp_dir_ + "/level1.gz";
    std::string l9 = tmp_dir_ + "/level9.gz";

    ArchiveOptions opts1; opts1.compression_level = 1;
    ArchiveOptions opts9; opts9.compression_level = 9;

    auto w1 = create_writer(l1, opts1);
    ASSERT_NE(w1, nullptr);
    w1->add_file(large_);
    w1->close();

    auto w9 = create_writer(l9, opts9);
    ASSERT_NE(w9, nullptr);
    w9->add_file(large_);
    w9->close();

    // Both should decompress to identical content
    auto r1 = create_reader(l1);
    ASSERT_NE(r1, nullptr);
    auto d1 = r1->read_entry("");
    r1->close();
    ASSERT_TRUE(d1.ok());

    auto r9 = create_reader(l9);
    ASSERT_NE(r9, nullptr);
    auto d9 = r9->read_entry("");
    r9->close();
    ASSERT_TRUE(d9.ok());

    std::string s1(d1->data(), d1->size());
    std::string s9(d9->data(), d9->size());
    EXPECT_EQ(s1, s9);
    EXPECT_EQ(s1, read_content(large_));
}

TEST_F(AdvancedTest, Bzip2CompressionLevel) {
    std::string arc = tmp_dir_ + "/bzip2_lvl.bz2";
    ArchiveOptions opts;
    opts.compression_level = 9;

    auto w = create_writer(arc, opts);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto data = r->read_entry("");
    r->close();
    ASSERT_TRUE(data.ok());
    std::string content(data->data(), data->size());
    EXPECT_EQ(content, "Hello, world!\n");
}

TEST_F(AdvancedTest, XzCompressionLevel) {
    std::string arc = tmp_dir_ + "/xz_lvl.xz";
    ArchiveOptions opts;
    opts.compression_level = 3;

    auto w = create_writer(arc, opts);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto data = r->read_entry("");
    r->close();
    ASSERT_TRUE(data.ok());
    std::string content(data->data(), data->size());
    EXPECT_EQ(content, "Hello, world!\n");
}

TEST_F(AdvancedTest, ZstdCompressionLevel) {
    std::string arc = tmp_dir_ + "/zstd_lvl.zst";
    ArchiveOptions opts;
    opts.compression_level = 10;

    auto w = create_writer(arc, opts);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto data = r->read_entry("");
    r->close();
    ASSERT_TRUE(data.ok());
    std::string content(data->data(), data->size());
    EXPECT_EQ(content, "Hello, world!\n");
}

// ====================================================================
// Magic-byte / format auto-detection integration
// ====================================================================
TEST_F(AdvancedTest, AutoDetectZipByMagic) {
    std::string arc = tmp_dir_ + "/detect_test.zip";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->close();

    ArchiveOptions opts;
    opts.format = Format::AUTO;
    auto r = create_reader(arc, opts);
    ASSERT_NE(r, nullptr);
    auto entries = r->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_GE(entries->size(), 1U);
    r->close();
}

TEST_F(AdvancedTest, AutoDetectGzipByMagic) {
    std::string arc = tmp_dir_ + "/detect_test.gz";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->close();

    ArchiveOptions opts;
    opts.format = Format::AUTO;
    auto r = create_reader(arc, opts);
    ASSERT_NE(r, nullptr);
    auto data = r->read_entry("");
    ASSERT_TRUE(data.ok());
    r->close();
}

// ====================================================================
// Edge cases — empty files, binary data, large files
// ====================================================================
TEST_F(AdvancedTest, ZipEmptyFile) {
    std::string arc = tmp_dir_ + "/empty.zip";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(empty_);  // empty file
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto entries = r->entries();
    ASSERT_TRUE(entries.ok());
    ASSERT_GE(entries->size(), 1U);
    EXPECT_EQ((*entries)[0].size, 0);

    std::string out = tmp_dir_ + "/empty_out";
    EXPECT_FALSE(r->extract(out));
    r->close();

    EXPECT_TRUE(fs::exists(out + "/empty.txt"));
    EXPECT_EQ(fs::file_size(out + "/empty.txt"), 0);
}

TEST_F(AdvancedTest, ZipBinaryData) {
    std::string arc = tmp_dir_ + "/binary.zip";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(binary_);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto data = r->read_entry("binary.bin");
    r->close();
    ASSERT_TRUE(data.ok());

    auto orig = read_file_bytes(binary_);
    ASSERT_EQ(data->size(), orig.size());
    EXPECT_EQ(memcmp(data->data(), orig.data(), data->size()), 0);
}

TEST_F(AdvancedTest, ZipLargeFile) {
    std::string arc = tmp_dir_ + "/large.zip";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(large_);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);

    std::string out = tmp_dir_ + "/large_out";
    EXPECT_FALSE(r->extract(out));
    r->close();

    EXPECT_TRUE(fs::exists(out + "/large.txt"));
    EXPECT_EQ(fs::file_size(out + "/large.txt"), fs::file_size(large_));
}

TEST_F(AdvancedTest, GzipBinaryData) {
    std::string arc = tmp_dir_ + "/binary.gz";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(binary_);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto data = r->read_entry("");
    r->close();
    ASSERT_TRUE(data.ok());

    auto orig = read_file_bytes(binary_);
    ASSERT_EQ(data->size(), orig.size());
    EXPECT_EQ(memcmp(data->data(), orig.data(), data->size()), 0);
}

TEST_F(AdvancedTest, GzipLargeFile) {
    std::string arc = tmp_dir_ + "/large.gz";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(large_);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto data = r->read_entry("");
    r->close();
    ASSERT_TRUE(data.ok());
    EXPECT_EQ(data->size(), static_cast<size_t>(fs::file_size(large_)));
}

// ====================================================================
// add_from_memory for multiple backends
// ====================================================================
TEST_F(AdvancedTest, ZipFromMemory) {
    std::string arc = tmp_dir_ + "/mem.zip";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);

    const char* data = "memory-content-1";
    EXPECT_FALSE(w->add_from_memory("mem/a.txt", data, strlen(data)));

    const char* data2 = "memory-content-2-foo";
    EXPECT_FALSE(w->add_from_memory("mem/b.txt", data2, strlen(data2)));
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto entries = r->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_EQ(entries->size(), 2U);

    auto d1 = r->read_entry("mem/a.txt");
    ASSERT_TRUE(d1.ok());
    EXPECT_EQ(std::string(d1->data(), d1->size()), "memory-content-1");

    auto d2 = r->read_entry("mem/b.txt");
    ASSERT_TRUE(d2.ok());
    EXPECT_EQ(std::string(d2->data(), d2->size()), "memory-content-2-foo");
    r->close();
}

TEST_F(AdvancedTest, TarFromMemory) {
    std::string arc = tmp_dir_ + "/mem.tar";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);

    const char* data = "tar-memory-test";
    EXPECT_FALSE(w->add_from_memory("data.txt", data, strlen(data)));
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto entries = r->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_EQ(entries->size(), 1U);

    auto d = r->read_entry("data.txt");
    ASSERT_TRUE(d.ok());
    EXPECT_EQ(std::string(d->data(), d->size()), "tar-memory-test");
    r->close();
}

TEST_F(AdvancedTest, GzipFromMemory) {
    std::string arc = tmp_dir_ + "/mem.gz";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);

    const char* data = "gzip-memory-data";
    EXPECT_FALSE(w->add_from_memory("ignored", data, strlen(data)));
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto d = r->read_entry("");
    r->close();
    ASSERT_TRUE(d.ok());
    EXPECT_EQ(std::string(d->data(), d->size()), "gzip-memory-data");
}

// ====================================================================
// Extract-entry (single entry extraction)
// ====================================================================
TEST_F(AdvancedTest, ZipExtractEntry) {
    std::string arc = tmp_dir_ + "/extentry.zip";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->add_file(lorem_);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);

    std::string out1 = tmp_dir_ + "/ext1.txt";
    std::string out2 = tmp_dir_ + "/ext2.txt";
    EXPECT_FALSE(r->extract_entry("hello.txt", out1));
    EXPECT_FALSE(r->extract_entry("lorem.txt", out2));
    r->close();

    EXPECT_TRUE(fs::exists(out1));
    EXPECT_TRUE(fs::exists(out2));
    EXPECT_EQ(read_content(out1), "Hello, world!\n");
    EXPECT_EQ(read_content(out2), read_content(lorem_));
}

TEST_F(AdvancedTest, TarExtractEntry) {
    std::string arc = tmp_dir_ + "/extentry.tar";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->add_file(empty_);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);

    std::string out = tmp_dir_ + "/ext_h.txt";
    EXPECT_FALSE(r->extract_entry("hello.txt", out));
    r->close();

    EXPECT_TRUE(fs::exists(out));
    EXPECT_EQ(read_content(out), "Hello, world!\n");
}

// ====================================================================
// Compound tar extensions (.tar.bz2, .tar.xz, .tar.zst)
// ====================================================================
TEST_F(AdvancedTest, TarBz2CreateExtract) {
    std::string arc = tmp_dir_ + "/test.tar.bz2";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->close();
    EXPECT_TRUE(fs::exists(arc));
    EXPECT_GT(fs::file_size(arc), 0);

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto entries = r->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_GE(entries->size(), 1U);
    r->close();
}

TEST_F(AdvancedTest, TarXzCreateExtract) {
    std::string arc = tmp_dir_ + "/test.tar.xz";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->close();
    EXPECT_TRUE(fs::exists(arc));
    EXPECT_GT(fs::file_size(arc), 0);

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto entries = r->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_GE(entries->size(), 1U);
    r->close();
}

TEST_F(AdvancedTest, TarZstCreateExtract) {
    std::string arc = tmp_dir_ + "/test.tar.zst";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->close();
    EXPECT_TRUE(fs::exists(arc));
    EXPECT_GT(fs::file_size(arc), 0);

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto entries = r->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_GE(entries->size(), 1U);
    r->close();
}

// ====================================================================
// Password-protected archive (ZIP AES-256)
// ====================================================================
TEST_F(AdvancedTest, ZipPasswordCorrect) {
    std::string arc = tmp_dir_ + "/pwd.zip";
    ArchiveOptions w_opts;
    w_opts.password = "secret123";
    w_opts.encryption = EncryptionMethod::AES_256;

    auto w = create_writer(arc, w_opts);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->add_file(lorem_);
    w->close();

    ArchiveOptions r_opts;
    r_opts.password = "secret123";
    auto r = create_reader(arc, r_opts);
    ASSERT_NE(r, nullptr);
    auto entries = r->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_EQ(entries->size(), 2U);

    auto data = r->read_entry("hello.txt");
    ASSERT_TRUE(data.ok());
    EXPECT_EQ(std::string(data->data(), data->size()), "Hello, world!\n");
    r->close();
}

TEST_F(AdvancedTest, ZipPasswordWrong) {
    std::string arc = tmp_dir_ + "/pwd2.zip";
    ArchiveOptions w_opts;
    w_opts.password = "correct";
    w_opts.encryption = EncryptionMethod::AES_256;

    auto w = create_writer(arc, w_opts);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->close();

    ArchiveOptions r_opts;
    r_opts.password = "wrongpass";
    auto r = create_reader(arc, r_opts);
    ASSERT_NE(r, nullptr);

    auto data = r->read_entry("hello.txt");
    EXPECT_FALSE(data.ok());
    r->close();
}

// ====================================================================
// Multipart / split writer (ZIP only)
// ====================================================================
TEST_F(AdvancedTest, ZipSplitLargeArchive) {
    std::string arc = tmp_dir_ + "/split.zip";
    ArchiveOptions opts;
    opts.multipart_mode = MultipartMode::SPLIT;
    opts.volume_size = 512;  // very small to force splitting

    auto w = create_writer(arc, opts);
    ASSERT_NE(w, nullptr);
    w->add_file(large_);  // 92KB file with 512B split should produce many parts
    w->close();

    // The last part should exist
    EXPECT_TRUE(fs::exists(arc));

    // Read the split archive (minizip-ng handles multi-part)
    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto entries = r->entries();
    if (entries.ok()) {
        EXPECT_GE(entries->size(), 1U);
    }
    r->close();
}

// ====================================================================
// format_name() coverage
// ====================================================================
TEST(FormatTest, AllFormatNames) {
    EXPECT_STREQ(format_name(Format::ZIP), "ZIP");
    EXPECT_STREQ(format_name(Format::TAR), "TAR");
    EXPECT_STREQ(format_name(Format::GZIP), "GZIP");
    EXPECT_STREQ(format_name(Format::BZIP2), "BZIP2");
    EXPECT_STREQ(format_name(Format::XZ), "XZ");
    EXPECT_STREQ(format_name(Format::ZSTD), "ZSTD");
    EXPECT_STREQ(format_name(Format::SEVEN_ZIP), "7z");
    EXPECT_STREQ(format_name(Format::RAR), "RAR");
    EXPECT_STREQ(format_name(Format::AUTO), "AUTO");
}

// ====================================================================
// Symlink handling (TAR only on platforms that support symlinks)
// ====================================================================
TEST_F(AdvancedTest, TarSymlink) {
#ifdef _WIN32
    GTEST_SKIP() << "symlinks behave differently on Windows CI";
    return;
#endif
    // Create a symlink target file and a symlink pointing to it
    std::string target_file = tmp_dir_ + "/link_target.txt";
    std::string link_path   = tmp_dir_ + "/link.txt";
    std::ofstream(target_file) << "symlink-content";

    fs::remove(link_path);
    std::error_code ec;
    fs::create_symlink("link_target.txt", link_path, ec);
    if (ec) {
        GTEST_SKIP() << "symlinks not available on this platform";
        return;
    }

    std::string arc = tmp_dir_ + "/sym.tar";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(target_file);
    w->add_file(link_path);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto entries = r->entries();
    ASSERT_TRUE(entries.ok());

    bool found_link = false;
    for (auto& e : *entries) {
        if (e.is_symlink) {
            found_link = true;
            EXPECT_FALSE(e.symlink_target.empty());
        }
    }
    EXPECT_TRUE(found_link);

    std::string out = tmp_dir_ + "/sym_out";
    EXPECT_FALSE(r->extract(out));
    r->close();

    // The symlink should exist and point to the correct target
    EXPECT_TRUE(fs::exists(out + "/link.txt"));
    std::error_code ec2;
    bool is_sym = fs::is_symlink(fs::path(out + "/link.txt"), ec2);
    if (!ec2 && is_sym) {
        auto readlink_target = fs::read_symlink(fs::path(out + "/link.txt"), ec2);
        if (!ec2) {
            EXPECT_EQ(readlink_target.filename().string(), "link_target.txt");
        }
    }
}

// ====================================================================
// BZIP2 reader tests
// ====================================================================
TEST_F(AdvancedTest, Bzip2LargeFile) {
    std::string arc = tmp_dir_ + "/large.bz2";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(large_);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto data = r->read_entry("");
    r->close();
    ASSERT_TRUE(data.ok());
    EXPECT_EQ(data->size(), static_cast<size_t>(fs::file_size(large_)));
}

TEST_F(AdvancedTest, XzLargeFile) {
    std::string arc = tmp_dir_ + "/large.xz";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(large_);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto data = r->read_entry("");
    r->close();
    ASSERT_TRUE(data.ok());
    EXPECT_EQ(data->size(), static_cast<size_t>(fs::file_size(large_)));
}

TEST_F(AdvancedTest, ZstdLargeFile) {
    std::string arc = tmp_dir_ + "/large.zst";
    auto w = create_writer(arc);
    ASSERT_NE(w, nullptr);
    w->add_file(large_);
    w->close();

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    auto data = r->read_entry("");
    r->close();
    ASSERT_TRUE(data.ok());
    EXPECT_EQ(data->size(), static_cast<size_t>(fs::file_size(large_)));
}

// ====================================================================
// Single-stream backends: verify is_open() semantics
// ====================================================================
TEST_F(AdvancedTest, ReaderIsOpen) {
    std::string arc = tmp_dir_ + "/ropen.gz";
    {
        auto w = create_writer(arc);
        ASSERT_NE(w, nullptr);
        w->add_file(hello_);
        w->close();
    }

    auto r = create_reader(arc);
    ASSERT_NE(r, nullptr);
    EXPECT_TRUE(r->is_open());
    r->close();
}

// ====================================================================
// Create and immediately close writer (no files added)
// ====================================================================
TEST_F(AdvancedTest, CloseEmptyWriter) {
    auto w = create_writer(tmp_dir_ + "/empty_arc.zip");
    ASSERT_NE(w, nullptr);
    EXPECT_FALSE(w->close());  // close() should succeed even with no files
}

// ====================================================================
// Combined read+write test with different formats via factory
// ====================================================================
TEST_F(AdvancedTest, MultiFormatRoundtrip) {
    struct FormatTest {
        std::string ext;
        bool is_single;  // single-stream vs multi-entry
    };
    std::vector<FormatTest> formats = {
        {".zip", false},
        {".tar", false},
        {".gz",  true},
        {".bz2", true},
        {".xz",  true},
        {".zst", true},
    };

    for (auto& ft : formats) {
        std::string arc = tmp_dir_ + "/roundtrip" + ft.ext;
        auto w = create_writer(arc);
        ASSERT_NE(w, nullptr) << ft.ext;
        EXPECT_FALSE(w->add_file(hello_));
        if (!ft.is_single) {
            EXPECT_FALSE(w->add_file(lorem_));
        }
        w->close();
        EXPECT_TRUE(fs::exists(arc)) << ft.ext;

        auto r = create_reader(arc);
        ASSERT_NE(r, nullptr) << ft.ext;
        auto entries = r->entries();
        ASSERT_TRUE(entries.ok()) << ft.ext;

        if (ft.is_single) {
            EXPECT_EQ(entries->size(), 1U);
        } else {
            EXPECT_GE(entries->size(), 2U);
        }
        r->close();
    }
}

// ====================================================================
// Resolve format with explicit hint (override extension)
// ====================================================================
TEST_F(AdvancedTest, ExplicitFormatHint) {
    std::string arc = tmp_dir_ + "/override.xyz";
    ArchiveOptions opts;
    opts.format = Format::ZIP;

    auto w = create_writer(arc, opts);
    ASSERT_NE(w, nullptr);
    w->add_file(hello_);
    w->close();
    EXPECT_TRUE(fs::exists(arc));

    // Read with explicit format hint — should detect as ZIP
    ArchiveOptions ropts;
    ropts.format = Format::ZIP;
    auto r = create_reader(arc, ropts);
    ASSERT_NE(r, nullptr);
    auto entries = r->entries();
    EXPECT_TRUE(entries.ok());
    r->close();
}

// ====================================================================
// RAR file — read-only, writer returns nullptr
// ====================================================================
TEST_F(AdvancedTest, RarWriteReturnsNull) {
    auto w = create_writer(tmp_dir_ + "/test.rar");
    EXPECT_EQ(w, nullptr);
}

TEST_F(AdvancedTest, RarReadNonExistent) {
    auto r = create_reader(tmp_dir_ + "/nope.rar");
    // Reader creation may succeed (libarchive) but entries() should fail
    if (r) {
        auto entries = r->entries();
        EXPECT_FALSE(entries.ok());
        r->close();
    }
}
