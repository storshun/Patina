// (0.31) A model's own materials as a paint layer: the textures and colors of each material slot are drawn into
// the layer in texture space, sampled at the model's UVs, so a textured model from another program (a game asset,
// a scan, a generated model) arrives looking as it did there and can be worked on from that.
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <map>

#include "core/Image.h"
#include "core/Log.h"
#include "core/MeshIO.h"
#include "core/ModelMaterials.h"
#include "core/Paths.h"
#include "engine/Document.h"
#include "gfx/GpuMesh.h"
#include "gfx/Shader.h"

namespace patina {

namespace fs = std::filesystem;

namespace {

enum Mode { ModeColor = 0, ModeValue = 1, ModeNormal = 2, ModeEmissive = 3, ModeOpacity = 4, ModeHeight = 5,
            ModeVertexColor = 6 };

struct Job {
  int slot = 0;
  int channel = 0;      // Channel
  int mode = ModeColor;
  const ModelMap* map = nullptr;  // null: the value alone
  vec4 value{1.f};      // used without the map, or times the map (glTF)
  bool valueWithMap = true;
  int component = 0;
  bool invert = false;
  bool flipGreen = false;
  float normalScale = 1.f;
  int alphaMode = 0;
  float cutoff = 0.5f;
  bool bump = false;    // OBJ / FBX bump: a normal map or a height map, decided when it is read
  bool ownOpacityMap = false;  // OBJ / FBX: an opacity map of its own (its alpha, or gray without one)
};

struct Decoded {
  Texture2D tex;
  int w = 0, h = 0, fileChannels = 4;
  bool ok = false;
  bool looksNormal = false;  // bluish and centered: a tangent-space normal map
  bool black = false;        // every sample black (a glow map with no glow)
};

bool decodeMap(const ModelMap& m, Decoded& d) {
  std::vector<uint8_t> file;
  const uint8_t* bytes = m.data.data();
  size_t len = m.data.size();
  if (!m.path.empty()) {
    bool ok = false;
    std::string s = paths::readFile(m.path, &ok);
    if (!ok) return false;
    file.assign(s.begin(), s.end());
    bytes = file.data();
    len = file.size();
  }
  if (!len) return false;
  int ch = 4;
  if (!imageInfo(bytes, len, d.w, d.h, ch)) return false;
  d.fileChannels = ch;
  std::vector<uint8_t> px;
  int bits = 8;
  if (!decodeSamples(bytes, len, 4, true, px, d.w, d.h, bits)) return false;
  // the average color of a few thousand samples tells a normal map from a height map
  size_t n = size_t(d.w) * d.h, step = std::max<size_t>(1, n / 4096);
  double r = 0, g = 0, b = 0, cnt = 0;
  float hi = 0.f;
  for (size_t i = 0; i < n; i += step) {
    float v[3];
    for (int k = 0; k < 3; ++k)
      v[k] = bits == 16 ? ((const uint16_t*)px.data())[i * 4 + k] / 65535.f : px[i * 4 + k] / 255.f;
    r += v[0], g += v[1], b += v[2], cnt += 1;
    hi = std::max({hi, v[0], v[1], v[2]});
  }
  d.black = hi < 1.5f / 255.f;
  r /= cnt, g /= cnt, b /= cnt;
  d.looksNormal = b > 0.7 && std::abs(r - 0.5) < 0.15 && std::abs(g - 0.5) < 0.15;
  int levels = 1 + (int)std::floor(std::log2((double)std::max(d.w, d.h)));
  d.tex.create(d.w, d.h, bits == 16 ? GL_RGBA16 : GL_RGBA8, levels);
  d.tex.uploadRaw(GL_RGBA, bits == 16 ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, px.data());
  d.tex.setWrap(GL_REPEAT, GL_REPEAT);
  d.tex.setFilter(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
  d.ok = true;
  return true;
}

}  // namespace

bool Document::importModelTextures(const std::string& path, bool automatic, json* report, std::string* err,
                                   bool growResolution) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    return false;
  };
  if (err) err->clear();
  if (!hasMesh()) return fail("No model is open");
  ModelMaterials mm;
  if (!loadModelMaterials(path, mm, err)) return false;
  bool fromBlender = fs::path(path).extension() == ".ptmesh";
  if (fromBlender && !blenderMaterials) mm = ModelMaterials();
  // the file's mesh, when it is the same as the open one (asked for again: the UVs may have been edited since)
  Mesh fm;
  bool same = !automatic && patina::loadMesh(path, fm, nullptr) && fm.faceStart == mesh.faceStart;
  // vertex colors: the file's, else the open model's while they still fit it
  std::vector<vec4> vcol;
  if (same && fm.loopColor.size() == fm.loopCount()) vcol = fm.loopColor;
  else if (mesh.loopColor.size() == mesh.loopCount()) vcol = mesh.loopColor;
  bool hasVC = !vcol.empty();
  std::vector<std::string> notes = mm.notes;
  // (0.44) vertex colors from Blender that are one flat color say nothing (a black or white Color attribute an
  // importer left, that no material shows): left out of a model that opens, where they would hide everything else.
  // (glTF always multiplies COLOR_0 in, and a PLY's colors are its look: those stay.)
  bool shownByMaterial = false;
  for (auto& sl : mm.slots) shownByMaterial |= sl.defined && sl.vertexColors == 1;
  if (hasVC && automatic && fromBlender && !shownByMaterial) {
    bool flat = true;
    for (size_t i = 1; i < vcol.size() && flat; ++i)
      flat = !glm::any(glm::greaterThan(glm::abs(vec3(vcol[i]) - vec3(vcol[0])), vec3(1.5f / 255.f)));
    if (flat) {
      hasVC = false;
      notes.push_back("vertex colors that are one flat color");
      PLOG_I("The model's vertex colors are one flat color: left out");
    }
  }
  if (mm.slots.empty() && !hasVC) return automatic ? false : fail("The model has no materials or vertex colors");
  if (automatic && !mm.interesting() && !hasVC) return false;

  // UVs to sample at: the file's when it is the same mesh (this works after UV edits too), else the current ones
  // (mending and reducing keep each corner's UV)
  std::vector<vec2> uvs = mesh.loopUV;
  bool haveUVs = !uvsGenerated;
  if (same) {
    double area = 0;
    for (uint32_t f = 0; f < fm.faceCount(); ++f) area += std::abs(fm.faceAreaUV(f));
    if (area > 1e-8) uvs = fm.loopUV, haveUVs = true;
  }
  if (!haveUVs && mm.hasAnyMap()) notes.push_back("the model has no UVs, so its textures cannot be placed: colors only");

  // the jobs: per slot, per channel
  std::vector<Job> jobs;
  std::vector<std::string> missing;
  int usedSlots = 0;
  auto usable = [&](const ModelMap& m) -> const ModelMap* {
    if (m.missing && !m.name.empty() &&
        std::find(missing.begin(), missing.end(), m.name) == missing.end())
      missing.push_back(m.name);
    return haveUVs && m.valid() ? &m : nullptr;
  };
  std::vector<uint32_t> slotFaces(mm.slots.size(), 0);
  for (uint32_t f = 0; f < mesh.faceCount(); ++f) {
    uint32_t s = f < mesh.faceMaterial.size() ? mesh.faceMaterial[f] : 0;
    if (s < slotFaces.size()) ++slotFaces[s];
  }
  for (int s = 0; s < (int)mm.slots.size(); ++s) {
    const ModelMaterial& m = mm.slots[s];
    if (!m.defined || !slotFaces[s]) continue;
    ++usedSlots;
    bool wm = !m.mapReplacesValue;
    Job j;
    j.slot = s;
    j.valueWithMap = wm;
    // color
    j.channel = (int)Channel::Color, j.mode = ModeColor, j.map = usable(m.baseColorMap);
    j.value = vec4(vec3(m.baseColor), 1.f);
    jobs.push_back(j);
    // roughness and metallic
    j.mode = ModeValue;
    j.channel = (int)Channel::Roughness, j.map = usable(m.roughnessMap), j.value = vec4(m.roughness);
    j.component = m.roughnessMap.channel, j.invert = m.roughnessMap.invert;
    jobs.push_back(j);
    j.channel = (int)Channel::Metallic, j.map = usable(m.metallicMap), j.value = vec4(m.metallic);
    j.component = m.metallicMap.channel, j.invert = m.metallicMap.invert;
    jobs.push_back(j);
    j.component = 0, j.invert = false;
    // normal (or a bump map: a normal map or heights)
    if (const ModelMap* n = usable(m.normalMap)) {
      j.channel = (int)Channel::Normal, j.mode = ModeNormal, j.map = n;
      j.flipGreen = m.normalDirectX, j.normalScale = m.normalScale;
      jobs.push_back(j);
    } else if (const ModelMap* b = usable(m.bumpMap)) {
      j.channel = (int)Channel::Normal, j.mode = ModeNormal, j.map = b, j.bump = true;
      j.normalScale = m.normalScale;
      jobs.push_back(j);
      j.bump = false;
    }
    j.flipGreen = false, j.normalScale = 1.f;
    // glow: glTF multiplies the map by its factor (no factor, no glow); OBJ and FBX glow where they have a map
    const ModelMap* em = usable(m.emissiveMap);
    bool glowValue = std::max({m.emissive.x, m.emissive.y, m.emissive.z}) > 1e-4f;
    if (glowValue || (em && m.mapReplacesValue)) {
      j.channel = (int)Channel::Emissive, j.mode = ModeEmissive, j.map = em, j.value = vec4(m.emissive, 1.f);
      jobs.push_back(j);
    }
    // opacity: glTF in the base color's alpha, OBJ / FBX in its own map or value
    if (m.alphaMode != 0) {
      j.channel = (int)Channel::Opacity, j.mode = ModeOpacity;
      j.alphaMode = m.alphaMode, j.cutoff = m.alphaCutoff;
      if (m.mapReplacesValue) {
        j.map = usable(m.opacityMap), j.value = vec4(m.opacity);
        j.component = 3;  // alpha unless the map has none (decided when it is read)
        j.ownOpacityMap = true;
      } else {
        j.map = usable(m.baseColorMap), j.value = vec4(m.baseColor.w), j.component = 3;
      }
      jobs.push_back(j);
      j.alphaMode = 0, j.component = 0, j.ownOpacityMap = false;
    }
    if (m.coat > 0.f || m.coatMap.valid()) {
      j.channel = (int)Channel::Coat, j.mode = ModeValue, j.map = usable(m.coatMap), j.value = vec4(m.coat);
      j.valueWithMap = true;
      jobs.push_back(j);
    }
    if (m.sheen > 0.f) {
      j.channel = (int)Channel::Sheen, j.mode = ModeValue, j.map = usable(m.sheenMap), j.value = vec4(m.sheen);
      j.component = 1, j.valueWithMap = true;
      jobs.push_back(j);
    }
    if (m.occlusionMap.valid()) notes.push_back("ambient occlusion maps (bake Patina's own for smart masks)");
  }
  // vertex colors: glTF multiplies the base color by them (its rule); OBJ, FBX and PLY show them where a slot has
  // no color texture (as Blender does). Faces of a slot without a material start from white.
  std::vector<char> vcOn;
  if (hasVC) {
    uint32_t maxSlot = 0;
    for (uint32_t f = 0; f < mesh.faceCount(); ++f)
      maxSlot = std::max(maxSlot, f < mesh.faceMaterial.size() ? mesh.faceMaterial[f] : 0u);
    vcOn.assign(maxSlot + 1, 1);
    std::vector<char> bare(maxSlot + 1, 0);
    for (uint32_t f = 0; f < mesh.faceCount(); ++f) {
      uint32_t s = f < mesh.faceMaterial.size() ? mesh.faceMaterial[f] : 0;
      bool defined = s < mm.slots.size() && mm.slots[s].defined;
      if (!defined) bare[s] = 1;
      else if (mm.slots[s].mapReplacesValue && usable(mm.slots[s].baseColorMap)) vcOn[s] = 0;
    }
    // none of the faces shows them (each slot has a color texture): no vertex colors after all
    bool any = false;
    for (uint32_t f = 0; f < mesh.faceCount() && !any; ++f) any = vcOn[f < mesh.faceMaterial.size() ? mesh.faceMaterial[f] : 0];
    if (!any) hasVC = false;
    for (uint32_t s = 0; s <= maxSlot; ++s)
      if (bare[s]) {
        Job j;
        j.slot = (int)s;
        j.channel = (int)Channel::Color, j.mode = ModeColor, j.value = vec4(1.f);
        jobs.push_back(j);
        ++usedSlots;
      }
  }
  if (!usedSlots) return automatic ? false : fail("None of the model's faces use a material");

  // the texture set grows to the textures' size when they are bigger (up to a 4K atlas)
  int maxMap = 0;
  std::map<std::string, std::pair<int, int>> sizes;
  for (auto& jb : jobs) {
    if (!jb.map || sizes.count(jb.map->key())) continue;
    int w = 0, h = 0, c = 0;
    bool ok = jb.map->path.empty() ? imageInfo(jb.map->data.data(), jb.map->data.size(), w, h, c)
                                   : imageInfo(jb.map->path, w, h, c);
    sizes[jb.map->key()] = {ok ? w : 0, ok ? h : 0};
    maxMap = std::max({maxMap, ok ? w : 0, ok ? h : 0});
  }
  int grid = std::max(1, mesh.tiles.grid);
  int oldRes = tileRes();
  if (automatic && growResolution && maxMap > oldRes) {
    int want = 1, cap = 1;
    while (want < maxMap) want *= 2;
    while (cap * 2 * grid <= 4096) cap *= 2;  // the atlas stays at 4K or less
    want = std::min(want, cap);
    if (want > oldRes) changeResolution(want);
  }

  checkpoint();
  Layer& layer = addPaintLayer("From the model", 0);
  uint32_t uid = layer.uid;
  ivec2 res = tex.size();
  for (auto& jb : jobs) layer.ensureChannel(jb.channel, res);

  // per loop: the UV to sample at and the face's slot
  std::vector<vec4> aux(mesh.loopCount());
  for (uint32_t f = 0; f < mesh.faceCount(); ++f) {
    float s = float(f < mesh.faceMaterial.size() ? mesh.faceMaterial[f] : 0);
    for (uint32_t l = mesh.faceStart[f]; l < mesh.faceStart[f + 1]; ++l) aux[l] = vec4(uvs[l], s, 0.f);
  }
  gpu.setAux(aux);

  glDisable(GL_DEPTH_TEST);
  glDisable(GL_CULL_FACE);
  glDisable(GL_BLEND);
  glDisable(GL_SCISSOR_TEST);
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  Shader& sh = ShaderCache::get("uvraster.vs.glsl", "model_texture.fs.glsl");
  sh.set("uUVTransform", vec4(1, 1, 0, 0));
  sh.set("uUseAuxAsUV", 0);
  sh.set("uUseMeshUV", 0);
  sh.set("uTile", -1);
  sh.set("uSkipFlag", GpuMesh::kMirrorCopyFlag);
  Texture2D white(1, 1, GL_RGBA8, 1);
  white.clear(vec4(1.f));

  int mapsRead = 0, drawn = 0;  // channels written (bits)
  std::vector<std::string> unreadable;
  auto draw = [&](const Job& jb, const Decoded* d) {
    Texture2D& target = *layer.channels[jb.channel];
    Framebuffer fbo;
    fbo.attach(0, target);
    fbo.setDrawBuffers(1);
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, target.width(), target.height());
    bool hasMap = d && d->ok;
    int mode = jb.mode;
    if (jb.bump && hasMap && !d->looksNormal) mode = ModeHeight;
    vec4 f = jb.value;
    if (hasMap && !jb.valueWithMap) f = vec4(1.f);
    // a black glow map: no glow (Patina's own exports and many others always write one)
    if (jb.mode == ModeEmissive && hasMap && d->black) return;
    // an opacity map read from alpha only, without alpha: nothing to say (left out)
    if (jb.mode == ModeOpacity && hasMap && jb.ownOpacityMap && jb.map->alphaOnly &&
        (d->fileChannels == 1 || d->fileChannels == 3))
      return;
    int component = jb.component;
    if (mode == ModeOpacity && hasMap && jb.ownOpacityMap && (d->fileChannels == 1 || d->fileChannels == 3))
      component = 0;
    sh.set("uSlot", jb.slot);
    sh.set("uMode", mode);
    sh.set("uHasMap", hasMap ? 1 : 0);
    sh.set("uUVXf", hasMap ? jb.map->uvTransform : mat3(1.f));
    sh.set("uFactor", f);
    sh.set("uChannel", component);
    sh.set("uInvert", jb.invert ? 1 : 0);
    sh.set("uFlipGreen", jb.flipGreen ? 1 : 0);
    sh.set("uNormalScale", jb.normalScale);
    sh.set("uFromAlpha", component == 3 ? 1 : 0);
    sh.set("uAlphaMode", jb.alphaMode);
    sh.set("uCutoff", jb.cutoff);
    sh.tex("uMap", 0, hasMap ? d->tex.id() : white.id());
    sh.use();
    gpu.drawTriangles();
    drawn |= 1 << jb.channel;
  };
  // a bump map read as heights goes to the height channel
  auto retarget = [&](Job& jb, const Decoded& d) {
    if (jb.bump && d.ok && !d.looksNormal) {
      jb.channel = (int)Channel::Height;
      layer.ensureChannel(jb.channel, res);
    }
  };
  // value-only jobs first, then each image once for every job that uses it
  for (auto& jb : jobs)
    if (!jb.map) draw(jb, nullptr);
  std::map<std::string, std::vector<Job*>> byImage;
  for (auto& jb : jobs)
    if (jb.map) byImage[jb.map->key()].push_back(&jb);
  for (auto& [key, list] : byImage) {
    Decoded d;
    if (decodeMap(*list.front()->map, d)) ++mapsRead;
    else unreadable.push_back(list.front()->map->name);
    for (Job* jb : list) {
      retarget(*jb, d);
      draw(*jb, &d);
    }
  }
  // vertex colors over the colors drawn: multiplied in (sRGB values multiply as linear ones nearly do)
  if (hasVC && layer.hasChannel((int)Channel::Color)) {
    std::vector<vec4> caux(mesh.loopCount());
    for (uint32_t f = 0; f < mesh.faceCount(); ++f) {
      uint32_t s = f < mesh.faceMaterial.size() ? mesh.faceMaterial[f] : 0;
      float on = s < vcOn.size() && vcOn[s] ? 1.f : 0.f;
      for (uint32_t l = mesh.faceStart[f]; l < mesh.faceStart[f + 1]; ++l) caux[l] = vec4(vec3(vcol[l]), on);
    }
    gpu.setAux(caux);
    Texture2D& target = *layer.channels[(int)Channel::Color];
    Framebuffer fbo;
    fbo.attach(0, target);
    fbo.setDrawBuffers(1);
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, target.width(), target.height());
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_DST_COLOR, GL_ZERO, GL_ZERO, GL_ONE);
    sh.set("uSlot", -1);
    sh.set("uMode", ModeVertexColor);
    sh.set("uHasMap", 0);
    sh.tex("uMap", 0, white.id());
    sh.use();
    gpu.drawTriangles();
    glDisable(GL_BLEND);
    drawn |= 1 << (int)Channel::Color;
  }
  // channels nothing was written to are left out (the normal channel of a bump map read as heights...)
  for (int c = 0; c < kChannelCount; ++c)
    if (layer.hasChannel(c) && !(drawn & (1 << c))) layer.channels[c].reset();

  if (findLayer(uid)) selected = indexOf(uid);
  editMask = false;
  markComposite();
  checkpoint("Textures from the model");

  std::vector<std::string> chans;
  for (int c = 0; c < kChannelCount; ++c)
    if (layer.hasChannel(c)) chans.push_back(channelInfo((Channel)c).key);
  PLOG_I("Brought in the model's materials: %d slot(s), %d texture(s)%s%s", usedSlots, mapsRead,
         hasVC ? ", vertex colors" : "", missing.empty() ? "" : " (some textures not found)");
  if (report) {
    std::sort(notes.begin(), notes.end());
    notes.erase(std::unique(notes.begin(), notes.end()), notes.end());
    *report = {{"layer", indexOf(uid)},
               {"name", layer.name},
               {"slots", usedSlots},
               {"textures", mapsRead},
               {"channels", chans},
               {"missing", missing},
               {"unreadable", unreadable},
               {"notes", notes},
               {"resolution", tileRes()},
               {"resolution_before", oldRes},
               {"uvs", haveUVs},
               {"vertex_colors", hasVC}, {"vertex_colours", hasVC}};  // vertex_colours: the 0.32 to 0.35 name
  }
  return true;
}

}  // namespace patina
