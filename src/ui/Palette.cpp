// Command palette (Ctrl+Shift+P, F3 with the Blender keymap): one search box for every action,
// window, view mode, command, material, brush, layer and recent file.
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

#include "core/Log.h"
#include "ui/App.h"

namespace patina {

namespace fs = std::filesystem;

namespace {

// Subsequence match: every query character in order. Higher is better; < 0 = no match. Word starts
// and runs of consecutive characters score, and a match in the first word counts double.
static bool wordStartAt(const std::string& t, size_t i) {
  if (i == 0) return true;
  unsigned char a = (unsigned char)t[i - 1], b = (unsigned char)t[i];
  return !std::isalnum(a) || (std::isupper(b) && std::islower(a)) || (std::isdigit(b) && !std::isdigit(a));
}

}  // namespace

int fuzzyScore(const std::string& text, const std::string& query) {
  if (query.empty()) return 1;
  std::string lt = text, lq = query;
  for (auto& c : lt) c = (char)std::tolower((unsigned char)c);
  for (auto& c : lq) c = (char)std::tolower((unsigned char)c);
  // the query as one piece ranks far above scattered letters, at a word start higher still;
  // the label's group ("Material: ") is skipped so it doesn't count as a word start
  size_t body = lt.find(": ");
  body = body == std::string::npos ? 0 : body + 2;
  size_t at = lt.find(lq, body);
  if (at != std::string::npos) {
    bool start = at == body || !std::isalnum((unsigned char)lt[at - 1]);
    return (start ? 2000 : 1200) - int(at - body) * 4 - int(lt.size() - body);
  }
  // scattered letters: each one either follows the previous match or starts a word ("abl" ->
  // Add Black Layer); a few letters may land elsewhere ("prefs" -> Preferences), but not many,
  // or "rust" would find Brush: Stipple
  std::vector<char> qc;
  std::vector<bool> mustStart;
  bool nextStart = false;
  for (char c : lq) {
    if (c == ' ') {
      nextStart = true;
      continue;
    }
    qc.push_back(c), mustStart.push_back(nextStart), nextStart = false;
  }
  const size_t m = qc.size(), n = lt.size();
  if (!m) return 1;
  const int INF = 1 << 20;
  std::vector<int> prev(n, INF), cur(n, INF);
  for (size_t i = 0; i < n; ++i)
    if (lt[i] == qc[0] && (!mustStart[0] || wordStartAt(text, i))) prev[i] = wordStartAt(text, i) ? 0 : 2;  // starting mid-word counts double
  for (size_t j = 1; j < m; ++j) {
    int best = INF;  // fewest jumps with letter j-1 at least two characters back
    for (size_t i = 0; i < n; ++i) {
      if (i >= 2) best = std::min(best, prev[i - 2]);
      cur[i] = INF;
      if (lt[i] != qc[j]) continue;
      bool ws = wordStartAt(text, i);
      if (mustStart[j] && !ws) continue;
      if (i >= 1 && prev[i - 1] < INF && !mustStart[j]) cur[i] = prev[i - 1];  // right after the last one
      if (best < INF) cur[i] = std::min(cur[i], best + (ws ? 0 : 1));
    }
    std::swap(prev, cur);
  }
  int jumps = *std::min_element(prev.begin(), prev.end());
  if (jumps >= INF || jumps > int(m) / 3) return -1;
  return 900 - jumps * 200 - int(n);
}

void App::openCommandPalette() {
  if (!engine.features.on("command_palette")) return;
  showPalette = true;
  paletteFocus = true;
  paletteQuery[0] = 0;
  paletteSel = 0;
}

void App::buildPaletteEntries() {
  paletteEntries.clear();
  auto add = [&](std::string group, std::string label, std::string hint, std::function<void()> run) {
    paletteEntries.push_back({group + ": " + label, std::move(hint), std::move(run)});
  };
  Document& d = engine.doc;
  // shortcut actions
  // actions that act on what is under the mouse in a view can't run from here
  static const char* underMouse[] = {"color.sample", "brush.radius", "brush.strength", "brush.flow", "brush.hardness",
                                     "brush.angle", "stencil.toggle", "stencil.edit", "view.pie", "view.pie_channels",
                                     "tool.pie", "app.favorites", "object.move", "object.turn", "object.scale"};
  for (auto& a : keymap) {
    std::string id = a.id;
    if (std::find_if(std::begin(underMouse), std::end(underMouse), [&](const char* s) { return id == s; }) !=
        std::end(underMouse))
      continue;
    // the switched-off features' actions
    if ((!engine.features.on("macro_recorder") && id.rfind("macro.", 0) == 0) ||
        (!engine.features.on("texture_set_tools") && (id == "brush.lock_set" || id == "view.isolate_set")) ||
        (!engine.features.on("command_palette") && (id == "app.palette" || id == "app.palette_alt")) ||
        ((!engine.features.on("retopology") || !engine.features.on("retopo_preview_toggle")) && id == "view.retopo_preview") ||
        (!engine.features.on("viewport_pins") && id == "uv.unpin") || (!engine.features.on("view_snapping") && id == "view.back") ||
        (!engine.features.on("scene_objects") && (id == "file.add_model" || id == "tool.object" || id == "view.local")))
      continue;
    add(T_(a.group), T_(a.label), a.chord ? chordName(a.chord, true) : "", [this, id] { pendingAction = id; });
  }
  // (0.46) the layouts and the Guided layout's steps
  if (engine.features.on("guided_layout")) {
    add(T_("Layout"), T_("Guided: the work in five steps"), "", [this] { setLayoutMode(true); });
    add(T_("Layout"), T_("Full: every panel"), "", [this] { setLayoutMode(false); });
    static const char* kStepNames[] = {N_("1 Model: open it, check it, its textures"), N_("2 UVs: check and unwrap"),
                                       N_("3 Bake: the mesh maps"), N_("4 Paint: layers, materials, brushes"),
                                       N_("5 Export: send to Blender, texture files")};
    for (int k = 0; k < (int)GuidedStep::Count; ++k)
      add(T_("Step"), T_(kStepNames[k]), "", [this, k] {
        setLayoutMode(true);
        setGuidedStep((GuidedStep)k);
      });
  }
  // (1.2) the scene's objects: paint another one
  if (engine.features.on("scene_objects") && engine.objectCount() > 1)
    for (int i = 0; i < engine.objectCount(); ++i)
      if (i != engine.activeObject())
        add(T_("Object"), F_("Paint %s", engine.object(i).displayName().c_str()), "", [this, i] { selectObjectUI(i); });
  // windows
  add(T_("Window"), T_("Preferences"), "", [this] { showPreferences = true; });
  add(T_("Window"), T_("Keyboard shortcuts"), "", [this] { showKeymap = true; });
  add(T_("Window"), T_("Help: shortcuts and usage"), "", [this] { showHelp = true; });
  add(T_("Window"), T_("Export textures"), "", [this] { exportWhat = 0, showExport = true; });
  add(T_("Window"), T_("Render image"), "", [this] { showRender = true; });
  if (engine.features.on("face_sets")) add(T_("Window"), T_("Face sets"), "", [this] { openFaceSets(); });
  add(T_("Window"), T_("Texel density"), "", [this] { showDensity = true; });
  add(T_("Window"), T_("Bake from high-poly"), "", [this] { showHighPoly = true; });
  if (engine.features.on("bake_down"))
    add(T_("Window"), T_("Bake down to a low-poly model"), "", [this] { showBakeDown = true; });
  add(T_("Window"), T_("Material editor"), "", [this] { showMaterialEditor = true; });
  add(T_("Window"), T_("Welcome"), "", [this] { showWelcome = true; });
  add(T_("Window"), T_("About Patina"), "", [this] { showAbout = true; });
  if (engine.features.on("symmetry_check")) add(T_("Window"), T_("Mesh symmetry check"), "", [this] { showSymmetry = true; });
  if (engine.features.on("auto_texture"))
    add(T_("Layers"), T_("Auto texture from material slots"), "", [this] { openAutoTexture(); });
  if (engine.features.on("macro_recorder")) add(T_("Window"), T_("Macro recorder"), "", [this] { showMacros = true; });
  if (engine.features.on("patina_assistant")) add(T_("Window"), T_("Assistant"), "", [this] { openAssistant(); });
  // view modes (by the names the View menus show, so they translate)
  for (int m = 0; m < (int)ViewMode::Count; ++m) {
    ViewMode vm = (ViewMode)m;
    if (!viewModeAvailable(vm)) continue;
    add(T_("View"), T_(viewModeLabel(m)), "", [this, vm] { engine.viewOptions.mode = vm; });
  }
  // (wireframe and seams on / off: shortcut actions since 0.40)
  if (engine.features.on("axis_gizmo"))
    add(T_("View"), T_("Axis gizmo on / off"), "", [this] { engine.viewOptions.axisGizmo = !engine.viewOptions.axisGizmo; });
  // commands that need no arguments (anything with required arguments is left to the console)
  static const char* skip[] = {"new_project", "list_commands", "__stop_server", "ping", "get_log", "screenshot_window",
                               "render_view", "render_uv", "get_texture", "undo", "redo"};
  for (auto& [name, cmd] : engine.commands.all()) {
    if (std::find_if(std::begin(skip), std::end(skip), [&](const char* s) { return name == s; }) != std::end(skip)) continue;
    if (cmd.schema.contains("required") && !cmd.schema["required"].empty()) continue;
    if ((name == "mesh_symmetry" && !engine.features.on("symmetry_check")) ||
        (name == "auto_texture" && !engine.features.on("auto_texture")) ||
        (name.rfind("macro_", 0) == 0 && !engine.features.on("macro_recorder")))
      continue;
    std::string desc = cmd.description.substr(0, cmd.description.find(". "));
    if (desc.size() > 90) desc = desc.substr(0, 87) + "...";
    std::string n = name;
    add(T_("Command"), n, desc, [this, n] {
      json r;
      if (runCommand(n.c_str(), json::object(), &r)) {
        std::string s = r.dump();
        if (s.size() > 160) s = s.substr(0, 157) + "...";
        setStatus(n + ": " + s);
        PLOG_I("%s -> %s", n.c_str(), r.dump().c_str());
      }
    });
  }
  // materials, brushes, layers, recent files
  for (auto& e : engine.materials.entries()) {
    if (e->transient) continue;
    std::string id = e->def.id;
    add(T_("Material"), e->def.name, e->def.category, [this, id] { engine.selectMaterial(id); });
  }
  for (auto& b : engine.brushes.presets()) {
    std::string id = b.id;
    add(T_("Brush"), b.name, b.category, [this, id] { engine.selectBrush(id); });
  }
  for (int i = (int)d.layers.size() - 1; i >= 0; --i) {
    uint32_t uid = d.layers[i]->uid;
    add(T_("Layer"), d.layers[i]->name, "", [this, uid] {
      int idx = engine.doc.indexOf(uid);
      if (idx >= 0) engine.doc.selected = idx;
    });
  }
  for (auto& f : recentFiles) {
    std::string path = f;
    add(T_("Open recent"), fs::path(f).filename().string(), f, [this, path] {
      replaceProject(T_("open another file"), [this, path] {
        std::string err;
        if (!engine.openFile(path, &err)) showError(F_("Could not open %s", path.c_str()), err);
        else addRecentFile(path);
      });
    });
  }
  // feature switches
  for (auto& f : Features::all()) {
    std::string id = f.id;
    bool on = engine.features.on(id);
    add(T_("Feature"), on ? F_("Switch off: %s", T_(f.name)) : F_("Switch on: %s", T_(f.name)), "", [this, id, on] {
      engine.features.set(id, !on);
      saveSession(true);
      const char* name = T_(Features::find(id)->name);
      setStatus(on ? F_("Switched off: %s", name) : F_("Switched on: %s", name));
    });
  }
}

void App::drawCommandPalette() {
  if (!showPalette) return;
  if (!engine.features.on("command_palette")) {
    showPalette = false;
    return;
  }
  if (paletteFocus) buildPaletteEntries();
  ImGuiViewport* vp = ImGui::GetMainViewport();
  float w = std::min(640.f * uiScale, vp->WorkSize.x - 40.f);
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + (vp->WorkSize.x - w) * 0.5f, vp->WorkPos.y + 60.f * uiScale));
  ImGui::SetNextWindowSize(ImVec2(w, 0));
  if (paletteFocus) ImGui::SetNextWindowFocus();  // once: a click elsewhere closes it
  if (!ImGui::Begin("##palette", nullptr,
                    ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  if (paletteFocus) {
    ImGui::SetKeyboardFocusHere();
    paletteFocus = false;
  }
  ImGui::SetNextItemWidth(-1);
  bool enter = ImGui::InputTextWithHint("##q", T_("Search actions, commands, materials, layers, files..."), paletteQuery,
                                        sizeof paletteQuery, ImGuiInputTextFlags_EnterReturnsTrue);
  std::string q = paletteQuery;
  // rank: score, then recently used, then alphabetical
  std::vector<std::pair<int, int>> hits;  // score, index
  for (int i = 0; i < (int)paletteEntries.size(); ++i) {
    int s = fuzzyScore(paletteEntries[i].label, q);
    if (s < 0) continue;
    auto rec = std::find(paletteRecent.begin(), paletteRecent.end(), paletteEntries[i].label);
    if (rec != paletteRecent.end()) s += q.empty() ? 1000 - int(rec - paletteRecent.begin()) : 25;
    hits.push_back({s, i});
  }
  std::stable_sort(hits.begin(), hits.end(), [](auto& a, auto& b) { return a.first > b.first; });
  const int shown = std::min<int>((int)hits.size(), 14);
  if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) paletteSel = std::min(paletteSel + 1, std::max(0, shown - 1));
  if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) paletteSel = std::max(paletteSel - 1, 0);
  paletteSel = std::clamp(paletteSel, 0, std::max(0, shown - 1));
  int run = -1;
  for (int k = 0; k < shown; ++k) {
    const PaletteEntry& e = paletteEntries[hits[k].second];
    ImGui::PushID(k);
    if (ImGui::Selectable(e.label.c_str(), k == paletteSel)) run = hits[k].second;
    if (!e.hint.empty()) {
      ImGui::SameLine(w * 0.62f);
      ImGui::TextDisabled("%s", e.hint.c_str());
    }
    ImGui::PopID();
  }
  bool askAssistant = false;
  if (hits.empty() && !q.empty() && engine.features.on("patina_assistant")) {
    // nothing matches: the words may be a request
    if (ImGui::Selectable(F_("Ask the Assistant: %s", q.c_str()).c_str(), true) || enter) askAssistant = true;
  } else if (hits.empty())
    ImGui::TextDisabled("%s", T_("Nothing matches"));
  else if ((int)hits.size() > shown) ImGui::TextDisabled(T_("%zu more: keep typing"), hits.size() - shown);
  if (enter && shown > 0) run = hits[paletteSel].second;
  bool close = ImGui::IsKeyPressed(ImGuiKey_Escape) || (!ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !paletteFocus);
  ImGui::End();
  if (askAssistant) {
    showPalette = false;
    paletteQuery[0] = 0;
    openAssistant(q);
  } else if (run >= 0) {
    PaletteEntry e = paletteEntries[run];  // copy: running it may rebuild the list
    paletteRecent.erase(std::remove(paletteRecent.begin(), paletteRecent.end(), e.label), paletteRecent.end());
    paletteRecent.insert(paletteRecent.begin(), e.label);
    if (paletteRecent.size() > 20) paletteRecent.resize(20);
    showPalette = false;
    e.run();
  } else if (close) {
    showPalette = false;
  }
}

}  // namespace patina
