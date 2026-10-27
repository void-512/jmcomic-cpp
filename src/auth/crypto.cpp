#include "jmcomic/crypto.hpp"
#include <openssl/evp.h>
#include <memory>
#include <stdexcept>
#include <limits>
namespace jm {
namespace {
std::string digest(const EVP_MD* algorithm, std::span<const unsigned char> data) {
  unsigned char result[EVP_MAX_MD_SIZE]; unsigned length{};
  if (!EVP_Digest(data.data(), data.size(), result, &length, algorithm, nullptr))
    throw std::runtime_error("Cryptographic digest failed");
  constexpr char hex[] = "0123456789abcdef";
  std::string out; out.reserve(length*2);
  for (unsigned i=0; i<length; ++i) { out += hex[result[i] >> 4]; out += hex[result[i] & 15]; }
  return out;
}
}
std::string md5(const std::string& t) { return digest(EVP_md5(), {reinterpret_cast<const unsigned char*>(t.data()), t.size()}); }
std::string sha256(std::span<const unsigned char> b) { return digest(EVP_sha256(), b); }
std::string decrypt_data(std::uint64_t ts, const std::string& encoded) {
  if (encoded.empty() || encoded.size()%4 || encoded.size() > 16*1024*1024)
    throw std::runtime_error("Invalid encrypted response size");
  Bytes cipher(encoded.size()/4*3);
  int n = EVP_DecodeBlock(cipher.data(), reinterpret_cast<const unsigned char*>(encoded.data()), static_cast<int>(encoded.size()));
  if (n < 0) throw std::runtime_error("Invalid response base64");
  if (encoded.back() == '=') --n;
  if (encoded[encoded.size()-2] == '=') --n;
  if (n <= 0 || n%16) throw std::runtime_error("Invalid encrypted response blocks");
  const auto key = md5(std::to_string(ts) + "185Hcomic3PAPP7R");
  std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
  if (!ctx || EVP_DecryptInit_ex(ctx.get(), EVP_aes_256_ecb(), nullptr,
      reinterpret_cast<const unsigned char*>(key.data()), nullptr) != 1)
    throw std::runtime_error("Could not initialize response decryption");
  std::string plain(static_cast<std::size_t>(n)+16, '\0'); int written{}, tail{};
  if (EVP_DecryptUpdate(ctx.get(), reinterpret_cast<unsigned char*>(plain.data()), &written, cipher.data(), n) != 1 ||
      EVP_DecryptFinal_ex(ctx.get(), reinterpret_cast<unsigned char*>(plain.data())+written, &tail) != 1)
    throw std::runtime_error("Response decryption failed (invalid key or padding)");
  plain.resize(static_cast<std::size_t>(written+tail)); return plain;
}
}
