#pragma once
#include <vector>

#include "core/Math.h"

namespace patina {

struct Mesh;

struct RayHit {
  bool hit = false;
  float t = std::numeric_limits<float>::max();
  uint32_t tri = 0;
  vec2 bary{0};  // weights for vertex 1 and 2 (w0 = 1 - u - v)
  vec3 position{0};
  vec3 normal{0};    // interpolated shading normal
  vec3 faceNormal{0};
  vec2 uv{0};      // texture coordinate (the atlas position when the mesh has texture tiles)
  vec2 meshUV{0};  // the UV as authored
  uint32_t face = 0;
};

// Bounding-volume hierarchy over the mesh triangulation (object space).
class Bvh {
 public:
  void build(const Mesh& mesh);
  // (1.2) skipFaces: per face, nonzero for faces the ray goes through (hidden ones)
  RayHit raycast(const Mesh& mesh, const vec3& origin, const vec3& dir, float tMax = 1e30f,
                 const std::vector<uint8_t>* skipFaces = nullptr) const;
  // Closest point on the surface to p (within maxDist). Fills tri, bary, position, uv, face.
  RayHit closestPoint(const Mesh& mesh, const vec3& p, float maxDist = 1e30f) const;
  // All triangles whose bounds intersect a sphere (for 3D-brush and fill tools).
  void querySphere(const vec3& c, float r, std::vector<uint32_t>& outTris) const;
  bool empty() const { return nodes_.empty(); }

  struct Node {
    AABB box;
    uint32_t left = 0;   // interior: the right child (the left one is the next node); leaf: the first tri index
    uint32_t count = 0;  // 0 = interior
  };
  // (0.39) for the path tracer's copy on the GPU; serial() changes with every build
  const std::vector<Node>& nodes() const { return nodes_; }
  const std::vector<uint32_t>& triIndices() const { return triIdx_; }
  uint64_t serial() const { return serial_; }

 private:
  uint64_t serial_ = 0;
  std::vector<Node> nodes_;
  std::vector<uint32_t> triIdx_;
  std::vector<AABB> triBox_;
  std::vector<vec3> triCenter_;
  uint32_t buildRec(uint32_t begin, uint32_t end, int depth);
};

}  // namespace patina
