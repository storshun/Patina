#pragma once
// Retopology, 0.15: joining the quads of zones made at different resolutions. Zones at one resolution
// are made together (a group); where groups meet, their border is split into chains at the points
// where three groups (or an open edge) meet. Along each chain the coarser side's quads within half a
// quad of it are taken out, and the gap is filled with a row that steps from one side's count to the
// other's in 3-to-1 steps (an odd difference needs one triangle). The small holes left where chains
// meet are filled with quads (and a triangle where the count is odd).
#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "core/Math.h"

namespace patina::retopo {

struct JoinChain {
  std::vector<vec3> line;  // along the border, end to end (closed: the last point is the first)
  bool closed = false;
  int g = 0, h = 0;  // the groups on its two sides
};

struct JoinStats {
  int chains = 0, steps31 = 0, oddSteps = 0, holesFilled = 0, welded = 0, gridHoles = 0;  // (gridHoles: holes closed with a grid)
  std::vector<vec3> patched;  // the middles of the bigger holes filled (where zones meet in a tight spot)
  std::string why;            // when it couldn't be done
};

// P/quads/tris with each face's group (all made already; the row's faces get group -1). groupL: each
// group's quad size. openEdges: the model's own open edges (holes along them are left open). On
// success the faces taken out are gone and bandV marks the joining rows' vertices (to be smoothed).
// snap: puts a point back on the model's surface (the rings filling a big hole follow the surface).
bool joinGroups(std::vector<vec3>& P, std::vector<std::array<uint32_t, 4>>& quads, std::vector<int>& quadGroup,
                std::vector<std::array<uint32_t, 3>>& tris, std::vector<int>& triGroup, const std::vector<JoinChain>& chains,
                const std::vector<double>& groupL, const std::vector<std::array<vec3, 2>>& openEdges, std::vector<char>& bandV,
                JoinStats& stats, const std::function<vec3(const vec3&)>& snap = {});

// One stepped row of quads between two runs of vertices (going the same way; closed: two loops), as
// the joins make them. The new faces face the way of any face already on F's first edge.
bool stepRow(std::vector<vec3>& P, std::vector<std::array<uint32_t, 4>>& quads, std::vector<std::array<uint32_t, 3>>& tris,
             const std::vector<uint32_t>& F, const std::vector<uint32_t>& C, bool closed);

// A hole closed with rings shrinking in from its outline and a small fill in the middle (V: the
// outline, the way the new faces run along it).
void capLoop(std::vector<vec3>& P, std::vector<std::array<uint32_t, 4>>& quads, std::vector<std::array<uint32_t, 3>>& tris,
             const std::vector<uint32_t>& V);

}  // namespace patina::retopo
