#pragma once
#include <cstdint>
#include <stdexcept>

namespace jm::detail {
inline void check_size(int width, int height) {
  constexpr std::uint64_t max_pixels = 24'000'000;
  if (width <= 0 || height <= 0 || width > 32768 || height > 32768 ||
      static_cast<std::uint64_t>(width) * static_cast<unsigned>(height) > max_pixels)
    throw std::runtime_error("Invalid image dimensions or image exceeds 24 megapixel limit");
}
}
