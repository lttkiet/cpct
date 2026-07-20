#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

inline fs::path test_data_dir() {
    return fs::path(__FILE__).parent_path() / "data";
}

inline std::string read_file(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in),
                       std::istreambuf_iterator<char>());
}

inline std::vector<char> read_file_bytes(const std::string& path) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    auto size = in.tellg();
    in.seekg(0);
    std::vector<char> data(static_cast<size_t>(size));
    in.read(data.data(), size);
    return data;
}
