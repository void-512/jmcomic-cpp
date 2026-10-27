#pragma once
#include "jmcomic/models.hpp"
#include "jmcomic/crypto.hpp"
namespace jm {
struct Image { int width{}, height{}; Bytes rgba; };
unsigned strip_count(Id scramble_id, Id chapter_id, const std::string& stem);
Image decode_image(std::span<const unsigned char> bytes);
void unscramble(Image&, unsigned strips);
Bytes encode_png(const Image& image);
bool verified_page(const fs::path& output, const Json& identity);
void save_page(const fs::path& output, const Image& image, const Json& identity);
}
