#pragma once
#include <string>
#include <vector>

#include "core/Math.h"

namespace patina {

// A texture a model's material uses: a file beside the model, or bytes inside it (GLB, embedded FBX media).
struct ModelMap {
  std::string path;           // resolved file, or empty
  std::vector<uint8_t> data;  // embedded encoded image (PNG, JPG...), or empty
  std::string name;           // as the model names it (for reports)
  bool missing = false;       // named by the model but not found
  // UV transform (KHR_texture_transform, FBX texture transforms) in Patina's UV space (v up): uv' = M * (u, v, 1)
  mat3 uvTransform{1.f};
  int channel = 0;      // for one-value maps: the channel that holds it (0 R, 1 G, 2 B, 3 A)
  bool invert = false;  // 1 - value (glossiness as roughness)
  bool alphaOnly = false;  // opacity from the image's alpha only (FBX transparency slots often hold an opaque JPG)
  bool valid() const { return !path.empty() || !data.empty(); }
  // the same image as another map (one decode for both)
  std::string key() const;
};

// One material slot of a model as the file describes it. Colors are linear (glTF factors, MTL and FBX values);
// a map and a value together multiply (glTF), except where `mapReplacesValue` says the value is only for when
// there is no map (OBJ and FBX, as Blender reads them).
struct ModelMaterial {
  std::string name;
  bool defined = false;  // the slot has a material in the file
  vec4 baseColor{1.f};   // linear rgb, alpha = opacity
  float metallic = 1.f, roughness = 1.f;
  vec3 emissive{0.f};    // linear, strength included
  float normalScale = 1.f;
  float opacity = 1.f;   // OBJ "d", FBX opacity (alpha of baseColor for glTF)
  int alphaMode = 0;     // 0 opaque, 1 cut-out at alphaCutoff, 2 blended
  float alphaCutoff = 0.5f;
  float sheen = 0.f, coat = 0.f;
  bool mapReplacesValue = false;
  int vertexColors = -1;  // Blender: 1 a Color Attribute into Base Color, 0 not (-1: other formats)

  ModelMap baseColorMap;
  ModelMap roughnessMap;          // one value (glTF: G of the metallic-roughness map)
  ModelMap metallicMap;           // one value (glTF: B of the same map)
  ModelMap normalMap;
  bool normalDirectX = false;     // green points down
  ModelMap bumpMap;               // OBJ map_Bump / FBX bump: a height map, or a normal map (checked on import)
  ModelMap emissiveMap;
  ModelMap opacityMap;            // alpha (or, without alpha, the red channel)
  ModelMap occlusionMap;          // not used for paint (reported)
  ModelMap coatMap, sheenMap;

  bool hasAnyMap() const;
  // Worth bringing into a project: a map, or values that are not a plain default gray.
  bool interesting() const;
};

struct ModelMaterials {
  std::vector<ModelMaterial> slots;  // index = Mesh::faceMaterial
  std::vector<std::string> notes;    // things the file uses that Patina leaves out
  bool hasAnyMap() const;
  bool interesting() const;
};

// Reads the materials of a .gltf / .glb / .obj (with its .mtl) / .fbx file, or of a .ptmesh from Blender (the
// Patina Bridge add-on 0.13 describes each slot's Principled BSDF in its header: "materials"). Slots are in the order the mesh
// loaders give faces (Mesh::faceMaterial). False (with err) when the file cannot be read; a file without
// materials gives an empty list.
bool loadModelMaterials(const std::string& path, ModelMaterials& out, std::string* err = nullptr);
// Finds a texture a model names: as written, beside the model, in textures/ folders and in FBX .fbm folders.
std::string findModelTexture(const std::string& modelPath, const std::string& named);

}  // namespace patina
