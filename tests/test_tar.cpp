#include <gtest/gtest.h>
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;

using namespace compress;

class TarTest : public ::testing::Test {
protected:
    std::string tmp_dir_;
    std::string test_file_;
    std::string archive_;

    void SetUp() override {
        tmp_dir_ = (fs::temp_directory_path() / "cpct_tar_test").string();
        tmp_dir_ = tmp_dir_ + std::to_string(::time(nullptr));
        fs::create_directories(tmp_dir_);

        test_file_ = tmp_dir_ + "/hello.txt";
        archive_   = tmp_dir_ + "/test.tar";

        std::ofstream(test_file_) << "Hello, TAR test!";
    }

    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }
};

TEST_F(TarTest, CreateAndExtract) {
    auto writer = create_writer(archive_);
    ASSERT_NE(writer, nullptr);

    Error err = writer->add_file(test_file_);
    EXPECT_FALSE(err) << err.message;
    writer->close();

    EXPECT_TRUE(fs::exists(archive_));

    // List
    auto reader = create_reader(archive_);
    ASSERT_NE(reader, nullptr);

    auto entries = reader->entries();
    ASSERT_TRUE(entries.ok());
    ASSERT_EQ(entries->size(), 1);
    EXPECT_EQ((*entries)[0].path, "hello.txt");

    // Extract
    std::string extract_dir = tmp_dir_ + "/extract_tar";
    err = reader->extract(extract_dir);
    EXPECT_FALSE(err) << err.message;
    reader->close();

    EXPECT_TRUE(fs::exists(extract_dir + "/hello.txt"));
}

TEST_F(TarTest, ReadEntry) {
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
    EXPECT_EQ(content, "Hello, TAR test!");
}

TEST_F(TarTest, CompressedTarGz) {
    std::string tgz_archive = tmp_dir_ + "/test.tar.gz";
    std::string tgz_file = tmp_dir_ + "/sample.txt";
    std::ofstream(tgz_file) << "Hello, compressed TAR!";

    auto writer = create_writer(tgz_archive);
    ASSERT_NE(writer, nullptr);

    Error err = writer->add_file(tgz_file);
    EXPECT_FALSE(err) << err.message;
    writer->close();

    EXPECT_TRUE(fs::exists(tgz_archive));
    EXPECT_GT(fs::file_size(tgz_archive), 0);

    auto reader = create_reader(tgz_archive);
    ASSERT_NE(reader, nullptr);
    auto entries = reader->entries();
    ASSERT_TRUE(entries.ok());
    ASSERT_GE(entries->size(), 1);
    reader->close();
}
