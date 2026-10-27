#include "jmcomic/client.hpp"
#include "jmcomic/storage.hpp"
#include <algorithm>
#include <ctime>
#include <regex>
namespace jm {
namespace {
std::string params_string(const std::vector<std::pair<std::string,std::string>>& params) {
  std::string s;
  for (const auto& [key,value] : params) { if (!s.empty()) s+='&'; s+=Http::escape(key)+"="+Http::escape(value); }
  return s;
}
bool retry_request(const HttpError& e, int attempt, int retries) { return e.transient && attempt<retries && e.retry_after<=60; }
}
Client::Client(Settings settings, fs::path directory)
  : http_(settings), session_file_(std::move(directory)/"jmcomic-session.json") {
  settings.validate();
  if (fs::exists(session_file_)) {
    const auto j=read_json(session_file_);
    if (j.value("origin",std::string{}) == settings.api_url) cookies_=j.at("cookies").get<std::vector<std::string>>();
#if defined(__unix__) || defined(__APPLE__)
    fs::permissions(session_file_,fs::perms::owner_read|fs::perms::owner_write,fs::perm_options::replace);
#endif
  }
}
void Client::persist_session() {
  if (cookies_.empty()) {
    std::error_code ec; fs::remove(session_file_,ec);
    if (ec) throw std::runtime_error("Cannot remove expired session file: "+ec.message());
    return;
  }
  atomic_json(session_file_,{{"origin",http_.settings().api_url},{"cookies",cookies_}},true);
}
bool Client::has_session() const { std::lock_guard lock(api_mutex_); return !cookies_.empty(); }
Json Client::request(const std::string& path, const std::vector<std::pair<std::string,std::string>>& params,
                     bool post, std::stop_token stop, bool auth) {
  std::lock_guard lock(api_mutex_);
  for (int attempt=0;;++attempt) {
    const auto ts=static_cast<std::uint64_t>(std::time(nullptr));
    const auto encoded=params_string(params);
    HttpRequest request;
    request.url=http_.settings().api_url+path;
    request.post=post; request.cookies=cookies_; request.limit=16*1024*1024;
    request.headers={"token: "+md5(std::to_string(ts)+(path=="/chapter_view_template" ? "18comicAPPContent" : "18comicAPP")),
                     "tokenparam: "+std::to_string(ts)+",2.0.13"};
    if (post) request.form=encoded;
    else if (!encoded.empty()) request.url+="?"+encoded;
    try {
      auto response=http_.perform(request,stop);
      if (path=="/chapter_view_template") return std::string(response.body.begin(),response.body.end());
      Json envelope;
      try { envelope=Json::parse(response.body); }
      catch (const Json::exception&) { throw std::runtime_error("API returned non-JSON data; check endpoint or service challenge"); }
      const auto code=json_text(envelope.value("code",Json{}));
      if (code != "200") {
        if (auth || path=="/login") {
          cookies_.clear(); std::error_code ec; fs::remove(session_file_,ec);
          throw AuthError("Authentication failed or session expired; log in again");
        }
        // Do not echo remote error bodies: they may contain credentials or tokens.
        throw std::runtime_error("API request rejected (code "+display_text(code).substr(0,12)+")");
      }
      if (!envelope.contains("data") || !envelope["data"].is_string())
        throw std::runtime_error("Unexpected API response format");
      Json data;
      try { data=Json::parse(decrypt_data(ts,envelope["data"].get<std::string>())); }
      catch (const Json::exception&) { throw std::runtime_error("Decrypted API data is not valid JSON"); }
      if (response.cookies != cookies_) {
        cookies_=std::move(response.cookies);
        persist_session();
      }
      return data;
    } catch (const HttpError& e) {
      if (auth && (e.status==401 || e.status==403)) {
        cookies_.clear(); std::error_code ec; fs::remove(session_file_,ec);
        throw AuthError("Session expired or access denied; log in again");
      }
      if (post || !retry_request(e,attempt,http_.settings().retries)) throw;
      Http::wait(stop,std::chrono::seconds(std::max(e.retry_after,1<<attempt)));
    }
  }
}
Listing Client::search(const std::string& query, int page, const std::string& sort, std::stop_token stop) {
  if (query.empty() || page<1 || (sort!="mr" && sort!="mv" && sort!="tf")) throw std::runtime_error("Invalid search parameters");
  return parse_listing(request("/search",{{"main_tag","0"},{"search_query",query},{"page",std::to_string(page)},{"o",sort}},false,stop),false);
}
Comic Client::comic(Id id, std::stop_token stop) {
  if (id<=0) throw std::runtime_error("Invalid comic ID");
  auto c=parse_comic(request("/album",{{"id",std::to_string(id)}},false,stop));
  if (c.id!=id) throw std::runtime_error("API returned a different comic ID");
  return c;
}
Chapter Client::chapter(Id id, std::stop_token stop) {
  if (id<=0) throw std::runtime_error("Invalid chapter ID");
  const auto d=request("/chapter",{{"id",std::to_string(id)}},false,stop);
  Chapter c{json_id(d.at("id")),json_text(d.at("name")),d.at("images").get<std::vector<std::string>>(),0};
  if (c.id!=id || c.images.empty()) throw std::runtime_error("Chapter unavailable or empty");
  if (d.contains("scramble_id")) c.scramble_id=json_id(d.at("scramble_id"));
  else {
    const auto html=request("/chapter_view_template",{{"id",std::to_string(id)},{"v",std::to_string(std::time(nullptr))},
      {"mode","vertical"},{"page","0"},{"app_img_shunt","1"},{"express","off"}},false,stop).get<std::string>();
    std::smatch match;
    if (!std::regex_search(html,match,std::regex(R"(\b(?:var\s+)?scramble_id\s*=\s*['"]?([0-9]+))")))
      throw std::runtime_error("Cannot determine image scramble ID; refusing to save scrambled pages");
    c.scramble_id=parse_id(match[1]);
  }
  return c;
}
std::string Client::login(const std::string& user, const std::string& password, std::stop_token stop) {
  if (user.empty() || password.empty()) throw std::runtime_error("Enter a username and password");
  std::lock_guard lock(api_mutex_);
  logout();
  try {
    const auto d=request("/login",{{"username",user},{"password",password}},true,stop,true);
    const auto username=json_text(d.at("username"));
    if (username.empty() || cookies_.empty()) throw AuthError("Login did not create a session");
    persist_session(); return username;
  } catch (...) { cookies_.clear(); std::error_code ec; fs::remove(session_file_,ec); throw; }
}
std::string Client::profile(std::stop_token stop) {
  std::lock_guard lock(api_mutex_);
  if (cookies_.empty()) throw AuthError("Log in to browse favorites");
  const auto d=request("/login",{},true,stop,true);
  const auto user=json_text(d.value("username",Json{}));
  if (user.empty()) { logout(); throw AuthError("Session expired; log in again"); }
  return user;
}
void Client::logout() {
  std::lock_guard lock(api_mutex_);
  cookies_.clear();
  std::error_code ec; fs::remove(session_file_,ec);
  if (ec) throw std::runtime_error("Cannot remove saved session: "+ec.message());
}
Listing Client::favorites(int page, Id folder, std::stop_token stop) {
  std::lock_guard lock(api_mutex_);
  if (cookies_.empty()) throw AuthError("Log in to browse favorites");
  if (page<1 || folder<0) throw std::runtime_error("Invalid favorites page or folder");
  return parse_listing(request("/favorite",{{"page",std::to_string(page)},{"folder_id",std::to_string(folder)},{"o","mr"}},false,stop,true),true);
}
Bytes Client::image(Id chapter, const std::string& filename, std::stop_token stop) {
  if (filename.empty() || filename=="." || filename==".." || filename.find_first_of("/\\")!=std::string::npos)
    throw std::runtime_error("Invalid image filename from API");
  HttpRequest request;
  request.url=http_.settings().image_url+"/media/photos/"+std::to_string(chapter)+"/"+Http::escape(filename);
  auto response=http_.perform(request,stop);
  if (response.body.empty()) {
    request.url+="?ts="+std::to_string(std::time(nullptr));
    response=http_.perform(request,stop);
  }
  if (response.body.empty()) throw std::runtime_error("Empty image response");
  return std::move(response.body);
}
}
