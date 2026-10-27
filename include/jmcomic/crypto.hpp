#pragma once
#include <cstdint>
#include <span>
#include <string>
#include <vector>
namespace jm {
using Bytes = std::vector<unsigned char>;
std::string md5(const std::string& text);
std::string sha256(std::span<const unsigned char> bytes);
std::string decrypt_data(std::uint64_t timestamp, const std::string& encoded);
}
