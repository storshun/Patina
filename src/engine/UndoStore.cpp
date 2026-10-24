#include "engine/UndoStore.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

#include <miniz.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace patina {

size_t physicalMemoryBytes() {
#ifdef _WIN32
  MEMORYSTATUSEX m{};
  m.dwLength = sizeof m;
  return GlobalMemoryStatusEx(&m) ? size_t(m.ullTotalPhys) : 0;
#else
  long pages = sysconf(_SC_PHYS_PAGES), page = sysconf(_SC_PAGE_SIZE);
  return pages > 0 && page > 0 ? size_t(pages) * size_t(page) : 0;
#endif
}

namespace {

// Worker threads for compressing and unpacking (a few, so painting keeps its speed).
class Pool {
 public:
  Pool() {
    unsigned n = std::clamp(std::thread::hardware_concurrency() / 2, 1u, 12u);
    for (unsigned i = 0; i < n; ++i) threads_.emplace_back([this] { work(); });
  }
  ~Pool() {
    {
      std::lock_guard<std::mutex> l(m_);
      stop_ = true;
    }
    cv_.notify_all();
    for (auto& t : threads_) t.join();
  }
  void run(std::function<void()> job) {
    {
      std::lock_guard<std::mutex> l(m_);
      jobs_.push_back(std::move(job));
      ++pending_;
    }
    cv_.notify_one();
  }
  void wait() {
    std::unique_lock<std::mutex> l(m_);
    idle_.wait(l, [&] { return pending_ == 0; });
  }

 private:
  void work() {
    for (;;) {
      std::function<void()> job;
      {
        std::unique_lock<std::mutex> l(m_);
        cv_.wait(l, [&] { return stop_ || !jobs_.empty(); });
        if (jobs_.empty()) return;
        job = std::move(jobs_.front());
        jobs_.pop_front();
      }
      job();
      std::lock_guard<std::mutex> l(m_);
      if (--pending_ == 0) idle_.notify_all();
    }
  }
  std::vector<std::thread> threads_;
  std::deque<std::function<void()>> jobs_;
  std::mutex m_;
  std::condition_variable cv_, idle_;
  size_t pending_ = 0;
  bool stop_ = false;
};

Pool& pool() {
  static Pool p;
  return p;
}

struct Format {
  GLenum fmt, type;
  int bytes;
};

bool formatOf(GLenum internal, Format& f) {
  switch (internal) {
    case GL_RGBA8: f = {GL_RGBA, GL_UNSIGNED_BYTE, 4}; return true;
    case GL_RG8: f = {GL_RG, GL_UNSIGNED_BYTE, 2}; return true;
    case GL_R8: f = {GL_RED, GL_UNSIGNED_BYTE, 1}; return true;
    case GL_RGBA16: f = {GL_RGBA, GL_UNSIGNED_SHORT, 8}; return true;
    case GL_RG16: f = {GL_RG, GL_UNSIGNED_SHORT, 4}; return true;
    case GL_R16: f = {GL_RED, GL_UNSIGNED_SHORT, 2}; return true;
    case GL_RGBA16F: f = {GL_RGBA, GL_HALF_FLOAT, 8}; return true;
    case GL_RG16F: f = {GL_RG, GL_HALF_FLOAT, 4}; return true;
    case GL_R16F: f = {GL_RED, GL_HALF_FLOAT, 2}; return true;
    case GL_RGBA32F: f = {GL_RGBA, GL_FLOAT, 16}; return true;
    case GL_RG32F: f = {GL_RG, GL_FLOAT, 8}; return true;
    case GL_R32F: f = {GL_RED, GL_FLOAT, 4}; return true;
    default: return false;
  }
}

constexpr size_t kChunk = size_t(2) << 20;  // compressed in 2 MB pieces, side by side
std::atomic<size_t> g_inflight{0};          // raw bytes read back and not compressed yet
std::atomic<size_t> g_unpacking{0};         // raw bytes being unpacked (at most about 1 GB ahead)

}  // namespace

struct UndoTexture::Packed {
  int w = 0, h = 0;
  GLenum internal = 0;
  Format f{};
  size_t rawSize = 0;
  // per chunk: deflated, or (uniform) one pixel repeated
  struct Chunk {
    std::vector<uint8_t> data;
    bool uniform = false;
  };
  std::vector<Chunk> chunks;
  std::unique_ptr<uint8_t[]> raw;  // until every chunk is compressed (not zero-filled: it is overwritten)
  std::atomic<int> left{0};
  std::atomic<size_t> packedSize{0};
  std::mutex m;
  std::condition_variable done;
  // unpacking (prefetch): every chunk side by side into `out`
  bool unpacking = false;
  std::unique_ptr<uint8_t[]> out;
  std::atomic<int> outLeft{0};

  void wait() {
    std::unique_lock<std::mutex> l(m);
    done.wait(l, [&] { return left.load() == 0; });
  }
  size_t ram() const { return left.load() ? rawSize : packedSize.load(); }
};

UndoTexture::~UndoTexture() = default;

int UndoTexture::pixelBytes(GLenum internal) {
  Format f;
  return formatOf(internal, f) ? f.bytes : 0;
}

bool UndoTexture::pack() {
  if (!gpu_ || !gpu_->valid()) return false;
  Format f;
  if (!formatOf(gpu_->format(), f)) return false;
  auto p = std::make_shared<Packed>();
  p->w = gpu_->width(), p->h = gpu_->height(), p->internal = gpu_->format(), p->f = f;
  p->rawSize = size_t(p->w) * p->h * f.bytes;
  p->raw.reset(new uint8_t[p->rawSize]);
  g_inflight += p->rawSize;
  gpu_->downloadRaw(f.fmt, f.type, p->raw.get(), p->rawSize);
  gpu_.reset();
  size_t n = (p->rawSize + kChunk - 1) / kChunk;
  p->chunks.resize(n);
  p->left = (int)n;
  for (size_t i = 0; i < n; ++i)
    pool().run([p, i] {
      size_t at = i * kChunk, len = std::min(kChunk, p->rawSize - at);
      const uint8_t* src = p->raw.get() + at;
      Packed::Chunk& c = p->chunks[i];
      // the same pixel all over (an empty or flat area): that pixel
      size_t bpp = size_t(p->f.bytes);
      bool same = len % bpp == 0;
      for (size_t k = bpp; same && k < len; k += bpp) same = std::memcmp(src, src + k, bpp) == 0;
      if (same) {
        c.uniform = true;
        c.data.assign(src, src + bpp);
      } else {
        mz_ulong out = mz_compressBound((mz_ulong)len);
        c.data.resize(out);
        if (mz_compress2(c.data.data(), &out, src, (mz_ulong)len, MZ_BEST_SPEED) != MZ_OK) out = 0;
        c.data.resize(out);
        c.data.shrink_to_fit();
      }
      p->packedSize += c.data.size();
      std::lock_guard<std::mutex> l(p->m);
      if (--p->left == 0) {
        p->raw.reset();
        g_inflight -= p->rawSize;
        p->done.notify_all();
      }
    });
  packed_ = std::move(p);
  return true;
}

void UndoTexture::prefetch() {
  Packed* p = packed_.get();
  if (!p || p->unpacking) return;
  size_t ahead = g_unpacking.load();
  if (ahead && ahead + p->rawSize > (size_t(1) << 30)) return;  // (take() unpacks it when its turn comes)
  p->wait();
  p->unpacking = true;
  g_unpacking += p->rawSize;
  p->out.reset(new uint8_t[p->rawSize]);
  p->outLeft = (int)p->chunks.size();
  std::shared_ptr<Packed> keep = packed_;
  for (size_t i = 0; i < p->chunks.size(); ++i)
    pool().run([keep, i] {
      Packed& q = *keep;
      size_t at = i * kChunk, len = std::min(kChunk, q.rawSize - at);
      const Packed::Chunk& c = q.chunks[i];
      uint8_t* dst = q.out.get() + at;
      if (c.uniform) {
        for (size_t k = 0; k + c.data.size() <= len; k += c.data.size()) std::memcpy(dst + k, c.data.data(), c.data.size());
      } else {
        mz_ulong out = (mz_ulong)len;
        if (c.data.empty() || mz_uncompress(dst, &out, c.data.data(), (mz_ulong)c.data.size()) != MZ_OK) std::memset(dst, 0, len);
      }
      std::lock_guard<std::mutex> l(q.m);
      if (--q.outLeft == 0) q.done.notify_all();
    });
}

std::unique_ptr<Texture2D> UndoTexture::take() {
  if (gpu_) return std::move(gpu_);
  if (!packed_) return nullptr;
  prefetch();
  std::shared_ptr<Packed> p = std::move(packed_);
  {
    std::unique_lock<std::mutex> l(p->m);
    p->done.wait(l, [&] { return p->outLeft.load() == 0; });
  }
  auto t = std::make_unique<Texture2D>(p->w, p->h, p->internal, 1);
  t->uploadRaw(p->f.fmt, p->f.type, p->out.get());
  g_unpacking -= p->rawSize;
  return t;
}

void UndoTexture::swapWith(std::unique_ptr<Texture2D>& live) {
  std::unique_ptr<Texture2D> t = take();
  std::swap(live, t);
  gpu_ = std::move(t);
}

size_t UndoTexture::gpuBytes() const {
  if (!gpu_) return 0;
  int b = pixelBytes(gpu_->format());
  return size_t(gpu_->width()) * gpu_->height() * size_t(b ? b : 4);
}

size_t UndoTexture::ramBytes() const { return packed_ ? packed_->ram() : 0; }

void UndoTexture::finishPacking() { pool().wait(); }

size_t UndoTexture::packingBytes() { return g_inflight.load(); }

}  // namespace patina
