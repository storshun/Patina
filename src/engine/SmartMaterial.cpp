#include "engine/SmartMaterial.h"

#include <stb_image_resize2.h>

#include <filesystem>
#include <map>

#include "core/Image.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "engine/Document.h"

namespace patina {

namespace fs = std::filesystem;

static fs::path userSmartDir() { return paths::userDataDir() / "smart_materials"; }
static fs::path builtinSmartDir() { return paths::dataDir() / "smart_materials"; }

static bool readJsonFile(const fs::path& p, json& out) {
  bool ok = false;
  std::string text = paths::readFile(p, &ok);
  if (!ok) return false;
  try {
    out = json::parse(text);
  } catch (std::exception&) {
    return false;
  }
  return out.is_object() && out.contains("layers");
}

std::vector<SmartMaterialInfo> listSmartMaterials() {
  std::vector<SmartMaterialInfo> out;
  std::map<std::string, size_t> byId;
  for (int pass = 0; pass < 2; ++pass) {
    fs::path dir = pass == 0 ? builtinSmartDir() : userSmartDir();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) continue;
    std::vector<fs::path> files;
    for (auto& e : fs::directory_iterator(dir, ec))
      if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
    std::sort(files.begin(), files.end());
    for (auto& f : files) {
      json j;
      if (!readJsonFile(f, j)) {
        PLOG_W("Not a smart material: %s", f.string().c_str());
        continue;
      }
      SmartMaterialInfo i;
      i.id = f.stem().string();
      i.name = j.value("name", i.id);
      i.description = j.value("description", std::string());
      i.path = f.string();
      i.builtin = pass == 0;
      i.layerCount = (int)j["layers"].size();
      for (auto& l : j["layers"]) {
        if (l.contains("textures") && !l["textures"].empty()) i.hasPaint = true;
        if (l.value("type", std::string()) == "fill" && l.contains("material") && l["material"].is_object())
          i.tileMaterial = l["material"].value("id", i.tileMaterial);
      }
      if (auto it = byId.find(i.id); it != byId.end()) out[it->second] = i;  // user file overrides
      else byId[i.id] = out.size(), out.push_back(i);
    }
  }
  return out;
}

static const int kChannelPngChannels[] = {4, 2, 2, 2, 4, 4, 2, 2, 2, 2};
static_assert(sizeof(kChannelPngChannels) / sizeof(kChannelPngChannels[0]) == kChannelCount);

static std::string texToBase64(const Texture2D& t, int channels, bool sixteen) {
  Image img = t.download(channels);
  auto png = sixteen ? encodePng16(img) : encodePng(img);
  return base64Encode(png.data(), png.size());
}

static bool base64ToTex(const std::string& b64, Texture2D& t) {
  auto bytes = base64Decode(b64);
  Image img;
  if (!decodeImage(bytes.data(), bytes.size(), img, Texture2D::channelsOf(t.format()))) return false;
  if (img.width != t.width() || img.height != t.height()) {
    Image r;
    r.resize(t.width(), t.height(), img.channels);
    stbir_resize_float_linear(img.pixels.data(), img.width, img.height, 0, r.pixels.data(), r.width, r.height, 0,
                              (stbir_pixel_layout)img.channels);
    img = std::move(r);
  }
  t.upload(img);
  return true;
}

bool saveSmartMaterial(Document& d, int index, const std::string& name, const std::string& description,
                       bool includePaint, std::string* outPath, std::string* err) {
  if (index < 0 || index >= (int)d.layers.size()) {
    if (err) *err = "Select the layer or folder to save";
    return false;
  }
  if (d.layers[index]->isBaked()) {
    if (err) *err = "A baked map's layer belongs to this model: it can't be a smart material";
    return false;
  }
  std::string nm = name.empty() ? d.layers[index]->name : name;
  std::string id = paths::sanitizeFileName(nm);
  if (id.empty()) id = "smart_material";
  json layers = json::array();
  uint32_t rootUid = d.layers[index]->uid;
  for (int i : d.subtree(index)) {  // bottom to top, the root (folder) last
    const Layer& l = *d.layers[i];
    json j = l.toJson();
    j.erase("has_mask");
    j.erase("expanded");
    if (l.uid == rootUid) j["parent"] = 0;
    if (includePaint) {
      json t = json::object();
      for (int c = 0; c < kChannelCount; ++c)
        if (l.hasChannel(c))
          t[channelInfo((Channel)c).key] = texToBase64(*l.channels[c], kChannelPngChannels[c], c == (int)Channel::Height);
      if (l.mask) t["mask"] = texToBase64(*l.mask, 1, false);
      j["textures"] = t;
    }
    layers.push_back(j);
  }
  json file = {{"patina_smart_material", 1}, {"name", nm}, {"description", description}, {"layers", layers}};
  fs::path dir = paths::ensureDir(userSmartDir());
  fs::path p = dir / (id + ".json");
  if (!paths::writeFileAtomic(p, file.dump(1))) {
    if (err) *err = "Could not write " + p.string();
    return false;
  }
  if (outPath) *outPath = p.string();
  PLOG_I("Saved smart material '%s' (%zu layers) to %s", nm.c_str(), layers.size(), p.string().c_str());
  return true;
}

int applySmartMaterial(Document& d, const std::string& idOrPath, std::string* err) {
  return applySmartMaterialAt(d, idOrPath, -2, false, false, err);
}

// (target -2: above the selection, inside a selected folder)
int applySmartMaterialAt(Document& d, const std::string& idOrPath, int target, bool into, bool below, std::string* err) {
  fs::path p = idOrPath;
  std::error_code ec;
  if (!fs::is_regular_file(p, ec)) {
    // (0.36) ids that had British spellings
    std::string id = idOrPath == "patina_smart_oxidised_copper" ? "patina_smart_oxidized_copper"  // the old id
                   : idOrPath == "patina_smart_stylised_edges"  ? "patina_smart_stylized_edges"   // the old id
                                                                 : idOrPath;
    p.clear();
    for (auto& i : listSmartMaterials())
      if (i.id == id || i.name == id) p = i.path;
  }
  json j;
  if (p.empty() || !readJsonFile(p, j)) {
    if (err) *err = "Smart material '" + idOrPath + "' not found";
    return -1;
  }
  if (!d.hasMesh()) {
    if (err) *err = "No mesh loaded";
    return -1;
  }
  auto& arr = j["layers"];
  if (arr.empty()) {
    if (err) *err = "The smart material has no layers";
    return -1;
  }
  // where it goes: like a new layer (above the selection, inside a selected folder), or where it was dropped
  d.checkpoint();
  uint32_t parent = 0;
  int at = (int)d.layers.size();
  if (target != -2) {
    at = d.dropIndex(target, into, below, &parent);
  } else if (Layer* sel = d.selectedLayer()) {
    if (sel->isFolder()) {
      parent = sel->uid;
      at = d.selected;
    } else {
      parent = d.effectiveParent(*sel);
      at = d.selected + 1;
    }
  }
  std::map<uint32_t, uint32_t> remap;
  for (auto& lj : arr) remap[lj.value("uid", 0u)] = d.nextUid++;
  bool needCurv = false, needAO = false, needThick = false;
  int inserted = 0;
  for (auto& lj : arr) {
    auto l = std::make_unique<Layer>();
    l->fromJson(lj);
    l->uid = remap[lj.value("uid", 0u)];
    uint32_t op = lj.value("parent", 0u);
    l->parent = op && remap.count(op) ? remap[op] : parent;
    if (lj.contains("textures"))
      for (auto& [key, val] : lj["textures"].items()) {
        if (key == "mask") {
          if (!base64ToTex(val.get<std::string>(), l->ensureMask(d.tex.size())))
            PLOG_W("Smart material: could not decode the mask of %s", l->name.c_str());
          continue;
        }
        int c = channelFromKey(key);
        if (c < 0) continue;
        if (!base64ToTex(val.get<std::string>(), l->ensureChannel(c, d.tex.size())))
          PLOG_W("Smart material: could not decode %s of %s", key.c_str(), l->name.c_str());
      }
    needCurv |= l->needsCurvature();
    needAO |= l->needsAO();
    needThick |= l->needsThickness();
    d.layers.insert(d.layers.begin() + at + inserted, std::move(l));
    ++inserted;
  }
  if (needCurv && !d.tex.hasCurvature()) d.tex.bakeCurvature(d.mesh, d.gpu, &d.bvh);
  if (needAO && !d.tex.hasAO()) d.tex.bakeAO(d.mesh, d.gpu, d.aoSamples);
  if (needThick && !d.tex.hasThickness()) d.tex.bakeThickness(d.mesh, d.bvh, d.thicknessRays);
  d.selected = at + inserted - 1;
  d.editMask = false;
  d.markComposite();
  d.checkpoint("Smart material: " + j.value("name", p.stem().string()));
  PLOG_I("Applied smart material %s", p.string().c_str());
  return d.selected;
}

json copyLayerSettings(const Layer& l) {
  json j = l.toJson();
  for (const char* k : {"uid", "name", "parent", "color_tag", "expanded", "has_mask"}) j.erase(k);
  return j;
}

void pasteLayerSettings(Layer& l, const json& s) {
  json j = s;
  std::string srcType = j.value("type", std::string());
  j.erase("type");
  const char* myType = l.type == LayerType::Fill ? "fill" : l.type == LayerType::Adjust ? "adjust"
                     : l.type == LayerType::Folder ? "folder" : l.type == LayerType::Baked ? "baked" : "paint";
  if (srcType != myType) {  // type-specific parameters only between layers of the same type
    j.erase("material");
    j.erase("adjust");
    j.erase("strength");
  }
  j.erase("baked_map");  // (a baked map layer stays the layer of its map)
  l.fromJson(j);
}

}  // namespace patina
