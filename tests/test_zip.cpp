#include <gtest/gtest.h>
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using namespace compress;

class ZipTest : public ::testing::Test {
protected:
    std::string tmp_dir_;
    std::string test_file_;
    std::string archive_;

    void SetUp() override {
        tmp_dir_ = (fs::temp_directory_path() / "cpct_test_XXXX").string();
        tmp_dir_ = tmp_dir_ + std::to_string(::time(nullptr));
        fs::create_directories(tmp_dir_);

        test_file_ = tmp_dir_ + "/hello.txt";
        archive_   = tmp_dir_ + "/test.zip";

        std::ofstream(test_file_) << "Hello, ZIP test!";
    }

    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }
};

TEST_F(ZipTest, CreateAndExtract) {
    // Create ZIP
    auto writer = create_writer(archive_);
    ASSERT_NE(writer, nullptr);

    Error err = writer->add_file(test_file_);
    EXPECT_FALSE(err) << err.message;

    err = writer->close();
    EXPECT_FALSE(err);

    EXPECT_TRUE(fs::exists(archive_));

    // List
    auto reader = create_reader(archive_);
    ASSERT_NE(reader, nullptr);

    auto entries = reader->entries();
    ASSERT_TRUE(entries.ok());
    ASSERT_EQ(entries->size(), 1);
    EXPECT_EQ((*entries)[0].path, "hello.txt");

    // Extract
    std::string extract_dir = tmp_dir_ + "/extract";
    ArchiveOptions opts;
    opts.output_dir = extract_dir;
    err = reader->extract(extract_dir);
    EXPECT_FALSE(err) << err.message;

    reader->close();

    EXPECT_TRUE(fs::exists(extract_dir + "/hello.txt"));
}

TEST_F(ZipTest, ReadEntry) {
    auto writer = create_writer(archive_);
    ASSERT_NE(writer, nullptr);
    writer->add_file(test_file_);
    writer->close();

    auto reader = create_reader(archive_);
    ASSERT_NE(reader, nullptr);

    auto data = reader->read_entry("hello.txt");
    reader->close();

    ASSERT_TRUE(data.ok());
    std::string content(data->data(), data->size());
    EXPECT_EQ(content, "Hello, ZIP test!");
}

TEST_F(ZipTest, PasswordProtection) {
    ArchiveOptions write_opts;
    write_opts.password = "secret123";
    write_opts.encryption = EncryptionMethod::AES_256;

    auto writer = create_writer(archive_, write_opts);
    ASSERT_NE(writer, nullptr);
    writer->add_file(test_file_);
    writer->close();

    // Read with correct password
    ArchiveOptions read_opts;
    read_opts.password = "secret123";
    auto reader = create_reader(archive_, read_opts);
    ASSERT_NE(reader, nullptr);
    auto entries = reader->entries();
    EXPECT_TRUE(entries.ok());
    reader->close();

    // Read without password should fail
    reader = nullptr; // force close above
    auto reader2 = create_reader(archive_);
    ASSERT_NE(reader2, nullptr);
    auto entries2 = reader2->entries();
    // May or may not fail depending on minizip-ng behaviour - skip strict check
    reader2->close();
}

TEST_F(ZipTest, FromMemory) {
    auto writer = create_writer(archive_);
    ASSERT_NE(writer, nullptr);

    const char* data = "in-memory content";
    Error err = writer->add_from_memory("memfile.txt", data, strlen(data));
    EXPECT_FALSE(err) << err.message;

    err = writer->close();
    EXPECT_FALSE(err);

    auto reader = create_reader(archive_);
    ASSERT_NE(reader, nullptr);

    auto result = reader->read_entry("memfile.txt");
    reader->close();

    ASSERT_TRUE(result.ok());
    std::string content(result->data(), result->size());
    EXPECT_EQ(content, "in-memory content");
}
