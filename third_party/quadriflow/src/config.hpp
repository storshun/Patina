#ifndef CONFIG_H_
#define CONFIG_H_

// Move settings to cmake to make CMake happy :)

// #define WITH_SCALE
// #define WITH_CUDA

const int GRAIN_SIZE = 1024;

#include <atomic>
#include <stdexcept>
namespace qflow {
// Patina: a run stopped part way (set for the thread running it; checked in the long loops)
extern thread_local const std::atomic<bool>* patina_cancel;
inline void patina_check_cancel() {
    if (patina_cancel && patina_cancel->load(std::memory_order_relaxed)) throw std::runtime_error("cancelled");
}
// Patina: in loops that can go round for ever on a mesh QuadriFlow doesn't expect (a fan that never meets an
// open edge): checks the cancel now and then, and gives up after far more turns than a real run takes
extern thread_local long long patina_loop_count;
inline void patina_loop_guard() {
    if ((++patina_loop_count & 0xFFF) == 0) {
        patina_check_cancel();
        if (patina_loop_count > 400000000LL) throw std::runtime_error("quadriflow: a loop that doesn't end");
    }
}
}  // namespace qflow

#ifdef LOG_OUTPUT

#define lprintf(...) printf(__VA_ARGS__)
#define lputs(...) puts(__VA_ARGS__)

#else

#define lprintf(...) void(0)
#define lputs(...) void(0)

#endif

#include <chrono>

namespace qflow {

// simulation of Windows GetTickCount()
unsigned long long inline GetCurrentTime64() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}
} // namespace qflow

#endif
