#include <gtest/gtest.h>
#include "compress/types.h"

using namespace compress;

TEST(TypesTest, ErrorOkByDefault) {
    Error e;
    EXPECT_FALSE(e);
    EXPECT_EQ(e.code, Error::OK);
    EXPECT_TRUE(e.message.empty());
}

TEST(TypesTest, ErrorMake) {
    Error e = Error::make(Error::FILE_NOT_FOUND, "test.txt");
    EXPECT_TRUE(e);
    EXPECT_EQ(e.code, Error::FILE_NOT_FOUND);
    EXPECT_EQ(e.message, "test.txt");
}

TEST(TypesTest, ErrorOk) {
    Error e = Error::ok();
    EXPECT_FALSE(e);
}

TEST(TypesTest, ResultOk) {
    Result<int> r{42, Error::ok()};
    EXPECT_TRUE(r.ok());
    EXPECT_EQ(*r, 42);
}

TEST(TypesTest, ResultError) {
    Result<int> r{0, Error::make(Error::FILE_NOT_FOUND)};
    EXPECT_FALSE(r.ok());
    EXPECT_TRUE(r.error);
}

TEST(TypesTest, ResultVoidOk) {
    Result<void> r{Error::ok()};
    EXPECT_TRUE(r.ok());
}

TEST(TypesTest, ParseSizeSuffix) {
    EXPECT_EQ(detail::parse_size_suffix("1024"), 1024ULL);
    EXPECT_EQ(detail::parse_size_suffix("1K"), 1024ULL);
    EXPECT_EQ(detail::parse_size_suffix("2M"), 2ULL * 1024 * 1024);
    EXPECT_EQ(detail::parse_size_suffix("1G"), 1ULL * 1024 * 1024 * 1024);
    EXPECT_EQ(detail::parse_size_suffix("0"), 0ULL);
}
