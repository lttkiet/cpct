#include <gtest/gtest.h>
#include "compress/types.h"

#include <cstdio>
#include <fstream>
#include <string>

namespace compress {
    Format detect_format(const std::string& path);
    Format detect_format_from_extension(const std::string& path);
    Format detect_format_from_magic(const std::string& path);
    const char* format_name(Format fmt);
}

using namespace compress;

TEST(FormatDetection, ExtensionZip) {
    EXPECT_EQ(detect_format_from_extension("archive.zip"), Format::ZIP);
    EXPECT_EQ(detect_format_from_extension("path/to/file.ZIP"), Format::ZIP);
}

TEST(FormatDetection, ExtensionTar) {
    EXPECT_EQ(detect_format_from_extension("archive.tar"), Format::TAR);
    EXPECT_EQ(detect_format_from_extension("archive.tar.gz"), Format::TAR);
    EXPECT_EQ(detect_format_from_extension("archive.tgz"), Format::TAR);
    EXPECT_EQ(detect_format_from_extension("archive.tar.bz2"), Format::TAR);
    EXPECT_EQ(detect_format_from_extension("archive.tar.xz"), Format::TAR);
    EXPECT_EQ(detect_format_from_extension("archive.tar.zst"), Format::TAR);
}

TEST(FormatDetection, ExtensionSingle) {
    EXPECT_EQ(detect_format_from_extension("file.gz"), Format::GZIP);
    EXPECT_EQ(detect_format_from_extension("file.bz2"), Format::BZIP2);
    EXPECT_EQ(detect_format_from_extension("file.xz"), Format::XZ);
    EXPECT_EQ(detect_format_from_extension("file.zst"), Format::ZSTD);
}

TEST(FormatDetection, Extension7zRar) {
    EXPECT_EQ(detect_format_from_extension("archive.7z"), Format::SEVEN_ZIP);
    EXPECT_EQ(detect_format_from_extension("archive.rar"), Format::RAR);
}

TEST(FormatDetection, FormatName) {
    EXPECT_STREQ(format_name(Format::ZIP), "ZIP");
    EXPECT_STREQ(format_name(Format::TAR), "TAR");
    EXPECT_STREQ(format_name(Format::SEVEN_ZIP), "7z");
    EXPECT_STREQ(format_name(Format::AUTO), "AUTO");
}

TEST(FormatDetection, Auto) {
    EXPECT_EQ(detect_format_from_extension("unknown.xyz"), Format::AUTO);
    EXPECT_EQ(detect_format_from_extension("noext"), Format::AUTO);
}
