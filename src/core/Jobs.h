#pragma once
// (1.2.15) Worker threads for slow work a frame must not wait for: reading the pictures of the material and brush
// libraries. Jobs run first in, first out; the ones still waiting when the queue goes (Patina closing) are dropped,
// not run. A job touches no GL and nothing the main thread may free: it hands its result over itself.
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace patina {

class JobQueue {
 public:
  explicit JobQueue(unsigned threads) {
    for (unsigned i = 0; i < std::max(1u, threads); ++i) threads_.emplace_back([this] { work(); });
  }
  ~JobQueue() {
    {
      std::lock_guard<std::mutex> l(m_);
      stop_ = true;
      jobs_.clear();
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
  }
  void run(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> l(m_);
      jobs_.push_back(std::move(job));
    }
    cv_.notify_one();
  }

 private:
  void work() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return stop_ || !jobs_.empty(); });
        if (stop_) return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
    }
  }
  std::vector<std::thread> threads_;
  std::deque<std::function<void()>> jobs_;
  std::mutex m_;
  std::condition_variable cv_;
  bool stop_ = false;
};

// The library pictures' queue: a few threads, so painting and the window keep their speed.
inline JobQueue& pictureJobs() {
  static JobQueue q(std::clamp(std::thread::hardware_concurrency() / 4, 1u, 4u));
  return q;
}

}  // namespace patina
