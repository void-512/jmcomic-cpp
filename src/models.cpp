#include "jmcomic/models.hpp"
#include <algorithm>
#include <charconv>
#include <cctype>
#include <iomanip>
#include <regex>
#include <sstream>
namespace jm {
void Settings::validate(bool allow_loopback) const {
  const std::regex https(R"(^https://[A-Za-z0-9.-]+(:[0-9]{1,5})?$)");
  const std::regex local(R"(^http://127\.0\.0\.1:[0-9]{1,5}$)");
  for (const auto* url : {&api_url, &image_url})
    if (!std::regex_match(*url, https) && !(allow_loopback && std::regex_match(*url, local)))
      throw std::runtime_error("API and image URLs must be HTTPS origins (no path, credentials, or trailing slash)");
  if (concurrency < 1 || concurrency > 8 || retries < 0 || retries > 5 || timeout_seconds < 5 ||
      timeout_seconds > 180 || request_interval_ms < 100 || request_interval_ms > 10000)
    throw std::runtime_error("Settings: workers 1–8, retries 0–5, timeout 5–180s, interval 100–10000ms");
  if (destination.empty()) throw std::runtime_error("Choose a download destination");
  if (!proxy.empty() && !std::regex_match(proxy, std::regex(R"(^(https?|socks5h?)://[^\s]+$)")))
    throw std::runtime_error("Proxy must use http://, https://, socks5:// or socks5h://");
}
std::string json_text(const Json& v) {
  if (v.is_string()) return v.get<std::string>();
  if (v.is_number()) return v.dump();
  if (v.is_array()) {
    std::string out;
    for (const auto& item : v) { if (!out.empty()) out += ", "; out += json_text(item); }
    return out;
  }
  return {};
}
Id parse_id(const std::string& value) {
  Id id{};
  auto [end, error] = std::from_chars(value.data(), value.data()+value.size(), id);
  if (error != std::errc{} || end != value.data()+value.size() || id <= 0)
    throw std::runtime_error("Expected a positive numeric comic or chapter ID");
  return id;
}
Id json_id(const Json& v) { return parse_id(json_text(v)); }
Comic parse_comic(const Json& d) {
  Comic c;
  c.id = json_id(d.at("id"));
  c.name = json_text(d.at("name"));
  c.author = json_text(d.value("author", Json{}));
  c.description = json_text(d.value("description", Json{}));
  c.views = json_text(d.value("total_views", Json{}));
  c.favorites = json_text(d.value("likes", Json{}));
  for (const auto& s : d.value("series", Json::array())) {
    int order{};
    const auto sort=json_text(s.at("sort"));
    const auto [end,ec]=std::from_chars(sort.data(),sort.data()+sort.size(),order);
    if (ec!=std::errc{} || end!=sort.data()+sort.size() || order<0)
      throw std::runtime_error("Invalid chapter order in API response");
    c.chapters.push_back({json_id(s.at("id")), json_text(s.value("name", Json{})), order});
  }
  std::stable_sort(c.chapters.begin(), c.chapters.end(), [](const auto& a, const auto& b){ return a.order < b.order; });
  if (c.chapters.empty()) c.chapters.push_back({c.id, c.name, 1});
  for (auto& ch : c.chapters)
    ch.name = std::to_string(ch.order) + (ch.name.empty() ? "" : " - " + ch.name);
  return c;
}
Listing parse_listing(const Json& d, bool favorites) {
  Listing l;
  if (d.contains("redirect_aid")) { l.redirect = json_id(d.at("redirect_aid")); return l; }
  const auto total = json_text(d.value("total", Json{0}));
  const auto [end, ec] = std::from_chars(total.data(), total.data()+total.size(), l.total);
  if (ec != std::errc{} || end != total.data()+total.size() || l.total < 0)
    throw std::runtime_error("Invalid listing total in API response");
  for (const auto& c : d.at(favorites ? "list" : "content"))
    l.comics.push_back({json_id(c.at("id")), json_text(c.at("name")), json_text(c.value("author", Json{}))});
  for (const auto& f : d.value("folder_list", Json::array())) {
    const auto fid = json_text(f.at("FID"));
    l.folders.push_back({fid == "0" ? 0 : parse_id(fid), json_text(f.at("name"))});
  }
  return l;
}
std::string display_text(std::string s) {
  for (char& c : s) if (static_cast<unsigned char>(c) < 32 || c == 127) c = ' ';
  return s;
}
std::string safe_name(std::string s) {
  s = display_text(std::move(s));
  for (char& c : s) if (std::string_view("<>:\"/\\|?*").find(c) != std::string_view::npos) c = '_';
  while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
  while (!s.empty() && (s.front() == ' ' || s.front() == '.')) s.erase(s.begin());
  if (s.size() > 140) {
    std::size_t n = 140;
    while (n && (static_cast<unsigned char>(s[n]) & 0xc0) == 0x80) --n;
    s.resize(n);
  }
  while (!s.empty() && (s.back()=='.' || s.back()==' ')) s.pop_back();
  if (s.empty()) s = "Untitled";
  auto prefix = s.substr(0, s.find('.'));
  std::transform(prefix.begin(), prefix.end(), prefix.begin(), [](unsigned char c){ return static_cast<char>(std::toupper(c)); });
  if (prefix == "CON" || prefix == "PRN" || prefix == "AUX" || prefix == "NUL" ||
      std::regex_match(prefix, std::regex("(COM|LPT)[1-9]"))) s.insert(s.begin(), '_');
  return s;
}
std::string page_name(std::size_t index) {
  std::ostringstream s; s << std::setw(3) << std::setfill('0') << index + 1 << ".png"; return s.str();
}
}
