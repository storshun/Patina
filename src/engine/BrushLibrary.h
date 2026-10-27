#pragma once
#include <filesystem>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "gfx/Texture.h"

namespace patina {

using json = nlohmann::json;

enum class BrushProjection { Screen = 0, Surface = 1, UV = 2 };

// Brush tip alpha: procedural shape or grayscale image.
struct BrushAlpha {
  std::string shape = "round";  // round square star splatter noise scratches grunge dots hatch cracks bristle spray leaf ring cloud rust
  std::string image;            // absolute path (overrides shape)
  float seed = 1.f;
  float param = 0.f;
  // (1.2) how an image is read: "auto" (its transparency if it has some; else dark on a light ground paints, as
  // Photoshop, GIMP and Krita tips are, or light on a dark one), "light" (white paints), "dark" (black paints), "alpha"
  std::string imageMode = "auto";
  bool operator==(const BrushAlpha&) const = default;
};

struct BrushPreset {
  std::string id;
  std::string name;
  std::string category = "Basic";
  BrushAlpha alpha;
  float size = 40.f;            // pixels (screen), world units * 100 (surface) or texels/1000 (uv)
  float hardness = 0.8f;        // 0 soft .. 1 hard
  float opacity = 1.f;          // stroke opacity cap
  float flow = 1.f;             // per-dab opacity
  float spacing = 0.12f;        // fraction of the diameter
  float angleDeg = 0.f;
  float angleJitter = 0.f;      // 0..1 (fraction of 360)
  bool followPath = false;
  bool tiltAngle = false;       // pen tilt turns the tip (the direction the pen leans)
  float sizeJitter = 0.f;
  float flowJitter = 0.f;
  float scatter = 0.f;          // offset jitter as fraction of the diameter
  float roundness = 1.f;
  bool pressureSize = true;
  bool pressureFlow = false;
  bool pressureOpacity = false;
  BrushProjection projection = BrushProjection::Screen;
  bool backfaceCulling = true;
  bool occlusion = true;        // false = paint through to hidden surfaces
  float angleFalloff = 0.f;     // 0 off; else cos of grazing limit (0.1 .. 0.6)
  float lazyRadius = 0.f;       // lazy-mouse radius in pixels

  json toJson() const;
  void fromJson(const json& j);
};

class BrushLibrary {
 public:
  void loadAll();  // builtin presets + data/brushes + user brushes
  // ---- (1.2) the user's brushes folder (userDataDir/brushes): its .json presets, and a brush for each image in it and
  // its folders (PNG, JPEG, TGA, BMP, GIF, PSD, EXR; a folder is a category) and each sampled tip of a Photoshop .abr
  // file. Ids "file:<path in the folder>" ("#<n>" after an .abr's), so a project's or session's brush is found again.
  static std::filesystem::path userDir();
  bool userFolderChanged();  // (names, sizes and times: cheap, for a look every few seconds)
  int rescan();              // loadAll again; returns how many brushes there are more (fewer: negative)
  // Copies image or .abr files into the folder (category: a folder of that name, "" the top) and rescans; returns the
  // ids of the brushes they made (an .abr: each of its tips)
  std::vector<std::string> importFiles(const std::vector<std::string>& files, const std::string& category, std::string* err);
  static bool isBrushImage(const std::filesystem::path& p);
  const std::vector<BrushPreset>& presets() const { return presets_; }
  const BrushPreset* find(const std::string& id) const;
  bool saveUserPreset(const BrushPreset& p, std::string* err = nullptr);
  // GPU alpha texture for a tip (cached by shape/seed/image). Returns 0 for "round".
  GLuint alphaTexture(const BrushAlpha& a);
  GLuint previewTexture(const BrushPreset& p);  // alpha thumbnail (64x64)
  // (1.2.13) the tip as a grayscale image on the CPU (at most 256 pixels wide; row 0 = the top, v = 1), for the brush
  // cursor's outline. nullptr for "round" (a circle) or a tip that can't be read.
  const Image* alphaPixels(const BrushAlpha& a);
  std::vector<std::string> shapes() const;

 private:
  std::vector<BrushPreset> presets_;
  std::string folderSignature_;
  std::string signature() const;
  struct Cached {
    BrushAlpha key;
    Texture2D tex;
    Image cpu;  // (1.2.13) alphaPixels: read back once
  };
  std::vector<std::unique_ptr<Cached>> alphas_;
};

}  // namespace patina
