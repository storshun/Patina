// (0.38) The view pie, after Blender's Z pie: hold Z over the 3D view, point toward an item and let go (or tap Z,
// then click one). (0.40) Four pies, eight items each round the pointer:
// - Z the view, laid out as Blender's shading pie: Wireframe left, Solid right, Lit down, Path traced up;
// - Shift+Z the channels (base color, normal, roughness...);
// - ` (the key under Esc) the edit modes: the tools;
// - Q Quick Favorites, as Blender's Q menu: eight items of the user's own. A right-click on a toolbar button or on a
//   pie item adds it; an empty place opens a picker.
#include <imgui.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

#include "core/Log.h"
#include "ui/App.h"
#include "ui/Theme.h"

namespace patina {

using PieItem = App::PieItem;
using PieKind = App::PieKind;

namespace {

// the slots, in Blender's order: left, right, down, up, up-left, up-right, down-left, down-right
const float kSlotAngle[8] = {kPi, 0.f, kPi * 0.5f, -kPi * 0.5f, -kPi * 0.75f, -kPi * 0.25f, kPi * 0.75f, kPi * 0.25f};

// the fixed pies' items, in slot order (the Quick Favorites are App::favorites)
const char* const kPieIds[3][8] = {
    {"view.wireframe", "view.solid", "view.lit", "view.path_traced", "view.unlit", "view.seams", "mode.uv_checker",
     "view.retopo_preview"},
    {"mode.basecolor", "mode.normal", "mode.roughness", "mode.metallic", "mode.height", "mode.emissive", "mode.ao",
     "mode.mask"},
    {"tool.paint", "tool.select", "tool.island", "tool.seams", "tool.erase", "tool.fill", "tool.zone_line", "tool.flow_loops"}};
// the 0.38 view pie (the pie_menus feature switched off)
const char* const kPie038[8] = {"view.wireframe", "view.solid", "view.unlit", "view.seams",
                                "tool.paint", "tool.island", "select.none", "tool.retopo_toggle"};
const char* const kPieNames[] = {N_("View"), N_("Channels"), N_("Edit mode"), N_("Quick Favorites")};
const char* const kPieKeys[] = {"view.pie", "view.pie_channels", "tool.pie", "app.favorites"};
const char* const kPieMcp[] = {"view", "channels", "edit", "favorites"};

// the shortcut actions as pie items: a short name and an icon (the others: the action's name up to " (", a star)
struct ActionLook {
  const char* id;
  const char* name;
  const char* icon;
};
const ActionLook kLooks[] = {
    {"app.palette", N_("Command palette"), "more"},
    {"file.export", N_("Export textures"), nullptr},
    {"file.export_again", N_("Export again"), nullptr},
    {"file.save_incremental", N_("Save incremental"), nullptr},
    {"file.render", N_("Render image"), "render"},
    {"edit.undo", N_("Undo"), nullptr},
    {"edit.redo", N_("Redo"), nullptr},
    {"layer.add_menu", N_("Add menu"), "add_paint"},
    {"layer.new_paint", N_("New paint layer"), "add_paint"},
    {"layer.group", N_("Group into a folder"), "add_folder"},
    {"layer.duplicate", N_("Duplicate layer"), "duplicate"},
    {"layer.instance", N_("Make an instance"), "duplicate"},
    {"layer.delete", N_("Delete layer"), "delete"},
    {"layer.solo", N_("Solo the layer"), "view"},
    {"island.make", N_("Make island"), "make_island"},
    {"select.all", N_("Select all"), "sel_face"},
    {"select.none", N_("Clear selection"), "deselect"},
    {"select.grow", N_("Grow selection"), "sel_face"},
    {"select.shrink", N_("Shrink selection"), "sel_face"},
    {"uv.unpin", N_("Unpin islands"), "unpin"},
    {"color.swap", N_("Swap colors"), "swap_colors"},
    {"brush.smaller", N_("Brush smaller"), "size"},
    {"brush.bigger", N_("Brush bigger"), "size"},
    {"brush.radius", N_("Brush size"), "size"},
    {"brush.strength", N_("Brush opacity"), "opacity"},
    {"brush.flow", N_("Brush flow"), "flow_rate"},
    {"brush.hardness", N_("Brush hardness"), "hardness"},
    {"brush.angle", N_("Brush angle"), "brush_angle"},
    {"sym.x", N_("X mirror"), "mirror_x"},
    {"brush.lock_topology", N_("Topology lock"), "lock_topology"},
    {"brush.lock_island", N_("Island lock"), "lock_island"},
    {"brush.lock_set", N_("Set lock"), "lock_set"},
    {"view.isolate_set", N_("Isolate the texture set"), "lock_set"},
    {"view.back", N_("Back to your view"), "view_back"},
    {"macro.record", N_("Record a macro"), nullptr},
    {"macro.play", N_("Play the macro"), nullptr},
    {"stencil.toggle", N_("Stencil on / off"), "mask"},
    {"view.focus", N_("Hide panels"), "view"},
    {"view.frame", N_("Frame mesh"), "frame"},
    {"view.help", N_("Shortcuts"), nullptr},
    {"view.compare", N_("Compare view"), "view"},
    {"view.compare_snapshot", N_("Compare snapshot"), "view"},
    {"blender.send", N_("Send to Blender"), nullptr},
};
// not offered as pie items: under-the-pointer and held keys, the pies themselves, quitting, the second keys
const char* const kNotInPies[] = {"color.sample", "stencil.edit", "view.pie", "view.pie_channels", "tool.pie",
                                  "app.favorites", "file.quit", "edit.redo_alt", "view.frame_alt"};

const char* modeIcon(ViewMode m) {
  switch (m) {
    case ViewMode::BaseColor: return "channel_base";
    case ViewMode::Normal: case ViewMode::WorldNormal: return "channel_normal";
    case ViewMode::Roughness: return "channel_rough";
    case ViewMode::Metallic: return "channel_metal";
    case ViewMode::Height: return "channel_height";
    case ViewMode::Emissive: return "channel_emit";
    case ViewMode::AO: case ViewMode::BentNormal: return "channel_ao";
    case ViewMode::UVChecker: return "uv_checker";
    case ViewMode::LayerMask: return "mask";
    case ViewMode::UVIslands: case ViewMode::UVStretch: case ViewMode::TexelDensity: return "unwrap";
    default: return "view";
  }
}

struct PieLayout {
  float radius, dead, h, pad, isz;
  ImVec2 box[8][2];
};

PieLayout pieLayout(ImVec2 c, const std::vector<PieItem>& items, float s) {
  PieLayout L;
  L.radius = std::round(112.f * s);
  L.dead = std::round(20.f * s);
  L.h = std::round(ImGui::GetFrameHeight() * 1.3f);
  L.pad = std::round(8.f * s);
  L.isz = std::round(L.h * 0.68f);
  for (int i = 0; i < 8; ++i) {
    float w = L.pad * 3.f + L.isz + ImGui::CalcTextSize(items[i].label.c_str()).x;
    float ca = std::cos(kSlotAngle[i]), sa = std::sin(kSlotAngle[i]);
    // pushed out by half its width on the sides, so long names don't cover the middle
    ImVec2 at(c.x + ca * L.radius + (std::abs(ca) > 0.3f ? ca * w * 0.5f : 0.f), c.y + sa * L.radius * 0.82f);
    L.box[i][0] = ImVec2(std::round(at.x - w * 0.5f), std::round(at.y - L.h * 0.5f));
    L.box[i][1] = ImVec2(L.box[i][0].x + std::round(w), L.box[i][0].y + L.h);
  }
  return L;
}

const PieItem* findItem(const std::vector<PieItem>& v, const std::string& id) {
  for (auto& it : v)
    if (it.id == id) return &it;
  return nullptr;
}

}  // namespace

std::vector<std::string> App::defaultFavorites() {
  return {"select.none", "island.make", "sym.x", "color.swap", "view.back", "file.render", "", ""};
}

void App::toggleViewMode(ViewMode m) {
  ViewportOptions& vo = engine.viewOptions;
  if (vo.mode == m) {
    vo.mode = pie.modeBefore != m ? pie.modeBefore : ViewMode::Material;
  } else {
    pie.modeBefore = vo.mode;
    vo.mode = m;
  }
}

std::vector<PieItem> App::pieCatalog() {
  Engine& e = engine;
  ViewportOptions& vo = e.viewOptions;
  Document& d = e.doc;
  Painter& P = e.painter;
  std::vector<PieItem> v;
  v.reserve(160);
  auto add = [&](const char* id, const char* group, const char* icon, const char* label, const char* desc, bool on,
                 bool enabled, std::function<void()> run) {
    v.push_back({id, icon, T_(label), T_(desc), group, on, enabled, std::move(run)});
  };
  const bool retopo = e.features.on("retopology") && d.hasMesh();
  // ---- how the model shows
  add("view.lit", N_("View"), "view_lit", N_("Lit"), N_("The material, lit by the environment: the usual view"),
      vo.mode == ViewMode::Material, true, [this] {
        if (engine.viewOptions.mode != ViewMode::Material) pie.modeBefore = engine.viewOptions.mode;
        engine.viewOptions.mode = ViewMode::Material;
      });
  add("view.solid", N_("View"), "view_solid", N_("Solid"), N_("The shape alone: light gray, lit, no textures"),
      vo.mode == ViewMode::Solid, viewModeAvailable(ViewMode::Solid), [this] { toggleViewMode(ViewMode::Solid); });
  add("view.unlit", N_("View"), "view_unlit", N_("Unlit"), N_("The colors without the light (the base color); again: the view before"),
      vo.mode == ViewMode::BaseColor, true, [this] { toggleViewMode(ViewMode::BaseColor); });
  add("view.path_traced", N_("View"), "path_trace", N_("Path traced"),
      N_("Real shadows, reflections and bounced light; cleaner the longer nothing changes"), vo.mode == ViewMode::PathTraced,
      viewModeAvailable(ViewMode::PathTraced), [this] { toggleViewMode(ViewMode::PathTraced); });
  add("view.wireframe", N_("View"), "wire", N_("Wireframe"), N_("The mesh's edges over the model"), vo.wireframe, true,
      [&vo] { vo.wireframe = !vo.wireframe; });
  add("view.seams", N_("View"), "seams_show", N_("Seams"), N_("The UV seams in red"), vo.seams, true, [&vo] { vo.seams = !vo.seams; });
  add("view.retopo_preview", N_("View"), "retopo_view", N_("Retopo preview"),
      N_("The retopology's zones and quads over the model, whatever tool is chosen"), vo.retopoPreview,
      retopo && e.features.on("retopo_preview_toggle"), [&vo] { vo.retopoPreview = !vo.retopoPreview; });
  // every other view mode (the channels, the bakes, the checks): chosen again, the view before
  for (int m = 0; m < (int)ViewMode::Count; ++m) {
    ViewMode vm = (ViewMode)m;
    if (vm == ViewMode::Material || vm == ViewMode::Solid || vm == ViewMode::PathTraced) continue;  // (above)
    std::string id = std::string("mode.") + viewModeName(vm);
    v.push_back({id, modeIcon(vm), T_(viewModeLabel(m)), F_("Show %s (again: the view before)", T_(viewModeLabel(m))), N_("View mode"),
                 vo.mode == vm, viewModeAvailable(vm), [this, vm] { toggleViewMode(vm); }});
  }
  // ---- the edit modes: the tools
  struct ToolLook {
    const char* id;
    ViewportTool t;
    const char* icon;
    const char* label;
    const char* desc;
  };
  static const ToolLook kToolLooks[] = {
      {"tool.paint", ViewportTool::Paint, "paint", N_("Paint"), N_("The Paint tool")},
      {"tool.erase", ViewportTool::Erase, "erase", N_("Erase"), N_("The Erase tool")},
      {"tool.blur", ViewportTool::Blur, "blur", N_("Blur"), N_("The Blur tool")},
      {"tool.smudge", ViewportTool::Smudge, "smudge", N_("Smudge"), N_("The Smudge tool")},
      {"tool.clone", ViewportTool::Clone, "clone", N_("Clone"), N_("The Clone tool (Ctrl+click sets the source)")},
      {"tool.fill", ViewportTool::Fill, "fill", N_("Fill"), N_("Polygon fill: a face, an island, a part...")},
      {"tool.picker", ViewportTool::Picker, "pick", N_("Picker"), N_("The color picker")},
      {"tool.select", ViewportTool::SelectFaces, "select", N_("Select"), N_("Select faces")},
      {"tool.seams", ViewportTool::Seams, "seams", N_("Seams"), N_("Mark and clear UV seams")},
      {"tool.island", ViewportTool::Island, "island", N_("Island"), N_("The Island brush: paint a part, then make it an island")},
      {"tool.note", ViewportTool::Note, "note", N_("Note"), N_("Pin a comment to the model")},
      {"tool.object", ViewportTool::Object, "move", N_("Object"), N_("The Object tool: pick, move, turn and scale the scene's objects")},
      {"tool.zone_line", ViewportTool::ZoneLine, "zone_lines", N_("Retopo: lines"), N_("Retopology: zone lines (the Lines tool)")},
      {"tool.patch", ViewportTool::Patch, "zone", N_("Retopo: zones"), N_("Retopology: choose a zone")},
      {"tool.flow", ViewportTool::Flow, "flow", N_("Retopo: flow"), N_("Retopology: flow strokes")},
      {"tool.density", ViewportTool::Density, "density", N_("Retopo: density"), N_("Retopology: the density brush")},
  };
  for (auto& t : kToolLooks) {
    const bool rt = t.t == ViewportTool::ZoneLine || t.t == ViewportTool::Patch || t.t == ViewportTool::Flow || t.t == ViewportTool::Density;
    const bool on = tool == t.t && !(t.t == ViewportTool::Flow && e.retopo().flowLoops);
    ViewportTool tt = t.t;
    const bool avail = t.t == ViewportTool::Object ? engine.features.on("scene_objects") : !rt || retopo;
    add(t.id, N_("Tools"), t.icon, t.label, t.desc, rt ? on : tool == t.t, avail, [this, tt] { selectTool(tt); });
  }
  add("tool.flow_loops", N_("Tools"), "flow", N_("Retopo: edge loops"), N_("Retopology: strokes that make edge loops (the Flow tool)"),
      tool == ViewportTool::Flow && e.retopo().flowLoops, retopo, [this] {
        auto& S = engine.retopo();
        if (!S.flowLoops) S.flowLoops = true, engine.retopoChanged();
        selectTool(ViewportTool::Flow);
      });
  {
    // (the 0.38 view pie's item: the Lines tool, chosen again the edge-loop strokes)
    const bool onLines = tool == ViewportTool::ZoneLine;
    add("tool.retopo_toggle", N_("Tools"), onLines ? "flow" : "zone_lines", onLines ? N_("Retopo: edge loops") : N_("Retopo: lines"),
        onLines ? N_("Retopology: strokes that make edge loops (the Flow tool); again: zone lines")
                : N_("Retopology: zone lines (the Lines tool); again: strokes that make edge loops"),
        onLines || tool == ViewportTool::Flow, retopo, [this, onLines] {
          if (onLines) {
            auto& S = engine.retopo();
            if (!S.flowLoops) S.flowLoops = true, engine.retopoChanged();
            selectTool(ViewportTool::Flow);
          } else {
            selectTool(ViewportTool::ZoneLine);
          }
        });
  }
  // ---- the selection (straight away, whatever tool is chosen)
  const bool anySel = !d.selectedFaces().empty();
  auto selectCmd = [this](const char* mode) {
    return [this, mode] {
      try {
        engine.execute("select_faces", {{"mode", mode}});
      } catch (std::exception&) {
      }
    };
  };
  add("select.none", N_("UV"), "deselect", N_("Clear selection"), N_("Select no faces"), false, anySel, selectCmd("clear"));
  add("select.all", N_("UV"), "sel_face", N_("Select all"), N_("Select every face"), false, d.hasMesh(), selectCmd("all"));
  add("island.make", N_("UV"), "make_island", N_("Make island"),
      N_("Unwrap the painted / selected faces as one upright UV island"), false, anySel, [this] { makeIslandFromSelection(); });
  // ---- every other shortcut action, as if its key was pressed
  for (auto& a : keymap) {
    std::string id = a.id;
    if (findItem(v, id) || std::find_if(std::begin(kNotInPies), std::end(kNotInPies), [&](const char* s) { return id == s; }) !=
                               std::end(kNotInPies))
      continue;
    if ((!e.features.on("macro_recorder") && id.rfind("macro.", 0) == 0) ||
        (!e.features.on("texture_set_tools") && (id == "brush.lock_set" || id == "view.isolate_set")) ||
        (!e.features.on("command_palette") && id == "app.palette") || (!e.features.on("viewport_pins") && id == "uv.unpin") ||
        (!e.features.on("view_snapping") && id == "view.back") ||
        (!e.features.on("brush_keys") && (id == "brush.flow" || id == "brush.hardness" || id == "brush.angle")))
      continue;
    const ActionLook* look = nullptr;
    for (auto& l : kLooks)
      if (id == l.id) look = &l;
    std::string label = look ? T_(look->name) : T_(a.label);
    if (!look) {
      size_t cut = label.find(" (");
      if (cut != std::string::npos) label.resize(cut);
    }
    bool on = id == "sym.x"                 ? P.symmetry[0]
            : id == "brush.lock_topology" ? P.lockTopology
            : id == "brush.lock_island"   ? P.lockIsland
            : id == "brush.lock_set"      ? P.lockSet
            : id == "view.isolate_set"    ? vo.isolateSet
            : id == "view.focus"          ? distractionFree
            : id == "view.compare"        ? e.compare.enabled
                                          : false;
    v.push_back({id, look && look->icon ? look->icon : "star", label, T_(a.label), a.group, on, true, [this, id] { pendingAction = id; }});
  }
  return v;
}

std::vector<PieItem> App::pieItems(PieKind kind) {
  std::vector<PieItem> all = pieCatalog(), v(8);
  for (int i = 0; i < 8; ++i) {
    std::string id = kind == PieKind::Favorites ? (i < (int)favorites.size() ? favorites[i] : std::string())
                   : kind == PieKind::View && !engine.features.on("pie_menus") ? kPie038[i]
                                                                                : kPieIds[(int)kind][i];
    if (const PieItem* it = findItem(all, id)) {
      v[i] = *it;
    } else if (kind == PieKind::Favorites) {
      // an empty place (or a favorite whose feature is switched off): choose what goes here
      v[i] = {"", "star_add", T_("Add a favorite"), T_("Choose what goes here (right-click any toolbar button to add it)"), "", false, true,
              [this, i] { pie.pickSlot = i, pie.openPicker = true; }};
    } else {
      v[i] = {id, "star", id, "", "", false, false, [] {}};
    }
  }
  return v;
}

bool App::runPieItem(const std::string& id) {
  std::vector<PieItem> all = pieCatalog();
  const PieItem* it = findItem(all, id);
  if (!it || !it->enabled) return false;
  it->run();
  return true;
}

bool App::addFavorite(const std::string& id) {
  if (std::find(favorites.begin(), favorites.end(), id) != favorites.end()) return true;
  favorites.resize(8);
  for (auto& f : favorites)
    if (f.empty()) {
      f = id;
      saveSession(true);
      return true;
    }
  return false;
}

void App::favoriteMenuItems(const std::string& id) {
  if (!engine.features.on("pie_menus") || id.empty()) return;
  std::vector<PieItem> all = pieCatalog();
  if (!findItem(all, id)) return;
  auto rectOf = [this](const char* name) {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects[name] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
  };
  auto at = std::find(favorites.begin(), favorites.end(), id);
  if (at != favorites.end()) {
    bool go = ImGui::MenuItem(L_("Remove from Quick Favorites"));
    rectOf("menu.favorite");
    if (go) at->clear(), saveSession(true);
    return;
  }
  const bool room = std::count(favorites.begin(), favorites.end(), std::string()) > 0 || favorites.size() < 8;
  const char* k = keyLabel("app.favorites");
  bool go = ImGui::MenuItem(room ? L_("Add to Quick Favorites") : L_("Quick Favorites Are Full (Remove One First)"), k, false, room);
  rectOf("menu.favorite");
  if (go && addFavorite(id)) setStatus(F_("Added to Quick Favorites (%s)", k ? k : "Q"));
}

void App::favoritePickerUI() {
  const int slot = std::clamp(pie.pickSlot, 0, 7);
  favorites.resize(8);
  ImGui::TextDisabled("%s", F_("Quick Favorites: place %d of 8", slot + 1).c_str());
  const float w = std::round(340.f * uiScale);
  if (pie.pickFocus) ImGui::SetKeyboardFocusHere(), pie.pickFocus = false;
  ImGui::SetNextItemWidth(w);
  ImGui::InputTextWithHint("##q", T_("Search: a tool, a view, a shortcut..."), pie.pickQuery, sizeof pie.pickQuery);
  const bool enter = ImGui::IsKeyPressed(ImGuiKey_Enter) || ImGui::IsKeyPressed(ImGuiKey_KeypadEnter);
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    ImGui::CloseCurrentPopup();
    return;
  }
  std::vector<PieItem> all = pieCatalog();
  std::vector<std::pair<int, int>> order;  // (score, index)
  for (int i = 0; i < (int)all.size(); ++i) {
    int sc = fuzzyScore(all[i].label + " " + T_(all[i].group), pie.pickQuery);
    if (sc >= 0) order.push_back({sc, i});
  }
  if (pie.pickQuery[0]) std::stable_sort(order.begin(), order.end(), [](auto& a, auto& b) { return a.first > b.first; });
  auto choose = [&](const PieItem& it) {
    favorites[slot] = it.id;
    saveSession(true);
    setStatus(F_("Quick Favorites: %s", it.label.c_str()));
    ImGui::CloseCurrentPopup();
  };
  if (enter && !order.empty()) {
    choose(all[order[0].second]);
    return;
  }
  ImGui::BeginChild("##list", ImVec2(w, std::round(300.f * uiScale)), ImGuiChildFlags_Borders);
  const float rh = std::round(ImGui::GetFrameHeight() * 1.05f), isz = std::round(rh * 0.72f);
  for (auto& [sc, i] : order) {
    const PieItem& it = all[i];
    ImGui::PushID(it.id.c_str());
    const bool here = favorites[slot] == it.id;
    bool picked = ImGui::Selectable("##row", here, 0, ImVec2(0, rh));
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["favpick." + it.id] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ui::drawIcon(dl, it.icon, ImVec2(a.x + 4.f * uiScale, std::round(a.y + (rh - isz) * 0.5f)), isz,
                 ui::iconColors(it.enabled ? ui::IconState::Normal : ui::IconState::Disabled));
    const float ty = std::round(a.y + (rh - ImGui::GetTextLineHeight()) * 0.5f);
    dl->AddText(ImVec2(a.x + isz + 10.f * uiScale, ty), ImGui::GetColorU32(it.enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled), it.label.c_str());
    std::string right = T_(it.group);
    for (auto& k : keymap)
      if (it.id == k.id && k.chord) right = chordName(k.chord, true) + "   " + right;
    ImVec2 rs = ImGui::CalcTextSize(right.c_str());
    dl->AddText(ImVec2(b.x - rs.x - 6.f * uiScale, ty), ImGui::GetColorU32(ImGuiCol_TextDisabled), right.c_str());
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip) && !it.desc.empty()) ImGui::SetTooltip("%s", it.desc.c_str());
    ImGui::PopID();
    if (picked) {
      choose(it);
      break;
    }
  }
  ImGui::EndChild();
  if (!favorites[slot].empty() && ImGui::Button(L_("Empty this place"))) {
    favorites[slot].clear();
    saveSession(true);
    ImGui::CloseCurrentPopup();
  }
}

bool App::viewPieInput(ImVec2 origin, ImVec2 size, bool hovered) {
  ViewPie& Pi = pie;
  ImGuiIO& io = ImGui::GetIO();
  if (!engine.features.on("view_pie")) return Pi.open = false;
  if (!Pi.open) {
    const bool free = !ImGui::IsMouseDown(ImGuiMouseButton_Left) && !ImGui::IsMouseDown(ImGuiMouseButton_Middle) &&
                      !ImGui::IsMouseDown(ImGuiMouseButton_Right);
    if (!hovered || io.WantTextInput || engine.painter.active() || !free || radialEdit) return false;
    int kind = -1;
    for (int k = 0; k < (int)PieKind::Count && kind < 0; ++k)
      if ((k == 0 || engine.features.on("pie_menus")) && keyPressed(kPieKeys[k])) kind = k;
    if (kind < 0) return false;
    // round the pointer, kept inside the view as far as it fits
    const float mx = std::round(190.f * uiScale), my = std::round(130.f * uiScale);
    ImVec2 c = io.MousePos;
    if (size.x > 2 * mx) c.x = std::clamp(c.x, origin.x + mx, origin.x + size.x - mx);
    if (size.y > 2 * my) c.y = std::clamp(c.y, origin.y + my, origin.y + size.y - my);
    Pi.open = true, Pi.sticky = false, Pi.center = c, Pi.openedAt = nowSeconds(), Pi.hot = -1;
    Pi.kind = (PieKind)kind, Pi.key = kPieKeys[kind];
    if (std::abs(c.x - io.MousePos.x) > 1.f || std::abs(c.y - io.MousePos.y) > 1.f) Pi.sticky = true;  // (moved: click)
    engine.uiRects["pie.center"] = {c.x - 1, c.y - 1, 2, 2};
    return true;
  }
  std::vector<PieItem> items = pieItems(Pi.kind);
  PieLayout L = pieLayout(Pi.center, items, uiScale);
  // the item the pointer is on, or points toward from outside the middle
  ImVec2 m = io.MousePos;
  float dx = m.x - Pi.center.x, dy = m.y - Pi.center.y;
  int on = -1;
  for (int i = 0; i < 8; ++i)
    if (m.x >= L.box[i][0].x && m.x <= L.box[i][1].x && m.y >= L.box[i][0].y && m.y <= L.box[i][1].y) on = i;
  Pi.hot = on;
  if (Pi.hot < 0 && dx * dx + dy * dy > L.dead * L.dead) {
    float a = std::atan2(dy, dx), best = 1e9f;
    for (int i = 0; i < 8; ++i) {
      float d = std::abs(std::remainder(a - kSlotAngle[i], 2.f * kPi));
      if (d < best) best = d, Pi.hot = i;
    }
  }
  if (Pi.hot >= 0 && !items[Pi.hot].enabled) Pi.hot = -1;
  for (int i = 0; i < 8; ++i)  // (for tests and the manual's screenshots)
    engine.uiRects[std::string("pie.") + std::to_string(i)] = {L.box[i][0].x, L.box[i][0].y, L.box[i][1].x - L.box[i][0].x, L.h};
  auto choose = [&](int i) {
    Pi.open = false;
    if (i >= 0 && items[i].enabled) {
      items[i].run();
      if (!items[i].id.empty()) setStatus(F_("%s pie: %s", T_(kPieNames[(int)Pi.kind]), items[i].label.c_str()));
    }
  };
  if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
    Pi.open = false;
  } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
    // (0.40) on an item: its menu (add it to Quick Favorites, take it out); an empty place: the picker; elsewhere: close
    Pi.open = false;
    if (on >= 0 && engine.features.on("pie_menus")) {
      if (Pi.kind == PieKind::Favorites && items[on].id.empty()) {
        Pi.pickSlot = on, Pi.openPicker = true;
      } else if (!items[on].id.empty()) {
        Pi.menuKind = Pi.kind, Pi.menuSlot = on, Pi.menuId = items[on].id, Pi.openMenu = true;
      }
    }
  } else if (!Pi.sticky) {
    // held: letting go chooses what the pointer points at; a quick tap leaves it open for a click
    if (!keyHeld(Pi.key)) {
      if (Pi.hot >= 0) choose(Pi.hot);
      else if (nowSeconds() - Pi.openedAt < 0.3) Pi.sticky = true;
      else Pi.open = false;
    } else if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
      choose(Pi.hot);
    }
  } else {
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) choose(Pi.hot);
    else if (keyPressed(Pi.key)) Pi.open = false;
  }
  return true;
}

void App::viewPieDraw() {
  ViewPie& Pi = pie;
  // ---- (0.40) the popups: an item's right-click menu, the Quick Favorites picker
  if (Pi.openMenu) ImGui::OpenPopup("##piemenu"), Pi.openMenu = false;
  const bool pickNow = Pi.openPicker;
  if (pickNow) {
    ImGui::OpenPopup("##piepick");
    Pi.openPicker = false, Pi.pickQuery[0] = 0, Pi.pickFocus = true;
  }
  if (ImGui::BeginPopup("##piemenu")) {
    std::vector<PieItem> all = pieCatalog();
    const PieItem* it = findItem(all, Pi.menuId);
    ImGui::TextDisabled("%s", it ? it->label.c_str() : Pi.menuId.c_str());
    ImGui::Separator();
    if (Pi.menuKind == PieKind::Favorites) {
      if (ImGui::MenuItem(L_("Replace..."))) Pi.pickSlot = Pi.menuSlot, Pi.openPicker = true;
      bool go = ImGui::MenuItem(L_("Remove from Quick Favorites"));
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["menu.favorite"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
      if (go && Pi.menuSlot >= 0 && Pi.menuSlot < (int)favorites.size()) favorites[Pi.menuSlot].clear(), saveSession(true);
    } else {
      favoriteMenuItems(Pi.menuId);
    }
    for (auto& a : keymap)
      if (Pi.menuId == a.id) {
        ImGui::Separator();
        const char* k = keyLabel(a.id);
        ImGui::MenuItem(k ? F_("Shortcut: %s", k).c_str() : L_("No Shortcut"), nullptr, false, false);
        if (ImGui::MenuItem(L_("Change Shortcut..."))) showKeymap = true, keymapCapture = a.id, keymapScrollTo = true;
      }
    ImGui::EndPopup();
  }
  if (pickNow) ImGui::SetNextWindowPos(Pi.center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));  // (round the pie)
  if (ImGui::BeginPopup("##piepick")) {
    favoritePickerUI();
    ImGui::EndPopup();
  }
  if (!Pi.open) return;
  std::vector<PieItem> items = pieItems(Pi.kind);
  PieLayout L = pieLayout(Pi.center, items, uiScale);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 c = Pi.center;
  const float s = uiScale;
  // the middle: a ring, and the direction the pointer goes
  dl->AddCircleFilled(c, L.dead, IM_COL32(20, 20, 22, 170), 32);
  dl->AddCircle(c, L.dead, IM_COL32(255, 255, 255, 60), 32, std::max(1.f, 2.f * s));
  ImVec2 m = ImGui::GetIO().MousePos;
  float dx = m.x - c.x, dy = m.y - c.y;
  if (dx * dx + dy * dy > L.dead * L.dead * 0.25f) {
    float a = std::atan2(dy, dx);
    dl->PathArcTo(c, L.dead, a - 0.45f, a + 0.45f, 12);
    dl->PathStroke(ui::themeColor(ui::theme().accent), 0, std::max(2.f, 3.f * s));
  }
  // (0.40) which pie it is, over the middle
  if (engine.features.on("pie_menus")) {
    const char* t = T_(kPieNames[(int)Pi.kind]);
    ImVec2 ts = ImGui::CalcTextSize(t);
    ImVec2 p(std::round(c.x - ts.x * 0.5f), std::round(c.y - L.dead - 8.f * s - ts.y));
    dl->AddRectFilled(p - ImVec2(6, 2) * s, p + ts + ImVec2(6, 2) * s, IM_COL32(20, 20, 22, 190), 4.f * s);
    dl->AddText(p, ui::themeColor(ui::theme().accent), t);
  }
  // the items
  const ui::Theme& th = ui::theme();
  for (int i = 0; i < 8; ++i) {
    const PieItem& it = items[i];
    const bool hot = Pi.hot == i;
    const bool empty = it.id.empty();
    ImVec2 a = L.box[i][0], b = L.box[i][1];
    ImU32 bg = hot ? ui::themeColor(th.accentDeep) : it.on ? ui::themeColor(th.accentDim, 0.96f)
             : empty ? ui::themeColor(th.plate, 0.85f) : ui::themeColor(th.plate);
    dl->AddRectFilled(a + ImVec2(2, 3) * s, b + ImVec2(2, 3) * s, IM_COL32(0, 0, 0, 70), L.h * 0.5f);
    dl->AddRectFilled(a, b, bg, L.h * 0.5f);
    if (it.on && !hot) dl->AddRect(a, b, ui::themeColor(th.accent), L.h * 0.5f, 0, std::max(1.f, 1.5f * s));
    else dl->AddRect(a, b, IM_COL32(255, 255, 255, hot ? 90 : empty ? 50 : 28), L.h * 0.5f, 0, 1.f);
    ui::IconColors col = ui::iconColors(!it.enabled ? ui::IconState::Disabled : hot ? ui::IconState::Chosen : ui::IconState::Normal);
    ui::drawIcon(dl, it.icon, ImVec2(a.x + L.pad * 1.4f, std::round(a.y + (L.h - L.isz) * 0.5f)), L.isz, col);
    ImVec2 ts = ImGui::CalcTextSize(it.label.c_str());
    ImU32 tc = !it.enabled || (empty && !hot) ? IM_COL32(150, 150, 150, 170) : hot ? ui::themeColor(th.onAccent) : ImGui::GetColorU32(ImGuiCol_Text);
    dl->AddText(ImVec2(a.x + L.pad * 2.f + L.isz, std::round(a.y + (L.h - ts.y) * 0.5f)), tc, it.label.c_str());
  }
  // what the item under the pointer does, under the middle
  if (Pi.hot >= 0) {
    const char* t = items[Pi.hot].desc.c_str();
    ImVec2 ts = ImGui::CalcTextSize(t);
    ImVec2 p(std::round(c.x - ts.x * 0.5f), std::round(c.y + L.dead + 10.f * s));
    dl->AddRectFilled(p - ImVec2(8, 4) * s, p + ts + ImVec2(8, 4) * s, IM_COL32(20, 20, 22, 215), 5.f * s);
    dl->AddText(p, IM_COL32(230, 230, 230, 255), t);
  } else if (Pi.sticky) {
    const char* t = T_("Click an item (Esc: close)");
    ImVec2 ts = ImGui::CalcTextSize(t);
    ImVec2 p(std::round(c.x - ts.x * 0.5f), std::round(c.y + L.dead + 10.f * s));
    dl->AddText(p, IM_COL32(200, 200, 200, 220), t);
  }
}

// ------------------------------------------------------------------ Preferences > Pie menus

void App::drawPiePrefs() {
  if (!engine.features.on("view_pie")) return;
  ImGui::SeparatorText(T_("Pie menus"));
  {
    ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
    engine.uiRects["prefs.pies"] = {ra.x, ra.y, rb.x - ra.x, rb.y - ra.y};  // (the manual's screenshots)
  }
  const bool more = engine.features.on("pie_menus");
  auto key = [&](const char* id) {
    const char* k = keyLabel(id);
    return std::string(k ? k : T_("no key"));
  };
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::PushTextWrapPos(0.f);
  if (more)
    ImGui::TextWrapped(T_("Hold the key over the 3D view, point at an item and let go (or tap the key, then click). "
                          "%s: the view. %s: the channels. %s: the edit modes. %s: Quick Favorites, the eight below. "
                          "Right-click a toolbar button or a pie item to add it."),
                       key("view.pie").c_str(), key("view.pie_channels").c_str(), key("tool.pie").c_str(), key("app.favorites").c_str());
  else
    ImGui::TextWrapped(T_("Hold %s over the 3D view, point at an item and let go (or tap it, then click)."), key("view.pie").c_str());
  ImGui::PopTextWrapPos();
  ImGui::PopStyleColor();
  if (!more) return;
  favorites.resize(8);
  std::vector<PieItem> all = pieCatalog();
  const float bw = std::round(205.f * uiScale);
  int open = -1;
  for (int i = 0; i < 8; ++i) {
    ImGui::PushID(i);
    const PieItem* it = findItem(all, favorites[i]);
    std::string label = F_("%d. %s", i + 1, it ? it->label.c_str() : T_("(empty)"));
    if (ImGui::Button(label.c_str(), ImVec2(bw, 0))) open = i;
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["prefs.favorite." + std::to_string(i)] = {a.x, a.y, b.x - a.x, b.y - a.y};
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", T_("Choose what goes here"));
    ImGui::SameLine(0, 2.f * uiScale);
    ImGui::BeginDisabled(favorites[i].empty());
    if (ImGui::Button("x")) favorites[i].clear(), saveSession(true);
    ImGui::EndDisabled();
    if (i % 2 == 0) ImGui::SameLine(0, 12.f * uiScale);
    ImGui::PopID();
  }
  if (ImGui::Button(L_("Reset Quick Favorites"))) favorites = defaultFavorites(), saveSession(true);
  {
    ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
    engine.uiRects["prefs.pies_end"] = {ra.x, ra.y, rb.x - ra.x, rb.y - ra.y};
  }
  if (open >= 0) {
    pie.pickSlot = open, pie.pickQuery[0] = 0, pie.pickFocus = true;
    ImGui::OpenPopup("##prefpick");
  }
  if (ImGui::BeginPopup("##prefpick")) {
    favoritePickerUI();
    ImGui::EndPopup();
  }
}

// ------------------------------------------------------------------ MCP

void App::registerPieCommands() {
  engine.commands.add(
      {"pie_menus",
       "The pie menus over the 3D view (the window only): Z the view (lit, solid, unlit, path traced, wireframe, seams, "
       "UV checker, retopology preview), Shift+Z the channels, ` the edit modes (tools), Q Quick Favorites (eight items "
       "the user chose). Returns each pie's items in slot order (left, right, down, up, up-left, up-right, down-left, "
       "down-right) as {slot, id, label, on, enabled}, the keys, and which pie is open. favorites: up to 8 item ids "
       "(\"\" = an empty place) sets Quick Favorites; catalog: true also lists every id a pie can hold; run: an item id, "
       "done as if chosen in a pie.",
       {{"type", "object"},
        {"properties",
         {{"favorites", {{"type", "array"}, {"items", {{"type", "string"}}}, {"maxItems", 8}}},
          {"catalog", {{"type", "boolean"}}},
          {"run", {{"type", "string"}}}}}},
       [this](CommandContext&, const json& a) {
         json out = json::object();
         if (a.contains("favorites")) {
           std::vector<PieItem> all = pieCatalog();
           std::vector<std::string> f;
           for (auto& x : a["favorites"]) {
             std::string id = x.get<std::string>();
             if (!id.empty() && !findItem(all, id)) throw CommandError("unknown pie item: " + id + " (catalog: true lists them)");
             f.push_back(id);
           }
           if (f.size() > 8) throw CommandError("Quick Favorites hold 8 items");
           f.resize(8);
           favorites = f;
           saveSession(true);
         }
         if (a.contains("run")) {
           std::string id = a["run"];
           std::vector<PieItem> all = pieCatalog();
           const PieItem* it = findItem(all, id);
           if (!it) throw CommandError("unknown pie item: " + id + " (catalog: true lists them)");
           if (!it->enabled) throw CommandError(id + " is not available now");
           it->run();
           out["ran"] = id;
         }
         json pies = json::object(), keys = json::object();
         for (int k = 0; k < (int)PieKind::Count; ++k) {
           if (k && !engine.features.on("pie_menus")) continue;
           json list = json::array();
           std::vector<PieItem> items = pieItems((PieKind)k);
           for (int i = 0; i < 8; ++i)
             list.push_back({{"slot", i}, {"id", items[i].id}, {"label", items[i].label}, {"on", items[i].on}, {"enabled", items[i].enabled}});
           pies[kPieMcp[k]] = list;
           const char* kl = keyLabel(kPieKeys[k]);
           keys[kPieMcp[k]] = kl ? kl : "";
         }
         out["pies"] = pies;
         out["keys"] = keys;
         out["open"] = pie.open ? json(kPieMcp[(int)pie.kind]) : json();
         if (a.value("catalog", false)) {
           json c = json::array();
           for (auto& it : pieCatalog()) c.push_back({{"id", it.id}, {"label", it.label}, {"group", it.group}, {"enabled", it.enabled}});
           out["catalog"] = c;
         }
         return out;
       }});
}

}  // namespace patina
