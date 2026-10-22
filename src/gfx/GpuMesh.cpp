#include "gfx/GpuMesh.h"

#include <algorithm>
#include <utility>

namespace patina {

namespace {
struct Vtx {
  vec3 pos;
  vec3 nrm;
  vec2 uv;      // texture coordinate (atlas)
  vec4 tan;
  vec2 meshUV;  // the UV as authored (UV editor display)
};

std::vector<Vtx> buildVerts(const Mesh& m) {
  std::vector<Vtx> v(m.loopCount());
  for (uint32_t l = 0; l < m.loopCount(); ++l)
    v[l] = {m.positions[m.loopVert[l]], m.loopNormal[l], m.texUV(l), m.loopTangent[l], m.loopUV[l]};
  return v;
}

std::vector<uint32_t> buildIds(const Mesh& m) {
  std::vector<uint32_t> ids(m.loopCount() * 2);
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      ids[l * 2] = f;
      ids[l * 2 + 1] = l < m.loopIsland.size() ? m.loopIsland[l] : 0;
    }
  return ids;
}

// Replaces a vertex buffer's contents with a new buffer object. Updating in place
// (glNamedBufferSubData) raced with the draws on NVIDIA 610 drivers: the next draw could still
// read the old contents (selection flags, moved UVs), even after a flush. Fresh storage can't.
void renewBuffer(GLuint& buf, size_t bytes, const void* data) {
  GLuint fresh = 0;
  glCreateBuffers(1, &fresh);
  glNamedBufferStorage(fresh, (GLsizeiptr)std::max<size_t>(bytes, 4), data, GL_DYNAMIC_STORAGE_BIT);
  if (buf) glDeleteBuffers(1, &buf);
  buf = fresh;
}
}  // namespace

void GpuMesh::destroy() {
  GLuint bufs[] = {vbo_, ibo_, idsBuf_, flagBuf_, edgeIbo_, seamIbo_, auxBuf_};
  for (GLuint b : bufs)
    if (b) glDeleteBuffers(1, &b);
  if (vao_) glDeleteVertexArrays(1, &vao_);
  if (edgeVao_) glDeleteVertexArrays(1, &edgeVao_);
  vao_ = vbo_ = ibo_ = idsBuf_ = flagBuf_ = edgeIbo_ = seamIbo_ = edgeVao_ = auxBuf_ = 0;
}

GpuMesh& GpuMesh::operator=(GpuMesh&& o) noexcept {
  if (this == &o) return *this;
  destroy();
  vao_ = std::exchange(o.vao_, 0), vbo_ = std::exchange(o.vbo_, 0), ibo_ = std::exchange(o.ibo_, 0);
  idsBuf_ = std::exchange(o.idsBuf_, 0), flagBuf_ = std::exchange(o.flagBuf_, 0), auxBuf_ = std::exchange(o.auxBuf_, 0);
  edgeIbo_ = std::exchange(o.edgeIbo_, 0), seamIbo_ = std::exchange(o.seamIbo_, 0), edgeVao_ = std::exchange(o.edgeVao_, 0);
  serial_ = std::exchange(o.serial_, 0);
  auxSerial_ = std::exchange(o.auxSerial_, 0);
  triCount_ = std::exchange(o.triCount_, 0), edgeCount_ = std::exchange(o.edgeCount_, 0);
  seamCount_ = std::exchange(o.seamCount_, 0), loopCount_ = std::exchange(o.loopCount_, 0);
  userFlags_ = std::move(o.userFlags_), copyBits_ = std::move(o.copyBits_);
  return *this;
}

static uint64_t g_meshSerial = 0;

void GpuMesh::upload(const Mesh& m) {
  destroy();
  serial_ = ++g_meshSerial;
  loopCount_ = m.loopCount();
  auto verts = buildVerts(m);
  auto ids = buildIds(m);
  userFlags_.assign(m.loopCount(), 0);
  setCopyBits(m);
  std::vector<uint32_t> flags = copyBits_;

  glCreateBuffers(1, &vbo_);
  glNamedBufferStorage(vbo_, verts.size() * sizeof(Vtx), verts.data(), GL_DYNAMIC_STORAGE_BIT);
  glCreateBuffers(1, &ibo_);
  glNamedBufferStorage(ibo_, m.tris.size() * sizeof(glm::uvec3), m.tris.data(), 0);
  glCreateBuffers(1, &idsBuf_);
  glNamedBufferStorage(idsBuf_, ids.size() * 4, ids.data(), GL_DYNAMIC_STORAGE_BIT);
  glCreateBuffers(1, &flagBuf_);
  glNamedBufferStorage(flagBuf_, flags.size() * 4, flags.data(), GL_DYNAMIC_STORAGE_BIT);
  std::vector<vec4> aux(m.loopCount(), vec4(0));
  glCreateBuffers(1, &auxBuf_);
  glNamedBufferStorage(auxBuf_, aux.size() * sizeof(vec4), aux.data(), GL_DYNAMIC_STORAGE_BIT);
  triCount_ = (uint32_t)m.tris.size();

  glCreateVertexArrays(1, &vao_);
  glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(Vtx));
  glVertexArrayVertexBuffer(vao_, 1, idsBuf_, 0, 8);
  glVertexArrayVertexBuffer(vao_, 2, flagBuf_, 0, 4);
  glVertexArrayVertexBuffer(vao_, 3, auxBuf_, 0, sizeof(vec4));
  glVertexArrayElementBuffer(vao_, ibo_);
  glEnableVertexArrayAttrib(vao_, 6);
  glVertexArrayAttribFormat(vao_, 6, 4, GL_FLOAT, GL_FALSE, 0);
  glVertexArrayAttribBinding(vao_, 6, 3);
  auto attr = [&](GLuint i, int n, GLuint off) {
    glEnableVertexArrayAttrib(vao_, i);
    glVertexArrayAttribFormat(vao_, i, n, GL_FLOAT, GL_FALSE, off);
    glVertexArrayAttribBinding(vao_, i, 0);
  };
  attr(0, 3, offsetof(Vtx, pos));
  attr(1, 3, offsetof(Vtx, nrm));
  attr(2, 2, offsetof(Vtx, uv));
  attr(3, 4, offsetof(Vtx, tan));
  attr(7, 2, offsetof(Vtx, meshUV));
  glEnableVertexArrayAttrib(vao_, 4);
  glVertexArrayAttribIFormat(vao_, 4, 2, GL_UNSIGNED_INT, 0);
  glVertexArrayAttribBinding(vao_, 4, 1);
  glEnableVertexArrayAttrib(vao_, 5);
  glVertexArrayAttribIFormat(vao_, 5, 1, GL_UNSIGNED_INT, 0);
  glVertexArrayAttribBinding(vao_, 5, 2);

  // edges share the loop vertex buffer: pick one loop per edge end
  glCreateVertexArrays(1, &edgeVao_);
  glVertexArrayVertexBuffer(edgeVao_, 0, vbo_, 0, sizeof(Vtx));
  glVertexArrayVertexBuffer(edgeVao_, 2, flagBuf_, 0, 4);
  glEnableVertexArrayAttrib(edgeVao_, 0);
  glVertexArrayAttribFormat(edgeVao_, 0, 3, GL_FLOAT, GL_FALSE, offsetof(Vtx, pos));
  glVertexArrayAttribBinding(edgeVao_, 0, 0);
  glEnableVertexArrayAttrib(edgeVao_, 2);
  glVertexArrayAttribFormat(edgeVao_, 2, 2, GL_FLOAT, GL_FALSE, offsetof(Vtx, uv));
  glVertexArrayAttribBinding(edgeVao_, 2, 0);
  glEnableVertexArrayAttrib(edgeVao_, 5);
  glVertexArrayAttribIFormat(edgeVao_, 5, 1, GL_UNSIGNED_INT, 0);
  glVertexArrayAttribBinding(edgeVao_, 5, 2);
  glEnableVertexArrayAttrib(edgeVao_, 7);
  glVertexArrayAttribFormat(edgeVao_, 7, 2, GL_FLOAT, GL_FALSE, offsetof(Vtx, meshUV));
  glVertexArrayAttribBinding(edgeVao_, 7, 0);
  updateEdges(m);
}

void GpuMesh::updateEdges(const Mesh& m) {
  if (!edgeVao_) return;
  // Every face edge as a loop pair (so UV-space wireframes show per-island edges too)
  std::vector<uint32_t> lines, seams;
  lines.reserve(m.loopCount() * 2);
  for (uint32_t f = 0; f < m.faceCount(); ++f)
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) {
      uint32_t n = m.nextLoop(l, f);
      lines.push_back(l);
      lines.push_back(n);
      if (m.edgeFlags[m.loopEdge[l]] & EdgeSeam) {
        seams.push_back(l);
        seams.push_back(n);
      }
    }
  if (edgeIbo_) glDeleteBuffers(1, &edgeIbo_);
  if (seamIbo_) glDeleteBuffers(1, &seamIbo_);
  glCreateBuffers(1, &edgeIbo_);
  glNamedBufferStorage(edgeIbo_, std::max<size_t>(4, lines.size() * 4), lines.empty() ? nullptr : lines.data(), 0);
  glCreateBuffers(1, &seamIbo_);
  glNamedBufferStorage(seamIbo_, std::max<size_t>(4, seams.size() * 4), seams.empty() ? nullptr : seams.data(), 0);
  edgeCount_ = (uint32_t)lines.size();
  seamCount_ = (uint32_t)seams.size();
}

void GpuMesh::updateUVs(const Mesh& m) {
  serial_ = ++g_meshSerial;
  if (!vbo_ || m.loopCount() != loopCount_) {
    upload(m);
    return;
  }
  auto verts = buildVerts(m);
  renewBuffer(vbo_, verts.size() * sizeof(Vtx), verts.data());
  glVertexArrayVertexBuffer(vao_, 0, vbo_, 0, sizeof(Vtx));
  glVertexArrayVertexBuffer(edgeVao_, 0, vbo_, 0, sizeof(Vtx));
  auto ids = buildIds(m);
  renewBuffer(idsBuf_, ids.size() * 4, ids.data());
  glVertexArrayVertexBuffer(vao_, 1, idsBuf_, 0, 8);
  setCopyBits(m);  // mirror copies change with UV edits
  writeFlags();
}

void GpuMesh::setCopyBits(const Mesh& m) {
  copyBits_.assign(m.loopCount(), 0);
  bool flags = m.faceFlags.size() == m.faceCount();
  for (uint32_t f = 0; f < m.faceCount(); ++f) {
    uint32_t bits = (uint32_t(m.faceTile(f)) & 0xFFu) << kTileShift;
    if (flags && (m.faceFlags[f] & FaceMirrorCopy)) bits |= kMirrorCopyFlag;
    for (uint32_t l = m.faceStart[f]; l < m.faceStart[f + 1]; ++l) copyBits_[l] = bits;
  }
}

void GpuMesh::writeFlags() {
  if (!flagBuf_ || userFlags_.size() != loopCount_ || copyBits_.size() != loopCount_) return;
  std::vector<uint32_t> f(loopCount_);
  for (uint32_t i = 0; i < loopCount_; ++i) f[i] = userFlags_[i] | copyBits_[i];
  renewBuffer(flagBuf_, f.size() * 4, f.data());
  glVertexArrayVertexBuffer(vao_, 2, flagBuf_, 0, 4);
  glVertexArrayVertexBuffer(edgeVao_, 2, flagBuf_, 0, 4);
}

void GpuMesh::setAux(const std::vector<vec4>& a) {
  if (!auxBuf_ || a.size() != loopCount_) return;
  renewBuffer(auxBuf_, a.size() * sizeof(vec4), a.data());
  glVertexArrayVertexBuffer(vao_, 3, auxBuf_, 0, sizeof(vec4));
  auxSerial_ = ++g_meshSerial;
}

void GpuMesh::updateFlags(const std::vector<uint32_t>& f) {
  if (!flagBuf_ || f.size() != loopCount_) return;
  userFlags_ = f;
  writeFlags();
}

void GpuMesh::drawTriangles() const {
  glBindVertexArray(vao_);
  glDrawElements(GL_TRIANGLES, triCount_ * 3, GL_UNSIGNED_INT, nullptr);
}

void GpuMesh::drawEdges() const {
  glBindVertexArray(edgeVao_);
  glVertexArrayElementBuffer(edgeVao_, edgeIbo_);
  glDrawElements(GL_LINES, edgeCount_, GL_UNSIGNED_INT, nullptr);
}

void GpuMesh::drawSeams() const {
  if (!seamCount_) return;
  glBindVertexArray(edgeVao_);
  glVertexArrayElementBuffer(edgeVao_, seamIbo_);
  glDrawElements(GL_LINES, seamCount_, GL_UNSIGNED_INT, nullptr);
}

}  // namespace patina
