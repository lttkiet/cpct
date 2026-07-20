#include <gtest/gtest.h>
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace compress;

class XzTest : public ::testing::Test {
protected:
    std::string tmp_dir_;
    std::string test_file_;
    std::string archive_;

    void SetUp() override {
        tmp_dir_ = (fs::temp_directory_path() / "cpct_xz_test").string();
        fs::create_directories(tmp_dir_);
        test_file_ = tmp_dir_ + "/hello.txt";
        archive_   = tmp_dir_ + "/test.xz";
        std::ofstream(test_file_) << "Hello, XZ test!";
    }

    void TearDown() override {
        fs::remove_all(tmp_dir_);
    }
};

TEST_F(XzTest, CreateAndExtract) {
    auto writer = create_writer(archive_);
    ASSERT_NE(writer, nullptr);
    Error err = writer->add_file(test_file_);
    EXPECT_FALSE(err) << err.message;
    writer->close();
    EXPECT_TRUE(fs::exists(archive_));

    std::string extract_dir = tmp_dir_ + "/extract";
    auto reader = create_reader(archive_);
    ASSERT_NE(reader, nullptr);
    err = reader->extract(extract_dir);
    EXPECT_FALSE(err) << err.message;
    reader->close();
}

TEST_F(XzTest, ReadEntry) {
    auto writer = create_writer(archive_);
    ASSERT_NE(writer, nullptr);
    writer->add_file(test_file_);
    writer->close();

    auto reader = create_reader(archive_);
    ASSERT_NE(reader, nullptr);
    auto data = reader->read_entry("");
    reader->close();
    ASSERT_TRUE(data.ok());
    std::string content(data->data(), data->size());
    EXPECT_EQ(content, "Hello, XZ test!");
}
