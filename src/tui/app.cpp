#include "jmcomic/tui.hpp"
#include "jmcomic/client.hpp"
#include "jmcomic/downloader.hpp"
#include "jmcomic/storage.hpp"
#include <ftxui/component/component.hpp>
#include <ftxui/component/component_options.hpp>
#include <ftxui/component/event.hpp>
#include <ftxui/component/screen_interactive.hpp>
#include <ftxui/dom/elements.hpp>
#include <algorithm>
#include <charconv>
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
namespace jm {
namespace {
using namespace ftxui;
class Background {
 public:
  Background() : thread_([this](std::stop_token stop) {
    while (!stop.stop_requested()) {
      std::function<void(std::stop_token)> fn;
      {
        std::unique_lock lock(mutex_);
        ready_.wait(lock,stop,[&]{return !jobs_.empty();});
        if (stop.stop_requested()) return;
        fn=std::move(jobs_.front()); jobs_.pop_front();
      }
      fn(stop);
    }
  }) {}
  ~Background() { stop(); }
  void submit(std::function<void(std::stop_token)> fn) {
    { std::lock_guard lock(mutex_); jobs_.push_back(std::move(fn)); }
    ready_.notify_one();
  }
  void cancel() { thread_.request_stop(); ready_.notify_all(); }
  void stop() { cancel(); if (thread_.joinable()) thread_.join(); }
 private:
  std::mutex mutex_;
  std::condition_variable_any ready_;
  std::deque<std::function<void(std::stop_token)>> jobs_;
  std::jthread thread_;
};
int integer(const std::string& s) {
  int value{}; auto [end,ec]=std::from_chars(s.data(),s.data()+s.size(),value);
  if (ec!=std::errc{} || end!=s.data()+s.size()) throw std::runtime_error("Settings must contain whole numbers");
  return value;
}
class Application {
 public:
  Application(fs::path dir, Settings settings)
    : directory_(std::move(dir)), settings_(std::move(settings)), client_(std::make_shared<Client>(settings_,directory_)),
      downloads_([this]{ screen_.PostEvent(Event::Custom); }) {
    destination_=settings_.destination.string(); api_url_=settings_.api_url; image_url_=settings_.image_url;
    concurrency_=std::to_string(settings_.concurrency); timeout_=std::to_string(settings_.timeout_seconds);
    retries_=std::to_string(settings_.retries); interval_=std::to_string(settings_.request_interval_ms); proxy_=settings_.proxy;
  }
  ~Application() { background_.cancel(); downloads_.shutdown(); background_.stop(); }
  int run() {
    build();
    if (client_->has_session()) {
      auto client=client_;
      work("Checking saved session",[this,client](std::stop_token stop) {
        auto user=client->profile(stop);
        return [this,user]{ user_=user; status_="Session restored"; };
      });
    }
    screen_.Loop(root_);
    return 0;
  }
 private:
  using Update=std::function<void()>;
  void post(Update update) {
    { std::lock_guard lock(updates_mutex_); updates_.push_back(std::move(update)); }
    screen_.PostEvent(Event::Custom);
  }
  void work(std::string label, std::function<Update(std::stop_token)> fn) {
    if (busy_) { status_="An operation is running; please wait"; return; }
    busy_=true; status_=std::move(label)+"…";
    background_.submit([this,fn=std::move(fn)](std::stop_token stop) {
      try {
        auto update=fn(stop);
        post([this,update=std::move(update)] { busy_=false; update(); });
      } catch (const AuthError& e) {
        const auto message=std::string(e.what());
        post([this,message]{ busy_=false; user_.clear(); status_=message; });
      } catch (const Json::exception&) {
        post([this]{ busy_=false; status_="Unexpected data from API or local settings; check endpoint and retry"; });
      } catch (const std::exception& e) {
        const auto message=std::string(e.what());
        post([this,message]{ busy_=false; status_=display_text(message); });
      }
    });
  }
  void drain() {
    std::deque<Update> updates;
    { std::lock_guard lock(updates_mutex_); updates.swap(updates_); }
    for (auto& update:updates) update();
    progress_=downloads_.snapshot(); queue_labels_.clear();
    for (const auto& p:progress_) queue_labels_.push_back(display_text(p.comic+" / "+p.chapter)+" — "+state_name(p.state)+" "+std::to_string(p.done)+"/"+std::to_string(p.total));
    queue_index_=std::clamp(queue_index_,0,std::max(0,static_cast<int>(progress_.size())-1));
  }
  void show_comic(Comic comic) {
    comic_=std::move(comic); chapter_widgets_->DetachAllChildren(); choices_.clear(); chapter_labels_.clear();
    for (const auto& c:comic_.chapters) {
      chapter_labels_.push_back(display_text(c.name)+" ["+std::to_string(c.id)+"]");
      choices_.push_back(std::make_unique<bool>(true));
    }
    for (std::size_t i=0;i<choices_.size();++i) chapter_widgets_->Add(Checkbox(&chapter_labels_[i],choices_[i].get()));
    tab_=1; status_="Select chapters, choose a destination, then queue the download";
  }
  void lookup(Id id) {
    auto client=client_;
    work("Loading comic",[this,client,id](std::stop_token stop) {
      auto comic=client->comic(id,stop);
      return [this,comic=std::move(comic)]() mutable { show_comic(std::move(comic)); };
    });
  }
  void open_result() {
    if (result_index_>=0 && static_cast<std::size_t>(result_index_)<listing_.comics.size()) lookup(listing_.comics[static_cast<std::size_t>(result_index_)].id);
  }
  void list(bool favorite, int requested_page, bool new_query = false) {
    if (requested_page<1) return;
    auto client=client_; const auto keyword=new_query ? keyword_ : listed_keyword_;
    const auto sort=new_query ? order_keys_.at(static_cast<std::size_t>(order_)) : listed_order_;
    const auto folder=new_query ? folders_.at(static_cast<std::size_t>(folder_index_)).id : listed_folder_;
    work(favorite ? "Loading favorites" : "Searching",[this,client,keyword,sort,folder,favorite,requested_page](std::stop_token stop) {
      auto data=favorite ? client->favorites(requested_page,folder,stop) : client->search(keyword,requested_page,sort,stop);
      if (data.redirect) {
        auto comic=client->comic(data.redirect,stop);
        return Update([this,comic=std::move(comic)]() mutable {show_comic(std::move(comic));});
      }
      return Update([this,data=std::move(data),favorite,requested_page,folder,keyword,sort]() mutable {
        listed_keyword_=keyword; listed_order_=sort; listed_folder_=folder;
        listing_=std::move(data); favorites_view_=favorite; page_=requested_page;
        result_labels_.clear(); result_index_=0;
        for (const auto& c:listing_.comics) result_labels_.push_back(std::to_string(c.id)+"  "+display_text(c.name)+"  · "+display_text(c.author));
        if (favorite) {
          const auto selected_id=folder;
          folders_={{0,"All favorites"}};
          for (const auto& f:listing_.folders) if (f.id!=0) folders_.push_back(f);
          folder_labels_.clear(); folder_index_=0;
          for (std::size_t i=0;i<folders_.size();++i) { folder_labels_.push_back(display_text(folders_[i].name)); if (folders_[i].id==selected_id) folder_index_=static_cast<int>(i); }
        }
        tab_=0; status_=listing_.comics.empty() ? "No results on this page" : "Enter or Open selected shows details";
      });
    });
  }
  void queue_selected() {
    if (!comic_.id) { status_="Open a comic first"; return; }
    std::vector<ChapterInfo> chapters;
    for (std::size_t i=0;i<choices_.size();++i) if (*choices_[i]) chapters.push_back(comic_.chapters[i]);
    if (chapters.empty()) { status_="Select at least one chapter"; return; }
    const auto comic=comic_; const auto destination=fs::path(destination_); auto client=client_;
    work("Queueing selected chapters",[this,client,comic,chapters,destination](std::stop_token stop) {
      if (destination.empty()) throw std::runtime_error("Choose a destination directory");
      fs::create_directories(destination);
      for (const auto& chapter:chapters) {
        if (stop.stop_requested()) throw Cancelled();
        downloads_.enqueue(client,comic,chapter,destination);
      }
      return [this]{ tab_=3; status_="Downloads queued; completed pages will be verified when resuming"; };
    });
  }
  void apply_settings() {
    const auto progress=downloads_.snapshot();
    if (std::any_of(progress.begin(),progress.end(),[](const auto& p){return p.state==TaskState::Queued || p.state==TaskState::Discovering || p.state==TaskState::Downloading;})) {
      status_="Wait for downloads to stop before applying network settings"; return;
    }
    Settings s;
    try {
      s.api_url=api_url_; s.image_url=image_url_; s.destination=destination_; s.proxy=proxy_;
      s.concurrency=integer(concurrency_); s.timeout_seconds=integer(timeout_); s.retries=integer(retries_); s.request_interval_ms=integer(interval_); s.validate();
    } catch (const std::exception& e) { status_=e.what(); return; }
    work("Saving settings",[this,s](std::stop_token) {
      auto client=std::make_shared<Client>(s,directory_); save_settings(directory_,s);
      return [this,s,client]{ settings_=s; client_=client; user_.clear(); status_="Settings saved beside executable; open Account to check your session"; };
    });
  }
  void quit() {
    status_="Cancelling operations and closing…";
    background_.cancel(); downloads_.cancel_all(); screen_.ExitLoopClosure()();
  }
  void build() {
    InputOption search_option; search_option.on_enter=[this]{list(false,1,true);};
    auto search=Input(&keyword_,"Keyword",search_option);
    InputOption id_option; id_option.on_enter=[this]{ try {lookup(parse_id(id_));} catch(const std::exception& e){status_=e.what();} };
    auto id=Input(&id_,"Comic ID",id_option);
    auto sort=Toggle(&order_labels_,&order_);
    auto submit=Button("Search",[this]{list(false,1,true);});
    auto by_id=Button("Get ID",id_option.on_enter);
    auto favorite=Button("Favorites",[this]{list(true,1,true);});
    auto previous=Button("Previous",[this]{list(favorites_view_,page_-1);});
    auto next=Button("Next",[this]{ if (!listing_.comics.empty()) list(favorites_view_,page_+1); });
    auto folders=Dropdown(&folder_labels_,&folder_index_);
    auto load_folder=Button("Load folder",[this]{list(true,1,true);});
    MenuOption results_options;
    results_options.on_enter=[this]{open_result();};
    auto results=Menu(&result_labels_,&result_index_,results_options);
    auto results_frame=Renderer(results,[results]{return results->Render()|vscroll_indicator|frame|flex;});
    auto open=Button("Open selected",[this]{open_result();});
    auto search_controls=Container::Vertical({Container::Horizontal({search,submit}),sort,Container::Horizontal({id,by_id,favorite}),
      Container::Horizontal({folders,load_folder}),results_frame,Container::Horizontal({previous,next,open})});
    auto search_pane=Renderer(search_controls,[=,this] {
      return vbox({hbox({text("Search: "),search->Render()|flex,submit->Render()}),sort->Render(),
        hbox({text("Direct lookup: "),id->Render()|flex,by_id->Render(),favorite->Render()}),
        hbox({folders->Render()|flex,load_folder->Render()}),separator(),
        text(std::string(favorites_view_?"Favorites":"Search results")+" · page "+std::to_string(page_)+" · "+std::to_string(listing_.total)+" total"),
        results_frame->Render()|flex,separator(),hbox({previous->Render(),next->Render(),open->Render()})});
    });
    chapter_widgets_=Container::Vertical({});
    auto chapter_frame=Renderer(chapter_widgets_,[this]{return chapter_widgets_->Render()|vscroll_indicator|frame|flex;});
    auto all=Button("Select all",[this]{for(auto& choice:choices_) *choice=true;});
    auto none=Button("Select none",[this]{for(auto& choice:choices_) *choice=false;});
    auto destination=Input(&destination_,"Destination directory");
    auto enqueue=Button("Queue selected chapters",[this]{queue_selected();});
    auto details_controls=Container::Vertical({Container::Horizontal({all,none}),chapter_frame,destination,enqueue});
    auto details=Renderer(details_controls,[=,this] {
      return vbox({text(comic_.id ? display_text(comic_.name)+" ["+std::to_string(comic_.id)+"]" : "Open a comic from Search or Favorites")|bold,
        paragraph(display_text("Author: "+comic_.author+" · Views: "+comic_.views+" · Likes: "+comic_.favorites)),
        paragraph(display_text(comic_.description))|size(HEIGHT,LESS_THAN,4),separator(),hbox({all->Render(),none->Render()}),
        chapter_frame->Render()|flex,separator(),text("Destination (absolute or relative to current directory):"),destination->Render(),enqueue->Render()});
    });
    auto username=Input(&username_,"Username"); InputOption password_option; password_option.password=true;
    auto password=Input(&password_,"Password",password_option);
    auto login=Button("Log in",[this] {
      if (busy_) {status_="Wait for the current operation"; return;}
      auto client=client_; auto user=username_; auto secret=std::make_shared<std::string>(password_); password_.clear();
      work("Logging in",[this,client,user,secret](std::stop_token stop) {
        std::string name;
        try {name=client->login(user,*secret,stop);} catch (...) {std::fill(secret->begin(),secret->end(),'\0'); throw;}
        std::fill(secret->begin(),secret->end(),'\0');
        return [this,name]{user_=name; status_="Logged in; browse favorites from Search";};
      });
    });
    auto logout=Button("Log out",[this]{auto client=client_; work("Logging out",[this,client](std::stop_token){client->logout(); return [this]{user_.clear();status_="Logged out and saved session removed";};});});
    auto check=Button("Check saved session",[this]{auto client=client_;work("Checking session",[this,client](std::stop_token stop){auto user=client->profile(stop);return [this,user]{user_=user;status_="Session is valid";};});});
    auto account_controls=Container::Vertical({username,password,Container::Horizontal({login,logout,check})});
    auto account=Renderer(account_controls,[=,this]{return vbox({text(user_.empty()?"Guest / session not checked":"Signed in: "+display_text(user_)),separator(),username->Render(),password->Render(),
      hbox({login->Render(),logout->Render(),check->Render()}),separator(),paragraph("Session cookies are saved beside the executable with owner-only permissions on Linux. Passwords are used only for login and are never saved."),filler()});});
    auto queue=Menu(&queue_labels_,&queue_index_);
    auto queue_frame=Renderer(queue,[queue]{return queue->Render()|vscroll_indicator|frame|flex;});
    auto cancel=Button("Cancel selected",[this]{if(!progress_.empty()) downloads_.cancel(progress_[static_cast<std::size_t>(queue_index_)].task);});
    auto retry=Button("Retry / resume",[this]{try{if(busy_){status_="Wait for the current operation";return;}if(!progress_.empty()) downloads_.retry(progress_[static_cast<std::size_t>(queue_index_)].task,client_);}catch(const std::exception& e){status_=e.what();}});
    auto cancel_all=Button("Cancel all",[this]{downloads_.cancel_all();});
    auto queue_controls=Container::Vertical({queue_frame,Container::Horizontal({cancel,retry,cancel_all})});
    auto queue_pane=Renderer(queue_controls,[=,this] {
      Elements content; std::size_t done{},total{}; std::size_t complete{};
      for(const auto& p:progress_) {done+=p.done;total+=p.total;if(p.state==TaskState::Completed)++complete;}
      content.push_back(text(std::to_string(complete)+" / "+std::to_string(progress_.size())+" chapters complete · "+std::to_string(done)+" / "+std::to_string(total)+" discovered pages"));
      content.push_back(gauge(total ? static_cast<float>(done)/static_cast<float>(total) : 0)); content.push_back(separator());
      content.push_back(queue_frame->Render()|flex);
      if(!progress_.empty()) {
        const auto& p=progress_[static_cast<std::size_t>(queue_index_)];
        content.push_back(text(state_name(p.state)+" · "+std::to_string(p.skipped)+" verified pages reused · "+std::to_string(p.failed)+" failed"));
        content.push_back(gauge(p.total ? static_cast<float>(p.done)/static_cast<float>(p.total) : 0));
        content.push_back(paragraph(display_text(p.error)));
      }
      content.push_back(separator());content.push_back(hbox({cancel->Render(),retry->Render(),cancel_all->Render()}));return vbox(std::move(content));
    });
    auto api=Input(&api_url_,"HTTPS API origin"); auto images=Input(&image_url_,"HTTPS image origin");
    auto workers=Input(&concurrency_,"1–8"); auto timeout=Input(&timeout_,"5–180 seconds");
    auto retries=Input(&retries_,"0–5"); auto interval=Input(&interval_,"100–10000 milliseconds");
    auto proxy=Input(&proxy_,"Empty = direct connection",password_option);
    auto settings_destination=Input(&destination_,"Destination directory");
    auto save=Button("Apply and save",[this]{apply_settings();});
    auto settings_controls=Container::Vertical({api,images,settings_destination,workers,timeout,retries,interval,proxy,save});
    auto settings=Renderer(settings_controls,[=] {
      return vbox({text("API origin"),api->Render(),text("Image origin"),images->Render(),text("Download destination"),settings_destination->Render(),
        hbox({text("Parallel workers (1–8): "),workers->Render()|flex}),hbox({text("Request timeout in seconds: "),timeout->Render()|flex}),
        hbox({text("Retries: "),retries->Render()|flex}),hbox({text("Minimum interval between requests, ms: "),interval->Render()|flex}),
        text("Proxy URL (optional; hidden)"),proxy->Render(),save->Render(),filler(),
        paragraph("Apply network settings after active downloads stop. Destination changes apply when queueing chapters. Settings and session files live beside the executable.")})|vscroll_indicator|frame;
    });
    auto tabs=Toggle(&tab_labels_,&tab_);
    auto pages=Container::Tab({search_pane,details,account,queue_pane,settings},&tab_);
    auto quit_button=Button("Quit",[this]{quit();});
    auto controls=Container::Vertical({Container::Horizontal({tabs,quit_button}),pages});
    auto render=Renderer(controls,[=,this] {
      return vbox({hbox({text("JMComic")|bold,text("  "),tabs->Render()|flex,quit_button->Render()}),separator(),pages->Render()|flex,separator(),
        paragraph((busy_?"Working · ":"")+display_text(status_)),text("Tab / Shift-Tab: focus · arrows: navigate · Space: select · Enter: activate · Ctrl-Q: quit")|dim})|border;
    });
    root_=CatchEvent(render,[this](Event event) {
      if(event==Event::Custom) {drain();return true;}
      if(event==Event::CtrlQ) {quit();return true;}
      return false;
    });
  }
  ScreenInteractive screen_=ScreenInteractive::Fullscreen();
  fs::path directory_;
  Settings settings_;
  std::shared_ptr<Client> client_;
  Background background_;
  DownloadManager downloads_;
  std::mutex updates_mutex_;
  std::deque<Update> updates_;
  Component root_,chapter_widgets_;
  bool busy_=false,favorites_view_=false;
  int tab_=0,order_=0,page_=1,result_index_=0,folder_index_=0,queue_index_=0;
  std::string keyword_,id_,username_,password_,user_,destination_,api_url_,image_url_,concurrency_,timeout_,retries_,interval_,proxy_;
  Id listed_folder_{};
  std::string listed_keyword_,listed_order_="mr";
  std::string status_="Search by keyword or retrieve a comic ID";
  std::vector<std::string> tab_labels_{"Search","Chapters","Account","Queue","Settings"};
  std::vector<std::string> order_labels_{"Latest","Most clicks","Most favorites"};
  std::vector<std::string> order_keys_{"mr","mv","tf"};
  std::vector<std::string> result_labels_,chapter_labels_,queue_labels_,folder_labels_{"All favorites"};
  std::vector<Folder> folders_{{0,"All favorites"}};
  std::vector<std::unique_ptr<bool>> choices_;
  std::vector<Progress> progress_;
  Listing listing_; Comic comic_;
};
}
int run_tui(fs::path directory, Settings settings) { Application app(std::move(directory),std::move(settings)); return app.run(); }
}
