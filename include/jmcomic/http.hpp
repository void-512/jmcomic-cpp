#pragma once
#include "jmcomic/models.hpp"
#include "jmcomic/crypto.hpp"
#include <chrono>
#include <array>
#include <condition_variable>
#include <mutex>
#include <stop_token>
namespace jm {
struct HttpResponse { Bytes body; std::vector<std::string> cookies; };
struct HttpError : std::runtime_error {
  long status;
  int retry_after;
  bool transient;
  HttpError(std::string message, long code, int delay, bool retryable)
    : runtime_error(std::move(message)), status(code), retry_after(delay), transient(retryable) {}
};
struct HttpRequest {
  std::string url, form;
  bool post = false;
  bool image = false;
  std::vector<std::string> headers, cookies;
  std::size_t limit = 64 * 1024 * 1024;
};
class Http {
 public:
  explicit Http(Settings settings);
  HttpResponse perform(const HttpRequest&, std::stop_token stop = {});
  const Settings& settings() const { return settings_; }
  static std::string escape(const std::string&);
  static void wait(std::stop_token, std::chrono::milliseconds);
 private:
  Settings settings_;
  std::mutex rate_mutex_;
  std::condition_variable_any rate_ready_;
  std::chrono::steady_clock::time_point next_request_{};
  std::array<std::chrono::steady_clock::time_point,8> next_image_requests_{};
  std::chrono::steady_clock::time_point cooldown_{};
};
}
