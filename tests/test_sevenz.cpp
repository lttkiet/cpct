#include <gtest/gtest.h>
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace compress;

class SevenZTest : public ::testing::Test {
protected:
    std::string tmp_dir_;
    std::string test_file_;
    std::string archive_;

    void SetUp() override {
        tmp_dir_ = (fs::temp_directory_path() / "cpct_7z_test").string();
        fs::create_directories(tmp_dir_);
        test_file_ = tmp_dir_ + "/hello.txt";
        archive_   = tmp_dir_ + "/test.7z";
        std::ofstream(test_file_) << "Hello, 7z test!";
    }

    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }
};

TEST_F(SevenZTest, CreateAndExtract) {
    auto writer = create_writer(archive_);
    if (!writer) {
        GTEST_SKIP() << "7z writer requires LZMA SDK (not available)";
        return;
    }

    Error err = writer->add_file(test_file_);
    EXPECT_FALSE(err) << err.message;
    writer->close();
    EXPECT_TRUE(fs::exists(archive_));

    auto reader = create_reader(archive_);
    ASSERT_NE(reader, nullptr);
    auto entries = reader->entries();
    ASSERT_TRUE(entries.ok());
    EXPECT_GE(entries->size(), 1);
    reader->close();
}

TEST_F(SevenZTest, ReadEntry) {
    auto writer = create_writer(archive_);
    if (!writer) {
        GTEST_SKIP() << "7z writer requires LZMA SDK (not available)";
        return;
    }

    writer->add_file(test_file_);
    writer->close();

    auto reader = create_reader(archive_);
    ASSERT_NE(reader, nullptr);
    auto data = reader->read_entry("hello.txt");
    reader->close();
    ASSERT_TRUE(data.ok());
    std::string content(data->data(), data->size());
    EXPECT_EQ(content, "Hello, 7z test!");
}
