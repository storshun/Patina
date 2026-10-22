#include "Test.h"
#include <cstring>
namespace ptest {
std::vector<Case>& registry() { static std::vector<Case> r; return r; }
int failures = 0;
}
int main(int argc, char** argv) {
  const char* group = argc > 1 ? argv[1] : nullptr;
  const char* only = argc > 2 ? argv[2] : nullptr;
  int run = 0;
  for (auto& c : ptest::registry()) {
    if (group && c.group != group) continue;
    if (only && c.name != only) continue;
    int before = ptest::failures;
    std::fprintf(stderr, "[ RUN  ] %s.%s\n", c.group.c_str(), c.name.c_str());
    c.fn();
    std::fprintf(stderr, "[ %s ] %s.%s\n", ptest::failures == before ? " OK " : "FAIL", c.group.c_str(), c.name.c_str());
    ++run;
  }
  std::fprintf(stderr, "%d tests, %d failed checks\n", run, ptest::failures);
  return ptest::failures ? 1 : 0;
}
