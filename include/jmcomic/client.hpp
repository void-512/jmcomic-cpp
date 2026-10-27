#pragma once
#include "jmcomic/http.hpp"
namespace jm {
class Client {
 public:
  Client(Settings settings, fs::path directory);
  Listing search(const std::string&, int page, const std::string& sort, std::stop_token = {});
  Comic comic(Id, std::stop_token = {});
  Chapter chapter(Id, std::stop_token = {});
  std::string login(const std::string& user, const std::string& password, std::stop_token = {});
  std::string profile(std::stop_token = {});
  void logout();
  bool has_session() const;
  Listing favorites(int page, Id folder, std::stop_token = {});
  Bytes image(Id chapter, const std::string& filename, std::stop_token = {});
  const Settings& settings() const { return http_.settings(); }
 private:
  Json request(const std::string& path, const std::vector<std::pair<std::string,std::string>>& params,
               bool post, std::stop_token stop, bool auth = false);
  void persist_session();
  Http http_;
  fs::path session_file_;
  mutable std::recursive_mutex api_mutex_;
  std::vector<std::string> cookies_;
};
}
