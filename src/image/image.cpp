#include "jmcomic/image.hpp"
#include "jmcomic/storage.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include <webp/decode.h>
#include <webp/demux.h>
#define STBI_MAX_DIMENSIONS 32768
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_ONLY_GIF
#define STBI_NO_STDIO
#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>
namespace jm {
namespace {
constexpr std::uint64_t max_pixels=24'000'000;
void check_size(int w, int h) {
  if (w<=0 || h<=0 || w>32768 || h>32768 || static_cast<std::uint64_t>(w)*static_cast<unsigned>(h)>max_pixels)
    throw std::runtime_error("Invalid image dimensions or image exceeds 24 megapixel limit");
}
}
unsigned strip_count(Id scramble, Id id, const std::string& stem) {
  if (id<scramble) return 0;
  if (id<268850) return 10;
  const auto last=static_cast<unsigned char>(md5(std::to_string(id)+stem).back());
  return (last % (id<421926 ? 10U : 8U))*2U+2U;
}
Image decode_image(std::span<const unsigned char> bytes) {
  if (bytes.empty() || bytes.size()>64*1024*1024) throw std::runtime_error("Invalid image data size");
  Image image;
  if (bytes.size()>=12 && std::memcmp(bytes.data(),"RIFF",4)==0 && std::memcmp(bytes.data()+8,"WEBP",4)==0) {
    WebPBitstreamFeatures features{};
    if (WebPGetFeatures(bytes.data(),bytes.size(),&features)!=VP8_STATUS_OK) throw std::runtime_error("Invalid WebP header");
    check_size(features.width,features.height);
    image.width=features.width; image.height=features.height;
    image.rgba.resize(static_cast<std::size_t>(image.width)*static_cast<std::size_t>(image.height)*4);
    if (features.has_animation) {
      WebPData data{bytes.data(),bytes.size()}; WebPAnimDecoderOptions options;
      if (!WebPAnimDecoderOptionsInit(&options)) throw std::runtime_error("WebP decoder version mismatch");
      options.color_mode=MODE_RGBA; options.use_threads=0;
      std::unique_ptr<WebPAnimDecoder,decltype(&WebPAnimDecoderDelete)> decoder(WebPAnimDecoderNew(&data,&options),WebPAnimDecoderDelete);
      unsigned char* frame{}; int timestamp{};
      if (!decoder || !WebPAnimDecoderGetNext(decoder.get(),&frame,&timestamp)) throw std::runtime_error("Cannot decode animated WebP");
      std::copy_n(frame,image.rgba.size(),image.rgba.data());
    } else if (!WebPDecodeRGBAInto(bytes.data(),bytes.size(),image.rgba.data(),image.rgba.size(),image.width*4))
      throw std::runtime_error("WebP decoding failed");
  } else {
    int channels{};
    if (!stbi_info_from_memory(bytes.data(),static_cast<int>(bytes.size()),&image.width,&image.height,&channels))
      throw std::runtime_error("Unsupported or invalid image (expected WebP, JPEG, PNG or GIF)");
    check_size(image.width,image.height);
    std::unique_ptr<stbi_uc,decltype(&stbi_image_free)> pixels(stbi_load_from_memory(bytes.data(),static_cast<int>(bytes.size()),&image.width,&image.height,&channels,4),stbi_image_free);
    if (!pixels) throw std::runtime_error("Image decoding failed; response may be incomplete");
    image.rgba.assign(pixels.get(),pixels.get()+static_cast<std::size_t>(image.width)*static_cast<std::size_t>(image.height)*4);
  }
  return image;
}
void unscramble(Image& image, unsigned count) {
  check_size(image.width,image.height);
  const auto stride=static_cast<std::size_t>(image.width)*4;
  if (image.rgba.size()!=stride*static_cast<std::size_t>(image.height)) throw std::runtime_error("Invalid image buffer");
  if (count==0) return;
  if (count>20) throw std::runtime_error("Invalid scramble strip count");
  Bytes output(image.rgba.size());
  const auto height=static_cast<unsigned>(image.height), base=height/count, remainder=height%count;
  for (unsigned i=0;i<count;++i) {
    const auto source=height-base*(i+1)-remainder;
    const auto dest=base*i+(i==0 ? 0 : remainder);
    const auto lines=base+(i==0 ? remainder : 0);
    std::copy_n(image.rgba.data()+source*stride,lines*stride,output.data()+dest*stride);
  }
  image.rgba=std::move(output);
}
Bytes encode_png(const Image& image) {
  check_size(image.width,image.height);
  if (image.rgba.size()!=static_cast<std::size_t>(image.width)*static_cast<std::size_t>(image.height)*4)
    throw std::runtime_error("Invalid PNG image buffer");
  int size{};
  std::unique_ptr<unsigned char,decltype(&std::free)> data(
    stbi_write_png_to_mem(image.rgba.data(),image.width*4,image.width,image.height,4,&size),std::free);
  if (!data || size<=0) throw std::runtime_error("PNG encoding failed");
  return Bytes(data.get(),data.get()+size);
}
bool verified_page(const fs::path& output, const Json& identity) {
  try {
    const auto record=read_json(output.string()+".json");
    if (record.at("identity")!=identity || record.value("version",0)!=1) return false;
    const auto bytes=read_bytes(output,128*1024*1024);
    if (sha256(bytes)!=record.at("sha256").get<std::string>()) return false;
    // The digest authenticates an output previously produced by our PNG encoder.
    return bytes.size()>32 && std::memcmp(bytes.data(),"\x89PNG\r\n\x1a\n",8)==0;
  } catch (...) { return false; }
}
void save_page(const fs::path& path, const Image& image, const Json& identity) {
  auto bytes=encode_png(image);
  auto record=Json{{"version",1},{"identity",identity},{"sha256",sha256(bytes)},{"width",image.width},{"height",image.height}};
  atomic_write(path,bytes);
  atomic_json(path.string()+".json",record);
}
}
