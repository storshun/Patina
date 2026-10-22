#pragma once
// Regions and smart mask stacks for the commands that take them (add_tint_layer,
// add_material_layer, apply_smart_material, update_layer_mask): a region is one or more parts
// (mesh_parts) with parts left out; the mask is a list of smart masks multiplied together.
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace patina {

class Engine;

namespace regions {

using json = nlohmann::json;

json regionSchema();  // {part | parts, except}
json maskSchema();    // [{type, level, contrast, noise, scale, invert, direction}]

// A region as faces, or (when it is only material slots) as slots, which the material-slot
// smart mask follows exactly, whatever later happens to the UVs.
struct Region {
  bool given = false;
  bool slotsOnly = false;
  std::vector<uint32_t> slots;
  std::vector<uint32_t> faces;
  std::vector<std::string> ids;
};
// a["region"] (absent: not given); throws CommandError for parts that don't exist
Region regionArg(Engine& e, const json& a);

// Gives the layer its mask: the region (material-slot masks, or a painted mask of its faces)
// with the smart masks multiplied on top. Without either, the mask is left alone. With
// keepRegion, an existing region (the first effects of the stack) stays and only the smart
// masks are replaced; with keepMasks, the smart masks stay and only the region is replaced.
void applyRegionAndMasks(Engine& e, int index, const Region& r, const json& masks, bool keepRegion = false,
                         bool keepMasks = false);

}  // namespace regions
}  // namespace patina
