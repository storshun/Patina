#pragma once
#include <vector>

#include "core/Mesh.h"
#include "gfx/GL.h"

namespace patina {

// GPU copy of a Mesh. One GPU vertex per loop (face corner) so UVs, normals and
// tangents can be discontinuous. Attributes:
//   0 pos(vec3) 1 normal(vec3) 2 uv(vec2: texture / atlas) 3 tangent(vec4) 4 ids(uvec2: face, island)
//   5 flags(uint) 6 aux(vec4) 7 mesh uv(vec2: as authored)
class GpuMesh {
 public:
  GpuMesh() = default;
  ~GpuMesh() { destroy(); }
  // (1.2) moved, never copied (the scene's objects move in and out of the engine's document)
  GpuMesh(const GpuMesh&) = delete;
  GpuMesh& operator=(const GpuMesh&) = delete;
  GpuMesh(GpuMesh&& o) noexcept { *this = std::move(o); }
  GpuMesh& operator=(GpuMesh&& o) noexcept;
  void upload(const Mesh& m);
  void updateUVs(const Mesh& m);              // after UV edits (UVs, tangents, islands)
  void updateFlags(const std::vector<uint32_t>& loopFlags);  // selection highlight etc.
  // Set on every loop of a mirror copy face (FaceMirrorCopy): such faces share their texels with
  // the half they copy, so texture-space bakes leave them out. Kept through updateFlags.
  static constexpr uint32_t kMirrorCopyFlag = 1u << 8;
  // The face's texture tile (Mesh::faceTile) in flag bits 16..23 (UV editor: show one tile).
  static constexpr uint32_t kTileShift = 16;
  void updateEdges(const Mesh& m);            // seams changed
  void setAux(const std::vector<vec4>& perLoop);  // attribute 6 (reprojection target UVs, curvature...)
  void destroy();
  // (0.39) the path tracer reads these as storage buffers: per loop 14 floats (pos 3, normal 3, texture uv 2,
  // tangent 4, mesh uv 2); per triangle 3 loop indices. serial() changes when they do.
  GLuint vertexBuffer() const { return vbo_; }
  GLuint indexBuffer() const { return ibo_; }
  uint64_t serial() const { return serial_; }
  uint64_t auxSerial() const { return auxSerial_; }  // (1.2) changes with every setAux

  void drawTriangles() const;
  void drawEdges() const;    // all edges as lines (positions from loops)
  void drawSeams() const;    // only seam edges
  bool valid() const { return vao_ != 0; }
  uint32_t triangleCount() const { return triCount_; }

 private:
  GLuint vao_ = 0, vbo_ = 0, ibo_ = 0, idsBuf_ = 0, flagBuf_ = 0, auxBuf_ = 0;
  uint64_t serial_ = 0, auxSerial_ = 0;
  GLuint edgeIbo_ = 0, seamIbo_ = 0, edgeVao_ = 0;
  uint32_t triCount_ = 0, edgeCount_ = 0, seamCount_ = 0, loopCount_ = 0;
  std::vector<uint32_t> userFlags_, copyBits_;
  void setCopyBits(const Mesh& m);
  void writeFlags();
};

}  // namespace patina
