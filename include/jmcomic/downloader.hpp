#pragma once
#include "jmcomic/client.hpp"
#include <condition_variable>
#include <deque>
#include <functional>
#include <memory>
#include <thread>
namespace jm {
enum class TaskState { Queued, Discovering, Downloading, Pausing, Paused, Completed, Failed, Cancelled };
std::string state_name(TaskState);
struct Progress {
  std::uint64_t task{};
  std::string comic, chapter, error;
  Id chapter_id{};
  TaskState state = TaskState::Queued;
  std::size_t done{}, total{}, failed{}, skipped{};
};
class DownloadManager {
 public:
  using Notify = std::function<void()>;
  explicit DownloadManager(Notify);
  ~DownloadManager();
  std::uint64_t enqueue(std::shared_ptr<Client>, const Comic&, const ChapterInfo&, fs::path destination);
  std::vector<Progress> snapshot() const;
  void pause(std::uint64_t);
  void pause_all();
  void resume(std::uint64_t, std::shared_ptr<Client> client = {});
  void resume_all(std::shared_ptr<Client> client = {});
  void cancel(std::uint64_t);
  void cancel_all();
  void shutdown();
 private:
  struct Task;
  struct Job;
  void ensure_workers(const Settings&);
  void resume_task(std::shared_ptr<Task>&, const std::shared_ptr<Client>&);
  bool cancel_task(const std::shared_ptr<Task>&);
  void remove_cancelled(const std::shared_ptr<Task>&);
  void run(std::stop_token, int worker, bool processing);
  void discover(const std::shared_ptr<Task>&);
  bool page(Job, bool processing);
  void changed();
  Notify notify_;
  mutable std::mutex mutex_;
  std::condition_variable_any ready_;
  std::deque<Job> jobs_;
  std::deque<Job> processing_jobs_;
  std::size_t buffered_bytes_ = 0;
  std::vector<std::shared_ptr<Task>> tasks_;
  std::vector<std::jthread> workers_;
  std::vector<std::jthread> processors_;
  std::uint64_t next_task_ = 1;
  bool stopping_ = false;
};
}
