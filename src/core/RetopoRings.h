#pragma once
// Retopology, 0.16: a zone between two edge loops (round an eye, a tube of an arm) filled with rings of
// quads that follow both loops. The rings are the level lines of a smooth (harmonic) blend from one
// loop to the other, about a quad apart; the count round each ring stays the same from ring to ring
// unless the quads would stretch too far, and then changes in 3-to-1 steps (as where zones join).
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "core/Math.h"

namespace patina::retopo {

struct RingFill {
  std::vector<vec3> pos;
  std::vector<std::array<uint32_t, 4>> quads;
  std::vector<std::array<uint32_t, 3>> tris;
  int rings = 0;  // rows of quads from one loop to the other
  // the two edge loops as made: their vertices (in pos) and positions, in order round each
  std::vector<std::vector<uint32_t>> loopVerts;
  std::vector<std::vector<vec3>> loopPos;
  bool ok = false;
  std::string why;  // when it couldn't be done
};

// Whether these triangles make a ring: one piece with exactly two edge loops round it (no handles)
bool isRing(const std::vector<glm::uvec3>& tri);

// tri over pos: a ring (isRing); L: the quads' size. The faces turn the same way as the triangles.
// fixed: loops already made by a ring beside this one (points along one of its edge loops): that loop
// gets exactly these points, so the two rings share them. loose (per vertex of pos, or empty): on a
// border that takes whatever count the ring gives it (the model's open edge, a small disc or a ring made
// after this one); a loop along such a border keeps the count of the other loop further (no 3-to-1 steps
// are needed there), while a loop along another zone gets about as many points as that zone has there.
// plan: counts chosen for loops beforehand (planRingCounts), found by where they are.
struct RingPlanLoop {
  vec3 center{0};
  double length = 0;  // (world units)
  long count = 0;
};
RingFill fillRings(const std::vector<vec3>& pos, const std::vector<glm::uvec3>& tri, float L,
                   const std::vector<std::vector<vec3>>& fixed = {}, const std::vector<char>& loose = {},
                   const std::vector<RingPlanLoop>& plan = {});

// The counts round a chain of ring zones' loops (an eye's rings, one inside the next), chosen all at once:
// each loop's quads between kMinW and kMaxW of a quad wide; 3-to-1 steps (two poles each way) as few as
// can be, inside the rings or where a loop meets another zone (which has about length / L points there);
// a little rather there, so the rings keep one count. Per loop: its length in quads, whether it meets
// another zone (else the model's open edge, a disc or a ring: any count), and the two loops of each ring.
struct RingChainLoop {
  double lengthQ = 0;  // (in the rings' quads)
  bool meetsZone = false;
  long fixedCount = 0;  // a count it must have (the zone beside it made its loop already), or 0
};
std::vector<long> planRingCounts(const std::vector<RingChainLoop>& loops, const std::vector<std::array<int, 2>>& rings);
inline constexpr double kRingMinW = 0.55, kRingMaxW = 1.6;

// How many points the loop in fixed that runs all the way along Q has (as fillRings takes them), or 0
size_t matchingLoopCount(const std::vector<vec3>& Q, const std::vector<std::vector<vec3>>& fixed, float L);
// How many of a zone's edge loop's n points a ring taking it may put onto its line (where the loop leaves
// the line for a quad, or jogs across it)
size_t movableLoopPoints(size_t n);

// A small disc (one edge loop round it, no holes) closed with rings shrinking in from its loop and a
// small fill in the middle (as a pupil or the inside of the lips); fixed as for fillRings.
bool isDisc(const std::vector<glm::uvec3>& tri);
RingFill fillCap(const std::vector<vec3>& pos, const std::vector<glm::uvec3>& tri, float L,
                 const std::vector<std::vector<vec3>>& fixed = {}, const std::vector<char>& open = {});

}  // namespace patina::retopo
