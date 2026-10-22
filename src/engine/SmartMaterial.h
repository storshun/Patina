#pragma once
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace patina {

using json = nlohmann::json;
class Document;
struct Layer;

// Smart materials: a layer (usually a folder of fill layers with smart masks) saved to a
// file and applied to any project. Painted textures and masks are included only when
// asked, because they follow the UV layout of the mesh they were painted on.
struct SmartMaterialInfo {
  std::string id;           // file stem
  std::string name;
  std::string description;
  std::string path;
  bool builtin = false;     // shipped in <data>/smart_materials
  int layerCount = 0;
  bool hasPaint = false;
  std::string tileMaterial;  // (0.22.5) a material in it, for its tile: the top fill layer's
};

std::vector<SmartMaterialInfo> listSmartMaterials();
// Saves layer `index` (with its contents if it is a folder). Returns the file path.
bool saveSmartMaterial(Document& d, int index, const std::string& name, const std::string& description,
                       bool includePaint, std::string* outPath, std::string* err);
// Inserts the smart material above the selection (inside a selected folder). Returns the
// flat index of its top layer, or -1.
int applySmartMaterial(Document& d, const std::string& idOrPath, std::string* err);
// (0.22.5) dropped on the layer stack: where it lands (Document::dropIndex; target -1: the bottom)
int applySmartMaterialAt(Document& d, const std::string& idOrPath, int target, bool into, bool below, std::string* err);

// Copy / paste layer settings: opacity, blend, channels, smart mask and the type
// specific parameters (fill material, adjustment) when both layers have that type.
json copyLayerSettings(const Layer& l);
void pasteLayerSettings(Layer& l, const json& settings);

}  // namespace patina
