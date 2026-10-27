#pragma once
#include "jmcomic/models.hpp"
#include "jmcomic/crypto.hpp"
#include <span>
namespace jm {
fs::path executable_directory(const char* argv0);
Settings load_settings(const fs::path& directory);
void save_settings(const fs::path& directory, const Settings& settings);
Bytes read_bytes(const fs::path& path, std::size_t limit = 64 * 1024 * 1024);
void atomic_write(const fs::path& path, std::span<const unsigned char> data, bool private_file = false);
void atomic_json(const fs::path& path, const Json& json, bool private_file = false);
Json read_json(const fs::path& path);
}
