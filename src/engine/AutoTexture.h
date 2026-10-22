#pragma once
#include <string>
#include <vector>

#include "core/Mesh.h"
#include "engine/MaterialLibrary.h"

namespace patina {

// Auto-texture: a library material for each material slot, guessed from the slot's name
// ("Skin", "Jeans", "M_Boots_Leather", "hair.001"...). Each guess becomes a fill layer masked
// by the "material slot" smart mask, so it covers exactly that slot's faces.
struct AutoTextureMatch {
  uint32_t slot = 0;
  std::string slotName;
  int faces = 0;
  std::string materialId, materialName;  // empty: no confident match
  float score = 0.f;
  std::vector<std::string> alternatives;  // next best ids
};

// The words a name is made of, lower case: "M_BootLeather.001" -> m, boot, leather
std::vector<std::string> nameTokens(const std::string& s);

// How well a material fits a slot name (0 = not at all; 3 or more is a confident match).
// style: "" (either), "realistic" or "stylized" (toon / anime / hand-painted are preferred).
float scoreMaterialForName(const std::string& slotName, const MaterialDef& def, const std::string& style = "");

std::vector<AutoTextureMatch> matchSlotMaterials(const Mesh& m, const MaterialLibrary& lib,
                                                 const std::string& style = "");

}  // namespace patina
