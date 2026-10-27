#include "jmcomic/http.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <charconv>
#include <ctime>
#include <memory>
#include <thread>
namespace jm {
namespace {
struct CurlRuntime {
  CurlRuntime() { if (curl_global_init(CURL_GLOBAL_DEFAULT)) throw std::runtime_error("Cannot initialize HTTP library"); }
  ~CurlRuntime() { curl_global_cleanup(); }
};
void initialize() { static CurlRuntime runtime; }
struct Transfer {
  Bytes bytes;
  std::size_t limit;
  std::stop_token stop;
  int retry_after{};
  bool overflow=false;
};
std::size_t body_callback(char* data, std::size_t size, std::size_t count, void* ptr) noexcept {
  auto& t=*static_cast<Transfer*>(ptr);
  const auto n=size*count;
  if (t.stop.stop_requested()) return 0;
  if (n > t.limit-t.bytes.size()) { t.overflow=true; return 0; }
  try { t.bytes.insert(t.bytes.end(), data, data+n); return n; } catch (...) { return 0; }
}
int progress_callback(void* ptr, curl_off_t, curl_off_t, curl_off_t, curl_off_t) noexcept {
  return static_cast<Transfer*>(ptr)->stop.stop_requested() ? 1 : 0;
}
std::size_t header_callback(char* data, std::size_t size, std::size_t count, void* ptr) noexcept {
  const auto n = size*count;
  try {
    std::string s(data,n);
    constexpr std::string_view key="retry-after:";
    if (s.size() >= key.size() && std::equal(key.begin(),key.end(),s.begin(), [](char a, char b){ return a == std::tolower(static_cast<unsigned char>(b)); })) {
      s.erase(0,key.size()); auto start=s.find_first_not_of(" \t");
      if (start != std::string::npos) s.erase(0,start);
      long seconds{}; auto [end, ec] = std::from_chars(s.data(),s.data()+s.size(),seconds);
      if (ec != std::errc{} || end == s.data()) {
        const auto date=curl_getdate(s.c_str(),nullptr);
        if (date != -1) seconds=static_cast<long>(date-std::time(nullptr));
      }
      static_cast<Transfer*>(ptr)->retry_after=static_cast<int>(std::clamp(seconds,0L,86400L));
    }
  } catch (...) { return 0; }
  return n;
}
}
Http::Http(Settings settings) : settings_(std::move(settings)) { initialize(); }
std::string Http::escape(const std::string& s) {
  initialize();
  std::unique_ptr<char, decltype(&curl_free)> escaped(curl_easy_escape(nullptr,s.data(),static_cast<int>(s.size())),curl_free);
  if (!escaped) throw std::runtime_error("Cannot encode request");
  return escaped.get();
}
void Http::wait(std::stop_token stop, std::chrono::milliseconds duration) {
  const auto until=std::chrono::steady_clock::now()+duration;
  while (std::chrono::steady_clock::now() < until) {
    if (stop.stop_requested()) throw Cancelled();
    std::this_thread::sleep_for(std::min(std::chrono::milliseconds(25),std::chrono::duration_cast<std::chrono::milliseconds>(until-std::chrono::steady_clock::now())));
  }
  if (stop.stop_requested()) throw Cancelled();
}
HttpResponse Http::perform(const HttpRequest& req, std::stop_token stop) {
  { // Pace each image slot independently; server cooldowns apply to every request.
    std::unique_lock lock(rate_mutex_);
    for (;;) {
      if (stop.stop_requested()) throw Cancelled();
      const auto now=std::chrono::steady_clock::now();
      const auto remaining=std::chrono::duration_cast<std::chrono::seconds>(cooldown_-now);
      if (remaining>std::chrono::seconds(60))
        throw HttpError("Service rate limit is still active; retry later",429,static_cast<int>(remaining.count())+1,false);
      auto* next=&next_request_;
      if (req.image) next=&*std::min_element(next_image_requests_.begin(),next_image_requests_.begin()+settings_.concurrency);
      const auto at=std::max(*next,cooldown_);
      if (now>=at) {
        *next=now+std::chrono::milliseconds(settings_.request_interval_ms);
        break;
      }
      rate_ready_.wait_until(lock,stop,at,[]{return false;});
    }
  }
  // A per-thread easy handle retains connection and DNS caches between pages.
  thread_local std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> easy(curl_easy_init(),curl_easy_cleanup);
  if (!easy) throw std::runtime_error("Cannot create HTTP request");
  CURL* h=easy.get();
  curl_easy_reset(h);
  curl_easy_setopt(h,CURLOPT_COOKIELIST,"ALL");
  Transfer t{{},req.limit,stop};
  curl_easy_setopt(h,CURLOPT_URL,req.url.c_str());
  curl_easy_setopt(h,CURLOPT_PROXY,settings_.proxy.c_str());
  curl_easy_setopt(h,CURLOPT_PROTOCOLS_STR,"http,https");
  curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,0L);
  curl_easy_setopt(h,CURLOPT_NOSIGNAL,1L);
  curl_easy_setopt(h,CURLOPT_CONNECTTIMEOUT,static_cast<long>(std::min(10,settings_.timeout_seconds)));
  curl_easy_setopt(h,CURLOPT_TIMEOUT,static_cast<long>(settings_.timeout_seconds));
  curl_easy_setopt(h,CURLOPT_LOW_SPEED_LIMIT,100L);
  curl_easy_setopt(h,CURLOPT_LOW_SPEED_TIME,15L);
  curl_easy_setopt(h,CURLOPT_USERAGENT,"Mozilla/5.0 (X11; Linux aarch64) AppleWebKit/537.36 Chrome/128.0.0.0 Safari/537.36");
  curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,body_callback);
  curl_easy_setopt(h,CURLOPT_WRITEDATA,&t);
  curl_easy_setopt(h,CURLOPT_HEADERFUNCTION,header_callback);
  curl_easy_setopt(h,CURLOPT_HEADERDATA,&t);
  curl_easy_setopt(h,CURLOPT_NOPROGRESS,0L);
  curl_easy_setopt(h,CURLOPT_XFERINFOFUNCTION,progress_callback);
  curl_easy_setopt(h,CURLOPT_XFERINFODATA,&t);
  curl_easy_setopt(h,CURLOPT_COOKIEFILE,"");
  for (const auto& cookie : req.cookies) curl_easy_setopt(h,CURLOPT_COOKIELIST,cookie.c_str());
  curl_slist* headers=nullptr;
  for (const auto& s : req.headers) {
    auto* updated=curl_slist_append(headers,s.c_str());
    if (!updated) { curl_slist_free_all(headers); throw std::bad_alloc(); }
    headers=updated;
  }
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> owned_headers(headers,curl_slist_free_all);
  curl_easy_setopt(h,CURLOPT_HTTPHEADER,headers);
  if (req.post) { curl_easy_setopt(h,CURLOPT_POST,1L); curl_easy_setopt(h,CURLOPT_POSTFIELDS,req.form.c_str()); }
  const auto result=curl_easy_perform(h);
  if (stop.stop_requested()) throw Cancelled();
  if (t.overflow) throw std::runtime_error("Server response exceeds safety limit");
  if (result != CURLE_OK) {
    const bool transient = result == CURLE_COULDNT_CONNECT || result == CURLE_COULDNT_RESOLVE_HOST ||
      result == CURLE_OPERATION_TIMEDOUT || result == CURLE_RECV_ERROR || result == CURLE_SEND_ERROR ||
      result == CURLE_PARTIAL_FILE || result == CURLE_GOT_NOTHING;
    throw HttpError(std::string("Network: ")+curl_easy_strerror(result),0,0,transient);
  }
  long status{}; curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&status);
  if (status != 200) {
    if (status==429 || t.retry_after>0) {
      std::lock_guard lock(rate_mutex_);
      cooldown_=std::max(cooldown_,std::chrono::steady_clock::now()+std::chrono::seconds(std::max(1,t.retry_after)));
      rate_ready_.notify_all();
    }
    std::string message = "HTTP " + std::to_string(status);
    if (status == 403) message += ": access denied or service challenge";
    if (status == 429) message += ": rate limited; wait before retrying";
    if (status >= 300 && status < 400) message += ": endpoint moved; update the origin in Settings";
    if (t.retry_after) message += " (Retry-After " + std::to_string(t.retry_after) + "s)";
    throw HttpError(message,status,t.retry_after,status==408 || status==429 || status>=500);
  }
  curl_slist* cookie_list=nullptr; curl_easy_getinfo(h,CURLINFO_COOKIELIST,&cookie_list);
  std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)> owned_cookies(cookie_list,curl_slist_free_all);
  HttpResponse response{std::move(t.bytes),{}};
  for (auto* c=cookie_list;c;c=c->next) response.cookies.emplace_back(c->data);
  return response;
}
}
