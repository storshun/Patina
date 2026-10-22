// (0.36) Right-click menus: in the 3D view for the spot under the pointer, and on the panels' items and the empty
// parts of their lists. Each menu has what can be done there, and its items show their shortcuts.
#include <imgui.h>
#include <imgui_internal.h>

#include <cstring>

#include "core/Log.h"
#include "core/Paths.h"
#include "core/UVIsland.h"
#include "ui/App.h"

#include <SDL3/SDL.h>

namespace patina {

static std::string hexOf(vec3 c) {
  char b[16];
  auto ch = [](float v) { return int(std::clamp(v, 0.f, 1.f) * 255.f + 0.5f); };
  snprintf(b, sizeof b, "#%02X%02X%02X", ch(c.x), ch(c.y), ch(c.z));
  return b;
}

// "#RRGGBB" / "RRGGBB" / "#RGB" from the clipboard
static bool parseHex(const char* s, vec3& out) {
  if (!s) return false;
  while (*s == ' ' || *s == '\t' || *s == '\n') ++s;
  if (*s == '#') ++s;
  size_t n = 0;
  while (std::isxdigit((unsigned char)s[n])) ++n;
  if (n != 6 && n != 3) return false;
  unsigned v = (unsigned)std::strtoul(std::string(s, n).c_str(), nullptr, 16);
  if (n == 3) v = ((v >> 8 & 15) * 17) << 16 | ((v >> 4 & 15) * 17) << 8 | (v & 15) * 17;
  out = vec3((v >> 16 & 255) / 255.f, (v >> 8 & 255) / 255.f, (v & 255) / 255.f);
  return true;
}

void openFolder(const std::filesystem::path& p) {
#ifdef _WIN32
  std::string url = "file:///" + paths::ensureDir(p).generic_string();
#else
  std::string url = "file://" + paths::ensureDir(p).generic_string();
#endif
  SDL_OpenURL(url.c_str());
}

// ------------------------------------------------------------------ the 3D view

void App::viewportContextMenu(ImVec2 origin, ImVec2 avail) {
  viewportMenuOpen = false;
  if (!ImGui::BeginPopup("##viewctx")) return;
  viewportMenuOpen = true;
  Document& d = engine.doc;
  Painter& P = engine.painter;
  const RayHit& h = viewportMenuHit;
  const Mesh& m = d.mesh;
  // what is under the pointer
  if (h.hit && h.face < m.faceCount()) {
    std::string what = F_("Face %u", h.face);
    if (m.loopIsland.size() == m.loopCount()) what += "  |  " + F_("UV island %u", m.loopIsland[m.faceStart[h.face]] + 1);
    if (m.materialNames.size() > 1 && h.face < m.faceMaterial.size()) what += "  |  " + m.slotName(m.faceMaterial[h.face]);
    if (uv::isPinned(m, h.face)) what += std::string("  |  ") + T_("pinned");
    ImGui::TextDisabled("%s", what.c_str());
  } else {
    ImGui::TextDisabled("%s", d.hasMesh() ? T_("Not on the model") : T_("No model open"));
  }
  // (1.2) another object of the scene under the pointer: paint it
  if (!h.hit && engine.objectCount() > 1 && engine.features.on("scene_objects")) {
    Engine::ScenePick sp = engine.pickScene(viewportMenuAt);
    if (sp.object >= 0 && sp.object != engine.activeObject() &&
        ImGui::MenuItem(F_("Paint %s", engine.object(sp.object).displayName().c_str()).c_str(), T_("double-click")))
      selectObjectUI(sp.object);
  }
  ImGui::Separator();
  if (h.hit) {
    if (ImGui::MenuItem(L_("Pick the color here"), T_("Alt+right-click"))) {
      if (engine.pickAt(viewportMenuAt, true, nullptr)) {
        noteColorUsed(P.material.flat.color);
        setStatus(T_("Picked color - the brush now paints these flat values"));
      }
    }
    if (ImGui::BeginMenu(L_("Layers painted here"))) {
      if (layersAtMenu.empty()) ImGui::MenuItem(L_("No layer paints here"), nullptr, false, false);
      for (auto& lh : layersAtMenu) {
        if (lh.index < 0 || lh.index >= (int)d.layers.size()) continue;
        const Layer& l = *d.layers[lh.index];
        std::string pct = lh.shown ? F_("%.0f%%", lh.amount * 100.f) : F_("%.0f%%, hidden", lh.amount * 100.f);
        if (ImGui::MenuItem((l.name + "##" + std::to_string(l.uid)).c_str(), pct.c_str(), d.selected == lh.index))
          d.selected = lh.index, d.editMask = false;
      }
      ImGui::Separator();
      ImGui::TextDisabled("%s", T_("Pick one to select it"));
      ImGui::EndMenu();
    }
    struct Scope {
      FillMode mode;
      const char* fill;
      const char* select;
    };
    std::vector<Scope> scopes = {{FillMode::Face, N_("This face"), N_("This face")},
                                 {FillMode::UVIsland, N_("Its UV island"), N_("Its UV island")},
                                 {FillMode::Connected, N_("The connected part"), N_("The connected part")}};
    if (m.materialNames.size() > 1) scopes.push_back({FillMode::Material, N_("Its material slot"), N_("Its material slot")});
    if (ImGui::BeginMenu(L_("Fill with the paint material"))) {
      for (auto& s : scopes)
        if (ImGui::MenuItem(L_(s.fill))) {
          auto faces = P.facesForFill(d, h.face, s.mode);
          PaintTool keep = P.tool;
          P.tool = PaintTool::Paint;
          if (P.fillFaces(d, engine.materials, faces)) setStatus(F_("Filled %zu faces", faces.size()));
          else setStatus(T_("Nothing to fill: select a layer and enable at least one channel"));
          P.tool = keep;
        }
      ImGui::Separator();
      if (ImGui::MenuItem(L_("Erase this UV island"))) {
        auto faces = P.facesForFill(d, h.face, FillMode::UVIsland);
        PaintTool keep = P.tool;
        P.tool = PaintTool::Erase;
        if (P.fillFaces(d, engine.materials, faces)) setStatus(F_("Erased %zu faces", faces.size()));
        P.tool = keep;
      }
      ImGui::EndMenu();
    }
    if (ImGui::BeginMenu(L_("Select"))) {
      auto select = [&](const std::vector<uint32_t>& faces, bool add, const char* label) {
        auto before = d.faceSelection;
        d.faceSelection.resize(m.faceCount(), 0);
        if (!add) std::fill(d.faceSelection.begin(), d.faceSelection.end(), 0);
        for (uint32_t f : faces) d.faceSelection[f] = 1;
        d.updateSelectionFlags();
        d.pushSelectionUndo(before, label);
        setStatus(F_("%zu faces selected", d.selectedFaces().size()));
      };
      for (auto& s : scopes) {
        if (ImGui::MenuItem(L_(s.select))) select(P.facesForFill(d, h.face, s.mode), false, "Select faces");
      }
      if (ImGui::MenuItem(L_("Add its UV island to the selection"))) select(P.facesForFill(d, h.face, FillMode::UVIsland), true, "Select faces");
      ImGui::Separator();
      if (ImGui::MenuItem(L_("All"), keyLabel("select.all"))) select(P.facesForFill(d, 0, FillMode::Mesh), false, "Select all faces");
      if (ImGui::MenuItem(L_("None"), keyLabel("select.none"))) select({}, false, "Select no faces");
      if (ImGui::MenuItem(L_("Invert"))) {
        std::vector<uint32_t> inv;
        d.faceSelection.resize(m.faceCount(), 0);
        for (uint32_t f = 0; f < m.faceCount(); ++f)
          if (!d.faceSelection[f]) inv.push_back(f);
        select(inv, false, "Invert the selection");
      }
      ImGui::EndMenu();
    }
    // (0.37.1) pins: the island under the pointer, and the selection's (Alt+P)
    if (engine.features.on("viewport_pins")) {
      const bool pinned = uv::isPinned(m, h.face);
      if (ImGui::MenuItem(pinned ? L_("Unpin its UV island") : L_("Pin its UV island")))
        uvRunOnSelection("uv_pin", {{"pin", !pinned}, {"faces", json::array({h.face})}},
                         pinned ? T_("Unpinned the island: UV edits can move it again") : T_("Pinned the island: UV edits leave it where it is"));
      if (pinnedSelectedFaces() && ImGui::MenuItem(L_("Unpin the selected islands"), keyLabel("uv.unpin"))) uvPinSelection(false, false);
    }
    if (ImGui::MenuItem(L_("Add a note here"))) {
      uint32_t id = addNoteAt(viewportMenuAt, "");
      if (id) beginNoteEdit(id, ImVec2(origin.x + viewportMenuAt.x, origin.y + viewportMenuAt.y)), noteEditIsNew = true;
    }
    if (ImGui::MenuItem(L_("Set the clone source here"), T_("Ctrl+click (Clone)"))) {
      P.setCloneSource(h.uv);
      cloneMarkerValid = true;
      cloneMarkerPos = h.position;
      cloneMarkerUV = h.uv;
      setStatus(T_("Clone source set: paint with the Clone tool to copy from here"));
    }
    if (ImGui::MenuItem(L_("Center the view here"))) engine.camera.target = engine.toWorld(h.position);
    // (1.2) faces in batches, from the face under the pointer
    if (ImGui::BeginMenu(L_("Select"))) {
      uint32_t k = select::nearestEdge(m, h.face, h.position);
      json at = {{"face", h.face}, {"edge", k}};
      auto run = [&](const char* cmd, json args) {
        for (auto& [key, v] : at.items()) args[key] = v;
        json r;
        if (runCommand(cmd, args, &r)) setStatus(F_("%d faces selected", r.value("selected", 0)));
      };
      const bool quad = m.faceSize(h.face) == 4;
      if (ImGui::MenuItem(L_("The loop across this edge"), T_("Alt+click (Select)"), false, quad)) run("select_loop", json::object());
      if (ImGui::MenuItem(L_("The ring along this edge"), T_("Ctrl+Alt+click (Select)"), false, quad)) run("select_loop", {{"ring", true}});
      if (!quad) ImGui::SetItemTooltip("%s", T_("Loops and rings run through quads: this face isn't one"));
      if (ImGui::MenuItem(L_("The connected part"))) run("select_linked", {{"by", "connected"}});
      if (ImGui::MenuItem(L_("The flat region"))) run("select_linked", {{"by", "flat"}, {"angle", selectFlatAngle}});
      if (ImGui::MenuItem(L_("The UV island"))) run("select_linked", {{"by", "island"}});
      if (m.materialNames.size() > 1 && ImGui::MenuItem(L_("The material slot"))) run("select_linked", {{"by", "material"}});
      ImGui::EndMenu();
    }
    ImGui::Separator();
  }
  // tools, mirrors and locks
  if (ImGui::BeginMenu(L_("Tool"))) {
    struct T {
      ViewportTool t;
      const char* name;
      const char* key;
    };
    const T paintTools[] = {{ViewportTool::Paint, N_("Paint"), "tool.paint"},   {ViewportTool::Erase, N_("Erase"), "tool.erase"},
                            {ViewportTool::Blur, N_("Blur"), "tool.blur"},      {ViewportTool::Smudge, N_("Smudge"), "tool.smudge"},
                            {ViewportTool::Clone, N_("Clone"), "tool.clone"},   {ViewportTool::Fill, N_("Fill"), "tool.fill"},
                            {ViewportTool::Picker, N_("Pick"), "tool.picker"}};
    const T uvTools[] = {{ViewportTool::SelectFaces, N_("Select faces"), "tool.select"},
                         {ViewportTool::Seams, N_("Seams"), "tool.seams"},
                         {ViewportTool::Island, N_("Island brush"), "tool.island"}};
    const T retopoTools[] = {{ViewportTool::ZoneLine, N_("Zone lines"), "tool.zone_line"},
                             {ViewportTool::Patch, N_("Zones"), "tool.patch"},
                             {ViewportTool::Flow, N_("Flow strokes"), "tool.flow"},
                             {ViewportTool::Density, N_("Density brush"), "tool.density"}};
    auto items = [&](const T* list, size_t n) {
      for (size_t i = 0; i < n; ++i)
        if (ImGui::MenuItem(L_(list[i].name), keyLabel(list[i].key), tool == list[i].t)) selectTool(list[i].t);
    };
    ImGui::SeparatorText(T_("Paint"));
    items(paintTools, std::size(paintTools));
    ImGui::SeparatorText(T_("UV"));
    items(uvTools, std::size(uvTools));
    if (engine.features.on("retopology")) {
      ImGui::SeparatorText(T_("Retopology"));
      ImGui::BeginDisabled(!d.hasMesh());
      items(retopoTools, std::size(retopoTools));
      ImGui::EndDisabled();
    }
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Note"), keyLabel("tool.note"), tool == ViewportTool::Note)) selectTool(ViewportTool::Note);
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(L_("Symmetry"))) {
    if (ImGui::MenuItem(L_("Mirror X"), keyLabel("sym.x"), P.symmetry[0])) P.symmetry[0] = !P.symmetry[0];
    if (ImGui::MenuItem(L_("Mirror Y"), nullptr, P.symmetry[1])) P.symmetry[1] = !P.symmetry[1];
    if (ImGui::MenuItem(L_("Mirror Z"), nullptr, P.symmetry[2])) P.symmetry[2] = !P.symmetry[2];
    ImGui::Separator();
    ImGui::SetNextItemWidth(140 * uiScale);
    ImGui::SliderInt(L_("Radial copies"), &P.radialCount, 1, 32);
    ImGui::SetNextItemWidth(140 * uiScale);
    ImGui::Combo(L_("Radial axis"), &P.radialAxis, "X\0Y\0Z\0");
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(L_("Brush"))) {
    if (ImGui::MenuItem(L_("Topology lock"), keyLabel("brush.lock_topology"), P.lockTopology)) P.lockTopology = !P.lockTopology;
    if (ImGui::MenuItem(L_("Island lock"), keyLabel("brush.lock_island"), P.lockIsland)) P.lockIsland = !P.lockIsland;
    if (engine.features.on("texture_set_tools") && (m.tiles.active() || m.materialNames.size() > 1) &&
        ImGui::MenuItem(L_("Set lock"), keyLabel("brush.lock_set"), P.lockSet))
      P.lockSet = !P.lockSet;
    ImGui::Separator();
    const char* proj[] = {N_("Screen"), N_("Surface 3D"), N_("UV")};
    for (int i = 0; i < 3; ++i)
      if (ImGui::MenuItem(L_(proj[i]), nullptr, (int)P.brush.projection == i)) P.brush.projection = (BrushProjection)i;
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Swap the colors"), keyLabel("color.swap"))) {
      std::swap(P.material.flat.color, secondaryColor);
      if (!P.material.id.empty()) P.material.id.clear(), engine.selectedMaterialId.clear();
    }
    ImGui::EndMenu();
  }
  // the view
  if (ImGui::BeginMenu(L_("View"))) {
    Camera& cam = engine.camera;
    if (ImGui::MenuItem(L_("Frame the model"), keyLabel("view.frame"))) engine.frameMesh();
    ImGui::Separator();
    struct V {
      const char* name;
      const char* key;
      const char* view;
    };
    const V views[] = {{N_("Front"), "Numpad 1", "front"}, {N_("Back"), "Ctrl+Numpad 1", "back"}, {N_("Right"), "Numpad 3", "right"},
                       {N_("Left"), "Ctrl+Numpad 3", "left"}, {N_("Top"), "Numpad 7", "top"},   {N_("Bottom"), "Ctrl+Numpad 7", "bottom"}};
    for (auto& v : views) {
      float y, p;
      if (ImGui::MenuItem(L_(v.name), v.key) && Camera::viewAngles(v.view, y, p)) snapToView(y, p);
    }
    if (ImGui::MenuItem(L_("Orthographic"), "Numpad 5", cam.ortho)) cam.ortho = !cam.ortho, engine.viewMemory.orthoBySnap = false;
    if (viewSnapping() && ImGui::MenuItem(L_("Back to your view"), keyLabel("view.back"), false, engine.viewMemory.hasBack)) goBackView();
    ImGui::Separator();
    if (ImGui::BeginMenu(L_("View mode"))) {
      int vm = (int)engine.viewOptions.mode;
      for (int k = 0; k < (int)ViewMode::Count; ++k)
        if (viewModeAvailable((ViewMode)k) && ImGui::MenuItem(L_(viewModeLabel(k)), nullptr, k == vm)) engine.viewOptions.mode = (ViewMode)k;
      ImGui::EndMenu();
    }
    ImGui::MenuItem(L_("Wireframe"), nullptr, &engine.viewOptions.wireframe);
    ImGui::MenuItem(L_("Seams"), nullptr, &engine.viewOptions.seams);
    ImGui::MenuItem(L_("Note pins"), nullptr, &showNotePins);
    if (engine.features.on("retopology") && engine.features.on("retopo_preview_toggle"))
      ImGui::MenuItem(L_("Retopology preview"), keyLabel("view.retopo_preview"), &engine.viewOptions.retopoPreview);
    if (engine.features.on("axis_gizmo")) ImGui::MenuItem(L_("Axis gizmo"), nullptr, &engine.viewOptions.axisGizmo);
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Compare (split view)"), keyLabel("view.compare"))) toggleCompare();
    if (ImGui::MenuItem(L_("Hide the panels"), keyLabel("view.focus"), distractionFree))
      distractionFree = !distractionFree, distractionFreeSince = nowSeconds();
    ImGui::EndMenu();
  }
  if (engine.features.on("retopology") && retopoTool()) {
    ImGui::Separator();
    if (ImGui::MenuItem(retopoMakingQuads() ? L_("Making quads...") : L_("Make quads"), nullptr, false, !retopoMakingQuads())) retopoBuild();
    if (ImGui::MenuItem(L_("Retopology window..."))) showRetopoWindow();
  }
  ImGui::EndPopup();
}

// ------------------------------------------------------------------ panels

void App::historyContextMenu(int index) {
  Document& d = engine.doc;
  UndoStack& u = d.undo;
  if (!ImGui::BeginPopupContextItem("##histctx")) return;
  ImGui::TextDisabled("%s", index == 0 ? T_("(opened)") : u.label(index - 1).c_str());
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Go back to here"), nullptr, false, !engine.painter.active() && (size_t)index != u.position())) d.jumpTo((size_t)index);
  if (ImGui::MenuItem(L_("Undo"), keyLabel("edit.undo"), false, u.position() > 0)) doUndo();
  if (ImGui::MenuItem(L_("Redo"), keyLabel("edit.redo"), false, u.position() < u.count())) doRedo();
  ImGui::Separator();
  if (ImGui::BeginMenu(L_("Clear the history"), u.count() > 0)) {
    ImGui::TextDisabled("%s", T_("Frees its memory; the steps can't be undone after."));
    if (ImGui::MenuItem(L_("Clear"))) d.clearHistory(), setStatus(T_("History cleared"));
    ImGui::EndMenu();
  }
  ImGui::EndPopup();
}

// which: 0 primary, 1 secondary, 2 a swatch (index), 3 a recent color (index)
void App::colorsContextMenu(int which, vec3 color, int index) {
  Painter& P = engine.painter;
  auto setPrimary = [&](vec3 c) {
    P.material.id.clear(), engine.selectedMaterialId.clear();
    P.material.flat.color = c;
  };
  ImGui::TextDisabled("%s", hexOf(color).c_str());
  ImGui::Separator();
  if (which != 0 && ImGui::MenuItem(L_("Use as the primary color"))) setPrimary(color);
  if (which != 1 && ImGui::MenuItem(L_("Use as the secondary color"))) secondaryColor = color;
  if (which <= 1 && ImGui::MenuItem(L_("Swap the colors"), keyLabel("color.swap"))) {
    std::swap(P.material.flat.color, secondaryColor);
    P.material.id.clear(), engine.selectedMaterialId.clear();
  }
  if (which != 2 && ImGui::MenuItem(L_("Add to the swatches"))) swatches.push_back(color);
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Copy the hex code"))) ImGui::SetClipboardText(hexOf(color).c_str());
  vec3 pasted;
  bool canPaste = parseHex(ImGui::GetClipboardText(), pasted);
  if (which <= 2 && ImGui::MenuItem(L_("Paste a hex code"), nullptr, false, canPaste)) {
    if (which == 0) setPrimary(pasted);
    else if (which == 1) secondaryColor = pasted;
    else if (index >= 0 && index < (int)swatches.size()) swatches[index] = pasted;
  }
  if (which == 2 && index >= 0 && index < (int)swatches.size()) {
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Replace with the primary color"))) swatches[index] = P.material.flat.color;
    if (ImGui::MenuItem(L_("Move left"), nullptr, false, index > 0)) std::swap(swatches[index], swatches[index - 1]);
    if (ImGui::MenuItem(L_("Move right"), nullptr, false, index + 1 < (int)swatches.size())) std::swap(swatches[index], swatches[index + 1]);
    if (ImGui::MenuItem(L_("Remove"))) swatches.erase(swatches.begin() + index);
  }
  if (which == 3) {
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Clear the recent colors"))) recentColors.clear();
  }
}

bool App::notesContextMenu(uint32_t id) {
  Document& d = engine.doc;
  Note* n = d.findNote(id);
  if (!n) return false;
  vec2 vp = lastViewportSize;
  ImVec2 at(lastViewportOrigin.x + vp.x * 0.5f, lastViewportOrigin.y + vp.y * 0.4f);
  if (ImGui::MenuItem(L_("Go there"))) {
    if (n->view.is_object()) engine.camera.fromJson(n->view);
    else engine.camera.target = engine.toWorld(n->position);
  }
  if (ImGui::MenuItem(L_("Edit"))) {
    if (n->view.is_object()) engine.camera.fromJson(n->view);
    beginNoteEdit(id, at);
  }
  if (ImGui::MenuItem(n->resolved ? L_("Reopen") : L_("Resolve"))) n->resolved = !n->resolved, d.dirty = true;
  if (ImGui::MenuItem(L_("Copy the text"))) ImGui::SetClipboardText(n->text.c_str());
  ImGui::Separator();
  return ImGui::MenuItem(L_("Delete"));
}

void App::consoleContextMenu() {
  if (ImGui::MenuItem(L_("Copy the log"))) {
    std::string all;
    for (auto& e : Log::snapshot(2000)) all += e.text + "\n";
    ImGui::SetClipboardText(all.c_str());
  }
  if (ImGui::MenuItem(L_("Copy the errors and warnings"))) {
    std::string all;
    for (auto& e : Log::snapshot(2000))
      if (e.level == LogLevel::Warn || e.level == LogLevel::Error) all += e.text + "\n";
    ImGui::SetClipboardText(all.c_str());
  }
  if (ImGui::MenuItem(L_("Clear"))) Log::clear();
  ImGui::Separator();
  if (ImGui::MenuItem(L_("List the commands"))) consoleInput = "list_commands";
}

void App::layersEmptyMenu() {
  Document& d = engine.doc;
  if (ImGui::MenuItem(L_("New paint layer"), keyLabel("layer.new_paint"))) d.addPaintLayer();
  if (ImGui::MenuItem(L_("New fill layer"))) addFillLayerUI(-1);
  if (ImGui::MenuItem(L_("New fill layer with a black mask"))) addFillLayerUI(0.f);
  if (ImGui::MenuItem(L_("New adjustment layer"))) d.addAdjustLayer();
  if (ImGui::MenuItem(L_("New folder"))) d.addFolder();
  if (ImGui::BeginMenu(L_("Baked map layer"))) {
    if (!bakedMapMenuItems()) ImGui::TextDisabled("%s", T_("Every baked map has its layer"));
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(L_("Smart material"))) {
    for (auto& sm : listSmartMaterials())
      if (ImGui::MenuItem((sm.name + "##" + sm.id).c_str())) {
        std::string err;
        if (applySmartMaterial(d, sm.path, &err) < 0) showError(T_("Could not apply the smart material"), err);
        else setStatus(F_("Added the smart material %s", sm.name.c_str()));
      }
    ImGui::EndMenu();
  }
  ImGui::Separator();
  bool anyFolder = false;
  for (auto& l : d.layers) anyFolder |= l->isFolder();
  if (ImGui::MenuItem(L_("Open all folders"), nullptr, false, anyFolder))
    for (auto& l : d.layers) l->expanded = true;
  if (ImGui::MenuItem(L_("Close all folders"), nullptr, false, anyFolder))
    for (auto& l : d.layers) l->expanded = false;
  if (ImGui::MenuItem(L_("Show all layers (end solo)"), nullptr, false, d.soloUid != 0)) d.soloUid = 0, d.compositeDirty = true;
}

void App::materialsEmptyMenu() {
  if (ImGui::MenuItem(L_("Import a folder of maps..."))) openFileDialog(DialogPurpose::ImportMaterial);
  if (ImGui::MenuItem(L_("New procedural material..."))) {
    editMaterial = MaterialDef();
    editMaterial.id = "user_material";
    editMaterial.name = "My Material";
    editMaterial.category = "User";
    editMaterial.proc.generator = 1;
    editMaterialDirty = true;
    showMaterialEditor = true;
  }
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Open your materials folder"))) openFolder(paths::userDataDir() / "materials");
}

void App::brushesEmptyMenu() {
  if (ImGui::MenuItem(L_("Open your brushes folder"))) openFolder(paths::userDataDir() / "brushes");
}

}  // namespace patina
