#include "engine/MaterialLibrary.h"

#include "core/I18n.h"

#include <stb_image_resize2.h>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <set>

#include "core/Image.h"
#include "core/Log.h"
#include "core/Paths.h"
#include "engine/Environment.h"
#include "gfx/Shader.h"

namespace patina {

namespace fs = std::filesystem;

static const std::vector<std::string> kGenerators = {
    "flat",   "two_tone", "brushed_metal", "scratched_metal", "rust",         "concrete", "wood",
    "stone",  "leather",  "fabric",        "tiles",           "plastic",      "car_paint", "dirt",
    "chipped_paint", "carbon_fiber", "marble", "cells", "emissive_grid",
    // 19..
    "diamond_plate", "hammered", "corrugated", "woven", "bark", "terrazzo", "sand", "moss", "asphalt", "camo",
    "perforated", "panels",
    // 31..
    "bricks", "parquet", "hex_tiles", "scales", "grille", "quilted", "knit", "knurling", "stucco", "twill",
    "skin", "hair", "brush_strokes", "painted_stone", "painted_wood", "painted_metal",
    // 47..
    "stripes", "checks", "dots", "animal_print", "cracked_earth", "flagstone", "pebbles", "strata", "crackle",
    "chainmail", "damascus", "circuit", "stained_glass", "mosaic", "sequins", "paper",
    // 0.26
    "fur", "feathers", "leaves", "rope", "damask", "ice", "crosshatch", "lace", "net", "hair_cards", "velvet"};

// What each generator's colors and Extra values are for (index = generator). A setting with def > 0 uses
// that value when it is left at 0.
static const GeneratorInfo kGeneratorInfo[] = {
    {"flat", N_("Flat"), N_("Basic"), N_("Color||"), {{N_("Roughness noise"), 0, 1}}},
    {"two_tone", N_("Two tone"), N_("Basic"), N_("First color|Second color|"),
     {{N_("Balance"), -0.5f, 0.5f, 0, nullptr, N_("Moves the split toward color A or B")}, {N_("Softness"), 0, 0.5f}}},
    {"brushed_metal", N_("Brushed metal"), N_("Metal"), N_("Metal||"), {}},
    {"scratched_metal", N_("Scratched metal"), N_("Metal"), N_("Metal|Scratches|"), {{N_("Scratches"), 0, 4, 3}}},
    {"rust", N_("Rust"), N_("Metal"), N_("Metal|Rust|Dark rust"),
     {{N_("Clean metal"), 0, 1, 0, nullptr, N_("0 rust all over, 1 almost none")}}},
    {"concrete", N_("Concrete"), N_("Stone and ground"), N_("Concrete|Patches|"), {{N_("Pore size"), 0, 0.5f, 0.12f}}},
    {"wood", N_("Wood"), N_("Wood and organic"), N_("Light grain|Dark grain|"), {}},
    {"stone", N_("Stone"), N_("Stone and ground"), N_("Stone|Second tone|Cracks"), {{N_("Crack width"), 0, 0.3f, 0.06f}}},
    {"leather", N_("Leather"), N_("Fabric and leather"), N_("Leather|Second tone|"), {}},
    {"fabric", N_("Fabric"), N_("Fabric and leather"), N_("Thread|Second thread|"), {{N_("Two-color weave"), 0, 1}}},
    {"tiles", N_("Tiles"), N_("Tiles and building"), N_("Tile|Second tile|Grout"),
     {{N_("Grout width"), 0, 0.3f, 0.05f}, {N_("Rows per column"), 0, 4, 1}, {N_("Brick offset"), 0, 1, 0, N_("Off|On")}}},
    {"plastic", N_("Plastic"), N_("Basic"), N_("Plastic||"), {}},
    {"car_paint", N_("Car paint"), N_("Paint and finishes"), N_("Paint|Flakes|"), {}},
    {"dirt", N_("Dirt"), N_("Stone and ground"), N_("Soil|Second soil|Pebbles"), {}},
    {"chipped_paint", N_("Chipped paint"), N_("Paint and finishes"), N_("Paint|Metal under|Chip edges"),
     {{N_("Intact paint"), 0, 1, 0, nullptr, N_("0 chipped all over, 1 almost no chips")}}},
    {"carbon_fiber", N_("Carbon fiber"), N_("Sci-fi and tech"), N_("Fiber|Second fiber|"), {}},
    {"marble", N_("Marble"), N_("Stone and ground"), N_("Marble|Second tone|Veins"), {}},
    {"cells", N_("Cells"), N_("Patterns and prints"), N_("Cell|Second cell|Edges"), {{N_("Edge width"), 0, 0.3f, 0.05f}}},
    {"emissive_grid", N_("Glowing grid"), N_("Sci-fi and tech"), N_("Panel|Lines|"), {{N_("Line width"), 0, 0.3f, 0.03f}}},
    {"diamond_plate", N_("Diamond plate"), N_("Metal"), N_("Plate|Lozenges|"), {{N_("Lozenge size"), 0, 0.5f, 0.42f}}},
    {"hammered", N_("Hammered metal"), N_("Metal"), N_("Metal|Ridges|"), {}},
    {"corrugated", N_("Corrugated sheet"), N_("Metal"), N_("Metal|Grime|"), {{N_("Grime in the valleys"), 0, 2}, {N_("Run-off streaks"), 0, 2}}},
    {"woven", N_("Basket weave"), N_("Fabric and leather"), N_("Strands|Cross strands|Gaps"), {{N_("Cross strand tone"), 0, 1}}},
    {"bark", N_("Bark"), N_("Wood and organic"), N_("Bark|Lichen|Fissures"), {{N_("Lichen"), 0, 1}}},
    {"terrazzo", N_("Terrazzo"), N_("Stone and ground"), N_("Matrix|Chips|Second chips"), {{N_("Chip size"), 0, 0.5f, 0.3f}}},
    {"sand", N_("Sand"), N_("Stone and ground"), N_("Sand|Second sand|Sparkle"), {{N_("Ripples"), 0, 2, 1}}},
    {"moss", N_("Moss"), N_("Stone and ground"), N_("Moss|Moss tips|Soil"),
     {{N_("Bare soil"), 0, 1, 0.45f, nullptr, N_("0 moss all over, 1 almost none")}}},
    {"asphalt", N_("Asphalt"), N_("Stone and ground"), N_("Asphalt|Stones|Tar and cracks"), {{N_("Cracks"), 0, 0.5f, 0.08f}}},
    {"camo", N_("Camouflage"), N_("Patterns and prints"), N_("Base|Second|Third"), {{N_("Dark blotches"), 0, 1}, {N_("Edge softness"), 0, 0.3f, 0.02f}}},
    {"perforated", N_("Perforated sheet"), N_("Metal"), N_("Sheet|Hole rims|Holes"),
     {{N_("Hole size"), 0, 0.5f, 0.3f}, {}, {N_("Staggered"), 0, 1, 0, N_("Off|On")}}},
    {"panels", N_("Sci-fi panels"), N_("Sci-fi and tech"), N_("Panel|Second panel|Grooves"),
     {{N_("Groove width"), 0, 0.1f, 0.012f}, {N_("Rivets"), 0, 1, 0, N_("Off|On")}, {N_("Panel heights"), 0, 1}}},
    {"bricks", N_("Bricks"), N_("Tiles and building"), N_("Brick|Second brick|Mortar"),
     {{N_("Joint width"), 0, 0.5f, 0.1f}, {N_("Bond"), 0, 3, 0, N_("Running|Stack|Herringbone|Basket weave")}, {N_("Wear"), 0, 1},
      {"Length : height", 0, 8, 3}}},
    {"parquet", N_("Parquet and planks"), N_("Wood and organic"), N_("Light wood|Dark wood|Gaps"),
     {{N_("Gap width"), 0, 0.2f, 0.03f}, {N_("Layout"), 0, 2, 0, N_("Floorboards|Herringbone|Basket")}, {N_("Length : width"), 0, 12, 5},
      {"Grain contrast", 0, 3, 1}}},
    {"hex_tiles", N_("Hexagon tiles"), N_("Tiles and building"), N_("Tile|Second tile|Grout"),
     {{N_("Grout width"), 0, 0.3f, 0.05f}, {N_("Bevel"), 0, 0.5f, 0.1f}, {N_("Colors"), 0, 1, 0, N_("Random|Every third tile B")},
      {"Honeycomb", 0, 1, 0, N_("Off|On")}}},
    {"scales", N_("Scales"), N_("Wood and organic"), N_("Scale|Rim and second tone|Shadow"),
     {{N_("Pointed"), 0, 1, 0, nullptr, N_("0 round fish scales, 1 pointed dragon scales")}, {N_("Rim highlight"), 0, 1}, {N_("Keel"), 0, 1},
      {"Scale size", 0, 1, 0.62f}}},
    {"grille", N_("Grille and mesh"), N_("Metal"), N_("Wire|Oxide|Holes"),
     {{N_("Wire width"), 0, 0.5f, 0.16f}, {N_("Type"), 0, 2, 0, N_("Expanded metal|Chain-link|Woven wire")}, {N_("Strand tilt"), 0, 2, 1}}},
    {"quilted", N_("Quilted"), N_("Fabric and leather"), N_("Cushion|Stitches|Seams"),
     {{N_("Stitches"), 0, 1}, {N_("Buttons"), 0, 1, 0, N_("Off|On")}, {N_("Layout"), 0, 2, 0, N_("Diamond|Square|Channels")}, {N_("Leather grain"), 0, 1}}},
    {"knit", N_("Knit"), N_("Fabric and leather"), N_("Yarn|Stripes|Between stitches"),
     {{N_("Ribbing"), 0, 8, 0, nullptr, N_("Purl columns every n stitches (0 none)")}, {N_("Stripes"), 0, 16}, {N_("Fuzz"), 0, 2, 1},
      {"Seed stitch", 0, 1, 0, N_("Off|On")}}},
    {"knurling", N_("Knurling and grips"), N_("Metal"), N_("Metal|Worn crests|Groove dirt"),
     {{N_("Type"), 0, 3, 0, N_("Diamond|Straight|Rubber dots|Square")}, {N_("Worn crests"), 0, 1}, {N_("Flat tops"), 0, 0.9f},
      {"Groove dirt", 0, 1, 0.5f}}},
    {"stucco", N_("Stucco"), N_("Tiles and building"), N_("Plaster|Splats|Dirt"),
     {{N_("Splat coverage"), 0, 1, 0.5f}, {N_("Trowel flattening"), 0, 1}, {N_("Sand grain"), 0, 1}, {N_("Orange peel"), 0, 1}}},
    {"twill", N_("Twill and satin"), N_("Fabric and leather"), N_("Warp|Weft|Tartan third color"),
     {{N_("Weave"), 0, 2, 0, N_("2/1 twill|3/1 twill (denim)|Satin")}, {N_("Tartan repeats"), 0, 8}, {N_("Pleats"), 0, 16}, {N_("Sheen"), 0, 1}}},
    {"skin", N_("Skin"), N_("Character"), N_("Skin|Redness|Freckles"), {{N_("Pores"), 0, 1}, {N_("Freckles"), 0, 1}, {N_("Fine creases"), 0, 1}, {N_("Redness"), 0, 1}}},
    {"hair", N_("Hair"), N_("Character"), N_("Hair|Highlight|Gaps between locks"),
     {{N_("Lock gaps"), 0, 1}, {N_("Highlight band"), 0, 1}, {N_("Cel"), 0, 1, 0, nullptr, N_("0 soft, 1 two clean tones and a zig-zag highlight")},
      {"Waves", 0, 8}}},
    {"brush_strokes", N_("Brush strokes"), N_("Hand-painted"), N_("Base|Shade|Light"),
     {{N_("Softness"), 0, 1, 0, nullptr, N_("0 hard cel bands, 1 soft gradients")}, {N_("Direction"), 0, 3, 0, N_("Horizontal|Diagonal|Vertical|Other diagonal")},
      {"Stroke length", 0, 8, 3}, {N_("Bristle streaks"), 0, 1}}},
    {"painted_stone", N_("Hand-painted stone"), N_("Hand-painted"), N_("Stone|Second stone|Outlines and shadow"),
     {{N_("Outline width"), 0, 0.3f, 0.05f}, {N_("Layout"), 0, 1, 0, N_("Cobbles|Wall blocks")}, {N_("Painted chips"), 0, 1}, {N_("Highlights"), 0, 2, 1}}},
    {"painted_wood", N_("Hand-painted wood"), N_("Hand-painted"), N_("Wood|Light edges|Grain and outlines"),
     {{N_("Grain lines"), 0, 8, 3}, {N_("Knots"), 0, 1}, {N_("Light edge width"), 0, 0.5f, 0.12f}, {N_("Nails"), 0, 1, 0, N_("Off|On")}}},
    {"painted_metal", N_("Hand-painted metal"), N_("Hand-painted"), N_("Metal|Light|Shade and outlines"),
     {{N_("Bevel width"), 0, 0.3f, 0.08f}, {N_("Rivets"), 0, 1, 0, N_("Off|On")}, {N_("Painted scratches"), 0, 1}, {N_("Gradient"), 0, 1}}},
    // 0.24
    {"stripes", N_("Stripes"), N_("Patterns and prints"), N_("Ground|Stripes|Third stripe or pinstripe"),
     {{N_("Stripe width"), 0, 1, 0, nullptr, N_("Part of each repeat (0: half, or thin for pinstripes)")},
      {"Style", 0, 4, 0, N_("Plain|Pinstripe|Chevron|Wavy|Three colors")}, {N_("Direction"), 0, 3, 0, N_("Across|Along|Diagonal|Other diagonal")},
      {"Woven", 0, 1, 0, nullptr, N_("0 a smooth print, 1 woven threads")}}},
    {"checks", N_("Checks"), N_("Patterns and prints"), N_("First|Second|Argyle lines"),
     {{N_("Style"), 0, 4, 0, N_("Checkerboard|Gingham|Houndstooth|Argyle|Windowpane")}, {N_("Line width"), 0, 0.3f, 0.05f},
      {"Woven", 0, 1, 0, nullptr, N_("0 a smooth print, 1 woven threads")}, {N_("Diamond height"), 0, 4, 1.5f, nullptr, N_("Argyle: height to width")}}},
    {"dots", N_("Dots"), N_("Patterns and prints"), N_("Ground|Dots|Second dots"),
     {{N_("Dot size"), 0, 0.5f, 0.3f}, {N_("Layout"), 0, 3, 0, N_("Grid|Staggered|Scattered|Halftone")}, {N_("Raised"), 0, 1, 0, nullptr, N_("0 printed, 1 domed studs")},
      {"Woven", 0, 1}}},
    {"animal_print", N_("Animal print"), N_("Patterns and prints"), N_("Coat|Marks|Centers and patches"),
     {{N_("Animal"), 0, 5, 0, N_("Leopard|Cheetah|Zebra|Tiger|Giraffe|Cow")}, {N_("Fur"), 0, 1, 0, nullptr, N_("0 a smooth print, 1 short fur")},
      {"Mark size", 0, 2, 1}}},
    {"cracked_earth", N_("Cracked earth"), N_("Stone and ground"), N_("Earth|Dry earth|Cracks"),
     {{N_("Crack width"), 0, 0.3f, 0.05f}, {N_("Finer cracks"), 0, 1}, {N_("Curled edges"), 0, 1},
      {"Glowing cracks", 0, 1, 0, nullptr, N_("The Emissive color, in the cracks: lava")}}},
    {"flagstone", N_("Flagstones"), N_("Stone and ground"), N_("Stone|Second stone|Joints"),
     {{N_("Joint width"), 0, 0.3f, 0.08f}, {N_("Uneven"), 0, 1}, {N_("Riven layers"), 0, 1}, {N_("Tufts in the joints"), 0, 1}}},
    {"pebbles", N_("Pebbles"), N_("Stone and ground"), N_("Pebble|Second pebble|Between"),
     {{N_("Gap"), 0, 0.5f, 0.12f}, {N_("Size variation"), 0, 1}, {N_("Flat stones"), 0, 1}, {N_("Gravel between"), 0, 1}}},
    {"strata", N_("Rock layers"), N_("Stone and ground"), N_("First layer|Second layer|Third layer"),
     {{N_("Folding"), 0, 1, 0.3f}, {N_("Erosion"), 0, 1}, {N_("Cracks"), 0, 1}, {N_("Sand grain"), 0, 1}}},
    {"crackle", N_("Crackle"), N_("Paint and finishes"), N_("Glaze or paint|Under the paint|Cracks"),
     {{N_("Crack width"), 0, 0.2f, 0.03f}, {N_("Style"), 0, 2, 0, N_("Crazed glaze|Alligator paint|Craquelure")}, {N_("Finer cracks"), 0, 1},
      {"Crack depth", 0, 2, 1}}},
    {"chainmail", N_("Chainmail"), N_("Metal"), N_("Rings|Tarnish|Between the rings"),
     {{N_("Wire"), 0, 0.3f, 0.12f}, {N_("Ring size"), 0, 0.6f, 0.42f}, {N_("Lean"), 0, 1.5f, 0.6f}, {N_("Tarnish"), 0, 1}}},
    {"damascus", N_("Damascus steel"), N_("Metal"), N_("Bright steel|Dark steel|"),
     {{N_("Folding"), 0, 2, 0.5f}, {N_("Pattern"), 0, 2, 0, N_("Random|Ladder|Raindrop")}, {N_("Etch depth"), 0, 1}, {N_("Contrast"), 0, 2, 1}}},
    {"circuit", N_("Circuit board"), N_("Sci-fi and tech"), N_("Board|Traces|Chips"),
     {{N_("Trace density"), 0, 1, 0.5f}, {N_("Chips"), 0, 1}, {N_("Glowing traces"), 0, 1, 0, nullptr, N_("The Emissive color, along the traces")},
      {"Trace width", 0, 0.5f, 0.18f}}},
    {"stained_glass", N_("Stained glass"), N_("Tiles and building"), N_("First glass|Second glass|Third glass"),
     {{N_("Lead width"), 0, 0.3f, 0.06f}, {N_("Layout"), 0, 2, 0, N_("Random pieces|Square panes|Diamond panes")},
      {"Glow", 0, 1, 0, nullptr, N_("Lit from behind: the pieces glow in their own colors")}, {N_("Rippled glass"), 0, 1}}},
    {"mosaic", N_("Mosaic"), N_("Tiles and building"), N_("Tile|Second tile|Grout"),
     {{N_("Grout width"), 0, 0.3f, 0.08f}, {N_("Shape"), 0, 2, 0, N_("Squares|Penny rounds|Tesserae")},
      {"Color patches", 0, 1, 0, nullptr, N_("0 each tile A or B, 1 blended in large patches")}, {N_("Bevel"), 0, 0.5f, 0.15f}}},
    {"sequins", N_("Sequins and glitter"), N_("Fabric and leather"), N_("Sequins|Second sequins|Fabric"),
     {{N_("Sequin size"), 0, 1, 0.65f}, {N_("Tilt"), 0, 1, 0.5f}, {N_("Kind"), 0, 1, 0, N_("Sequins|Glitter")}, {N_("Center holes"), 0, 1}}},
    {"paper", N_("Paper"), N_("Wood and organic"), N_("Paper|Fibers and stains|Ink"),
     {{N_("Crumple"), 0, 1}, {N_("Lines"), 0, 3, 0, N_("None|Ruled|Squared|Dot grid")}, {N_("Fibers"), 0, 1}, {N_("Age stains"), 0, 1}}},
    // 0.26
    {"fur", N_("Fur"), N_("Fabric and leather"), N_("Coat|Tips|Deep between clumps"),
     {{N_("Hair length"), 0, 3, 1}, {N_("Clumping"), 0, 1, 0.6f}, {N_("Wave"), 0, 1}, {N_("Lighter tips"), 0, 1}}},
    {"feathers", N_("Feathers"), N_("Wood and organic"), N_("Vane|Shaft and edges|Markings and shadow"),
     {{N_("Length : width"), 0, 4, 2}, {N_("Barbs"), 0, 48, 24}, {N_("Markings"), 0, 3, 0, N_("None|Bars|Dark tips|Eyes")}, {N_("Ruffle"), 0, 1}}},
    {"leaves", N_("Leaves"), N_("Wood and organic"), N_("Leaves|Second leaf color|Ground or gaps"),
     {{N_("Style"), 0, 2, 0, N_("Fallen leaves|One leaf (a card)|Foliage")}, {N_("Leaf width"), 0, 0.6f, 0.35f}, {N_("Veins"), 0, 1},
      {"Color change", 0, 1, 0, nullptr, N_("How much the leaves vary toward the second color (autumn)")}}},
    {"rope", N_("Rope"), N_("Fabric and leather"), N_("Fiber|Highlights|Grooves"),
     {{N_("Style"), 0, 2, 0, N_("Twisted|Braided|Coiled")}, {N_("Strands"), 0, 8, 3}, {N_("Twist"), 0, 4, 1}, {N_("Fuzz"), 0, 1}}},
    {"damask", N_("Damask"), N_("Patterns and prints"), N_("Ground|Ornament|Outline"),
     {{N_("Style"), 0, 2, 0, N_("Medallion|Floral trellis|Fleur-de-lis")}, {N_("Ornament size"), 0, 1.5f, 1},
      {"Outline", 0, 0.05f, 0.012f}, {N_("Woven"), 0, 1, 0, nullptr, N_("A woven fabric under the pattern")}}},
    {"ice", N_("Ice"), N_("Stone and ground"), N_("Deep ice|Frost and bubbles|Cracks"),
     {{N_("Cracks"), 0, 1}, {N_("Bubbles"), 0, 1}, {N_("Frost"), 0, 1}, {N_("Crack depth"), 0, 2, 1}}},
    {"crosshatch", N_("Crosshatching"), N_("Hand-painted"), N_("Paper|Ink|Darkest ink"),
     {{N_("Line width"), 0, 0.8f, 0.3f}, {N_("Layers"), 0, 4, 3}, {N_("Contrast"), 0, 1}, {N_("Wobble"), 0, 1}}},
    {"lace", N_("Lace"), N_("Fabric and leather"), N_("Thread|Cord|Holes"),
     {{N_("Motif size"), 0, 1.5f, 1}, {N_("Net fineness"), 0, 24, 10}, {N_("Style"), 0, 2, 0, N_("Roses|Vines|Geometric")},
      {"Cord", 0, 0.06f, 0.02f}}},
    {"net", N_("Net and mesh"), N_("Fabric and leather"), N_("Thread|Knots|Holes"),
     {{N_("Thread width"), 0, 0.4f, 0.12f}, {N_("Style"), 0, 3, 0, N_("Fishnet|Honeycomb|Square mesh|Knotted net")}, {N_("Knots"), 0, 1},
      {"Stretch", 0, 2, 0, nullptr, N_("Cells taller than they are wide")}}},
    {"hair_cards", N_("Hair cards"), N_("Character"), N_("Hair|Tips and highlight|Gaps"),
     {{N_("Strands"), 0, 120, 40}, {N_("Clumping"), 0, 1, 0.5f}, {N_("Short strands"), 0, 0.9f, 0.35f, nullptr, N_("How much shorter the shortest strands are")},
      {"Lighter tips", 0, 1}}},
    {"velvet", N_("Velvet"), N_("Fabric and leather"), N_("Pile|Crushed pile|Sheer ground"),
     {{N_("Style"), 0, 3, 0, N_("Plain|Crushed|Corduroy|Devore (burnt-out)")}, {N_("Pattern size"), 0, 2, 1}, {N_("Pile grain"), 0, 1},
      {"Wales", 0, 64, 24, nullptr, N_("Ribs across a tile (corduroy)")}}},
};
static_assert(sizeof(kGeneratorInfo) / sizeof(kGeneratorInfo[0]) == 74, "a GeneratorInfo for every generator");

const GeneratorInfo& generatorInfo(int g) {
  constexpr int n = sizeof(kGeneratorInfo) / sizeof(kGeneratorInfo[0]);
  return kGeneratorInfo[std::clamp(g, 0, n - 1)];
}

std::vector<std::string> generatorNames() { return kGenerators; }
bool generatorHasHoles(int g) {
  // perforated, grille, chainmail, leaves (one leaf, foliage), lace, net, hair cards, velvet (devoré)
  return g == 29 || g == 35 || g == 56 || g == 65 || g == 70 || g == 71 || g == 72 || g == 73;
}
int generatorFromName(const std::string& n) {
  for (size_t i = 0; i < kGenerators.size(); ++i)
    if (kGenerators[i] == n) return (int)i;
  return 0;
}

// ------------------------------------------------------------------ JSON

static json v3(const vec3& v) { return {v.x, v.y, v.z}; }
static vec3 v3(const json& j, vec3 d) {
  if (j.is_array() && j.size() >= 3) return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
  if (j.is_string()) {  // "#rrggbb"
    std::string s = j;
    if (s.size() == 7 && s[0] == '#') {
      unsigned v = std::stoul(s.substr(1), nullptr, 16);
      return {((v >> 16) & 255) / 255.f, ((v >> 8) & 255) / 255.f, (v & 255) / 255.f};
    }
  }
  if (j.is_number()) return vec3(j.get<float>());
  return d;
}
static vec2 v2(const json& j, vec2 d) {
  if (j.is_array() && j.size() >= 2) return {j[0].get<float>(), j[1].get<float>()};
  if (j.is_number()) return vec2(j.get<float>());
  return d;
}

json MaterialDef::toJson() const {
  json j = {{"id", id}, {"name", name}, {"category", category}, {"description", description},
            {"default_scale", defaultScale}, {"tags", tags}};
  if (type == MaterialType::Flat) {
    j["type"] = "flat";
    j["color"] = v3(flat.color);
    j["roughness"] = flat.roughness;
    j["metallic"] = flat.metallic;
    j["height"] = flat.height;
    j["emissive"] = v3(flat.emissive);
    if (flat.opacity < 1.f) j["opacity"] = flat.opacity;
    if (flat.sheen > 0.f) j["sheen"] = flat.sheen;
    if (flat.coat > 0.f) j["coat"] = flat.coat;
    if (flat.subsurface > 0.f) j["subsurface"] = flat.subsurface;
  } else if (type == MaterialType::Procedural) {
    j["type"] = "procedural";
    j["generator"] = kGenerators[std::clamp(proc.generator, 0, (int)kGenerators.size() - 1)];
    j["params"] = {{"color_a", v3(proc.colorA)},   {"color_b", v3(proc.colorB)},   {"color_c", v3(proc.colorC)},
                   {"roughness", {proc.roughness.x, proc.roughness.y}},
                   {"metallic", {proc.metallic.x, proc.metallic.y}},
                   {"scale", proc.scale},          {"detail", proc.detail},        {"seed", proc.seed},
                   {"height", proc.heightAmp},     {"variation", proc.variation},
                   {"normal_strength", proc.normalStrength},
                   {"extra", {proc.extra.x, proc.extra.y, proc.extra.z, proc.extra.w}},
                   {"emissive", v3(proc.emissive)}};
    if (proc.warp > 0.f) j["params"]["warp"] = proc.warp, j["params"]["warp_scale"] = proc.warpScale;
    if (proc.grime > 0.f) j["params"]["grime"] = proc.grime, j["params"]["grime_color"] = v3(proc.grimeColor);
    if (proc.wear > 0.f) j["params"]["wear"] = proc.wear, j["params"]["wear_color"] = v3(proc.wearColor);
    if (proc.opacity < 1.f) j["params"]["opacity"] = proc.opacity;
    if (proc.sheen > 0.f) j["params"]["sheen"] = proc.sheen;
    if (proc.coat > 0.f) j["params"]["coat"] = proc.coat;
    if (proc.subsurface > 0.f) j["params"]["subsurface"] = proc.subsurface;
    if (proc.cutout) j["params"]["cutout"] = true;
  } else {
    j["type"] = "image";
    json m = json::object();
    for (auto& [k, p] : maps) m[k] = fs::path(p).filename().string();
    j["maps"] = m;
    j["normal_directx"] = normalDirectX;
  }
  return j;
}

bool MaterialDef::fromJson(const json& j, MaterialDef& d, const std::string& baseDir, std::string* err) {
  try {
    d.id = j.at("id").get<std::string>();
    d.name = j.value("name", d.id);
    d.category = j.value("category", std::string("Misc"));
    d.description = j.value("description", std::string());
    d.defaultScale = j.value("default_scale", 1.f);
    if (j.contains("tags")) d.tags = j["tags"].get<std::vector<std::string>>();
    std::string type = j.value("type", std::string("procedural"));
    if (type == "flat") {
      d.type = MaterialType::Flat;
      d.flat.color = v3(j.value("color", json()), d.flat.color);
      d.flat.roughness = j.value("roughness", 0.5f);
      d.flat.metallic = j.value("metallic", 0.f);
      d.flat.height = j.value("height", 0.5f);
      d.flat.emissive = v3(j.value("emissive", json()), vec3(0));
      d.flat.opacity = std::clamp(j.value("opacity", 1.f), 0.f, 1.f);
      d.flat.sheen = std::clamp(j.value("sheen", 0.f), 0.f, 1.f);
      d.flat.coat = std::clamp(j.value("coat", 0.f), 0.f, 1.f);
      d.flat.subsurface = std::clamp(j.value("subsurface", 0.f), 0.f, 1.f);
    } else if (type == "image") {
      d.type = MaterialType::Image;
      for (auto& [k, v] : j.at("maps").items()) {
        fs::path p = v.get<std::string>();
        if (p.is_relative()) p = fs::path(baseDir) / p;
        d.maps[k] = p.string();
      }
      d.normalDirectX = j.value("normal_directx", false);
    } else {
      d.type = MaterialType::Procedural;
      d.proc.generator = generatorFromName(j.value("generator", std::string("flat")));
      const json& p = j.contains("params") ? j["params"] : json::object();
      d.proc.colorA = v3(p.value("color_a", json()), d.proc.colorA);
      d.proc.colorB = v3(p.value("color_b", json()), d.proc.colorB);
      d.proc.colorC = v3(p.value("color_c", json()), d.proc.colorC);
      d.proc.roughness = v2(p.value("roughness", json()), d.proc.roughness);
      d.proc.metallic = v2(p.value("metallic", json()), d.proc.metallic);
      d.proc.scale = p.value("scale", d.proc.scale);
      d.proc.detail = p.value("detail", d.proc.detail);
      d.proc.seed = p.value("seed", d.proc.seed);
      d.proc.heightAmp = p.value("height", d.proc.heightAmp);
      d.proc.variation = p.value("variation", d.proc.variation);
      d.proc.normalStrength = p.value("normal_strength", d.proc.normalStrength);
      if (p.contains("extra")) {
        auto& e = p["extra"];
        for (size_t i = 0; i < std::min<size_t>(4, e.size()); ++i) d.proc.extra[(int)i] = e[i].get<float>();
      }
      d.proc.emissive = v3(p.value("emissive", json()), vec3(0));
      d.proc.warp = p.value("warp", 0.f);
      d.proc.warpScale = p.value("warp_scale", d.proc.warpScale);
      d.proc.grime = p.value("grime", 0.f);
      d.proc.grimeColor = v3(p.value("grime_color", json()), d.proc.grimeColor);
      d.proc.wear = p.value("wear", 0.f);
      d.proc.wearColor = v3(p.value("wear_color", json()), d.proc.wearColor);
      d.proc.opacity = std::clamp(p.value("opacity", 1.f), 0.f, 1.f);
      d.proc.sheen = std::clamp(p.value("sheen", 0.f), 0.f, 1.f);
      d.proc.coat = std::clamp(p.value("coat", 0.f), 0.f, 1.f);
      d.proc.subsurface = std::clamp(p.value("subsurface", 0.f), 0.f, 1.f);
      d.proc.cutout = p.value("cutout", false);
    }
  } catch (std::exception& e) {
    if (err) *err = e.what();
    return false;
  }
  return true;
}

json MaterialRef::toJson() const {
  return {{"id", id},
          {"flat", {{"color", v3(flat.color)}, {"roughness", flat.roughness}, {"metallic", flat.metallic},
                    {"height", flat.height}, {"emissive", v3(flat.emissive)}, {"opacity", flat.opacity},
                    {"sheen", flat.sheen}, {"coat", flat.coat}, {"subsurface", flat.subsurface}}},
          {"mapping", mapping},
          {"scale", scale},
          {"rotation", rotationDeg},
          {"offset", {offset.x, offset.y}},
          {"triplanar_sharpness", triplanarSharpness},
          {"triplanar_blend", triplanarBlend ? "height" : "smooth"},
          {"tint", v3(tint)},
          {"roughness_scale", roughnessScale},
          {"roughness_offset", roughnessOffset},
          {"metallic_override", metallicOverride},
          {"height_scale", heightScale},
          {"height_offset", heightOffset},
          {"normal_strength", normalStrength},
          {"emissive_strength", emissiveStrength}};
}

void MaterialRef::fromJson(const json& j) {
  id = MaterialLibrary::currentId(j.value("id", id));
  if (j.contains("flat")) {
    auto& f = j["flat"];
    flat.color = v3(f.value("color", json()), flat.color);
    flat.roughness = f.value("roughness", flat.roughness);
    flat.metallic = f.value("metallic", flat.metallic);
    flat.height = f.value("height", flat.height);
    flat.emissive = v3(f.value("emissive", json()), flat.emissive);
    flat.opacity = f.value("opacity", flat.opacity);
    flat.sheen = f.value("sheen", flat.sheen);
    flat.coat = f.value("coat", flat.coat);
    flat.subsurface = f.value("subsurface", flat.subsurface);
  }
  mapping = j.value("mapping", mapping);
  scale = j.value("scale", scale);
  rotationDeg = j.value("rotation", rotationDeg);
  offset = v2(j.value("offset", json()), offset);
  triplanarSharpness = j.value("triplanar_sharpness", triplanarSharpness);
  if (j.contains("triplanar_blend")) {
    const json& b = j["triplanar_blend"];
    if (b.is_string()) triplanarBlend = b.get<std::string>() == "smooth" ? 0 : 1;
    else if (b.is_number()) triplanarBlend = b.get<int>() ? 1 : 0;
  }
  tint = v3(j.value("tint", json()), tint);
  roughnessScale = j.value("roughness_scale", roughnessScale);
  roughnessOffset = j.value("roughness_offset", roughnessOffset);
  metallicOverride = j.value("metallic_override", metallicOverride);
  heightScale = j.value("height_scale", heightScale);
  heightOffset = j.value("height_offset", heightOffset);
  normalStrength = j.value("normal_strength", normalStrength);
  emissiveStrength = j.value("emissive_strength", emissiveStrength);
}

// ------------------------------------------------------------------ loading

void MaterialLibrary::loadAll() {
  entries_.clear();
  loadDir((paths::dataDir() / "materials").string(), true);
  // built-in library in a fixed category order (files load alphabetically)
  static const char* kOrder[] = {"Metal", "Paint", "Stone", "Ground", "Wood", "Fabric", "Organic",
                                 "Character", "Stylized", "Plastic", "Sci-Fi", "Emissive", "Basic"};
  auto rank = [](const std::string& c) {
    for (int i = 0; i < (int)(sizeof kOrder / sizeof *kOrder); ++i)
      if (c == kOrder[i]) return i;
    return 100;
  };
  std::stable_sort(entries_.begin(), entries_.end(),
                   [&](const auto& a, const auto& b) { return rank(a->def.category) < rank(b->def.category); });
  loadDir((paths::userDataDir() / "materials").string(), false);
  PLOG_I("Material library: %zu materials", entries_.size());
}

void MaterialLibrary::loadDir(const std::string& dir, bool builtin) {
  std::error_code ec;
  if (!fs::exists(dir, ec)) return;
  std::vector<fs::path> files;
  for (auto& e : fs::recursive_directory_iterator(dir, ec))
    if (e.is_regular_file() && e.path().extension() == ".json") files.push_back(e.path());
  std::sort(files.begin(), files.end());
  for (auto& f : files) {
    bool ok = false;
    std::string text = paths::readFile(f, &ok);
    if (!ok) continue;
    json j;
    try {
      j = json::parse(text);
    } catch (std::exception& e) {
      PLOG_W("Material %s: %s", f.string().c_str(), e.what());
      continue;
    }
    // a file may hold one material or {"materials": [...]}
    std::vector<json> list;
    if (j.contains("materials")) list = j["materials"].get<std::vector<json>>();
    else list.push_back(j);
    for (auto& mj : list) {
      MaterialDef d;
      std::string err;
      if (!MaterialDef::fromJson(mj, d, f.parent_path().string(), &err)) {
        PLOG_W("Material in %s: %s", f.string().c_str(), err.c_str());
        continue;
      }
      d.builtin = builtin;
      d.sourcePath = f.string();
      if (auto* existing = find(d.id)) {
        existing->def = d;  // user overrides builtin with the same id
        existing->gpu = MaterialGpu();
        continue;
      }
      auto e = std::make_unique<Entry>();
      e->def = std::move(d);
      entries_.push_back(std::move(e));
    }
  }
}

// (0.36) the built-in ids that had British spellings: projects, scripts and smart materials made before still work
const std::string& MaterialLibrary::currentId(const std::string& id) {
  static const std::map<std::string, std::string> renamed = {
      {"patina_flat_grey", "patina_flat_gray"},           {"patina_plastic_satin_grey", "patina_plastic_satin_gray"},  // the old id
      {"patina_wool_grey", "patina_wool_gray"},           {"patina_knit_seed_grey", "patina_knit_seed_gray"},  // the old id
      {"patina_windowpane_grey", "patina_windowpane_gray"}, {"patina_knurled_aluminium", "patina_knurled_aluminum"},  // the old id
      {"patina_scale_armour", "patina_scale_armor"},      {"patina_honeycomb_armour", "patina_honeycomb_armor"}  // the old id
  };
  auto it = renamed.find(id);
  return it == renamed.end() ? id : it->second;
}

MaterialLibrary::Entry* MaterialLibrary::find(const std::string& idIn) {
  const std::string& id = currentId(idIn);
  for (auto& e : entries_)
    if (e->def.id == id) return e.get();
  // projects saved before the built-in library was renamed use the unprefixed ids
  if (!id.empty() && id.rfind("patina_", 0) != 0) {
    std::string alias = "patina_" + id;
    for (auto& e : entries_)
      if (e->def.id == alias && e->def.builtin) return e.get();
  }
  return nullptr;
}

std::vector<std::string> MaterialLibrary::categories() const {
  std::vector<std::string> c;
  for (auto& e : entries_)
    if (std::find(c.begin(), c.end(), e->def.category) == c.end()) c.push_back(e->def.category);
  return c;
}

static std::string lower(std::string s) {
  std::transform(s.begin(), s.end(), s.begin(), ::tolower);
  return s;
}

MaterialLibrary::Entry* MaterialLibrary::importImageFile(const std::string& file, std::string* err) {
  std::error_code ec;
  if (!fs::is_regular_file(file, ec)) {
    if (err) *err = "Not a file: " + file;
    return nullptr;
  }
  MaterialDef d;
  d.type = MaterialType::Image;
  d.name = fs::path(file).stem().string();
  d.id = "img_" + paths::sanitizeFileName(lower(d.name));
  d.category = "Imported";
  d.description = "Image";
  fs::path dst = paths::ensureDir(paths::userDataDir() / "materials" / d.id);
  fs::path to = dst / fs::path(file).filename();
  fs::copy_file(file, to, fs::copy_options::overwrite_existing, ec);
  d.maps["basecolor"] = (ec ? fs::path(file) : to).string();
  return saveUserMaterial(d, err);
}

MaterialLibrary::Entry* MaterialLibrary::saveUserMaterial(const MaterialDef& def, std::string* err) {
  fs::path dir = paths::ensureDir(paths::userDataDir() / "materials" / paths::sanitizeFileName(def.id));
  fs::path file = dir / "material.json";
  if (!paths::writeFileAtomic(file, def.toJson().dump(2))) {
    if (err) *err = "Could not write " + file.string();
    return nullptr;
  }
  Entry* e = find(def.id);
  if (!e) {
    entries_.push_back(std::make_unique<Entry>());
    e = entries_.back().get();
  }
  e->def = def;
  e->def.builtin = false;
  e->def.sourcePath = file.string();
  e->gpu = MaterialGpu();
  PLOG_I("Saved material '%s' to %s", def.name.c_str(), file.string().c_str());
  return e;
}

MaterialLibrary::Entry* MaterialLibrary::upsertTransient(const MaterialDef& def) {
  Entry* e = find(def.id);
  if (!e) {
    entries_.push_back(std::make_unique<Entry>());
    e = entries_.back().get();
  }
  e->def = def;
  e->transient = true;
  e->gpu = MaterialGpu();
  return e;
}

bool MaterialLibrary::removeUserMaterial(const std::string& id) {
  for (size_t i = 0; i < entries_.size(); ++i) {
    auto& e = entries_[i];
    if (e->def.id != id || e->def.builtin) continue;
    std::error_code ec;
    fs::remove(e->def.sourcePath, ec);
    entries_.erase(entries_.begin() + i);
    return true;
  }
  return false;
}

// ------------------------------------------------------------------ GPU

void MaterialLibrary::ensureDefaults() {
  if (white_.valid()) return;
  white_.create(4, 4, GL_RGBA8, 1);
  white_.clear(vec4(1));
  flatNormal_.create(4, 4, GL_RGBA8, 1);
  flatNormal_.clear(vec4(0.5f, 0.5f, 1.f, 1.f));
  black_.create(4, 4, GL_RGBA8, 1);
  black_.clear(vec4(0, 0, 0, 1));
  extraDefault_.create(4, 4, GL_RGBA8, 1);
  extraDefault_.clear(vec4(1, 0, 0, 0));
}

static void setupTiling(Texture2D& t) {
  t.setWrap(GL_REPEAT, GL_REPEAT);
  t.setFilter(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
  if (GLAD_GL_EXT_texture_filter_anisotropic) glTextureParameterf(t.id(), GL_TEXTURE_MAX_ANISOTROPY_EXT, 8.f);
  t.generateMips();
}

static Image resizeTo(const Image& src, int size) {
  if (src.width == size && src.height == size) return src;
  Image dst;
  dst.resize(size, size, src.channels);
  stbir_resize_float_linear(src.pixels.data(), src.width, src.height, 0, dst.pixels.data(), size, size, 0,
                            (stbir_pixel_layout)src.channels);
  return dst;
}

bool MaterialLibrary::ensureBaked(Entry& e, int size) {
  // thumbnails use small bakes; painting asks for full resolution and upgrades them
  if (e.gpu.baked && e.gpu.bakedSize >= size) return true;
  Texture2D thumb = std::move(e.gpu.thumbnail);
  e.gpu = MaterialGpu();
  e.gpu.thumbnail = std::move(thumb);
  ensureDefaults();
  MaterialDef& d = e.def;
  auto& g = e.gpu;
  if (d.type == MaterialType::Flat) {
    g.color.create(4, 4, GL_RGBA8, 1);
    g.color.clear(vec4(d.flat.color, 1));
    g.rmh.create(4, 4, GL_RGBA8, 1);
    g.rmh.clear(vec4(d.flat.roughness, d.flat.metallic, d.flat.height, 1));
    g.normal.create(4, 4, GL_RGBA8, 1);
    g.normal.clear(vec4(0.5f, 0.5f, 1, 1));
    g.emissive.create(4, 4, GL_RGBA8, 1);
    g.emissive.clear(vec4(d.flat.emissive, 1));
    g.extra.create(4, 4, GL_RGBA8, 1);
    g.extra.clear(d.flat.extra());
    for (Texture2D* t : {&g.color, &g.rmh, &g.normal, &g.emissive, &g.extra}) t->setWrap(GL_REPEAT, GL_REPEAT);
  } else if (d.type == MaterialType::Procedural) {
    g.color.create(size, size, GL_RGBA8, 0);
    g.rmh.create(size, size, GL_RGBA8, 0);
    g.normal.create(size, size, GL_RGBA8, 0);
    g.emissive.create(size, size, GL_RGBA8, 0);
    g.extra.create(size, size, GL_RGBA8, 0);
    Framebuffer fbo;
    fbo.attach(0, g.color);
    fbo.attach(1, g.rmh);
    fbo.attach(2, g.emissive);
    fbo.attach(3, g.extra);
    fbo.setDrawBuffers(4);
    {
      gl::FramebufferScope fs(fbo.id());
      gl::ViewportScope vp(0, 0, size, size);
      glDisable(GL_DEPTH_TEST);
      glDisable(GL_BLEND);
      Shader& s = ShaderCache::get("fullscreen.vs.glsl", "material_bake.fs.glsl", {"BAKE_SURFACE"});
      if (!s.valid()) return false;
      const ProcParams& p = d.proc;
      s.set("uGen", p.generator);
      s.set("uColA", p.colorA);
      s.set("uColB", p.colorB);
      s.set("uColC", p.colorC);
      s.set("uRough", p.roughness);
      s.set("uMetal", p.metallic);
      s.set("uScale", p.scale);
      s.set("uDetail", p.detail);
      s.set("uSeed", p.seed);
      s.set("uHeightAmp", p.heightAmp);
      s.set("uVariation", p.variation);
      s.set("uExtra", p.extra);
      s.set("uEmissive", p.emissive);
      s.set("uWarp", vec2(p.warp, p.warpScale));
      s.set("uSurface", vec4(p.opacity, p.sheen, p.coat, p.subsurface));
      s.set("uCutout", p.cutout && generatorHasHoles(p.generator) ? 1 : 0);
      s.use();
      drawFullscreenTriangle();
    }
    if (d.proc.grime > 0.f || d.proc.wear > 0.f) {
      // grime and worn edges: from the height against its blurred self (the low parts, the raised edges)
      g.rmh.setWrap(GL_REPEAT, GL_REPEAT);
      g.rmh.setFilter(GL_LINEAR_MIPMAP_LINEAR, GL_LINEAR);
      g.rmh.generateMips();
      Texture2D col2, rmh2;
      col2.create(size, size, GL_RGBA8, 0);
      rmh2.create(size, size, GL_RGBA8, 0);
      Framebuffer ffbo;
      ffbo.attach(0, col2);
      ffbo.attach(1, rmh2);
      ffbo.setDrawBuffers(2);
      gl::FramebufferScope fs(ffbo.id());
      gl::ViewportScope vp(0, 0, size, size);
      Shader& s = ShaderCache::get("fullscreen.vs.glsl", "material_bake.fs.glsl", {"BAKE_FINISH"});
      if (s.valid()) {
        const ProcParams& p = d.proc;
        s.tex("uColorIn", 0, g.color.id());
        s.tex("uRMHIn", 1, g.rmh.id());
        s.set("uGrime", vec4(p.grimeColor, p.grime));
        s.set("uWear", vec4(p.wearColor, p.wear));
        s.set("uHeightAmp", p.heightAmp);
        s.set("uScale", p.scale);
        s.set("uSeed", p.seed);
        s.use();
        drawFullscreenTriangle();
        g.color = std::move(col2);
        g.rmh = std::move(rmh2);
      }
    }
    Framebuffer nfbo;
    nfbo.attach(0, g.normal);
    nfbo.setDrawBuffers(1);
    {
      gl::FramebufferScope fs(nfbo.id());
      gl::ViewportScope vp(0, 0, size, size);
      Shader& s = ShaderCache::get("fullscreen.vs.glsl", "material_bake.fs.glsl", {"BAKE_NORMAL"});
      s.tex("uRMH", 0, g.rmh.id());
      s.set("uNormalStrength", d.proc.normalStrength * 4.f);
      s.use();
      drawFullscreenTriangle();
    }
    for (Texture2D* t : {&g.color, &g.rmh, &g.normal, &g.emissive, &g.extra}) setupTiling(*t);
  } else {
    // image material: load maps, resize to a common size, pack rough/metal/height
    std::map<std::string, Image> imgs;
    int sz = 0;
    for (auto& [k, path] : d.maps) {
      Image img;
      if (!loadImage(path, img, k == "basecolor" || k == "emissive" || k == "normal" ? 3 : 1)) continue;
      std::string ext = fs::path(path).extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      bool floatFile = ext == ".exr" || ext == ".hdr";
      if (floatFile && (k == "basecolor" || k == "emissive")) {
        // float images hold linear color; the material textures are sRGB
        for (float& v : img.pixels) {
          v = std::clamp(v, 0.f, 1.f);
          v = v <= 0.0031308f ? v * 12.92f : 1.055f * std::pow(v, 1.f / 2.4f) - 0.055f;
        }
      }
      if (k == "height") {
        // displacement in scene units (EXR, 32-bit TIFF exports): fit into 0..1
        float lo = 1e30f, hi = -1e30f;
        for (float v : img.pixels) lo = std::min(lo, v), hi = std::max(hi, v);
        if (lo < -1e-4f || hi > 1.0001f) {
          float span = std::max(hi - lo, 1e-8f);
          for (float& v : img.pixels) v = (v - lo) / span;
        }
      }
      if (floatFile && k != "basecolor" && k != "emissive" && k != "height" && k != "normal")
        for (float& v : img.pixels) v = std::clamp(v, 0.f, 1.f);
      if (k == "normal" && floatFile) {
        // float normal maps may be stored signed (-1..1)
        float lo = 1e30f;
        for (float v : img.pixels) lo = std::min(lo, v);
        if (lo < -0.01f)
          for (float& v : img.pixels) v = std::clamp(v * 0.5f + 0.5f, 0.f, 1.f);
      }
      sz = std::max({sz, img.width, img.height});
      imgs[k] = std::move(img);
    }
    sz = std::clamp(sz, 4, 2048);
    int p2 = 4;
    while (p2 < sz) p2 <<= 1;
    sz = p2;
    auto upload = [&](Texture2D& t, const Image& img) {
      t.create(sz, sz, GL_RGBA8, 0);
      Image r = resizeTo(img, sz);
      Image rgba;
      rgba.resize(sz, sz, 4);
      for (size_t i = 0; i < size_t(sz) * sz; ++i) {
        for (int c = 0; c < 3; ++c) rgba.pixels[i * 4 + c] = r.pixels[i * r.channels + std::min(c, r.channels - 1)];
        rgba.pixels[i * 4 + 3] = 1;
      }
      t.upload(rgba);
      setupTiling(t);
    };
    if (imgs.count("basecolor")) {
      g.aspect = float(imgs["basecolor"].width) / float(std::max(1, imgs["basecolor"].height));
      upload(g.color, imgs["basecolor"]);
    }
    else {
      g.color.create(4, 4, GL_RGBA8, 1);
      g.color.clear(vec4(0.7f, 0.7f, 0.7f, 1));
    }
    if (imgs.count("normal")) {
      Image& n = imgs["normal"];
      if (d.normalDirectX)
        for (size_t i = 0; i < size_t(n.width) * n.height; ++i) n.pixels[i * 3 + 1] = 1.f - n.pixels[i * 3 + 1];
      upload(g.normal, n);
    } else {
      g.normal.create(4, 4, GL_RGBA8, 1);
      g.normal.clear(vec4(0.5f, 0.5f, 1, 1));
    }
    if (imgs.count("emissive")) upload(g.emissive, imgs["emissive"]);
    else {
      g.emissive.create(4, 4, GL_RGBA8, 1);
      g.emissive.clear(vec4(0, 0, 0, 1));
    }
    // pack R rough, G metal, B height
    Image rmh;
    rmh.resize(sz, sz, 4);
    Image rough = imgs.count("roughness") ? resizeTo(imgs["roughness"], sz) : Image();
    Image gloss = !rough.empty() || !imgs.count("glossiness") ? Image() : resizeTo(imgs["glossiness"], sz);
    Image metal = imgs.count("metallic") ? resizeTo(imgs["metallic"], sz) : Image();
    Image height = imgs.count("height") ? resizeTo(imgs["height"], sz) : Image();
    for (size_t i = 0; i < size_t(sz) * sz; ++i) {
      rmh.pixels[i * 4 + 0] = !rough.empty() ? rough.pixels[i] : !gloss.empty() ? 1.f - gloss.pixels[i] : 0.6f;
      rmh.pixels[i * 4 + 1] = !metal.empty() ? metal.pixels[i] : 0.f;
      rmh.pixels[i * 4 + 2] = !height.empty() ? height.pixels[i] : 0.5f;
      rmh.pixels[i * 4 + 3] = 1.f;
    }
    g.rmh.create(sz, sz, GL_RGBA8, 0);
    g.rmh.upload(rmh);
    setupTiling(g.rmh);
    // r opacity (an opacity map, else opaque); no sheen, clearcoat or subsurface
    Image extra;
    extra.resize(sz, sz, 4);
    Image op = imgs.count("opacity") ? resizeTo(imgs["opacity"], sz) : Image();
    for (size_t i = 0; i < size_t(sz) * sz; ++i) {
      extra.pixels[i * 4 + 0] = !op.empty() ? std::clamp(op.pixels[i], 0.f, 1.f) : 1.f;
      extra.pixels[i * 4 + 1] = extra.pixels[i * 4 + 2] = extra.pixels[i * 4 + 3] = 0.f;
    }
    g.extra.create(sz, sz, GL_RGBA8, 0);
    g.extra.upload(extra);
    setupTiling(g.extra);
    for (Texture2D* t : {&g.color, &g.normal, &g.emissive}) t->setWrap(GL_REPEAT, GL_REPEAT);
  }
  g.baked = true;
  g.bakedSize = d.type == MaterialType::Procedural ? size : (1 << 30);  // images/flat: always full quality
  return true;
}

void MaterialLibrary::rebake(Entry& e) {
  e.gpu = MaterialGpu();
}

GLuint MaterialLibrary::thumbnail(Entry& e, const Environment& env) {
  if (e.gpu.thumbnail.valid()) return e.gpu.thumbnail.id();
  if (!ensureBaked(e, 256)) return 0;
  const int S = 128;
  e.gpu.thumbnail.create(S, S, GL_RGBA8, 1);
  Framebuffer fbo;
  fbo.attach(0, e.gpu.thumbnail);
  fbo.setDrawBuffers(1);
  gl::FramebufferScope fs(fbo.id());
  gl::ViewportScope vp(0, 0, S, S);
  glDisable(GL_DEPTH_TEST);
  glDisable(GL_BLEND);
  glClearColor(0, 0, 0, 0);
  glClear(GL_COLOR_BUFFER_BIT);
  Shader& s = ShaderCache::get("fullscreen.vs.glsl", "thumb.fs.glsl");
  s.tex("uColorTex", 0, e.gpu.color.id());
  s.tex("uRMHTex", 1, e.gpu.rmh.id());
  s.tex("uNormalTex", 2, e.gpu.normal.id());
  s.tex("uEmissiveTex", 3, e.gpu.emissive.id());
  s.tex("uExtraTex", 6, e.gpu.extra.valid() ? e.gpu.extra.id() : extraDefault_.id());
  env.bind(s, 4, 5);
  s.set("uTiles", e.def.type == MaterialType::Flat ? 1.f : 2.f);
  s.set("uShape", 0);
  s.use();
  drawFullscreenTriangle();
  return e.gpu.thumbnail.id();
}

void MaterialLibrary::bind(const Shader& s, const MaterialRef& ref, int u, const mat4& screenVP, float screenAspect) {
  ensureDefaults();
  Entry* e = ref.id.empty() ? nullptr : find(ref.id);
  bool hasTex = e && e->def.type != MaterialType::Flat && ensureBaked(*e, bakeSize);
  FlatValues flat = ref.flat;
  if (e && e->def.type == MaterialType::Flat) flat = e->def.flat;
  s.set("uMatHasTex", hasTex ? 1 : 0);
  s.set("uMatMapping", ref.mapping);
  {
    int grid = atlas && atlas->active() ? atlas->grid : 1;
    s.set("uAtlasGrid", grid);
    if (grid > 1) {
      vec2 o[kMaxTiles];
      for (int t = 0; t < kMaxTiles; ++t) o[t] = vec2(atlas->uvOrigin(t));
      GLint loc = s.loc("uAtlasOrigin[0]");
      if (loc >= 0) glProgramUniform2fv(s.id(), loc, kMaxTiles, &o[0].x);
    }
  }
  float scale = ref.scale * (e ? e->def.defaultScale : 1.f);
  s.set("uMatScale", scale);
  s.set("uMatRotation", glm::radians(ref.rotationDeg));
  s.set("uMatOffset", ref.offset);
  s.set("uMatTriSharpness", ref.triplanarSharpness);
  s.set("uMatTriBlend", ref.triplanarBlend);
  s.set("uMatTint", ref.tint);
  s.set("uMatFlatColor", vec4(flat.color, 1));
  s.set("uMatFlat", vec4(flat.roughness, flat.metallic, flat.height, 0));
  s.set("uMatFlatEmissive", flat.emissive);
  s.set("uMatFlatExtra", flat.extra());
  s.set("uMatEmissiveStrength", ref.emissiveStrength);
  s.set("uMatRoughRemap", vec2(ref.roughnessScale, ref.roughnessOffset));
  s.set("uMatMetalOverride", ref.metallicOverride);
  s.set("uMatHeightScale", ref.heightScale);
  s.set("uMatHeightOffset", ref.heightOffset);
  s.set("uMatNormalStrength", ref.normalStrength);
  s.set("uMatScreenVP", screenVP);
  s.set("uMatScreenAspect", screenAspect);
  s.set("uMatImageAspect", hasTex ? e->gpu.aspect : 1.f);
  s.tex("uMatColorTex", u + 0, hasTex ? e->gpu.color.id() : white_.id());
  s.tex("uMatRMHTex", u + 1, hasTex ? e->gpu.rmh.id() : white_.id());
  s.tex("uMatNormalTex", u + 2, hasTex ? e->gpu.normal.id() : flatNormal_.id());
  s.tex("uMatEmissiveTex", u + 3, hasTex ? e->gpu.emissive.id() : black_.id());
  s.tex("uMatExtraTex", u + 4, hasTex && e->gpu.extra.valid() ? e->gpu.extra.id() : extraDefault_.id());
}

int MaterialLibrary::extraChannelsUsed(const MaterialRef& ref) {
  Entry* e = ref.id.empty() ? nullptr : find(ref.id);
  auto fromFlat = [](const FlatValues& f) {
    return (f.opacity < 0.999f ? 64 : 0) | (f.sheen > 0.001f ? 128 : 0) | (f.coat > 0.001f ? 256 : 0) |
           (f.subsurface > 0.001f ? 512 : 0);
  };
  if (!e) return fromFlat(ref.flat);
  const MaterialDef& d = e->def;
  if (d.type == MaterialType::Flat) return fromFlat(d.flat);
  if (d.type == MaterialType::Image) return d.maps.count("opacity") ? 64 : 0;
  const ProcParams& p = d.proc;
  return (p.opacity < 0.999f || (p.cutout && generatorHasHoles(p.generator)) ? 64 : 0) | (p.sheen > 0.001f ? 128 : 0) |
         (p.coat > 0.001f ? 256 : 0) | (p.subsurface > 0.001f ? 512 : 0);
}

FlatValues MaterialLibrary::averageValues(Entry& e) {
  if (e.def.type == MaterialType::Flat) return e.def.flat;
  FlatValues f;
  if (!ensureBaked(e, 256)) return f;
  auto avg = [](Texture2D& t) {
    int lvl = std::max(0, t.levels() - 1);
    Image i = t.download(4, lvl);
    vec4 s(0);
    for (int k = 0; k < i.width * i.height; ++k) s += vec4(i.pixels[k * 4], i.pixels[k * 4 + 1], i.pixels[k * 4 + 2], 1);
    return s / std::max(s.w, 1.f);
  };
  vec4 c = avg(e.gpu.color), r = avg(e.gpu.rmh), em = avg(e.gpu.emissive);
  if (e.gpu.extra.valid()) {
    int lvl = std::max(0, e.gpu.extra.levels() - 1);
    Image i = e.gpu.extra.download(4, lvl);
    vec4 x(0);
    for (int k = 0; k < i.width * i.height; ++k) x += vec4(i.pixels[k * 4], i.pixels[k * 4 + 1], i.pixels[k * 4 + 2], i.pixels[k * 4 + 3]);
    x /= float(std::max(1, i.width * i.height));
    f.opacity = x.x, f.sheen = x.y, f.coat = x.z, f.subsurface = x.w;
  }
  f.color = vec3(c);
  f.roughness = r.x;
  f.metallic = r.y;
  f.height = r.z;
  f.emissive = vec3(em);
  return f;
}

}  // namespace patina
