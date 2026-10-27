#include "jmcomic/storage.hpp"
#include <atomic>
#include <fstream>
#include <system_error>
#include <cstdlib>
#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif
namespace jm {
fs::path executable_directory(const char* argv0) {
#ifdef __linux__
  return fs::canonical("/proc/self/exe").parent_path();
#elif defined(_WIN32)
  std::wstring buffer(32768, L'\0');
  auto length = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
  if (!length || length == buffer.size()) throw std::runtime_error("Cannot locate executable");
  buffer.resize(length); return fs::path(buffer).parent_path();
#elif defined(__APPLE__)
  std::uint32_t size=0; _NSGetExecutablePath(nullptr, &size);
  std::string path(size, '\0');
  if (_NSGetExecutablePath(path.data(), &size)) throw std::runtime_error("Cannot locate executable");
  return fs::canonical(path.c_str()).parent_path();
#else
  return fs::canonical(argv0).parent_path();
#endif
  (void)argv0;
}
Bytes read_bytes(const fs::path& path, std::size_t limit) {
  const auto size = fs::file_size(path);
  if (size > limit) throw std::runtime_error("File exceeds size limit: " + path.string());
  Bytes bytes(static_cast<std::size_t>(size));
  std::ifstream file(path, std::ios::binary);
  if (!file || !file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())))
    throw std::runtime_error("Cannot read file: " + path.string());
  return bytes;
}
Json read_json(const fs::path& path) { auto bytes=read_bytes(path, 4*1024*1024); return Json::parse(bytes); }
void atomic_write(const fs::path& path, std::span<const unsigned char> bytes, bool private_file) {
  static std::atomic_uint64_t counter{};
  // A sibling is necessary for atomic rename, including across filesystem boundaries.
  auto temp = path;
#if defined(__unix__) || defined(__APPLE__)
  temp += "." + std::to_string(getpid()) + "." + std::to_string(counter++) + ".part";
#else
  temp += "." + std::to_string(counter++) + ".part";
#endif
  try {
#if defined(__unix__) || defined(__APPLE__)
    int fd = ::open(temp.c_str(), O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC, private_file ? 0600 : 0644);
    if (fd < 0) throw std::system_error(errno, std::generic_category(), "Cannot create " + temp.string());
    bool closed = false;
    try {
      std::size_t offset{};
      while (offset < bytes.size()) {
        const auto n = ::write(fd, bytes.data()+offset, bytes.size()-offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::system_error(errno, std::generic_category(), "Cannot write output");
        offset += static_cast<std::size_t>(n);
      }
      if (::fsync(fd)) throw std::system_error(errno, std::generic_category(), "Cannot flush output");
      closed = true;
      if (::close(fd)) throw std::system_error(errno, std::generic_category(), "Cannot close output");
    } catch (...) { if (!closed) ::close(fd); throw; }
#else
    std::ofstream stream(temp, std::ios::binary|std::ios::trunc);
    stream.write(reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    stream.close(); if (!stream) throw std::runtime_error("Cannot write output: " + temp.string());
    (void)private_file;
#endif
    fs::rename(temp, path);
  } catch (...) { std::error_code ec; fs::remove(temp, ec); throw; }
}
void atomic_json(const fs::path& path, const Json& j, bool priv) {
  const auto data = j.dump(2);
  atomic_write(path, {reinterpret_cast<const unsigned char*>(data.data()), data.size()}, priv);
}
Settings load_settings(const fs::path& directory) {
  Settings s; s.destination = directory / "Downloads";
  const auto path = directory / "jmcomic.json";
  if (!fs::exists(path)) return s;
  const auto j=read_json(path);
  s.api_url = j.value("api_url", s.api_url); s.image_url = j.value("image_url", s.image_url);
  s.destination = j.value("destination", s.destination.string());
  s.concurrency = j.value("concurrency", s.concurrency); s.timeout_seconds = j.value("timeout_seconds", s.timeout_seconds);
  s.retries = j.value("retries", s.retries); s.request_interval_ms = j.value("request_interval_ms", s.request_interval_ms);
  s.proxy = j.value("proxy", s.proxy); s.validate(); return s;
}
void save_settings(const fs::path& directory, const Settings& s) {
  s.validate();
  atomic_json(directory / "jmcomic.json", {{"api_url",s.api_url},{"image_url",s.image_url},{"destination",fs::absolute(s.destination).string()},
    {"concurrency",s.concurrency},{"timeout_seconds",s.timeout_seconds},{"retries",s.retries},
    {"request_interval_ms",s.request_interval_ms},{"proxy",s.proxy}}, true);
}
}
