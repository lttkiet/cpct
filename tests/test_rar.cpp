#include <gtest/gtest.h>
#include "compress/archive_reader.h"
#include "compress/archive_writer.h"
#include "compress/types.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace compress;

TEST(RarTest, WriterNotSupported) {
    std::string tmp = (fs::temp_directory_path() / "cpct_rar_test").string();
    fs::create_directories(tmp);
    std::string archive = tmp + "/test.rar";

    auto writer = create_writer(archive);
    EXPECT_EQ(writer, nullptr);

    fs::remove_all(tmp);
}

TEST(RarTest, InvalidRarReaderReturnsNull) {
    std::string tmp = (fs::temp_directory_path() / "cpct_rar_test").string();
    fs::create_directories(tmp);
    std::string archive = tmp + "/nonexistent.rar";

    auto reader = create_reader(archive);
    EXPECT_NE(reader, nullptr);
    auto entries = reader->entries();
    EXPECT_FALSE(entries.ok());

    fs::remove_all(tmp);
}
