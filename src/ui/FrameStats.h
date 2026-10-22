#pragma once
// For profiling (PATINA_FRAME_STATS=<file>): the CPU time of each part of a frame, and the GPU time of the
// 3D viewport and of the whole frame (timestamp queries, read a few frames later), summed over 120 frames
// and written as one line. Off (and free) without the variable.
#include <glad/gl.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>

namespace patina {

struct FrameStats {
  using clock = std::chrono::steady_clock;
  bool on = false;
  FILE* out = nullptr;
  clock::time_point last, frameStart;
  std::vector<std::pair<std::string, std::pair<double, double>>> cpu;  // section -> (sum, max) ms
  int frames = 0;
  double frameSum = 0, frameMax = 0;
  GLuint q[4][4] = {};  // per frame in flight: frame start, viewport start, viewport end, frame end
  bool pending[4] = {}, viewMarked[2] = {};
  int ring = 0;
  double gpuView = 0, gpuFrame = 0, gpuViewMax = 0, gpuFrameMax = 0;
  int gpuN = 0;
  std::string note;  // (what was going on: "drawing" and the like)

  void init() {
    const char* p = std::getenv("PATINA_FRAME_STATS");
    if (!p || !*p) return;
    out = std::fopen(p, "w");
    on = out != nullptr;
    if (on) glGenQueries(16, &q[0][0]);
  }
  void beginFrame() {
    if (!on) return;
    collect();
    frameStart = last = clock::now();
    glQueryCounter(q[ring][0], GL_TIMESTAMP);
    viewMarked[0] = viewMarked[1] = false;
  }
  void mark(const char* name) {
    if (!on) return;
    auto now = clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    for (auto& [n, v] : cpu)
      if (n == name) {
        v.first += ms, v.second = std::max(v.second, ms);
        return;
      }
    cpu.push_back({name, {ms, ms}});
  }
  void gpuMark(int k) {  // 1: the viewport's drawing starts, 2: ends
    if (!on || viewMarked[k - 1]) return;
    glQueryCounter(q[ring][k], GL_TIMESTAMP);
    viewMarked[k - 1] = true;
  }
  void endFrame() {
    if (!on) return;
    gpuMark(1), gpuMark(2);
    glQueryCounter(q[ring][3], GL_TIMESTAMP);
    pending[ring] = true;
    ring = (ring + 1) % 4;
    double ms = std::chrono::duration<double, std::milli>(clock::now() - frameStart).count();
    frameSum += ms, frameMax = std::max(frameMax, ms);
    if (++frames == 120) flush();
  }
  void collect() {
    for (int i = 0; i < 4; ++i) {
      if (!pending[i]) continue;
      GLint ready = 0;
      glGetQueryObjectiv(q[i][3], GL_QUERY_RESULT_AVAILABLE, &ready);
      if (!ready) continue;
      GLuint64 t[4];
      for (int k = 0; k < 4; ++k) glGetQueryObjectui64v(q[i][k], GL_QUERY_RESULT, &t[k]);
      double v = (t[2] - t[1]) * 1e-6, f = (t[3] - t[0]) * 1e-6;
      gpuView += v, gpuFrame += f, gpuViewMax = std::max(gpuViewMax, v), gpuFrameMax = std::max(gpuFrameMax, f), ++gpuN;
      pending[i] = false;
    }
  }
  void flush() {
    std::fprintf(out, "%s%.1f fps | frame %.2f ms (max %.2f) | gpu viewport %.2f (max %.2f), frame %.2f (max %.2f) |", note.c_str(),
                 1000.0 * frames / std::max(frameSum, 1e-9), frameSum / frames, frameMax, gpuN ? gpuView / gpuN : 0.0, gpuViewMax,
                 gpuN ? gpuFrame / gpuN : 0.0, gpuFrameMax);
    std::sort(cpu.begin(), cpu.end(), [](auto& a, auto& b) { return a.second.first > b.second.first; });
    for (auto& [n, v] : cpu) std::fprintf(out, " %s %.2f/%.2f", n.c_str(), v.first / frames, v.second);
    std::fprintf(out, "\n");
    std::fflush(out);
    cpu.clear(), frames = 0, frameSum = frameMax = 0, gpuView = gpuFrame = gpuViewMax = gpuFrameMax = 0, gpuN = 0;
  }
};

}  // namespace patina
