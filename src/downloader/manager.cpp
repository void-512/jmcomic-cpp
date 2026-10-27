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
  std::size_t active{};
  bool paused = false;
  bool remove_directory = false;
};
struct DownloadManager::Job {
  std::shared_ptr<Task> task;
  std::size_t index{};
  bool discover=false;
  Bytes bytes{};
  int attempt{};
  std::chrono::seconds delay{};
};
std::string state_name(TaskState s) {
  switch (s) {
    case TaskState::Queued: return "Queued";
    case TaskState::Discovering: return "Loading pages";
    case TaskState::Downloading: return "Downloading";
    case TaskState::Pausing: return "Pausing";
    case TaskState::Paused: return "Paused";
    case TaskState::Completed: return "Completed";
    case TaskState::Failed: return "Failed";
    case TaskState::Cancelled: return "Cancelled";
  }
  return {};
}
DownloadManager::DownloadManager(Notify notify) : notify_(std::move(notify)) {}
void DownloadManager::ensure_workers(const Settings& settings) {
  while (workers_.size()<static_cast<std::size_t>(settings.download_concurrency)) {
    const auto i=static_cast<int>(workers_.size());
    workers_.emplace_back([this,i](std::stop_token stop){run(stop,i,false);});
  }
  while (processors_.size()<static_cast<std::size_t>(settings.processing_concurrency)) {
    const auto i=static_cast<int>(processors_.size());
    processors_.emplace_back([this,i](std::stop_token stop){run(stop,i,true);});
  }
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
      if (t->progress.state==TaskState::Cancelled)
        throw std::runtime_error("This chapter is still stopping; queue it again shortly");
      if (t->progress.state==TaskState::Failed)
        throw std::runtime_error("This chapter is already in the queue; select Resume selected there");
      return t->progress.task;
    }
    ensure_workers(client->settings());
    auto t=std::make_shared<Task>(); id=next_task_++;
    t->progress.task=id; t->progress.comic=comic.name; t->progress.chapter=chapter.name; t->progress.chapter_id=chapter.id;
    t->client=std::move(client); t->comic=comic; t->chapter=chapter; t->directory=directory;
    tasks_.push_back(t); jobs_.push_back({t,0,true});
  }
  ready_.notify_all(); changed(); return id;
}
std::vector<Progress> DownloadManager::snapshot() const {
  std::lock_guard lock(mutex_); std::vector<Progress> out;
  for (const auto& task:tasks_) {
    if (task->progress.state==TaskState::Cancelled) continue;
    auto progress=task->progress;
    if (task->paused && (progress.state==TaskState::Queued || progress.state==TaskState::Discovering || progress.state==TaskState::Downloading))
      progress.state=task->active ? TaskState::Pausing : TaskState::Paused;
    out.push_back(std::move(progress));
  }
  return out;
}
void DownloadManager::pause(std::uint64_t id) {
  {
    std::lock_guard lock(mutex_);
    for (const auto& t:tasks_) if (t->progress.task==id) {
      const auto state=t->progress.state;
      if (state==TaskState::Queued || state==TaskState::Discovering || state==TaskState::Downloading) t->paused=true;
      break;
    }
  }
  changed();
}
void DownloadManager::pause_all() {
  {
    std::lock_guard lock(mutex_);
    for (const auto& t:tasks_) {
      const auto state=t->progress.state;
      if (state==TaskState::Queued || state==TaskState::Discovering || state==TaskState::Downloading) t->paused=true;
    }
  }
  changed();
}
void DownloadManager::resume_task(std::shared_ptr<Task>& t, const std::shared_ptr<Client>& client) {
  if (t->remove_directory) {
    if (t->progress.state==TaskState::Failed)
      throw std::runtime_error("Folder removal failed; use Cancel selected to retry");
    return;
  }
  if (t->progress.state==TaskState::Failed) {
    // Failed tasks have finished their writes; obsolete jobs keep their old object.
    const auto previous=t;
    previous->cancel.request_stop();
    std::erase_if(jobs_,[&](const Job& job){return job.task==previous;});
    auto replacement=std::make_shared<Task>();
    replacement->progress.task=t->progress.task; replacement->progress.comic=t->comic.name;
    replacement->progress.chapter=t->chapter.name; replacement->progress.chapter_id=t->chapter.id;
    replacement->client=client ? client : t->client; replacement->comic=t->comic; replacement->chapter=t->chapter;
    ensure_workers(replacement->client->settings());
    replacement->directory=t->directory; t=replacement;
    jobs_.push_back({t,0,true});
  } else if (t->paused && t->progress.state!=TaskState::Completed && t->progress.state!=TaskState::Cancelled) {
    if (client && client!=t->client) {
      if (t->active) throw std::runtime_error("Wait for this task to finish pausing before changing its network settings");
      t->client=client;
    }
    ensure_workers(t->client->settings());
    t->paused=false;
  }
}
void DownloadManager::resume(std::uint64_t id, std::shared_ptr<Client> client) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) throw std::runtime_error("Downloader is shutting down");
    for (auto& t:tasks_) if (t->progress.task==id) { resume_task(t,client); break; }
  }
  ready_.notify_all(); changed();
}
void DownloadManager::resume_all(std::shared_ptr<Client> client) {
  {
    std::lock_guard lock(mutex_);
    if (stopping_) throw std::runtime_error("Downloader is shutting down");
    for (const auto& t:tasks_)
      if (t->paused && t->active && client && client!=t->client &&
          (t->progress.state==TaskState::Queued || t->progress.state==TaskState::Discovering || t->progress.state==TaskState::Downloading))
        throw std::runtime_error("Wait for tasks to finish pausing before changing their network settings");
    for (auto& t:tasks_) resume_task(t,client);
  }
  ready_.notify_all(); changed();
}
bool DownloadManager::cancel_task(const std::shared_ptr<Task>& t) {
  t->remove_directory=true;
  t->cancel.request_stop();
  t->progress.state=TaskState::Cancelled;
  std::erase_if(jobs_,[&](const Job& job){return job.task==t;});
  std::erase_if(processing_jobs_,[&](const Job& job){
    if (job.task!=t) return false;
    buffered_bytes_-=job.bytes.size();
    --t->active;
    return true;
  });
  if (t->active) return false;
  // Reserve the directory until removal finishes, including against re-enqueue.
  ++t->active;
  return true;
}
void DownloadManager::remove_cancelled(const std::shared_ptr<Task>& t) {
  std::error_code error;
  fs::remove_all(t->directory,error);
  std::lock_guard lock(mutex_);
  --t->active;
  if (error) {
    t->progress.state=TaskState::Failed;
    t->progress.error="Cannot remove cancelled task folder: "+error.message()+"; cancel again to retry";
  } else {
    std::erase(tasks_,t);
  }
}
void DownloadManager::cancel(std::uint64_t id) {
  std::shared_ptr<Task> remove;
  {
    std::lock_guard lock(mutex_);
    for (const auto& t:tasks_) if (t->progress.task==id) {
      if (cancel_task(t)) remove=t;
      break;
    }
  }
  if (remove) remove_cancelled(remove);
  ready_.notify_all(); changed();
}
void DownloadManager::cancel_all() {
  std::vector<std::shared_ptr<Task>> remove;
  {
    std::lock_guard lock(mutex_);
    for (const auto& t:tasks_) if (cancel_task(t)) remove.push_back(t);
  }
  for (const auto& t:remove) remove_cancelled(t);
  ready_.notify_all(); changed();
}
void DownloadManager::shutdown() {
  std::vector<std::shared_ptr<Task>> remove;
  {
    std::lock_guard lock(mutex_);
    if (stopping_) return;
    stopping_=true;
    for (auto& t:tasks_) t->cancel.request_stop();
    for (const auto& job:processing_jobs_) --job.task->active;
    processing_jobs_.clear(); buffered_bytes_=0;
    for (const auto& t:tasks_) if (t->remove_directory && t->active==0) {
      ++t->active; remove.push_back(t);
    }
    for (auto& worker:workers_) worker.request_stop();
    for (auto& processor:processors_) processor.request_stop();
  }
  ready_.notify_all(); workers_.clear(); processors_.clear();
  for (const auto& t:remove) remove_cancelled(t);
}
void DownloadManager::run(std::stop_token stop, int worker, bool processing) {
  while (!stop.stop_requested()) {
    Job job;
    {
      std::unique_lock lock(mutex_);
      auto& queue=processing ? processing_jobs_ : jobs_;
      auto eligible=[&](const Job& j){
        const auto& settings=j.task->client->settings();
        if (j.task->cancel.stop_requested() || worker>=(processing ? settings.processing_concurrency : settings.download_concurrency)) return false;
        if (processing) return true; // Finish pages already in flight when paused.
        return !j.task->paused && (j.discover || (processing_jobs_.size()<8 && buffered_bytes_<64*1024*1024));
      };
      ready_.wait(lock,stop,[&]{ return stopping_ || std::any_of(queue.begin(),queue.end(),eligible); });
      if (stopping_ || stop.stop_requested()) return;
      const auto it=std::find_if(queue.begin(),queue.end(),eligible);
      job=std::move(*it); queue.erase(it);
      if (processing) buffered_bytes_-=job.bytes.size();
      else ++job.task->active;
    }
    ready_.notify_all();
    const auto task=job.task;
    bool handed_off=false;
    if (job.discover) discover(task); else handed_off=page(std::move(job),processing);
    bool remove=false;
    {
      std::lock_guard lock(mutex_);
      if (!handed_off) --task->active;
      if (!handed_off && task->remove_directory && task->active==0) {
        ++task->active;
        remove=true;
      }
    }
    if (remove) remove_cancelled(task);
    ready_.notify_all(); changed();
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
      if (t->cancel.stop_requested()) return;
      t->data=std::move(data); t->progress.total=t->data.images.size(); t->progress.state=TaskState::Downloading;
      for (std::size_t i=0;i<t->data.images.size();++i) jobs_.push_back({t,i,false});
    }
    ready_.notify_all();
  } catch (const std::exception& e) {
    std::lock_guard lock(mutex_);
    t->progress.state=t->cancel.stop_requested() ? TaskState::Cancelled : TaskState::Failed;
    t->progress.error=e.what();
  }
}
bool DownloadManager::page(Job job, bool processing) {
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
    if (job.delay.count()) { Http::wait(stop,job.delay); job.delay={}; }
    skipped=!processing && verified_page(output,identity);
    if (!skipped) {
      for (;;) {
        std::chrono::seconds delay{};
        try {
          if (!processing) {
            job.bytes=t->client->image(t->chapter.id,filename,stop);
            std::lock_guard lock(mutex_);
            if (stop.stop_requested() || stopping_) throw Cancelled();
            const auto size=job.bytes.size();
            processing_jobs_.push_back(std::move(job));
            buffered_bytes_+=size;
            ready_.notify_all();
            return true; // Keep this page active until processing finishes.
          }
          if (stop.stop_requested()) throw Cancelled();
          auto image=decode_image(job.bytes); Bytes{}.swap(job.bytes);
          unscramble(image,strips);
          if (stop.stop_requested()) throw Cancelled();
          save_page(output,image,identity); break;
        } catch (const Cancelled&) { throw; }
        catch (const HttpError& e) {
          if (!e.transient || job.attempt>=t->client->settings().retries || e.retry_after>60) throw;
          delay=std::chrono::seconds(std::max(e.retry_after,1<<job.attempt));
        } catch (const fs::filesystem_error&) { throw; }
        catch (const std::system_error&) { throw; }
        catch (const std::exception&) {
          if (job.attempt>=t->client->settings().retries) throw;
          delay=std::chrono::seconds(1<<job.attempt);
        }
        ++job.attempt;
        if (processing) {
          // Retry incomplete/corrupt images on the network pool, freeing the CPU worker.
          Bytes{}.swap(job.bytes); job.delay=delay;
          std::lock_guard lock(mutex_);
          if (stop.stop_requested() || stopping_) throw Cancelled();
          jobs_.push_back(std::move(job));
          ready_.notify_all();
          return false;
        }
        Http::wait(stop,delay);
      }
    }
    success=true;
  } catch (const std::exception& e) { error=e.what(); }
  {
    std::lock_guard lock(mutex_);
    ++t->finished;
    if (success) { ++t->progress.done; if (skipped) ++t->progress.skipped; }
    else if (!t->cancel.stop_requested()) { ++t->progress.failed; t->progress.error=error; }
    if (t->finished==t->progress.total && !t->cancel.stop_requested()) {
      t->progress.state=t->progress.failed ? TaskState::Failed : TaskState::Completed;
    }
  }
  return false;
}
}
