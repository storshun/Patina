#pragma once
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace patina {

using glm::ivec2;
using glm::mat3;
using glm::mat4;
using glm::quat;
using glm::vec2;
using glm::vec3;
using glm::vec4;

constexpr float kPi = 3.14159265358979323846f;

struct AABB {
  vec3 lo{std::numeric_limits<float>::max()};
  vec3 hi{-std::numeric_limits<float>::max()};
  void add(const vec3& p) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  void add(const AABB& b) {
    lo = glm::min(lo, b.lo);
    hi = glm::max(hi, b.hi);
  }
  bool valid() const { return lo.x <= hi.x; }
  vec3 center() const { return (lo + hi) * 0.5f; }
  vec3 size() const { return hi - lo; }
  float radius() const { return glm::length(size()) * 0.5f; }
};

struct Rect2 {
  vec2 lo{std::numeric_limits<float>::max()};
  vec2 hi{-std::numeric_limits<float>::max()};
  void add(const vec2& p) {
    lo = glm::min(lo, p);
    hi = glm::max(hi, p);
  }
  bool valid() const { return lo.x <= hi.x; }
  vec2 size() const { return hi - lo; }
  vec2 center() const { return (lo + hi) * 0.5f; }
};

inline float saturate(float v) { return std::clamp(v, 0.f, 1.f); }

// 2D cross product (z component)
inline float cross2(const vec2& a, const vec2& b) { return a.x * b.y - a.y * b.x; }

// p inside (or on an edge of) the 2D triangle abc, either winding. A triangle with no area
// contains nothing: one whose corners collapsed onto one point would otherwise "contain"
// every point (all three edge tests are zero).
inline bool pointInTriangle2(const vec2& a, const vec2& b, const vec2& c, const vec2& p) {
  if (std::abs(cross2(b - a, c - a)) < 1e-12f) return false;
  float d1 = cross2(b - a, p - a), d2 = cross2(c - b, p - b), d3 = cross2(a - c, p - c);
  bool neg = d1 < 0 || d2 < 0 || d3 < 0, pos = d1 > 0 || d2 > 0 || d3 > 0;
  return !(neg && pos);
}

// sRGB transfer function (one channel)
inline float srgbToLinear(float c) {
  return c <= 0.04045f ? c / 12.92f : std::pow((c + 0.055f) / 1.055f, 2.4f);
}
inline float linearToSrgb(float c) {
  return c <= 0.0031308f ? c * 12.92f : 1.055f * std::pow(c, 1.f / 2.4f) - 0.055f;
}

// Small deterministic hash-based RNG (PCG-ish) for jitter, reproducible in tests.
struct Rng {
  uint64_t state;
  explicit Rng(uint64_t seed = 0x853c49e6748fea9bULL) : state(seed ? seed : 1) {}
  uint32_t next() {
    uint64_t old = state;
    state = old * 6364136223846793005ULL + 1442695040888963407ULL;
    uint32_t xorshifted = uint32_t(((old >> 18u) ^ old) >> 27u);
    uint32_t rot = uint32_t(old >> 59u);
    return (xorshifted >> rot) | (xorshifted << ((32 - rot) & 31));
  }
  float uniform() { return (next() >> 8) * (1.0f / 16777216.0f); }        // [0,1)
  float signedUniform() { return uniform() * 2.f - 1.f; }                   // [-1,1)
};

}  // namespace patina
