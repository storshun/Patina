#pragma once
#include <memory>
#include <vector>

#include "core/Image.h"
#include "engine/Channels.h"
#include "engine/Layer.h"
#include "gfx/Texture.h"

namespace patina {

struct Mesh;
class Bvh;
class GpuMesh;
class MaterialLibrary;

// Owns everything that lives in texture space for one mesh: baked mesh maps,
// the composite of the layer stack and the final (dilated) output maps.
class TextureSet {
 public:
  void allocate(int resolution);
  int resolution() const { return res_; }
  ivec2 size() const { return {res_, res_}; }

  // Geometry in texture space. Call whenever UVs change.
  void bakeMeshMaps(const Mesh& mesh, GpuMesh& gpu);
  // (1.1) The same geometry (position with coverage, normal, tangent) of any mesh into textures of any one size
  // (RGBA32F, RGBA16F, RGBA16F): the bake-down reads the low-poly's before it becomes the model.
  static void rasterizeGeometry(GpuMesh& gpu, Texture2D& pos, Texture2D& nrm, Texture2D& tan);
  // Ambient occlusion (directions = samples) and curvature. Optional.
  // With `occluder` (a high-poly mesh) and per-texel position / normal textures, the AO is
  // evaluated on the high-poly surface instead of the low-poly one.
  void bakeAO(const Mesh& mesh, GpuMesh& gpu, int samples = 64, float strength = 1.f, GpuMesh* occluder = nullptr,
              const Texture2D* posTex = nullptr, const Texture2D* nrmTex = nullptr, const AABB* occluderBounds = nullptr);
  // Material slot per texel (Mesh::faceMaterial). Done by bakeMeshMaps unless the IDs came from a high-poly bake.
  void bakeMaterialIds(const Mesh& mesh, GpuMesh& gpu);
  // Rasterizes a per-loop value (vec4) into `target` (texture space), cleared to `clear` first.
  void rasterizeAux(GpuMesh& gpu, const std::vector<vec4>& aux, Texture2D& target, vec4 clear);
  bool hasHighNormal() const { return hasHighNormal_; }
  void setHighNormal(bool on) { hasHighNormal_ = on; }
  bool materialIdsFromHigh = false;
  // Curvature (0.5 = flat, brighter = convex edge, darker = crease). With a BVH it is measured
  // per texel with short rays (`rays` each way, reaching `radius`; <= 0: 3% of the bounding
  // radius), so it does not depend on tessellation; without one, per-vertex curvature. With
  // `detail` and a high-poly normal map baked, the normal map's own bumps and edges are added.
  void bakeCurvature(const Mesh& mesh, GpuMesh& gpu, const Bvh* bvh, float strength = 1.f, bool detail = true,
                     int rays = 16, float radius = -1.f);
  // Thickness (1 = thick, 0 = thin): rays cast into the model from every texel (CPU, BVH).
  // distance <= 0: half the bounding radius.
  void bakeThickness(const Mesh& mesh, const Bvh& bvh, int rays = 32, float distance = -1.f);
  bool hasAO() const { return hasAO_; }
  bool hasCurvature() const { return hasCurv_; }
  bool hasThickness() const { return hasThick_; }
  bool hasBentNormals() const { return hasBent_; }  // baked together with the AO
  bool curvatureHasDetail() const { return curvDetail_; }
  void setBaked(bool ao, bool curvature) { hasAO_ = ao, hasCurv_ = curvature; if (!ao) hasBent_ = false; }
  void setThicknessBaked(bool on) { hasThick_ = on; }
  void setBentBaked(bool on) { hasBent_ = on; }

  // ---- (0.41) baked map layers (LayerType::Baked)
  // The baked maps everything reads (smart masks, the 3D view, exports): the bake with its layer's fixes and
  // adjustments, or the bake as it is (no layer, or the layer hidden). aoMap, curvMap and thickMap stay the bakes.
  const Texture2D& bakedView(int k) const { return bakedOn_[std::clamp(k, 0, kBakedMapCount - 1)] ? bakedEff_[k] : bakedRaw(k); }
  const Texture2D& aoView() const { return bakedView(0); }
  const Texture2D& curvView() const { return bakedView(1); }
  const Texture2D& thickView() const { return bakedView(2); }
  const Texture2D& bakedRaw(int k) const { return k == 1 ? curvMap : k == 2 ? thickMap : aoMap; }
  bool hasBakedMap(int k) const { return k == 1 ? hasCurv_ : k == 2 ? hasThick_ : hasAO_; }
  bool bakedEdited(int k) const { return bakedOn_[std::clamp(k, 0, kBakedMapCount - 1)]; }
  // (0.44.2) the map's layer is switched on (or it has none): the 3D view shades with it and exports write it
  bool bakedShown(int k) const { return bakedShown_[std::clamp(k, 0, kBakedMapCount - 1)]; }
  // (0.44.2) the UVs each bake was made for (uv::uvHash; 0: not known). UV edits move a bake along with the paint,
  // which blurs and breaks it where islands changed: a new bake is exact again.
  uint64_t bakedUV[kBakedMapCount] = {};
  uint64_t bakeSerial() const { return bakeSerial_; }  // goes up with every bake of AO, curvature or thickness
  bool bakedLayersOn = true;  // feature "baked_layers": off, the layers change nothing
  // Evaluates the baked map layers into the views (composite does it first).
  void applyBakedLayers(const std::vector<std::unique_ptr<Layer>>& layers, const AABB& bounds);
  // A baked map layer's bake with its fixes (no adjustments) into `out` (R16F, made at the texture set's size):
  // what its brush strokes blur, smudge and clone.
  void bakedWithFixes(const Layer& L, Texture2D& out);

  // Composite the layer stack into the final maps.
  // Folders nest (Layer::parent); soloUid != 0 shows only that layer (or folder) over the base.
  // (0.44.2) `box` (texels x0, y0, x1, y1, exclusive): only that region changed (a brush stroke), so only it is
  // composited and its mip levels updated; the rest keeps the last composite. Null: everything.
  void composite(const std::vector<std::unique_ptr<Layer>>& layers, MaterialLibrary& lib, const AABB& bounds,
                 uint32_t soloUid = 0, const glm::ivec4* box = nullptr);
  void updateMipsRegion(Texture2D& t, glm::ivec4 box);

  // Moves texture content from old UVs (current GPU mesh UVs) to new UVs (per loop).
  // Rasterizes at the new UVs and samples `src` at the old ones. Result replaces `tex`.
  // newSize > 0: the result has that size (texture tiles: the atlas grows or shrinks).
  // `source`: an unchanged copy of `tex` (such as its undo copy) to read from, which saves a copy.
  // Scratch textures are kept between calls until releaseScratch().
  void reproject(GpuMesh& gpu, const std::vector<vec2>& newUVs, Texture2D& tex, int newSize = 0,
                 const Texture2D* source = nullptr);
  void releaseScratch() { scratch_.clear(); }
  // (1.2) An object of the scene not being painted: the composite's working textures go (about a sixth of a texture
  // set), made again by the next composite, which then does all of it
  void sleep();
  bool asleep() const { return res_ > 0 && !accColor.valid(); }

  // Fills the gutter around UV islands with the nearest island texel (current layout).
  void dilateInPlace(Texture2D& t);
  void dilateInto(const Texture2D& src, Texture2D& dst);  // the same, into another texture
  // Replaces `tex` by sampling it at per-texel source UVs (RG32F lookup, x < 0 = empty).
  void resampleByLookup(const Texture2D& lookup, Texture2D& tex);

  // Base values below all layers
  FlatValues base;
  vec3 baseNormal{0, 0, 1};
  float heightNormalStrength = 6.f;  // normal intensity derived from height
  bool dilate = true;

  // Mesh maps
  Texture2D posMap, nrmMap, tanMap;  // object-space position (w=coverage), normal (w=face), tangent
  Texture2D seedMap;                 // jump-flood nearest covered texel (RG, x < 0 = none)
  // Gutter texels next to a UV seam: the UV of the same surface point on the other side of
  // the seam (RG32F, x < 0 = none). Lets blur and smudge reach across seams. Built on demand.
  Texture2D seamMap;
  const Texture2D& ensureSeamMap(const Mesh& mesh, const Bvh& bvh);
  bool seamMapValid() const { return seamValid_; }
  Texture2D aoMap, curvMap;                   // R8 (as RGBA8)
  Texture2D thickMap;                         // r = thickness, 1 thick .. 0 thin (RGBA8)
  Texture2D bentMap;                          // object-space bent normal, xyz (RGBA16F)
  Texture2D matIdMap;                         // r = material slot / 255 (RGBA8)
  Texture2D highNormal;                       // object-space normal baked from a high-poly mesh (RGBA16F)
  // Composite and outputs
  Texture2D accColor, accRMH, accNormal, accEmissive, accExtra;
  Texture2D finColor, finRMH, finNormalDetail, finEmissive, finNormal;
  Texture2D finExtra;  // r opacity, g sheen, b clearcoat, a subsurface (0.26)
  // Defaults to bind for missing inputs
  Texture2D white, transparent, flatNormalSigned;

  uint64_t compositeCount = 0;
  // Which of opacity, sheen, clearcoat and subsurface (channel bits 64..512) the last composite used (the base
  // values, painted channels, fill materials): exports write only those maps.
  int extraUsed = 0;
  MaterialLibrary* materials = nullptr;  // mask Pattern effects sample library materials (set by the engine)

  // ---- tiles (exports larger than the texture set)
  // Makes this (allocated at the tile size) a window onto `full`: mesh maps rasterized for the
  // UV rectangle uv0 .. uv0 + uvSize at full detail, baked maps resampled, settings copied.
  void setupTile(const TextureSet& full, const Mesh& mesh, GpuMesh& gpu, vec2 uv0, vec2 uvSize, int fullRes);
  // Resamples the UV rectangle of `src` onto all of `dst` (bilinear, or nearest for id maps).
  static void resampleRegion(const Texture2D& src, Texture2D& dst, vec2 uv0, vec2 uvSize, bool nearest = false);
  int effectiveRes() const { return effectiveRes_ > 0 ? effectiveRes_ : res_; }

 private:
  bool bakeCurvatureRays(const Mesh& mesh, const Bvh& bvh, Texture2D& out, float strength, int rays, float radius);
  void bakeCurvatureVertices(const Mesh& mesh, GpuMesh& gpu, Texture2D& out, float strength, bool smoothOnly);
  void computeSeeds();
  void addNormalMapCurvature(float strength);
  std::vector<std::unique_ptr<Texture2D>> folderSnaps_;  // 5 per nesting depth (accumulator copies)
  Texture2D maskFx_[3];                                  // mask effect stack ping-pong (R16F)
  Texture2D maskView_;                                   // layerMaskTexture
  std::vector<std::unique_ptr<Texture2D>> scratch_;       // reproject: one per size and format
  Texture2D& scratch(int w, int h, GLenum fmt);
  void makeAccumulators();
 public:
  // Evaluates a layer's mask effect stack; returns the texture holding the result
  // (valid until the next call). Used by the compositor and the mask view.
  const Texture2D& evalMaskStack(const Layer& L, const AABB& bounds);
  // The layer's effective mask (painted mask x smart mask, or its effect stack), 0..1, row 0 = top.
  Image layerMaskImage(const Layer& L, const AABB& bounds);
  const Texture2D& layerMaskTexture(const Layer& L, const AABB& bounds);  // the same on the GPU (valid until the next call)
 private:
  int res_ = 0;
  int effectiveRes_ = 0;           // tiles: the resolution of the whole export
  vec4 tileUV_{0.f, 0.f, 1.f, 1.f};
  bool seamValid_ = false;
  bool hasAO_ = false, hasCurv_ = false, hasHighNormal_ = false, hasThick_ = false, hasBent_ = false;
  bool curvDetail_ = false;
  Texture2D bakedEff_[kBakedMapCount];  // (0.41) the baked maps with their layers (bakedOn_)
  bool bakedOn_[kBakedMapCount] = {};
  bool bakedShown_[kBakedMapCount] = {true, true, true};
  uint64_t bakeSerial_ = 0;
};

}  // namespace patina
