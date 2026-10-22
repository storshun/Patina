// Importing folders of PBR texture maps as image materials.
//
// Handles the common naming schemes (ambientCG, Poly Haven, Quixel, Substance export
// presets), packed maps, normal map conventions and multi texture-set exports.
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#include "core/Image.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "engine/MaterialLibrary.h"

namespace patina {
namespace fs = std::filesystem;

namespace {

std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char)std::tolower(c); });
  return s;
}

// What a file holds. Packed kinds are split into single channels on import.
enum class Kind { None, BaseColor, Roughness, Glossiness, Metallic, Normal, Height, Emissive, AO, Opacity,
                  ORM,        // R occlusion, G roughness, B metallic (Unreal ORM, Poly Haven ARM, glTF ORM)
                  MetalRough, // glTF metallicRoughness: G roughness, B metallic
                  MaskMap,    // Unity HDRP: R metallic, G occlusion, B detail mask, A smoothness
                  MetalSmooth // Unity URP / Standard: R metallic, A smoothness
};

struct Classified {
  Kind kind = Kind::None;
  size_t pos = std::string::npos;  // where the map token starts in the (lowercase) stem
  int normal = 0;                  // 1 OpenGL, 2 DirectX (from the name)
};

// Name tokens, most specific first (packed names contain "rough" / "metal" too).
Classified classify(const std::string& stemIn) {
  std::string n = lower(stemIn);
  Classified c;
  auto find = [&](std::initializer_list<const char*> tokens) {
    for (const char* t : tokens) {
      size_t p = n.find(t);
      if (p != std::string::npos) return p;
    }
    return std::string::npos;
  };
  auto set = [&](Kind k, size_t p) {
    c.kind = k;
    c.pos = p;
    return c;
  };
  size_t p;
  // short tokens must stand alone: "_arm_2k" is a packed map, "_armour" is not
  auto token = [&](const char* t) {
    size_t len = std::strlen(t);
    for (size_t q = n.find(t); q != std::string::npos; q = n.find(t, q + 1)) {
      bool before = q == 0 || n[q - 1] == '_' || n[q - 1] == '-' || n[q - 1] == ' ' || n[q - 1] == '.';
      bool after = q + len >= n.size() || !std::isalpha((unsigned char)n[q + len]);
      if (before && after) return q == 0 ? q : q - 1;
    }
    return std::string::npos;
  };
  if ((p = find({"occlusionroughnessmetallic", "occlusion_roughness_metallic", "occlusionroughnessmetalness"})) !=
          std::string::npos ||
      (p = token("orm")) != std::string::npos || (p = token("arm")) != std::string::npos)
    return set(Kind::ORM, p);
  if ((p = find({"maskmap", "mask_map"})) != std::string::npos) return set(Kind::MaskMap, p);
  if ((p = find({"metallicsmoothness", "metallic_smoothness", "metalsmoothness"})) != std::string::npos)
    return set(Kind::MetalSmooth, p);
  if ((p = find({"metallicroughness", "metallic_roughness", "metalroughness", "metalnessroughness"})) != std::string::npos)
    return set(Kind::MetalRough, p);
  if ((p = find({"normal", "_nrm", "-nrm", "_nor", "-nor", "_norm"})) != std::string::npos ||
      (n.size() >= 2 && n.compare(n.size() - 2, 2, "_n") == 0 && (p = n.size() - 2))) {
    bool dx = n.find("directx") != std::string::npos || n.find("_dx") != std::string::npos ||
              n.find("normaldx") != std::string::npos || n.find("-dx") != std::string::npos;
    bool gl = n.find("opengl") != std::string::npos || n.find("_gl") != std::string::npos ||
              n.find("normalgl") != std::string::npos || n.find("-gl") != std::string::npos;
    c.normal = dx ? 2 : gl ? 1 : 0;
    return set(Kind::Normal, p);
  }
  if ((p = find({"roughness", "rough", "_rgh"})) != std::string::npos) return set(Kind::Roughness, p);
  if ((p = find({"glossiness", "gloss", "smoothness"})) != std::string::npos) return set(Kind::Glossiness, p);
  if ((p = find({"metallic", "metalness", "metal", "_mtl"})) != std::string::npos) return set(Kind::Metallic, p);
  if ((p = find({"height", "displacement", "_disp", "-disp", "bump"})) != std::string::npos) return set(Kind::Height, p);
  if ((p = find({"emissive", "emission", "emit", "glow"})) != std::string::npos) return set(Kind::Emissive, p);
  if ((p = find({"mixed_ao", "ambientocclusion", "ambient_occlusion", "occlusion", "_ao", "-ao"})) != std::string::npos)
    return set(Kind::AO, p);
  if ((p = find({"opacity", "transparency", "_alpha"})) != std::string::npos &&
      n.find("albedotransparency") == std::string::npos)
    return set(Kind::Opacity, p);
  if ((p = find({"basecolor", "base_color", "basemap", "base_map", "albedo", "diffuse", "_diff", "-diff", "_col", "-col", "color", "colour"})) !=
      std::string::npos)
    return set(Kind::BaseColor, p);
  return c;
}

std::string groupKey(const std::string& stem, size_t pos) {
  std::string n = lower(stem).substr(0, std::min(pos, stem.size()));
  while (!n.empty() && (n.back() == '_' || n.back() == '-' || n.back() == '.' || n.back() == ' ')) n.pop_back();
  return n;
}

std::string prettyName(const std::string& key) {
  std::string s = key;
  for (auto& ch : s)
    if (ch == '_' || ch == '-') ch = ' ';
  bool up = true;
  for (auto& ch : s) {
    if (up && std::isalpha((unsigned char)ch)) ch = (char)std::toupper((unsigned char)ch);
    up = ch == ' ';
  }
  return s;
}

struct Source {
  fs::path path;
  Classified c;
};

}  // namespace

std::string MaterialLibrary::unsupportedMaterialReason(const std::string& path) {
  std::string ext = lower(fs::path(path).extension().string());
  if (ext == ".sbsar")
    return "An .sbsar is a compiled Substance Designer material, and only Adobe's Substance engine can render it. "
           "Open it in Substance Designer, Sampler or Player and export its outputs as images "
           "(base color, roughness, metallic, normal, height...), then import that folder into Patina.";
  if (ext == ".sbs")
    return "An .sbs is a Substance Designer graph. Patina can't evaluate Substance graphs: export the graph's "
           "outputs as images from Substance Designer and import that folder.";
  if (ext == ".spsm" || ext == ".spmsk" || ext == ".spp" || ext == ".sppr")
    return "This is a Substance Painter file (smart material, smart mask or project) in Adobe's own format. "
           "Export the textures from Painter (File > Export Textures) and import the folder into Patina, or "
           "rebuild the stack with Patina's smart materials.";
  return "";
}

std::vector<MaterialLibrary::Entry*> MaterialLibrary::importImageFolderAll(const std::string& dir,
                                                                           const ImageImportOptions& opt,
                                                                           std::vector<ImportReport>* report,
                                                                           std::string* err) {
  std::vector<Entry*> out;
  std::error_code ec;
  if (!fs::is_directory(dir, ec)) {
    if (err) *err = "Not a folder: " + dir;
    return out;
  }
  static const std::set<std::string> exts = {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".hdr", ".exr"};
  std::vector<Source> files;
  std::string unsupported;
  for (auto& e : fs::directory_iterator(dir, ec)) {
    if (!e.is_regular_file()) continue;
    std::string ext = lower(e.path().extension().string());
    if (!exts.count(ext)) {
      if (unsupported.empty()) unsupported = unsupportedMaterialReason(e.path().string());
      continue;
    }
    Classified c = classify(e.path().stem().string());
    if (c.kind != Kind::None) files.push_back({e.path(), c});
  }
  std::sort(files.begin(), files.end(), [](const Source& a, const Source& b) { return a.path < b.path; });
  if (files.empty()) {
    if (err)
      *err = !unsupported.empty() ? unsupported
                                  : "No recognizable texture maps (base color, roughness, normal...) in " + dir;
    return out;
  }
  // texture sets: files that share the name before the map token. Several sets only
  // count when at least two of them have a base color (a Substance Painter export of
  // several texture sets); otherwise the whole folder is one material.
  std::map<std::string, std::vector<Source>> groups;
  for (auto& f : files) groups[groupKey(f.path.stem().string(), f.c.pos)].push_back(f);
  int withColor = 0;
  for (auto& [k, g] : groups)
    if (std::any_of(g.begin(), g.end(), [](const Source& s) { return s.c.kind == Kind::BaseColor; })) ++withColor;
  if (withColor < 2) {
    std::vector<Source> all = files;
    groups.clear();
    groups[""] = all;
  }
  std::string folderName = fs::path(dir).filename().string();
  if (folderName.empty()) folderName = fs::path(dir).parent_path().filename().string();
  for (auto& [key, g] : groups) {
    if (withColor >= 2 &&
        !std::any_of(g.begin(), g.end(), [](const Source& s) { return s.c.kind == Kind::BaseColor; }))
      continue;  // stray maps without a texture set
    MaterialDef d;
    d.type = MaterialType::Image;
    std::string base = opt.name.empty() ? folderName : opt.name;
    d.name = groups.size() > 1 ? base + " - " + prettyName(key) : base;
    d.id = "img_" + paths::sanitizeFileName(lower(d.name));
    for (auto& ch : d.id)
      if (ch == ' ') ch = '_';
    d.category = "Imported";
    ImportReport rep;
    rep.name = d.name;
    rep.id = d.id;
    fs::path dst = paths::ensureDir(paths::userDataDir() / "materials" / d.id);
    // single maps first, packed maps fill what is still missing
    auto pick = [&](Kind k) -> const Source* {
      const Source* best = nullptr;
      for (auto& s : g)
        if (s.c.kind == k) {
          // two normal maps: prefer OpenGL
          if (!best || (k == Kind::Normal && best->c.normal == 2 && s.c.normal != 2)) best = &s;
        }
      return best;
    };
    auto copyIn = [&](const fs::path& p) {
      fs::path to = dst / p.filename();
      fs::copy_file(p, to, fs::copy_options::overwrite_existing, ec);
      return (ec ? fs::absolute(p) : to).string();
    };
    struct Single {
      Kind kind;
      const char* key;
    };
    for (Single s : {Single{Kind::BaseColor, "basecolor"}, {Kind::Roughness, "roughness"}, {Kind::Glossiness, "glossiness"},
                     {Kind::Metallic, "metallic"}, {Kind::Normal, "normal"}, {Kind::Height, "height"},
                     {Kind::Emissive, "emissive"}, {Kind::AO, "ao"}, {Kind::Opacity, "opacity"}}) {
      if (const Source* src = pick(s.kind)) {
        d.maps[s.key] = copyIn(src->path);
        rep.files[s.key] = src->path.filename().string();
      }
    }
    // packed maps -> single-channel PNGs next to the copies
    auto extract = [&](const Source& src, int channel, bool invert, const char* key, const char* chName) {
      if (d.maps.count(key)) return;
      Image img;
      if (!loadImage(src.path.string(), img, 4)) {
        rep.notes.push_back("could not read " + src.path.filename().string());
        return;
      }
      Image one;
      one.resize(img.width, img.height, 1);
      for (size_t i = 0; i < size_t(img.width) * img.height; ++i) {
        float v = img.pixels[i * 4 + channel];
        one.pixels[i] = invert ? 1.f - v : v;
      }
      fs::path to = dst / (src.path.stem().string() + "_" + key + ".png");
      if (!saveImage(to.string(), one, 16)) {
        rep.notes.push_back("could not write " + to.string());
        return;
      }
      d.maps[key] = to.string();
      rep.files[key] = src.path.filename().string() + " (" + chName + ")";
    };
    if (const Source* s = pick(Kind::ORM)) {
      extract(*s, 1, false, "roughness", "G");
      extract(*s, 2, false, "metallic", "B");
      extract(*s, 0, false, "ao", "R");
    }
    if (const Source* s = pick(Kind::MetalRough)) {
      extract(*s, 1, false, "roughness", "G");
      extract(*s, 2, false, "metallic", "B");
    }
    if (const Source* s = pick(Kind::MaskMap)) {
      extract(*s, 0, false, "metallic", "R");
      extract(*s, 3, true, "roughness", "1 - A");
      extract(*s, 1, false, "ao", "G");
    }
    if (const Source* s = pick(Kind::MetalSmooth)) {
      extract(*s, 0, false, "metallic", "R");
      extract(*s, 3, true, "roughness", "1 - A");
    }
    if (d.maps.count("roughness")) d.maps.erase("glossiness");
    // normal convention: from the name, else the export preset (Substance's Unreal
    // preset writes DirectX normals next to an OcclusionRoughnessMetallic map)
    bool dx = false;
    if (const Source* n = pick(Kind::Normal)) {
      if (opt.normal == NormalConvention::DirectX) dx = true;
      else if (opt.normal == NormalConvention::OpenGL) dx = false;
      else if (n->c.normal) dx = n->c.normal == 2;
      else {
        const Source* orm = pick(Kind::ORM);
        dx = orm && lower(orm->path.stem().string()).find("occlusionroughnessmetallic") != std::string::npos;
        if (dx) rep.notes.push_back("normal map read as DirectX (Unreal export preset)");
      }
    }
    d.normalDirectX = dx;
    rep.normalDirectX = dx;
    if (!d.maps.count("basecolor")) rep.notes.push_back("no base color map: gray is used");
    Entry* e = saveUserMaterial(d, err);
    if (!e) continue;
    out.push_back(e);
    if (report) report->push_back(rep);
    PLOG_I("Imported material '%s' (%zu maps%s)", d.name.c_str(), d.maps.size(), dx ? ", DirectX normal" : "");
  }
  if (out.empty() && err && err->empty()) *err = "No material could be imported from " + dir;
  return out;
}

MaterialLibrary::Entry* MaterialLibrary::importImageFolder(const std::string& dir, const std::string& name,
                                                           std::string* err) {
  ImageImportOptions o;
  o.name = name;
  auto v = importImageFolderAll(dir, o, nullptr, err);
  return v.empty() ? nullptr : v.front();
}

}  // namespace patina
