#pragma once
#include <algorithm>
#include <atomic>
#include <functional>
#include <thread>
#include <vector>

namespace patina {

// Runs fn(i) for every i in [0, n) on up to `maxThreads` threads (the caller's thread is one of
// them) and returns when all are done.
inline void parallelFor(size_t n, const std::function<void(size_t)>& fn, unsigned maxThreads = 16) {
  unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  unsigned nt = (unsigned)std::max<size_t>(1, std::min<size_t>({(size_t)maxThreads, (size_t)hw, n}));
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (size_t i; (i = next++) < n;) fn(i);
  };
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < nt; ++t) pool.emplace_back(work);
  work();
  for (auto& t : pool) t.join();
}

}  // namespace patina
