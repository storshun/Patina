#pragma once
#include <map>
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

#include "core/Mesh.h"
#include "engine/Channels.h"
#include "gfx/Texture.h"

namespace patina {

class Shader;
class Environment;
using json = nlohmann::json;

enum class MaterialType { Flat, Procedural, Image };

// Parameters of the procedural generators in material_bake.fs.glsl
struct ProcParams {
  int generator = 0;
  vec3 colorA{0.6f}, colorB{0.3f}, colorC{0.1f};  // sRGB
  vec2 roughness{0.5f, 0.7f};
  vec2 metallic{0.f, 0.f};
  float scale = 4.f;
  float detail = 5.f;
  float seed = 1.f;
  float heightAmp = 0.3f;
  float variation = 0.2f;
  float normalStrength = 1.f;
  vec4 extra{0.f};
  vec3 emissive{0.f};
  // 0.24, for every generator: wobble the pattern; dirt in the low parts; worn, lighter raised edges
  float warp = 0.f, warpScale = 4.f;
  float grime = 0.f;
  vec3 grimeColor{0.16f, 0.12f, 0.08f};
  float wear = 0.f;
  vec3 wearColor{0.85f, 0.82f, 0.76f};
  // 0.26: opacity (times the generator's own cut-outs: lace, nets, leaves), sheen, clearcoat, subsurface
  float opacity = 1.f;
  float sheen = 0.f;
  float coat = 0.f;
  float subsurface = 0.f;
  bool cutout = false;  // the generator's holes (net, lace, leaves, grilles...) are cut out (opacity 0)
};
// Generators whose pattern has holes (Cut out holes makes them see-through).
bool generatorHasHoles(int generator);

// What a generator's settings mean (the material editor, the docs and the API describe them from this).
struct GeneratorSetting {
  const char* label = nullptr;  // null: unused
  float lo = 0.f, hi = 1.f;
  float def = 0.f;              // what 0 means, when 0 stands for a default (else 0)
  const char* options = nullptr;  // a choice: the names, separated by '|' (the value is the index)
  const char* help = nullptr;
};
struct GeneratorInfo {
  const char* id;
  const char* name;
  const char* group;
  const char* colors;  // what colors A, B and C are for, separated by '|'
  GeneratorSetting extra[4];
};
const GeneratorInfo& generatorInfo(int generator);

struct MaterialDef {
  std::string id;
  std::string name;
  std::string category = "Misc";
  std::string description;
  MaterialType type = MaterialType::Procedural;
  ProcParams proc;
  FlatValues flat;  // flat materials
  std::map<std::string, std::string> maps;  // image materials: channel key -> absolute path
  bool normalDirectX = false;
  float defaultScale = 1.f;  // projection tiles per object-space unit
  bool builtin = false;
  std::string sourcePath;    // json file
  std::vector<std::string> tags;

  json toJson() const;
  static bool fromJson(const json& j, MaterialDef& out, const std::string& baseDir, std::string* err);
};

// How a library material is applied (brush or fill layer).
struct MaterialRef {
  std::string id;             // empty = flat
  FlatValues flat;            // used when id is empty or material is flat
  int mapping = 0;            // 0 triplanar, 1 uv, 2 screen planar
  float scale = 1.f;          // multiplies the material's default scale
  float rotationDeg = 0.f;
  vec2 offset{0.f};
  float triplanarSharpness = 4.f;
  int triplanarBlend = 1;     // 0 smooth cross-fade, 1 by height (crisp)
  vec3 tint{1.f};
  float roughnessScale = 1.f, roughnessOffset = 0.f;
  float metallicOverride = -1.f;
  float heightScale = 1.f, heightOffset = 0.f;
  float normalStrength = 1.f;
  float emissiveStrength = 1.f;

  json toJson() const;
  void fromJson(const json& j);
};

struct MaterialGpu {
  Texture2D color, rmh, normal, emissive;
  Texture2D extra;  // r opacity, g sheen, b clearcoat, a subsurface (0.26)
  Texture2D thumbnail;
  bool baked = false;
  int bakedSize = 0;
  float aspect = 1.f;  // source image width / height (image materials)
};

class MaterialLibrary {
 public:
  // The document's texture tiles (UV-mapped materials sample by the authored UV); set by the engine.
  const TileLayout* atlas = nullptr;
  struct Entry {
    MaterialDef def;
    MaterialGpu gpu;
    bool transient = false;  // in-memory only (material editor preview), hidden from lists
  };

  void loadAll();  // builtin + user dirs
  void loadDir(const std::string& dir, bool builtin);
  const std::vector<std::unique_ptr<Entry>>& entries() const { return entries_; }
  Entry* find(const std::string& id);
  static const std::string& currentId(const std::string& id);  // (0.36) an id renamed for American spelling: its new one
  std::vector<std::string> categories() const;

  // Imports a folder of PBR maps as user materials. Understands ambientCG, Poly Haven,
  // Quixel and Substance (Designer / Painter / Sampler export presets) naming, packed
  // maps (ORM / ARM, glTF metallicRoughness, Unity MaskMap and MetallicSmoothness),
  // OpenGL and DirectX normals, EXR / HDR / 16-bit maps, and a Substance Painter export
  // of several texture sets in one folder (one material per texture set).
  enum class NormalConvention { Auto, OpenGL, DirectX };
  struct ImageImportOptions {
    std::string name;  // material name (several texture sets: used as a prefix)
    NormalConvention normal = NormalConvention::Auto;
  };
  struct ImportReport {
    std::string name, id;
    std::map<std::string, std::string> files;  // channel -> source file it came from ("packed:G" noted)
    bool normalDirectX = false;
    std::vector<std::string> notes;
  };
  std::vector<Entry*> importImageFolderAll(const std::string& dir, const ImageImportOptions& opt,
                                           std::vector<ImportReport>* report, std::string* err);
  Entry* importImageFolder(const std::string& dir, const std::string& name = "", std::string* err = nullptr);
  // Why a file Patina cannot read as a material is unsupported (.sbsar, .spsm...), or "".
  static std::string unsupportedMaterialReason(const std::string& path);
  // Imports one image as a color-only material (decals, logos, photos).
  Entry* importImageFile(const std::string& file, std::string* err = nullptr);
  // Adds or replaces a user material (saved to the user library on disk).
  Entry* saveUserMaterial(const MaterialDef& def, std::string* err = nullptr);
  bool removeUserMaterial(const std::string& id);
  // Adds/replaces an in-memory material that is never written to disk.
  Entry* upsertTransient(const MaterialDef& def);

  // GPU resources (lazy). `size` = bake resolution for procedural materials.
  bool ensureBaked(Entry& e, int size = 1024);
  void rebake(Entry& e);
  GLuint thumbnail(Entry& e, const Environment& env);
  // Which of opacity, sheen, clearcoat and subsurface (channel bits 64..512) a material has at other than
  // their defaults (opaque, none): a brush paints only those of them (and those the layer has already).
  int extraChannelsUsed(const MaterialRef& ref);
  // Binds a MaterialRef for material.glsl; uses texture units [firstUnit, firstUnit+4].
  void bind(const Shader& s, const MaterialRef& ref, int firstUnit, const mat4& screenVP = mat4(1),
            float screenAspect = 1.f);
  // Average values of a material (for color picking / previews)
  FlatValues averageValues(Entry& e);

  int bakeSize = 1024;

 private:
  std::vector<std::unique_ptr<Entry>> entries_;
  Texture2D white_, flatNormal_, black_, extraDefault_;
  void ensureDefaults();
};

std::vector<std::string> generatorNames();
int generatorFromName(const std::string& n);

}  // namespace patina
