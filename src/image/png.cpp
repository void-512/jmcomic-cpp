#include "jmcomic/image.hpp"
#include "validation.hpp"
#include <algorithm>
#include <array>
#include <cstdlib>
#include <string_view>
#include <zlib-ng.h>

namespace jm {
namespace {
void append_u32(Bytes& output, std::uint32_t value) {
  for (int shift = 24; shift >= 0; shift -= 8)
    output.push_back(static_cast<unsigned char>(value >> shift));
}

void append_chunk(Bytes& output, std::string_view type, std::span<const unsigned char> data) {
  append_u32(output, static_cast<std::uint32_t>(data.size()));
  const auto start = output.size();
  output.insert(output.end(), type.begin(), type.end());
  if (!data.empty()) output.insert(output.end(), data.begin(), data.end());
  append_u32(output, zng_crc32_z(0, output.data() + start, type.size() + data.size()));
}

int paeth(int left, int above, int upper_left) {
  const auto prediction = left + above - upper_left;
  const auto a = std::abs(prediction - left);
  const auto b = std::abs(prediction - above);
  const auto c = std::abs(prediction - upper_left);
  if (a <= b && a <= c) return left;
  return b <= c ? above : upper_left;
}

std::span<const unsigned char> filter_row(std::span<const unsigned char> row,
                                        std::span<const unsigned char> previous, Bytes& scratch) {
  const auto size = row.size() + 1;
  std::array<std::uint64_t, 5> scores{};
  for (std::size_t filter = 0; filter < scores.size(); ++filter)
    scratch[filter * size] = static_cast<unsigned char>(filter);

  for (std::size_t i = 0; i < row.size(); ++i) {
    const int left = i >= 4 ? row[i - 4] : 0;
    const int above = previous.empty() ? 0 : previous[i];
    const int upper_left = !previous.empty() && i >= 4 ? previous[i - 4] : 0;
    const std::array<int, 5> predictions{0, left, above, (left + above) / 2, paeth(left, above, upper_left)};
    for (std::size_t filter = 0; filter < scores.size(); ++filter) {
      const auto value = static_cast<unsigned char>(row[i] - predictions[filter]);
      scratch[filter * size + i + 1] = value;
      scores[filter] += static_cast<unsigned>(std::min<int>(value, 256 - value));
    }
  }
  const auto best = static_cast<std::size_t>(std::min_element(scores.begin(), scores.end()) - scores.begin());
  return {scratch.data() + best * size, size};
}

class PngCompressor {
 public:
  PngCompressor() {
    if (zng_deflateInit(&stream_, Z_DEFAULT_COMPRESSION) != Z_OK)
      throw std::runtime_error("Cannot initialize PNG compression");
  }
  ~PngCompressor() { zng_deflateEnd(&stream_); }
  PngCompressor(const PngCompressor&) = delete;
  PngCompressor& operator=(const PngCompressor&) = delete;

  void write(Bytes& output, std::span<const unsigned char> input, int flush) {
    stream_.next_in = input.data();
    stream_.avail_in = static_cast<std::uint32_t>(input.size());
    int status;
    do {
      stream_.next_out = buffer_.data();
      stream_.avail_out = static_cast<std::uint32_t>(buffer_.size());
      status = zng_deflate(&stream_, flush);
      if (status != Z_OK && status != Z_STREAM_END)
        throw std::runtime_error("PNG compression failed");
      const auto size = buffer_.size() - stream_.avail_out;
      if (size) append_chunk(output, "IDAT", {buffer_.data(), size});
    } while (stream_.avail_in || (flush == Z_FINISH && status != Z_STREAM_END));
  }

 private:
  zng_stream stream_{};
  std::array<unsigned char, 64 * 1024> buffer_{};
};
}

Bytes encode_png(const Image& image) {
  detail::check_size(image.width, image.height);
  const auto stride = static_cast<std::size_t>(image.width) * 4;
  const auto height = static_cast<std::size_t>(image.height);
  if (image.rgba.size() != stride * height)
    throw std::runtime_error("Invalid PNG image buffer");

  Bytes output{137, 80, 78, 71, 13, 10, 26, 10};
  Bytes header;
  append_u32(header, static_cast<std::uint32_t>(image.width));
  append_u32(header, static_cast<std::uint32_t>(image.height));
  header.insert(header.end(), {8, 6, 0, 0, 0}); // 8-bit RGBA, no interlacing.
  append_chunk(output, "IHDR", header);

  PngCompressor compressor;
  Bytes scratch(5 * (stride + 1));
  const std::span<const unsigned char> pixels(image.rgba);
  for (std::size_t y = 0; y < height; ++y) {
    const auto row = pixels.subspan(y * stride, stride);
    const auto previous = y ? pixels.subspan((y - 1) * stride, stride) : std::span<const unsigned char>{};
    compressor.write(output, filter_row(row, previous, scratch), Z_NO_FLUSH);
  }
  compressor.write(output, {}, Z_FINISH);
  append_chunk(output, "IEND", {});
  return output;
}
}
