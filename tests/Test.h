#pragma once
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace ptest {
struct Case { std::string group, name; std::function<void()> fn; };
std::vector<Case>& registry();
extern int failures;
struct Reg { Reg(const char* g, const char* n, std::function<void()> f) { registry().push_back({g, n, f}); } };
}
#define PT_CAT2(a, b) a##b
#define PT_CAT(a, b) PT_CAT2(a, b)
#define TEST(group, name) \
  static void PT_CAT(test_, name)(); \
  static ptest::Reg PT_CAT(reg_, name)(#group, #name, PT_CAT(test_, name)); \
  static void PT_CAT(test_, name)()
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); ++ptest::failures; } } while (0)
#define CHECK_NEAR(a, b, eps) do { double _a = (a), _b = (b); if (std::abs(_a - _b) > (eps)) { std::fprintf(stderr, "  FAIL %s:%d: %s=%g vs %s=%g\n", __FILE__, __LINE__, #a, _a, #b, _b); ++ptest::failures; } } while (0)
