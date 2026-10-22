#pragma once
// Retopology, 0.23: a change remakes only the quads near it. The last quads are kept wherever nothing
// changed (lines drawn or taken away, flow strokes, painted density, a zone's resolution); near the change
// the quads of a new build are taken; between the two, one row of faces is laid, chosen so that as many of
// its vertices as can have four edges (corner quads where one side's outline turns, 3-to-1 steps only where
// the counts need them, a triangle only for an odd count). The edge lines crossing the row run on across it.
#include <functional>
#include <string>
#include <vector>

#include "core/Mesh.h"

namespace patina::retopo {

struct LocalStats {
  int keptFaces = 0, newFaces = 0, rowFaces = 0, rowTriangles = 0, seams = 0, linesAcross = 0;
  int rowPoles = 0;  // the row's vertices with other than four edges (or three on an open edge)
  std::string why;   // when it couldn't be done (the new build is used whole)
};

struct LocalChange {
  std::vector<std::vector<vec3>> lines;  // what changed: lines and strokes drawn or taken away (open polylines)
  std::vector<vec3> points;              // and points (painted density, a zone's faces)
  // closed lines among them (drawn or taken away): the inside of a small one is remade whole
  std::vector<std::vector<vec3>> loops;
};

// old: the quads kept where nothing changed; fresh: a build with the change, taken near it. L: the quads'
// size; reach: how far round the change the quads are remade. edgeLines: the edge lines now (they run on
// across the row). snap: a point put back on the model's surface. On success out holds the quads.
// near (when given): per vertex of out, 1 on the row and within two edges of it (where its poles may be
// tidied), 0 elsewhere.
bool keepAwayFromChange(const Mesh& old, const Mesh& fresh, const LocalChange& change, float L, float reach,
                        const std::vector<std::vector<vec3>>& edgeLines, const std::function<vec3(const vec3&)>& snap, Mesh& out,
                        LocalStats& stats, std::vector<char>* near = nullptr);

}  // namespace patina::retopo
