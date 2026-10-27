#pragma once
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>
#include <nlohmann/json.hpp>
namespace jm {
using Json = nlohmann::json;
namespace fs = std::filesystem;
using Id = std::int64_t;
struct Cancelled : std::runtime_error { Cancelled() : runtime_error("Cancelled") {} };
struct AuthError : std::runtime_error { using runtime_error::runtime_error; };
struct Settings {
  std::string api_url = "https://www.cdnhth.net";
  std::string image_url = "https://cdn-msp2.jmapiproxy2.cc";
  fs::path destination;
  int concurrency = 4;
  int timeout_seconds = 30;
  int retries = 3;
  int request_interval_ms = 250;
  std::string proxy; // Empty explicitly disables environment proxy settings.
  void validate(bool allow_loopback = false) const;
};
struct ChapterInfo { Id id{}; std::string name; int order{}; };
struct Comic {
  Id id{};
  std::string name, author, description, views, favorites;
  std::vector<ChapterInfo> chapters;
};
struct ComicSummary { Id id{}; std::string name, author; };
struct Folder { Id id{}; std::string name; };
struct Listing {
  std::vector<ComicSummary> comics;
  std::vector<Folder> folders;
  std::int64_t total{};
  Id redirect{};
};
struct Chapter { Id id{}; std::string name; std::vector<std::string> images; Id scramble_id{}; };
std::string json_text(const Json& value);
Id json_id(const Json& value);
Id parse_id(const std::string& value);
Comic parse_comic(const Json& data);
Listing parse_listing(const Json& data, bool favorites);
std::string safe_name(std::string name);
std::string display_text(std::string text);
std::string page_name(std::size_t index);
}
