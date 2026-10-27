#include "engine/BrushLibrary.h"

#include <algorithm>
#include <filesystem>

#include "core/Image.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "engine/BrushImport.h"
#include "gfx/Shader.h"

namespace patina {

namespace fs = std::filesystem;

static const std::vector<std::string> kShapes = {"round",  "square",  "star",   "splatter", "noise", "scratches",
                                                 "grunge", "dots",    "hatch",  "cracks",   "bristle", "spray",
                                                 "leaf",   "ring",    "cloud",  "rust"};

std::vector<std::string> BrushLibrary::shapes() const { return kShapes; }

json BrushPreset::toJson() const {
  return {{"id", id},
          {"name", name},
          {"category", category},
          {"alpha", {{"shape", alpha.shape}, {"image", alpha.image}, {"seed", alpha.seed}, {"param", alpha.param},
                     {"image_mode", alpha.imageMode}}},
          {"size", size},
          {"hardness", hardness},
          {"opacity", opacity},
          {"flow", flow},
          {"spacing", spacing},
          {"angle", angleDeg},
          {"angle_jitter", angleJitter},
          {"follow_path", followPath},
          {"tilt_angle", tiltAngle},
          {"size_jitter", sizeJitter},
          {"flow_jitter", flowJitter},
          {"scatter", scatter},
          {"roundness", roundness},
          {"pressure_size", pressureSize},
          {"pressure_flow", pressureFlow},
          {"pressure_opacity", pressureOpacity},
          {"projection", projection == BrushProjection::Screen ? "screen" : projection == BrushProjection::Surface ? "surface" : "uv"},
          {"backface_culling", backfaceCulling},
          {"occlusion", occlusion},
          {"angle_falloff", angleFalloff},
          {"lazy_radius", lazyRadius}};
}

void BrushPreset::fromJson(const json& j) {
  id = j.value("id", id);
  name = j.value("name", name.empty() ? id : name);
  category = j.value("category", category);
  if (j.contains("alpha")) {
    auto& a = j["alpha"];
    if (a.is_string()) alpha.shape = a.get<std::string>();
    else {
      alpha.shape = a.value("shape", alpha.shape);
      alpha.image = a.value("image", alpha.image);
      alpha.seed = a.value("seed", alpha.seed);
      alpha.param = a.value("param", alpha.param);
      alpha.imageMode = a.value("image_mode", alpha.imageMode);
    }
  }
  size = j.value("size", size);
  hardness = j.value("hardness", hardness);
  opacity = j.value("opacity", opacity);
  flow = j.value("flow", flow);
  spacing = j.value("spacing", spacing);
  angleDeg = j.value("angle", angleDeg);
  angleJitter = j.value("angle_jitter", angleJitter);
  followPath = j.value("follow_path", followPath);
  tiltAngle = j.value("tilt_angle", tiltAngle);
  sizeJitter = j.value("size_jitter", sizeJitter);
  flowJitter = j.value("flow_jitter", flowJitter);
  scatter = j.value("scatter", scatter);
  roundness = j.value("roundness", roundness);
  pressureSize = j.value("pressure_size", pressureSize);
  pressureFlow = j.value("pressure_flow", pressureFlow);
  pressureOpacity = j.value("pressure_opacity", pressureOpacity);
  std::string p = j.value("projection", std::string());
  if (p == "screen") projection = BrushProjection::Screen;
  else if (p == "surface") projection = BrushProjection::Surface;
  else if (p == "uv") projection = BrushProjection::UV;
  backfaceCulling = j.value("backface_culling", backfaceCulling);
  occlusion = j.value("occlusion", occlusion);
  angleFalloff = j.value("angle_falloff", angleFalloff);
  lazyRadius = j.value("lazy_radius", lazyRadius);
}

// Built-in presets: a varied starter kit. Users add more as JSON files.
static std::vector<BrushPreset> builtinPresets() {
  std::vector<BrushPreset> v;
  auto add = [&](const char* id, const char* name, const char* cat, json j) {
    BrushPreset p;
    p.id = id;
    p.name = name;
    p.category = cat;
    p.fromJson(j);
    v.push_back(p);
  };
  add("basic_hard", "Basic Hard", "Basic", {{"hardness", 0.92}, {"spacing", 0.08}});
  add("basic_soft", "Basic Soft", "Basic", {{"hardness", 0.0}, {"spacing", 0.08}, {"flow", 0.6}});
  add("airbrush", "Airbrush", "Basic", {{"hardness", 0.0}, {"flow", 0.08}, {"spacing", 0.05}, {"size", 90}, {"pressure_flow", true}, {"pressure_size", false}});
  add("square_hard", "Square", "Basic", {{"alpha", "square"}, {"hardness", 0.95}, {"spacing", 0.06}, {"follow_path", true}});
  add("pencil", "Pencil", "Basic", {{"hardness", 0.95}, {"size", 6}, {"spacing", 0.05}, {"pressure_flow", true}});
  add("marker", "Marker", "Basic", {{"alpha", "square"}, {"roundness", 0.35}, {"angle", 45}, {"hardness", 0.85}, {"spacing", 0.04}, {"size", 30}, {"flow", 0.85}});
  add("ink", "Ink Pen", "Basic", {{"hardness", 0.98}, {"size", 10}, {"spacing", 0.03}, {"lazy_radius", 14}});
  add("chalk", "Chalk", "Artistic", {{"alpha", {{"shape", "noise"}, {"param", 10}}}, {"hardness", 0.6}, {"spacing", 0.15}, {"angle_jitter", 1.0}, {"size", 45}});
  add("dry_brush", "Dry Brush", "Artistic", {{"alpha", "bristle"}, {"hardness", 0.5}, {"spacing", 0.05}, {"follow_path", true}, {"size", 70}, {"flow", 0.7}});
  add("hatching", "Hatching", "Artistic", {{"alpha", "hatch"}, {"hardness", 0.7}, {"spacing", 0.5}, {"size", 60}});
  add("stipple", "Stipple", "Artistic", {{"alpha", "dots"}, {"hardness", 0.8}, {"spacing", 0.6}, {"angle_jitter", 1.0}, {"size", 60}});
  add("spray", "Spray Can", "Artistic", {{"alpha", "spray"}, {"hardness", 0.7}, {"spacing", 0.1}, {"angle_jitter", 1.0}, {"size", 80}, {"flow", 0.6}});
  add("splatter", "Splatter", "Artistic", {{"alpha", "splatter"}, {"hardness", 0.8}, {"spacing", 1.2}, {"angle_jitter", 1.0}, {"size_jitter", 0.4}, {"scatter", 0.6}, {"size", 90}});
  add("leaves", "Leaves", "Artistic", {{"alpha", "leaf"}, {"hardness", 0.9}, {"spacing", 0.9}, {"angle_jitter", 1.0}, {"size_jitter", 0.5}, {"scatter", 1.0}, {"size", 50}});
  add("stamp_star", "Star Stamp", "Stamps", {{"alpha", "star"}, {"hardness", 0.95}, {"spacing", 2.0}, {"size", 60}});
  add("stamp_ring", "Ring Stamp", "Stamps", {{"alpha", "ring"}, {"hardness", 0.95}, {"spacing", 1.5}, {"size", 60}});
  add("grunge", "Grunge Dirt", "Weathering", {{"alpha", "grunge"}, {"hardness", 0.5}, {"spacing", 0.35}, {"angle_jitter", 1.0}, {"size_jitter", 0.3}, {"flow", 0.7}, {"size", 120}});
  add("scratches", "Scratches", "Weathering", {{"alpha", "scratches"}, {"hardness", 0.9}, {"spacing", 0.7}, {"angle_jitter", 0.15}, {"size_jitter", 0.3}, {"size", 110}});
  add("cracks", "Cracks", "Weathering", {{"alpha", "cracks"}, {"hardness", 0.8}, {"spacing", 0.8}, {"angle_jitter", 1.0}, {"size", 120}});
  add("rust_spots", "Rust Spots", "Weathering", {{"alpha", "rust"}, {"hardness", 0.6}, {"spacing", 0.45}, {"angle_jitter", 1.0}, {"size_jitter", 0.4}, {"scatter", 0.3}, {"size", 100}});
  add("dust_cloud", "Dust Cloud", "Weathering", {{"alpha", "cloud"}, {"hardness", 0.2}, {"spacing", 0.25}, {"angle_jitter", 1.0}, {"flow", 0.25}, {"size", 160}});
  add("edge_wear", "Edge Wear", "Weathering", {{"alpha", {{"shape", "noise"}, {"param", 14}}}, {"hardness", 0.7}, {"spacing", 0.2}, {"angle_jitter", 1.0}, {"size", 35}, {"angle_falloff", 0.0}});
  add("surface_sphere", "3D Surface Brush", "Projection", {{"projection", "surface"}, {"hardness", 0.6}, {"spacing", 0.1}, {"size", 10}});
  add("uv_brush", "UV Space Brush", "Projection", {{"projection", "uv"}, {"hardness", 0.8}, {"spacing", 0.08}, {"size", 30}});
  add("grazing_soft", "Soft (Angle Falloff)", "Projection", {{"hardness", 0.2}, {"angle_falloff", 0.25}, {"spacing", 0.08}, {"size", 70}});
  add("paint_through", "Paint Through", "Projection", {{"hardness", 0.7}, {"occlusion", false}, {"backface_culling", false}, {"spacing", 0.08}, {"size", 60}});
  add("blur_soft", "Blur", "Utility", {{"hardness", 0.3}, {"flow", 0.6}, {"spacing", 0.1}, {"size", 80}});
  return v;
}

void BrushLibrary::loadAll() {
  presets_ = builtinPresets();
  for (auto dir : {paths::dataDir() / "brushes", paths::userDataDir() / "brushes"}) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) continue;
    for (auto& e : fs::directory_iterator(dir, ec)) {
      if (e.path().extension() != ".json") continue;
      try {
        json j = json::parse(paths::readFile(e.path()));
        std::vector<json> list = j.contains("brushes") ? j["brushes"].get<std::vector<json>>() : std::vector<json>{j};
        for (auto& bj : list) {
          BrushPreset p;
          p.fromJson(bj);
          if (p.id.empty()) continue;
          if (!p.alpha.image.empty() && fs::path(p.alpha.image).is_relative())
            p.alpha.image = (e.path().parent_path() / p.alpha.image).string();
          auto it = std::find_if(presets_.begin(), presets_.end(), [&](auto& q) { return q.id == p.id; });
          if (it != presets_.end()) *it = p;
          else presets_.push_back(p);
        }
      } catch (std::exception& ex) {
        PLOG_W("Brush file %s: %s", e.path().string().c_str(), ex.what());
      }
    }
  }
  // (1.2) a brush for each image (and each tip of an .abr file) in the user's folder and its folders, unless a preset
  // there uses it already
  std::vector<BrushPreset> found;
  std::error_code ec;
  fs::path user = userDir();
  if (fs::exists(user, ec)) {
    std::vector<std::string> used;
    for (auto& p : presets_)
      if (!p.alpha.image.empty()) used.push_back(fs::weakly_canonical(p.alpha.image, ec).string());
    auto opts = fs::directory_options::skip_permission_denied;
    for (auto it = fs::recursive_directory_iterator(user, opts, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
      const fs::path& path = it->path();
      if (path.filename().string().rfind('.', 0) == 0) {  // (hidden files and folders)
        if (it->is_directory(ec)) it.disable_recursion_pending();
        continue;
      }
      if (!it->is_regular_file(ec)) continue;
      std::string ext = path.extension().string();
      for (auto& ch : ext) ch = (char)std::tolower((unsigned char)ch);
      const bool abr = ext == ".abr";
      if (!abr && !isBrushImage(path)) continue;
      fs::path rel = fs::relative(path, user, ec);
      std::string folder = rel.parent_path().generic_string();
      std::string stem = path.stem().string();
      std::string nice = stem;
      for (auto& ch : nice)
        if (ch == '_' || ch == '-') ch = ' ';
      BrushPreset base;
      base.size = 80.f, base.hardness = 1.f, base.spacing = 0.15f, base.followPath = false;
      if (abr) {
        std::string why;
        auto tips = brushimport::abrTipFiles(path.string(), &why);
        if (tips.empty() && !why.empty()) PLOG_W("Brushes: %s", why.c_str());
        for (size_t k = 0; k < tips.size(); ++k) {
          BrushPreset b = base;
          b.id = "file:" + rel.generic_string() + "#" + std::to_string(k + 1);
          b.name = nice + " " + std::to_string(k + 1);
          b.category = "Yours: " + (folder.empty() ? nice : folder + " / " + nice);
          b.alpha.image = tips[k];
          b.alpha.imageMode = "light";  // (Photoshop's tips: the amount of paint)
          found.push_back(b);
        }
        continue;
      }
      if (std::find(used.begin(), used.end(), fs::weakly_canonical(path, ec).string()) != used.end()) continue;
      BrushPreset b = base;
      b.id = "file:" + rel.generic_string();
      b.name = nice;
      b.category = folder.empty() ? std::string("Yours") : "Yours: " + folder;
      b.alpha.image = path.string();
      found.push_back(b);
    }
  }
  std::stable_sort(found.begin(), found.end(), [](const BrushPreset& a, const BrushPreset& b) {
    return a.category != b.category ? a.category < b.category : a.name < b.name;
  });
  // (the brushes of your own first in the Brushes panel)
  presets_.erase(std::remove_if(presets_.begin(), presets_.end(),
                                [&](const BrushPreset& q) {
                                  return std::any_of(found.begin(), found.end(), [&](const BrushPreset& b) { return b.id == q.id; });
                                }),
                 presets_.end());
  presets_.insert(presets_.begin(), found.begin(), found.end());
  folderSignature_ = signature();
  PLOG_I("Brush library: %zu presets (%zu from your brushes folder's images)", presets_.size(), found.size());
}

fs::path BrushLibrary::userDir() { return paths::userDataDir() / "brushes"; }

bool BrushLibrary::isBrushImage(const fs::path& p) {
  std::string ext = p.extension().string();
  for (auto& ch : ext) ch = (char)std::tolower((unsigned char)ch);
  for (const char* e : {".png", ".jpg", ".jpeg", ".tga", ".bmp", ".gif", ".psd", ".exr"})
    if (ext == e) return true;
  return false;
}

std::string BrushLibrary::signature() const {
  std::string sig;
  std::error_code ec;
  fs::path user = userDir();
  if (!fs::exists(user, ec)) return sig;
  for (auto it = fs::recursive_directory_iterator(user, fs::directory_options::skip_permission_denied, ec);
       !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
    if (!it->is_regular_file(ec)) continue;
    auto t = fs::last_write_time(it->path(), ec).time_since_epoch().count();
    sig += it->path().generic_string() + ":" + std::to_string(it->file_size(ec)) + ":" + std::to_string((long long)t) + "\n";
  }
  return sig;
}

bool BrushLibrary::userFolderChanged() { return signature() != folderSignature_; }

int BrushLibrary::rescan() {
  int before = (int)presets_.size();
  alphas_.clear();  // (an image changed in place makes its tip again)
  loadAll();
  return (int)presets_.size() - before;
}

std::vector<std::string> BrushLibrary::importFiles(const std::vector<std::string>& files, const std::string& category,
                                                   std::string* err) {
  std::vector<std::string> ids;
  std::error_code ec;
  fs::path dir = paths::ensureDir(category.empty() ? userDir() : userDir() / paths::sanitizeFileName(category));
  std::vector<fs::path> copied;
  for (auto& f : files) {
    fs::path src(f);
    std::string ext = src.extension().string();
    for (auto& ch : ext) ch = (char)std::tolower((unsigned char)ch);
    if (!isBrushImage(src) && ext != ".abr") {
      if (err) *err = src.filename().string() + ": not an image or a Photoshop brush file (.abr)";
      continue;
    }
    fs::path to = dir / src.filename();
    if (fs::exists(to, ec) && fs::equivalent(src, to, ec)) {
      copied.push_back(to);  // (already in the folder)
      continue;
    }
    for (int k = 2; fs::exists(to, ec) && k < 1000; ++k) to = dir / (src.stem().string() + "_" + std::to_string(k) + src.extension().string());
    fs::copy_file(src, to, ec);
    if (ec) {
      if (err) *err = "Could not copy " + src.string() + " into " + dir.string();
      continue;
    }
    copied.push_back(to);
  }
  rescan();
  for (auto& c : copied) {
    std::string rel = "file:" + fs::relative(c, userDir(), ec).generic_string();
    for (auto& p : presets_)
      if (p.id == rel || p.id.rfind(rel + "#", 0) == 0) ids.push_back(p.id);
  }
  return ids;
}

const BrushPreset* BrushLibrary::find(const std::string& id) const {
  for (auto& p : presets_)
    if (p.id == id) return &p;
  return nullptr;
}

bool BrushLibrary::saveUserPreset(const BrushPreset& p, std::string* err) {
  fs::path dir = paths::ensureDir(paths::userDataDir() / "brushes");
  fs::path file = dir / (paths::sanitizeFileName(p.id) + ".json");
  if (!paths::writeFileAtomic(file, p.toJson().dump(2))) {
    if (err) *err = "Could not write " + file.string();
    return false;
  }
  auto it = std::find_if(presets_.begin(), presets_.end(), [&](auto& q) { return q.id == p.id; });
  if (it != presets_.end()) *it = p;
  else presets_.push_back(p);
  return true;
}

const Image* BrushLibrary::alphaPixels(const BrushAlpha& a) {
  if (a.image.empty() && a.shape == "round") return nullptr;
  if (!alphaTexture(a)) return nullptr;
  for (auto& c : alphas_) {
    if (!(c->key == a)) continue;
    if (c->cpu.empty() && c->tex.valid()) {
      int level = 0;  // (a big image: a mip level near 256 pixels)
      while ((c->tex.width() >> level) > 256 && level + 1 < c->tex.levels()) ++level;
      c->cpu = c->tex.download(1, level);
    }
    return c->cpu.empty() ? nullptr : &c->cpu;
  }
  return nullptr;
}

GLuint BrushLibrary::alphaTexture(const BrushAlpha& a) {
  if (a.image.empty() && a.shape == "round") return 0;
  for (auto& c : alphas_)
    if (c->key == a) return c->tex.id();
  auto c = std::make_unique<Cached>();
  c->key = a;
  const int S = 256;
  if (!a.image.empty()) {
    Image rgba;
    if (loadImage(a.image, rgba, 4)) {
      // (1.2) which part paints: its transparency, or (an opaque image) the dark shape on a light ground (Photoshop,
      // GIMP and Krita tips) or the light one on a dark ground, as the border shows
      const size_t N = size_t(rgba.width) * rgba.height;
      std::string mode = a.imageMode;
      if (mode == "auto") {
        size_t clear = 0;
        for (size_t i = 0; i < N; ++i) clear += rgba.pixels[i * 4 + 3] < 0.98f;
        if (clear > N / 100) {
          mode = "alpha";
        } else {
          double border = 0;
          size_t n = 0;
          for (int x = 0; x < rgba.width; ++x)
            for (int y : {0, rgba.height - 1}) border += rgba.at(x, y)[0] + rgba.at(x, y)[1] + rgba.at(x, y)[2], ++n;
          for (int y = 0; y < rgba.height; ++y)
            for (int x : {0, rgba.width - 1}) border += rgba.at(x, y)[0] + rgba.at(x, y)[1] + rgba.at(x, y)[2], ++n;
          mode = n && border / (3.0 * n) > 0.5 ? "dark" : "light";
        }
      }
      Image img;
      img.resize(rgba.width, rgba.height, 1);
      for (size_t i = 0; i < N; ++i) {
        const float* p = &rgba.pixels[i * 4];
        float lum = 0.2126f * p[0] + 0.7152f * p[1] + 0.0722f * p[2];
        img.pixels[i] = mode == "alpha" ? p[3] : mode == "dark" ? (1.f - lum) * p[3] : lum * p[3];
      }
      c->tex.create(img.width, img.height, GL_R8, 0);
      c->tex.upload(img);
      c->tex.generateMips();  // (a big image drawn small: its mip levels)
    }
  }
  if (!c->tex.valid()) {
    c->tex.create(S, S, GL_R8, 0);
    Framebuffer fbo;
    fbo.attach(0, c->tex);
    fbo.setDrawBuffers(1);
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, S, S);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    Shader& s = ShaderCache::get("fullscreen.vs.glsl", "brush_alpha.fs.glsl");
    int shape = (int)(std::find(kShapes.begin(), kShapes.end(), a.shape) - kShapes.begin());
    if (shape >= (int)kShapes.size()) shape = 0;
    s.set("uShape", shape);
    s.set("uSeed", a.seed);
    s.set("uParams", vec4(a.param, 0, 0, 0));
    s.use();
    drawFullscreenTriangle();
    c->tex.generateMips();
  }
  c->tex.setWrap(GL_CLAMP_TO_BORDER, GL_CLAMP_TO_BORDER);
  float border[4] = {0, 0, 0, 0};
  glTextureParameterfv(c->tex.id(), GL_TEXTURE_BORDER_COLOR, border);
  GLuint id = c->tex.id();
  alphas_.push_back(std::move(c));
  return id;
}

GLuint BrushLibrary::previewTexture(const BrushPreset& p) {
  if (p.alpha.image.empty() && p.alpha.shape == "round") {
    BrushAlpha r;
    r.shape = "round";
    r.seed = -1;  // distinct cache key: render the round shape for previews
    for (auto& c : alphas_)
      if (c->key == r) return c->tex.id();
    auto c = std::make_unique<Cached>();
    c->key = r;
    c->tex.create(64, 64, GL_R8, 1);
    Framebuffer fbo;
    fbo.attach(0, c->tex);
    fbo.setDrawBuffers(1);
    gl::FramebufferScope fs(fbo.id());
    gl::ViewportScope vp(0, 0, 64, 64);
    glDisable(GL_BLEND);
    Shader& s = ShaderCache::get("fullscreen.vs.glsl", "brush_alpha.fs.glsl");
    s.set("uShape", 0);
    s.set("uSeed", 1.f);
    s.use();
    drawFullscreenTriangle();
    GLuint id = c->tex.id();
    alphas_.push_back(std::move(c));
    return id;
  }
  return alphaTexture(p.alpha);
}

}  // namespace patina
