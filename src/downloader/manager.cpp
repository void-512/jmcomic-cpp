#include "jmcomic/downloader.hpp"
#include "jmcomic/image.hpp"
#include <algorithm>
namespace jm {
struct DownloadManager::Task {
  Progress progress;
  std::shared_ptr<Client> client;
  Comic comic;
  ChapterInfo chapter;
  Chapter data;
  fs::path directory;
  std::stop_source cancel;
  std::size_t finished{};
};
struct DownloadManager::Job { std::shared_ptr<Task> task; std::size_t index{}; bool discover=false; };
std::string state_name(TaskState s) {
  switch (s) {
    case TaskState::Queued: return "Queued";
    case TaskState::Discovering: return "Loading pages";
    case TaskState::Downloading: return "Downloading";
    case TaskState::Completed: return "Completed";
    case TaskState::Failed: return "Failed";
    case TaskState::Cancelled: return "Cancelled";
  }
  return {};
}
DownloadManager::DownloadManager(Notify notify) : notify_(std::move(notify)) {
  for (int i=0;i<8;++i) workers_.emplace_back([this,i](std::stop_token stop){run(stop,i);});
}
DownloadManager::~DownloadManager() { shutdown(); }
void DownloadManager::changed() { if (notify_) notify_(); }
std::uint64_t DownloadManager::enqueue(std::shared_ptr<Client> client, const Comic& comic, const ChapterInfo& chapter, fs::path destination) {
  const auto directory=fs::weakly_canonical(fs::absolute(destination))/
    (safe_name(comic.name)+" ["+std::to_string(comic.id)+"]") /
    (safe_name(chapter.name)+" ["+std::to_string(chapter.id)+"]");
  std::uint64_t id{};
  {
    std::lock_guard lock(mutex_);
    if (stopping_) throw std::runtime_error("Downloader is shutting down");
    // Never allow two workers to write the same output directory concurrently.
    for (const auto& t:tasks_) if (t->directory==directory) {
      if (t->progress.state==TaskState::Failed || t->progress.state==TaskState::Cancelled)
        throw std::runtime_error("This chapter is already in the queue; select Retry / resume there");
      return t->progress.task;
    }
    auto t=std::make_shared<Task>(); id=next_task_++;
    t->progress.task=id; t->progress.comic=comic.name; t->progress.chapter=chapter.name; t->progress.chapter_id=chapter.id;
    t->client=std::move(client); t->comic=comic; t->chapter=chapter; t->directory=directory;
    tasks_.push_back(t); jobs_.push_back({t,0,true});
  }
  ready_.notify_all(); changed(); return id;
}
std::vector<Progress> DownloadManager::snapshot() const {
  std::lock_guard lock(mutex_); std::vector<Progress> out;
  for (const auto& task:tasks_) out.push_back(task->progress);
  return out;
}
void DownloadManager::cancel(std::uint64_t id) {
  {
    std::lock_guard lock(mutex_);
    for (auto& t:tasks_) if (t->progress.task==id && t->progress.state!=TaskState::Completed) {
      t->cancel.request_stop();
      // Marked cancelled after in-flight operations have stopped, so retry cannot race them.
      if (t->progress.state==TaskState::Queued || t->progress.state==TaskState::Failed) t->progress.state=TaskState::Cancelled;
    }
  }
  ready_.notify_all(); changed();
}
void DownloadManager::cancel_all() {
  for (const auto& p:snapshot()) cancel(p.task);
}
void DownloadManager::retry(std::uint64_t id, std::shared_ptr<Client> client) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) throw std::runtime_error("Downloader is shutting down");
    for (auto& t:tasks_) if (t->progress.task==id) {
      if (t->progress.state!=TaskState::Failed && t->progress.state!=TaskState::Cancelled)
        throw std::runtime_error("Retry is available after a task has stopped");
      // Replace the task object: obsolete queued jobs keep their cancelled object.
      t->cancel.request_stop();
      auto replacement=std::make_shared<Task>();
      replacement->progress.task=t->progress.task; replacement->progress.comic=t->comic.name;
      replacement->progress.chapter=t->chapter.name; replacement->progress.chapter_id=t->chapter.id;
      replacement->client=client ? client : t->client; replacement->comic=t->comic; replacement->chapter=t->chapter;
      replacement->directory=t->directory; t=replacement;
      jobs_.push_back({t,0,true}); break;
    }
  }
  ready_.notify_all(); changed();
}
void DownloadManager::shutdown() {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    stopping_=true;
    for (auto& t:tasks_) t->cancel.request_stop();
    for (auto& worker:workers_) worker.request_stop();
  }
  ready_.notify_all(); workers_.clear();
}
void DownloadManager::run(std::stop_token stop, int worker) {
  while (!stop.stop_requested()) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      auto eligible=[&](const Job& j){ return worker<j.task->client->settings().concurrency; };
      ready_.wait(lock,stop,[&]{ return stopping_ || std::any_of(jobs_.begin(),jobs_.end(),eligible); });
      if (stopping_ || stop.stop_requested()) return;
      const auto it=std::find_if(jobs_.begin(),jobs_.end(),eligible);
      job=std::move(*it); jobs_.erase(it);
    }
    if (job.discover) discover(job.task); else page(job);
  }
}
void DownloadManager::discover(const std::shared_ptr<Task>& t) {
  {
    std::lock_guard lock(mutex_);
    if (t->cancel.stop_requested()) { t->progress.state=TaskState::Cancelled; return; }
    t->progress.state=TaskState::Discovering;
  }
  changed();
  try {
    auto data=t->client->chapter(t->chapter.id,t->cancel.get_token());
    if (t->cancel.stop_requested()) throw Cancelled();
    fs::create_directories(t->directory);
    {
      std::lock_guard lock(mutex_);
      t->data=std::move(data); t->progress.total=t->data.images.size(); t->progress.state=TaskState::Downloading;
      for (std::size_t i=0;i<t->data.images.size();++i) jobs_.push_back({t,i,false});
    }
    ready_.notify_all();
  } catch (const std::exception& e) {
    std::lock_guard lock(mutex_);
    t->progress.state=t->cancel.stop_requested() ? TaskState::Cancelled : TaskState::Failed;
    t->progress.error=e.what();
  }
  changed();
}
void DownloadManager::page(const Job& job) {
  const auto& t=job.task;
  bool skipped=false, success=false;
  std::string error;
  try {
    const auto stop=t->cancel.get_token();
    if (stop.stop_requested()) throw Cancelled();
    const auto& filename=t->data.images[job.index];
    auto extension=fs::path(filename).extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    const auto strips=extension==".gif" ? 0U : strip_count(t->data.scramble_id,t->chapter.id,fs::path(filename).stem().string());
    const Json identity={{"chapter_id",t->chapter.id},{"filename",filename},{"strips",strips},{"decoder",1}};
    const auto output=t->directory/page_name(job.index);
    skipped=verified_page(output,identity);
    if (!skipped) {
      for (int attempt=0;;++attempt) {
        try {
          auto bytes=t->client->image(t->chapter.id,filename,stop);
          if (stop.stop_requested()) throw Cancelled();
          auto image=decode_image(bytes); bytes.clear(); bytes.shrink_to_fit();
          unscramble(image,strips);
          if (stop.stop_requested()) throw Cancelled();
          save_page(output,image,identity); break;
        } catch (const Cancelled&) { throw; }
        catch (const HttpError& e) {
          if (!e.transient || attempt>=t->client->settings().retries || e.retry_after>60) throw;
          Http::wait(stop,std::chrono::seconds(std::max(e.retry_after,1<<attempt)));
        } catch (const fs::filesystem_error&) { throw; }
        catch (const std::system_error&) { throw; }
        catch (const std::exception&) {
          if (attempt>=t->client->settings().retries) throw;
          Http::wait(stop,std::chrono::seconds(1<<attempt));
        }
      }
    }
    success=true;
  } catch (const std::exception& e) { error=e.what(); }
  {
    std::lock_guard lock(mutex_);
    ++t->finished;
    if (success) { ++t->progress.done; if (skipped) ++t->progress.skipped; }
    else if (!t->cancel.stop_requested()) { ++t->progress.failed; t->progress.error=error; }
    if (t->finished==t->progress.total) {
      t->progress.state=t->cancel.stop_requested() ? TaskState::Cancelled :
        (t->progress.failed ? TaskState::Failed : TaskState::Completed);
    }
  }
  changed();
}
}
