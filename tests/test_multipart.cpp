#include <gtest/gtest.h>
#include "compress/types.h"

#include <string>
#include <vector>

namespace compress {
    std::vector<std::string> find_multipart_parts(const std::string& primary_path);
    std::string multipart_filename(const std::string& base_path, int part_index,
                                    MultipartMode mode, const std::string& format_ext);
    namespace detail {
        uint64_t parse_size_suffix(const std::string& s);
    }
}

using namespace compress;

TEST(MultipartTest, ParseSizeSuffix) {
    EXPECT_EQ(detail::parse_size_suffix("1K"), 1024ULL);
    EXPECT_EQ(detail::parse_size_suffix("10M"), 10ULL * 1024 * 1024);
    EXPECT_EQ(detail::parse_size_suffix("2G"), 2ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(detail::parse_size_suffix("512"), 512ULL);
    EXPECT_EQ(detail::parse_size_suffix("0"), 0ULL);
    EXPECT_EQ(detail::parse_size_suffix(""), 0ULL);
}

TEST(MultipartTest, MultipartFilenames) {
    // ZIP split mode: first part
    std::string name = multipart_filename("archive.zip", 1, MultipartMode::SPLIT, ".zip");
    EXPECT_NE(name.find(".z01"), std::string::npos) << name;
    EXPECT_EQ(name, "archive.z01");

    // RAR volume mode
    std::string rarname = multipart_filename("archive.rar", 0, MultipartMode::RAR_VOLUME, ".rar");
    EXPECT_EQ(rarname, "archive.part1.rar");

    rarname = multipart_filename("archive.rar", 1, MultipartMode::RAR_VOLUME, ".rar");
    EXPECT_EQ(rarname, "archive.part2.rar");
}
