// Preferences that persist between sessions: keyboard shortcuts (keymap.json), the
// session (session.json: brush, material, tool, colors, recent files, UI scale,
// navigation preset), plus the error popup and the UI-scale rebuild.
#include <cfloat>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_internal.h>

#include <cstring>
#include <filesystem>

#include "core/Log.h"
#include "core/Paths.h"
#include "ui/App.h"

namespace patina {

namespace fs = std::filesystem;

const char* navPresetName(NavPreset p) {
  static const char* n[] = {N_("Patina (Alt+LMB orbit, MMB orbit, Shift+MMB pan)"),
                            N_("Blender (MMB orbit, Shift+MMB pan, Ctrl+MMB zoom)"),
                            N_("Maya / Substance (Alt+LMB orbit, Alt+MMB pan, Alt+RMB zoom)"),
                            N_("ZBrush style (RMB orbit, Alt+RMB pan, Shift+RMB zoom)")};
  return n[std::clamp((int)p, 0, (int)NavPreset::Count - 1)];
}

// ------------------------------------------------------------------ shortcut names

std::string chordName(ImGuiKeyChord c, bool pretty) {
  if (!c) return "";
  std::string s;
  if (c & ImGuiMod_Ctrl) s += "Ctrl+";
  if (c & ImGuiMod_Shift) s += "Shift+";
  if (c & ImGuiMod_Alt) s += "Alt+";
  if (c & ImGuiMod_Super) s += "Super+";
  ImGuiKey k = (ImGuiKey)(c & ~ImGuiMod_Mask_);
  std::string name = k == ImGuiKey_None ? "?" : ImGui::GetKeyName(k);
  if (pretty) {
    static const std::pair<const char*, const char*> nice[] = {
        {"KeypadDivide", "Numpad /"}, {"KeypadMultiply", "Numpad *"}, {"KeypadSubtract", "Numpad -"},
        {"KeypadAdd", "Numpad +"},    {"KeypadDecimal", "Numpad ."},  {"KeypadEnter", "Numpad Enter"},
        {"LeftBracket", "["},         {"RightBracket", "]"},          {"Apostrophe", "'"},
        {"Comma", ","},               {"Period", "."},                {"Slash", "/"},
        {"Semicolon", ";"},           {"Equal", "="},                 {"Minus", "-"},
        {"Backslash", "\\"},          {"GraveAccent", "`"}};
    for (auto& [from, to] : nice)
      if (name == from) name = to;
    if (name.rfind("Keypad", 0) == 0 && name.size() == 7) name = "Numpad " + name.substr(6);
  }
  return s + name;
}

ImGuiKeyChord chordFromName(const std::string& name) {
  ImGuiKeyChord mods = 0;
  std::string rest = name;
  for (;;) {
    size_t p = rest.find('+');
    if (p == std::string::npos || p + 1 >= rest.size()) break;
    std::string m = rest.substr(0, p);
    if (m == "Ctrl") mods |= ImGuiMod_Ctrl;
    else if (m == "Shift") mods |= ImGuiMod_Shift;
    else if (m == "Alt") mods |= ImGuiMod_Alt;
    else if (m == "Super") mods |= ImGuiMod_Super;
    else break;
    rest = rest.substr(p + 1);
  }
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k)
    if (rest == ImGui::GetKeyName((ImGuiKey)k)) return mods | k;
  return 0;
}

// ------------------------------------------------------------------ keymap

const char* keymapPresetName(KeymapPreset p) { return p == KeymapPreset::Blender ? "Blender" : "Patina"; }

// Every rebindable action with its default in both keymaps. The Blender column follows
// Blender's own keys where Blender has the same action (texture paint: S samples a color,
// X flips colors, F / Shift+F resize the brush and change its strength (0.40: G / Shift+G flow / hardness, Ctrl+F
// the angle, as Blender's texture angle), Shift+D
// duplicates, Shift+A adds, Ctrl+G groups, Numpad / isolates, Home frames, Ctrl+Space
// maximizes the view). Actions Blender has no key for keep Patina's key.
void App::initKeymap(KeymapPreset preset) {
  const ImGuiKeyChord C = ImGuiMod_Ctrl, S = ImGuiMod_Shift, A = ImGuiMod_Alt;
  keymap = {
      {"app.palette", N_("Command palette (search everything)"), N_("App"), C | S | ImGuiKey_P, ImGuiKey_F3, 0, 0},
      // (0.46) the keys other programs search with: F3 (Blender), Ctrl+K
      {"app.palette_alt", N_("Command palette (another key)"), N_("App"), ImGuiKey_F3, C | ImGuiKey_K, 0, 0},
      {"file.open", N_("Open"), N_("File"), C | ImGuiKey_O, C | ImGuiKey_O, 0, 0},
      {"file.save", N_("Save"), N_("File"), C | ImGuiKey_S, C | ImGuiKey_S, 0, 0},
      {"file.save_as", N_("Save as"), N_("File"), C | S | ImGuiKey_S, C | S | ImGuiKey_S, 0, 0},
      {"file.export", N_("Export textures"), N_("File"), C | ImGuiKey_E, C | ImGuiKey_E, 0, 0},
      {"file.export_again", N_("Export again (last settings)"), N_("File"), C | S | ImGuiKey_E, C | S | ImGuiKey_E, 0, 0},
      {"file.save_incremental", N_("Save incremental (name_v002)"), N_("File"), C | A | ImGuiKey_S, C | A | ImGuiKey_S, 0, 0},
      {"file.render", N_("Render image"), N_("File"), ImGuiKey_F12, ImGuiKey_F12, 0, 0},
      {"file.quit", N_("Quit"), N_("File"), C | ImGuiKey_Q, C | ImGuiKey_Q, 0, 0},
      {"edit.undo", N_("Undo"), N_("Edit"), C | ImGuiKey_Z, C | ImGuiKey_Z, 0, 0},
      {"edit.redo", N_("Redo"), N_("Edit"), C | S | ImGuiKey_Z, C | S | ImGuiKey_Z, 0, 0},
      {"edit.redo_alt", N_("Redo (alternative)"), N_("Edit"), C | ImGuiKey_Y, 0, 0, 0},
      {"layer.add_menu", N_("Add menu (layers, folders, smart materials)"), N_("Layers"), 0, S | ImGuiKey_A, 0, 0},
      {"layer.new_paint", N_("New paint layer"), N_("Layers"), C | ImGuiKey_L, 0, 0, 0},
      {"layer.group", N_("Group into a folder"), N_("Layers"), C | ImGuiKey_G, C | ImGuiKey_G, 0, 0},
      {"layer.duplicate", N_("Duplicate layer"), N_("Layers"), C | ImGuiKey_D, S | ImGuiKey_D, 0, 0},
      {"layer.instance", N_("Make an instance (linked duplicate)"), N_("Layers"), A | ImGuiKey_D, A | ImGuiKey_D, 0, 0},
      {"layer.delete", N_("Delete layer"), N_("Layers"), ImGuiKey_Delete, ImGuiKey_Delete, 0, 0},
      {"layer.solo", N_("Solo the selected layer (toggle)"), N_("Layers"), A | ImGuiKey_S, ImGuiKey_KeypadDivide, 0, 0},
      {"tool.paint", N_("Paint"), N_("Tools"), ImGuiKey_B, ImGuiKey_B, 0, 0},
      {"tool.erase", N_("Erase"), N_("Tools"), ImGuiKey_E, ImGuiKey_E, 0, 0},
      {"tool.blur", N_("Blur"), N_("Tools"), ImGuiKey_U, 0, 0, 0},
      {"tool.clone", N_("Clone"), N_("Tools"), ImGuiKey_C, ImGuiKey_C, 0, 0},
      {"tool.smudge", N_("Smudge"), N_("Tools"), ImGuiKey_J, ImGuiKey_J, 0, 0},
      {"tool.fill", N_("Polygon fill"), N_("Tools"), ImGuiKey_P, ImGuiKey_P, 0, 0},
      {"tool.picker", N_("Color picker (hold for a quick pick)"), N_("Tools"), ImGuiKey_I, 0, 0, 0},
      {"tool.select", N_("Select faces"), N_("Tools"), ImGuiKey_S, 0, 0, 0},
      {"tool.seams", N_("Mark seams"), N_("Tools"), ImGuiKey_M, ImGuiKey_M, 0, 0},
      {"tool.island", N_("Island brush (paint a region, then make it an island)"), N_("Tools"), ImGuiKey_L, ImGuiKey_L, 0, 0},
      {"tool.note", N_("Note: pin a comment to the model"), N_("Tools"), ImGuiKey_N, ImGuiKey_N, 0, 0},
      {"tool.zone_line", N_("Retopology: zone lines"), N_("Tools"), 0, 0, 0, 0},
      {"tool.patch", N_("Retopology: choose a zone"), N_("Tools"), 0, 0, 0, 0},
      {"tool.flow", N_("Retopology: flow strokes"), N_("Tools"), 0, 0, 0, 0},
      {"tool.density", N_("Retopology: density brush"), N_("Tools"), 0, 0, 0, 0},
      {"island.make", N_("Make an island from the painted / selected faces"), N_("UV"), ImGuiKey_Enter, ImGuiKey_U, 0, 0},
      {"select.all", N_("Select all faces, or none when some are selected"), N_("UV"), ImGuiKey_A, ImGuiKey_A, 0, 0},
      {"select.none", N_("Select no faces"), N_("UV"), A | ImGuiKey_A, A | ImGuiKey_A, 0, 0},
      {"select.grow", N_("Grow the face selection"), N_("UV"), C | ImGuiKey_KeypadAdd, C | ImGuiKey_KeypadAdd, 0, 0},
      {"select.shrink", N_("Shrink the face selection"), N_("UV"), C | ImGuiKey_KeypadSubtract, C | ImGuiKey_KeypadSubtract, 0, 0},
      {"uv.unpin", N_("Unpin the selected UV islands (3D view and UV editor)"), N_("UV"), A | ImGuiKey_P, A | ImGuiKey_P, 0, 0},
      {"color.sample", N_("Sample the color under the cursor"), N_("Painting"), 0, ImGuiKey_S, 0, 0},
      {"color.swap", N_("Swap primary / secondary color"), N_("Painting"), ImGuiKey_X, ImGuiKey_X, 0, 0},
      {"brush.smaller", N_("Brush smaller"), N_("Painting"), ImGuiKey_LeftBracket, ImGuiKey_LeftBracket, 0, 0},
      {"brush.bigger", N_("Brush bigger"), N_("Painting"), ImGuiKey_RightBracket, ImGuiKey_RightBracket, 0, 0},
      {"brush.radius", N_("Brush size (move the mouse, click to confirm)"), N_("Painting"), 0, ImGuiKey_F, 0, 0},
      {"brush.strength", N_("Brush opacity, Blender's strength (move the mouse, click to confirm)"), N_("Painting"), 0, S | ImGuiKey_F, 0, 0},
      {"brush.flow", N_("Brush flow (move the mouse, click to confirm)"), N_("Painting"), 0, ImGuiKey_G, 0, 0},
      {"brush.hardness", N_("Brush hardness (move the mouse, click to confirm)"), N_("Painting"), 0, S | ImGuiKey_G, 0, 0},
      {"brush.angle", N_("Brush angle (move the mouse round, click to confirm)"), N_("Painting"), 0, C | ImGuiKey_F, 0, 0},
      {"sym.x", N_("Toggle X mirror"), N_("Painting"), S | ImGuiKey_X, S | ImGuiKey_X, 0, 0},
      {"brush.lock_topology", N_("Topology lock: stay on the connected part of the mesh"), N_("Painting"), A | ImGuiKey_T, A | ImGuiKey_T, 0, 0},
      {"brush.lock_island", N_("Island lock: stay inside the UV island"), N_("Painting"), A | ImGuiKey_L, A | ImGuiKey_L, 0, 0},
      {"brush.lock_set", N_("Set lock: stay inside the texture set / material slot"), N_("Painting"), 0, 0, 0, 0},
      {"view.isolate_set", N_("Isolate the current texture set in the viewport"), N_("View"), 0, 0, 0, 0},
      {"view.retopo_preview", N_("Retopology preview on / off"), N_("View"), 0, 0, 0, 0},
      {"view.back", N_("Back to the view before the side / top view (again: the side view)"), N_("View"), ImGuiKey_Keypad0, ImGuiKey_Keypad0, 0, 0},
      {"view.pie", N_("View pie: hold over the 3D view (lit, solid, path traced, wireframe, seams...)"), N_("View"), ImGuiKey_Z, ImGuiKey_Z, 0, 0},
      {"view.pie_channels", N_("Channel pie: hold over the 3D view (base color, normal, roughness...)"), N_("View"), S | ImGuiKey_Z, S | ImGuiKey_Z, 0, 0},
      {"tool.pie", N_("Edit mode pie: hold over the 3D view (paint, select, island, seams, retopology...)"), N_("Tools"), ImGuiKey_GraveAccent, ImGuiKey_GraveAccent, 0, 0},
      {"app.favorites", N_("Quick Favorites pie: hold over the 3D view (your own eight)"), N_("App"), ImGuiKey_Q, ImGuiKey_Q, 0, 0},
      {"view.lit", N_("Lit: the material (the usual view)"), N_("View"), 0, 0, 0, 0},
      {"view.solid", N_("Solid: the shape alone (again: the view before)"), N_("View"), 0, 0, 0, 0},
      {"view.unlit", N_("Unlit: the base color (again: the view before)"), N_("View"), 0, 0, 0, 0},
      {"view.path_traced", N_("Path traced (again: the view before)"), N_("View"), 0, 0, 0, 0},
      {"view.wireframe", N_("Wireframe on / off"), N_("View"), 0, 0, 0, 0},
      {"view.seams", N_("Seams on / off"), N_("View"), 0, 0, 0, 0},
      {"macro.record", N_("Macro: start / stop recording"), N_("App"), 0, 0, 0, 0},
      {"macro.play", N_("Macro: play the recorded steps"), N_("App"), 0, 0, 0, 0},
      {"stencil.edit", N_("Stencil: hold and drag to move, Shift+drag to rotate, wheel to scale"), N_("Painting"), ImGuiKey_T, ImGuiKey_T, 0, 0},
      {"stencil.toggle", N_("Stencil on / off"), N_("Painting"), S | ImGuiKey_T, S | ImGuiKey_T, 0, 0},
      {"view.focus", N_("Hide panels (distraction-free)"), N_("View"), ImGuiKey_Tab, C | ImGuiKey_Space, 0, 0},
      {"view.frame", N_("Frame mesh"), N_("View"), ImGuiKey_F, ImGuiKey_Home, 0, 0},
      {"view.frame_alt", N_("Frame mesh (alternative)"), N_("View"), 0, 0, 0, 0},
      // (1.2.11) Shift+F; Numpad . in the Blender keymap, as Blender's View Selected (it framed the mesh before)
      {"view.frame_selected", N_("Frame the selection (the model when nothing is selected)"), N_("View"), S | ImGuiKey_F,
       ImGuiKey_KeypadDecimal, 0, 0},
      {"view.help", N_("Shortcuts window"), N_("View"), ImGuiKey_F1, ImGuiKey_F1, 0, 0},
      {"view.compare", N_("Split / compare view on / off"), N_("View"), ImGuiKey_Backslash, ImGuiKey_Backslash, 0, 0},
      {"view.compare_snapshot", N_("Compare: take a snapshot of the textures"), N_("View"), S | ImGuiKey_Backslash, S | ImGuiKey_Backslash, 0, 0},
      {"blender.send", N_("Send textures to Blender"), N_("Blender"), C | ImGuiKey_B, C | ImGuiKey_B, 0, 0},
      {"file.add_model", N_("Add a model to the scene (an object with its own layers)"), N_("File"), C | S | ImGuiKey_I, C | S | ImGuiKey_I, 0, 0},
      {"tool.object", N_("Object tool: pick, move, turn and scale the scene's objects (again: the next of the three)"), N_("Tools"), ImGuiKey_W, ImGuiKey_W, 0, 0},
      // (1.2.3) in the Object tool, as in Blender's Object Mode: the mouse moves, turns or scales the object (these keys
      // come before the same keys' other uses there)
      {"object.move", N_("Object tool: move the object with the mouse (X, Y, Z: one axis; click: keep it)"), N_("Tools"), ImGuiKey_G, ImGuiKey_G, 0, 0},
      {"object.turn", N_("Object tool: turn the object about the line of sight (X, Y, Z: an axis; click: keep it)"), N_("Tools"), ImGuiKey_R, ImGuiKey_R, 0, 0},
      {"object.scale", N_("Object tool: scale the object evenly (X, Y, Z: along one axis; click: keep it)"), N_("Tools"), ImGuiKey_S, ImGuiKey_S, 0, 0},
      {"view.local", N_("Only the active object (Local view) on / off"), N_("View"), ImGuiKey_Slash, ImGuiKey_Slash, 0, 0},
  };
  keymapPreset = preset;
  for (auto& a : keymap) a.chord = a.defaultChord = preset == KeymapPreset::Blender ? a.blenderChord : a.patinaChord;
}

void App::applyKeymapPreset(KeymapPreset preset, bool alsoNavigation) {
  initKeymap(preset);
  keymapCapture.clear();
  if (alsoNavigation) {
    navPreset = preset == KeymapPreset::Blender ? NavPreset::Blender : NavPreset::Patina;
    ctrlPaintsSecondary = preset == KeymapPreset::Blender;
  }
  saveKeymap();
  saveSession(true);
  setStatus(F_("%s keyboard shortcuts", keymapPresetName(preset)));
}

void App::loadKeymap() {
  initKeymap();
  bool ok = false;
  std::string text = paths::readFile(paths::configDir() / "keymap.json", &ok);
  if (!ok) return;
  try {
    json j = json::parse(text);
    json bindings = j;  // 0.3.0 files are a flat {action: key} map in the Patina keymap
    if (j.contains("bindings")) {
      initKeymap(j.value("preset", std::string()) == "blender" ? KeymapPreset::Blender : KeymapPreset::Patina);
      bindings = j["bindings"];
    }
    // defaults that changed between versions: an old file that still has the old
    // default gets the new one (0.4: U makes an island in the Blender keymap, not Blur)
    struct Changed {
      int before;  // keymap.json version the old default belongs to (< before)
      const char* id;
      KeymapPreset preset;
      const char* oldDefault;
    };
    static const Changed changed[] = {{4, "tool.blur", KeymapPreset::Blender, "U"},
                                      {5, "view.frame_alt", KeymapPreset::Blender, "KeypadDecimal"}};  // (1.2.11: View Selected)
    int version = j.value("version", 3);
    for (auto& a : keymap) {
      if (!bindings.contains(a.id) || !bindings[a.id].is_string()) continue;
      std::string v = bindings[a.id].get<std::string>();
      bool stale = false;
      for (auto& c : changed)
        if (version < c.before && keymapPreset == c.preset && !std::strcmp(c.id, a.id) && v == c.oldDefault) stale = true;
      if (stale) continue;
      a.chord = v.empty() ? 0 : chordFromName(v);
    }
  } catch (std::exception& e) {
    PLOG_W("keymap.json: %s", e.what());
  }
}

void App::saveKeymap() {
  json b = json::object();
  for (auto& a : keymap) b[a.id] = chordName(a.chord);
  json j = {{"version", 5}, {"preset", keymapPreset == KeymapPreset::Blender ? "blender" : "patina"}, {"bindings", b}};
  if (!paths::writeFileAtomic(paths::ensureDir(paths::configDir()) / "keymap.json", j.dump(1)))
    setStatus(T_("Could not save keymap.json"));
}

// (1.2.3) the keys that work in the Object tool only: there they come before the same keys' other uses (S selects
// faces, X swaps colors...), and elsewhere they do nothing
static bool objectToolKey(const char* id) { return !std::strncmp(id, "object.", 7); }

bool App::keyPressed(const char* id, bool repeat) const {
  if (!forcedAction.empty() && forcedAction == id) {
    forcedAction.clear();  // once
    return true;
  }
  if (objectModal >= 0) return false;  // (1.2.3) G, R or S under way: its own keys (X, Y, Z, numbers, Enter, Esc)
  const bool objectKeys = tool == ViewportTool::Object && engine.features.on("object_keys");
  for (auto& a : keymap)
    if (!std::strcmp(a.id, id)) {
      if (!a.chord) return false;
      if (objectToolKey(id) && !objectKeys) return false;
      if (objectKeys && !objectToolKey(id))
        for (auto& b : keymap)
          if (objectToolKey(b.id) && b.chord == a.chord) return false;
      ImGuiKey k = (ImGuiKey)(a.chord & ~ImGuiMod_Mask_);
      ImGuiKeyChord mods = a.chord & ImGuiMod_Mask_;
      return ImGui::IsKeyPressed(k, repeat) && (ImGuiKeyChord)ImGui::GetIO().KeyMods == mods;
    }
  return false;
}

bool App::keyHeld(const char* id) const {
  for (auto& a : keymap)
    if (!std::strcmp(a.id, id)) {
      if (!a.chord) return false;
      return ImGui::IsKeyDown((ImGuiKey)(a.chord & ~ImGuiMod_Mask_));
    }
  return false;
}

const char* App::keyLabel(const char* id) const {
  static std::string buf[8];
  static int n = 0;
  for (auto& a : keymap)
    if (!std::strcmp(a.id, id)) {
      std::string& b = buf[n++ & 7];
      b = chordName(a.chord, true);
      return b.empty() ? nullptr : b.c_str();
    }
  return nullptr;
}

void App::drawKeymapEditor() {
  if (!showKeymap) return;
  ImGui::SetNextWindowSize(ImVec2(560, 600) * uiScale, ImGuiCond_FirstUseEver);
  if (!ImGui::Begin(L_("Keyboard shortcuts"), &showKeymap)) {
    ImGui::End();
    return;
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(T_("Keymap"));
  ImGui::SameLine();
  for (int p = 0; p < (int)KeymapPreset::Count; ++p) {
    if (p) ImGui::SameLine();
    if (ImGui::RadioButton(keymapPresetName((KeymapPreset)p), (int)keymapPreset == p) && (int)keymapPreset != p)
      applyKeymapPreset((KeymapPreset)p, true);
  }
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Blender: the keys Blender uses for the same actions (S samples a color, F resizes the brush,\n"
                               "Shift+D duplicates, Shift+A adds...). Switching also sets the mouse navigation;\n"
                               "change it back in Edit > Preferences. Your own changes to the keys are replaced."));
  ImGui::TextWrapped(T_("Click a shortcut, then press the new key combination. Esc cancels, Backspace clears it. "
                        "Saved to %s."),
                     (paths::configDir() / "keymap.json").string().c_str());
  if (ImGui::Button(L_("Reset all to defaults"))) {
    for (auto& a : keymap) a.chord = a.defaultChord;
    saveKeymap();
  }
  ImGui::SameLine();
  ImGui::TextDisabled(T_("(defaults of the %s keymap)"), keymapPresetName(keymapPreset));
  ImGui::Separator();
  if (!keymapCapture.empty()) {
    // wait for a non-modifier key
    for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END; ++k) {
      ImGuiKey key = (ImGuiKey)k;
      if (!ImGui::IsKeyboardKey(key) || ImGui::IsLRModKey(key))
        continue;
      if (!ImGui::IsKeyPressed(key, false)) continue;
      for (auto& a : keymap)
        if (keymapCapture == a.id) {
          if (key == ImGuiKey_Escape) break;
          a.chord = key == ImGuiKey_Backspace ? 0 : (ImGuiKeyChord)(ImGui::GetIO().KeyMods | key);
          saveKeymap();
        }
      keymapCapture.clear();
      break;
    }
  }
  if (ImGui::BeginTable("##keys", 3, ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY)) {
    ImGui::TableSetupColumn(L_("Action"), ImGuiTableColumnFlags_WidthStretch);
    ImGui::TableSetupColumn(L_("Shortcut"), ImGuiTableColumnFlags_WidthFixed, 170 * uiScale);
    ImGui::TableSetupColumn("##reset", ImGuiTableColumnFlags_WidthFixed, 60 * uiScale);
    ImGui::TableSetupScrollFreeze(0, 1);
    ImGui::TableHeadersRow();
    const char* group = "";
    for (auto& a : keymap) {
      if (std::strcmp(group, a.group)) {
        group = a.group;
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::TextDisabled("%s", T_(group));
      }
      ImGui::PushID(a.id);
      ImGui::TableNextRow();
      ImGui::TableSetColumnIndex(0);
      ImGui::TextUnformatted(T_(a.label));
      ImGui::TableSetColumnIndex(1);
      // conflicts: the same chord on two actions
      bool clash = false;
      // ((1.2.3) the Object tool's own keys don't clash with the keys of other tools)
      for (auto& b : keymap) clash |= &b != &a && b.chord && b.chord == a.chord && objectToolKey(a.id) == objectToolKey(b.id);
      if (keymapScrollTo && keymapCapture == a.id) ImGui::SetScrollHereY(0.3f), keymapScrollTo = false;  // (asked from a toolbar button)
      std::string label = keymapCapture == a.id ? T_("press a key...") : a.chord ? chordName(a.chord, true) : T_("(none)");
      if (clash) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.55f, 0.4f, 1));
      if (ImGui::Button(label.c_str(), ImVec2(-1, 0))) keymapCapture = a.id;
      if (clash) {
        ImGui::PopStyleColor();
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Also used by another action"));
      }
      ImGui::TableSetColumnIndex(2);
      if (a.chord != a.defaultChord && ImGui::SmallButton(L_("reset"))) {
        a.chord = a.defaultChord;
        saveKeymap();
      }
      ImGui::PopID();
    }
    ImGui::EndTable();
  }
  ImGui::End();
}

// ------------------------------------------------------------------ session

static json v3(vec3 c) { return json::array({c.x, c.y, c.z}); }
static vec3 v3(const json& j, vec3 d) {
  if (!j.is_array() || j.size() < 3) return d;
  return {j[0].get<float>(), j[1].get<float>(), j[2].get<float>()};
}

void App::addRecentFile(const std::string& path) {
  if (path.empty()) return;
  std::error_code ec;
  std::string p = fs::weakly_canonical(path, ec).string();
  if (p.empty()) p = path;
  if (p.find("autosave.patina") != std::string::npos) return;
  recentFiles.erase(std::remove(recentFiles.begin(), recentFiles.end(), p), recentFiles.end());
  recentFiles.insert(recentFiles.begin(), p);
  if (recentFiles.size() > 12) recentFiles.resize(12);
  saveSession(true);
}

void App::noteColorUsed(vec3 c) {
  for (size_t i = 0; i < recentColors.size(); ++i)
    if (glm::length(recentColors[i] - c) < 1e-3f) {
      recentColors.erase(recentColors.begin() + i);
      break;
    }
  recentColors.insert(recentColors.begin(), c);
  if (recentColors.size() > 16) recentColors.resize(16);
}

void App::loadSession() {
  bool ok = false;
  std::string text = paths::readFile(paths::configDir() / "session.json", &ok);
  if (!ok) {
    // first start: a small default palette
    swatches = {{0.80f, 0.12f, 0.10f}, {0.95f, 0.55f, 0.10f}, {0.95f, 0.85f, 0.25f}, {0.25f, 0.60f, 0.20f},
                {0.15f, 0.40f, 0.80f}, {0.45f, 0.25f, 0.65f}, {0.55f, 0.35f, 0.20f}, {0.92f, 0.90f, 0.86f},
                {0.50f, 0.50f, 0.50f}, {0.08f, 0.08f, 0.09f}};
    layoutMode = LayoutMode::Guided;  // (0.46) a first start: the Guided layout (a Patina used before keeps the layout it had)
    firstStart = true;
    // (0.47) and the brush paints a color, not a material: a newcomer who picks the brush expects one
    engine.painter.material = MaterialRef();
    engine.painter.material.flat.color = vec3(0.72f, 0.18f, 0.13f);
    engine.painter.material.flat.roughness = 0.45f;
    engine.selectedMaterialId.clear();
    return;
  }
  try {
    json j = json::parse(text);
    recentFiles = j.value("recent_files", std::vector<std::string>());
    swatches.clear();
    for (auto& c : j.value("swatches", json::array())) swatches.push_back(v3(c, vec3(1)));
    recentColors.clear();
    for (auto& c : j.value("recent_colors", json::array())) recentColors.push_back(v3(c, vec3(1)));
    secondaryColor = v3(j.value("secondary_color", json()), secondaryColor);
    userUiScale = std::clamp(j.value("ui_scale", 1.f), 0.5f, 3.f);
    navPreset = (NavPreset)std::clamp(j.value("nav_preset", 0), 0, (int)NavPreset::Count - 1);
    ctrlPaintsSecondary = j.value("ctrl_paints_secondary", ctrlPaintsSecondary);
    autoOrtho = j.value("auto_ortho", autoOrtho);
    if (j.contains("quick_favorites") && j["quick_favorites"].is_array()) {
      favorites.clear();
      for (auto& f : j["quick_favorites"])
        if (f.is_string() && favorites.size() < 8) favorites.push_back(f.get<std::string>());
      favorites.resize(8);
    }
    animateViews = j.value("animate_views", animateViews);
    engine.features.fromJson(j.value("features", json::object()));
    toolbarNames = j.value("toolbar_names", toolbarNames);
    languageSaved = j.value("language", languageSaved);
    if (languageSaved != "en" && engine.features.on("translations")) {
      std::string err;
      if (!i18n::setLanguage(languageSaved, &err)) PLOG_W("Language %s: %s", languageSaved.c_str(), err.c_str());
    }
    paletteRecent = j.value("palette_recent", std::vector<std::string>());
    engine.assistantModelPath = j.value("assistant_model", std::string());
    assistant.history = j.value("assistant_history", std::vector<std::string>());
    islandOrient = std::clamp(j.value("island_orient", islandOrient), 0, 2);
    islandPinNew = j.value("island_pin", islandPinNew);
    islandReach = std::clamp(j.value("island_reach", islandReach), 0, 1);
    selectVisible = j.value("select_visible", selectVisible);
    {
      std::string layout = j.value("layout", std::string("full"));  // (1.2.7: "blender" too)
      layoutMode = layout == "guided" ? LayoutMode::Guided : layout == "blender" ? LayoutMode::Blender : LayoutMode::Full;
      blenderSessionLoad(j.value("blender_layout", json::object()));
    }
    ui::setTheme(ui::themeFromId(j.value("theme", std::string("patina"))));
    guidedStep = (GuidedStep)std::clamp(j.value("guided_step", 0), 0, (int)GuidedStep::Count - 1);
    islandDensityMode = std::clamp(j.value("island_density_mode", islandDensityMode), 0, 4);
    islandCustomDensity = j.value("island_custom_density", islandCustomDensity);
    renderW = j.value("render_width", renderW);
    materialList = j.value("materials_list", materialList);
    materialTile = std::clamp(j.value("materials_tile", materialTile), 48.f, 192.f);
    renderH = j.value("render_height", renderH);
    renderTransparent = j.value("render_transparent", renderTransparent);
    renderSS = j.value("render_supersample", renderSS);
    renderPathTraced = j.value("render_path_traced", renderPathTraced);
    renderSamples = std::clamp(j.value("render_samples", renderSamples), 1, 8192);
    {
      PathTraceSettings& pt = engine.viewOptions.pathTrace;
      json p = j.value("path_trace", json::object());
      pt.maxSamples = std::clamp(p.value("max_samples", pt.maxSamples), 1, 65536);
      pt.bounces = std::clamp(p.value("bounces", pt.bounces), 0, 32);
      pt.ground = p.value("ground", pt.ground);
      pt.clamp = std::clamp(p.value("clamp", pt.clamp), 0.f, 1000.f);
    }
    autoReloadMesh = j.value("auto_reload_mesh", autoReloadMesh);
    showWelcomeOnStart = j.value("show_welcome", showWelcomeOnStart);
    tutorialDismissed = j.value("tutorial_dismissed", tutorialDismissed);
    // the tool windows open when Patina last closed (their places are in layout.ini; Retopology is open,
    // as a tab, until closed)
    // 0.22: how lines behind the model are drawn, and whether new lines' ends snap
    engine.viewOptions.retopoLinesBehind = std::clamp(j.value("retopo_lines_behind", engine.viewOptions.retopoLinesBehind), 0, 2);
    // 0.37: the top bar's Retopology preview, and the axis gizmo
    engine.viewOptions.retopoPreview = j.value("retopo_preview", engine.viewOptions.retopoPreview);
    engine.viewOptions.axisGizmo = j.value("axis_gizmo", engine.viewOptions.axisGizmo);
    engine.retopoSnapEnds = j.value("retopo_snap_ends", engine.retopoSnapEnds);
    if (j.contains("windows") && j["windows"].is_object()) {
      const json& w = j["windows"];
      showRetopo = w.value("retopology", showRetopo);
      showFaceSets = w.value("face_sets", showFaceSets);
      assistant.show = w.value("assistant", assistant.show);
      showDensity = w.value("texel_density", showDensity);
      showSymmetry = w.value("mesh_symmetry", showSymmetry);
      showAutoTexture = w.value("auto_texture", showAutoTexture);
      showMacros = w.value("macros", showMacros);
      showMaterialEditor = w.value("material_editor", showMaterialEditor);
    }
    engine.autosaveMinutes = std::clamp(j.value("autosave_minutes", engine.autosaveMinutes), 0.0, 120.0);
    if (j.contains("undo") && j["undo"].is_object()) {  // (1.2.7)
      const json& u = j["undo"];
      try {
        engine.execute("undo_settings", u);
      } catch (...) {
      }
    }
    Painter& P = engine.painter;
    if (j.contains("brush")) {
      std::string id = j.value("brush_id", std::string());
      if (!id.empty()) engine.selectBrush(id);
      P.brush.fromJson(j["brush"]);
    }
    if (j.contains("material")) {
      std::string id = j.value("material_id", std::string());
      if (!id.empty() && engine.materials.find(id)) engine.selectMaterial(id);
      MaterialRef m = P.material;
      m.fromJson(j["material"]);
      if (!m.id.empty() && !engine.materials.find(m.id)) m.id.clear();
      P.material = m;
    }
    tool = (ViewportTool)std::clamp(j.value("tool", 0), 0, (int)ViewportTool::Island);
    if (tool == ViewportTool::SelectFaces || tool == ViewportTool::Seams || tool == ViewportTool::Island) tool = ViewportTool::Paint;
    P.channelMask = j.value("channel_mask", P.channelMask);
    if (j.value("channels_v", 1) < 2) P.channelMask = upgradeChannelMask(P.channelMask);
    if (j.contains("symmetry"))
      for (int i = 0; i < 3; ++i) P.symmetry[i] = j["symmetry"][i];
    P.radialCount = std::clamp(j.value("radial", 1), 1, 32);
    P.lockTopology = j.value("lock_topology", false);
    if (j.contains("pen") && j["pen"].is_object()) {
      const json& pj = j["pen"];
      pen.softness = std::clamp(pj.value("softness", 0.f), -1.f, 1.f);
      pen.minPressure = std::clamp(pj.value("min_pressure", 0.02f), 0.f, 0.5f);
      pen.fullPressure = std::clamp(pj.value("full_pressure", 1.f), 0.3f, 1.f);
      pen.eraserTipErases = pj.value("eraser_tip_erases", true);
      for (int b = 0; b < 2; ++b)
        pen.button[b] = (PenButtonAction)std::clamp(pj.value(b ? "button2" : "button1", b ? 1 : 0), 0, (int)PenButtonAction::Count - 1);
    }
    httpEnabled = j.value("http_enabled", false);
    httpLan = j.value("http_lan", false);
    httpPortSetting = std::clamp(j.value("http_port", 7870), 1, 65535);
    P.lockIsland = j.value("lock_island", false);
    P.radialAxis = std::clamp(j.value("radial_axis", 1), 0, 2);
    P.fillMode = (FillMode)std::clamp(j.value("fill_mode", (int)P.fillMode), 0, (int)FillMode::Material);
    P.heightMode = j.value("height_mode", P.heightMode);
    reprojectOnUVEdit = j.value("reproject_on_uv_edit", reprojectOnUVEdit);
    exportDir = j.value("export_dir", exportDir);
    exportTemplate = j.value("export_template", exportTemplate);
    exportPresetIdx = j.value("export_preset", exportPresetIdx);
  } catch (std::exception& e) {
    PLOG_W("session.json: %s", e.what());
  }
}

void App::saveSession(bool force) {
  if (!force && nowSeconds() - lastSessionSave < 5.0) return;
  lastSessionSave = nowSeconds();
  Painter& P = engine.painter;
  json sw = json::array(), rc = json::array();
  for (auto& c : swatches) sw.push_back(v3(c));
  for (auto& c : recentColors) rc.push_back(v3(c));
  json j = {{"recent_files", recentFiles},
            {"swatches", sw},
            {"recent_colors", rc},
            {"secondary_color", v3(secondaryColor)},
            {"ui_scale", userUiScale},
            {"nav_preset", (int)navPreset},
            {"ctrl_paints_secondary", ctrlPaintsSecondary},
            {"auto_ortho", autoOrtho},
            {"quick_favorites", favorites},
            {"animate_views", animateViews},
            {"toolbar_names", toolbarNames},
            {"language", languageSaved},
            {"island_orient", islandOrient},
            {"island_pin", islandPinNew},
            {"island_reach", islandReach},
            {"select_visible", selectVisible},
            {"layout", layoutMode == LayoutMode::Guided ? "guided" : layoutMode == LayoutMode::Blender ? "blender" : "full"},
            {"blender_layout", blenderSessionSave()},
            {"theme", ui::theme().id},
            {"guided_step", (int)guidedStep},
            {"island_density_mode", islandDensityMode},
            {"island_custom_density", islandCustomDensity},
            {"render_width", renderW},
            {"materials_list", materialList},
            {"materials_tile", materialTile},
            {"render_height", renderH},
            {"render_transparent", renderTransparent},
            {"render_supersample", renderSS},
            {"render_path_traced", renderPathTraced},
            {"render_samples", renderSamples},
            {"path_trace", {{"max_samples", engine.viewOptions.pathTrace.maxSamples}, {"bounces", engine.viewOptions.pathTrace.bounces},
                            {"ground", engine.viewOptions.pathTrace.ground}, {"clamp", engine.viewOptions.pathTrace.clamp}}},
            {"auto_reload_mesh", autoReloadMesh},
            {"show_welcome", showWelcomeOnStart},
            {"tutorial_dismissed", tutorialDismissed},
            {"retopo_lines_behind", engine.viewOptions.retopoLinesBehind},
            {"retopo_preview", engine.viewOptions.retopoPreview},
            {"axis_gizmo", engine.viewOptions.axisGizmo},
            {"retopo_snap_ends", engine.retopoSnapEnds},
            {"windows", {{"retopology", showRetopo}, {"face_sets", showFaceSets}, {"assistant", assistant.show}, {"texel_density", showDensity},
                         {"mesh_symmetry", showSymmetry}, {"auto_texture", showAutoTexture}, {"macros", showMacros},
                         {"material_editor", showMaterialEditor}}},
            {"autosave_minutes", engine.autosaveMinutes},
            {"undo", {{"steps", UndoStack::limits().steps}, {"ram_mb", UndoStack::limits().ramBytes >> 20},
                      {"gpu_mb", UndoStack::limits().gpuBytes >> 20}}},
            {"features", engine.features.toJson()},
            {"palette_recent", paletteRecent},
            {"assistant_model", engine.assistantModelPath},
            {"assistant_history", assistant.history},
            {"brush_id", engine.selectedBrushId},
            {"brush", P.brush.toJson()},
            {"material_id", engine.selectedMaterialId},
            {"material", P.material.toJson()},
            {"tool", (int)tool},
            {"channel_mask", P.channelMask},
            {"channels_v", 2},
            {"symmetry", {P.symmetry[0], P.symmetry[1], P.symmetry[2]}},
            {"radial", P.radialCount},
            {"lock_topology", P.lockTopology},
            {"http_enabled", httpEnabled},
            {"pen", {{"softness", pen.softness}, {"min_pressure", pen.minPressure}, {"full_pressure", pen.fullPressure},
                     {"eraser_tip_erases", pen.eraserTipErases}, {"button1", (int)pen.button[0]}, {"button2", (int)pen.button[1]}}},
            {"http_lan", httpLan},
            {"http_port", httpPortSetting},
            {"lock_island", P.lockIsland},
            {"radial_axis", P.radialAxis},
            {"fill_mode", (int)P.fillMode},
            {"height_mode", P.heightMode},
            {"reproject_on_uv_edit", reprojectOnUVEdit},
            {"export_dir", exportDir},
            {"export_template", exportTemplate},
            {"export_preset", exportPresetIdx}};
  std::string dump = j.dump(1);
  if (dump == lastSessionDump) return;
  if (paths::writeFileAtomic(paths::ensureDir(paths::configDir()) / "session.json", dump)) lastSessionDump = dump;
}

// ------------------------------------------------------------------ UI scale

void App::applyUiScale(float user) {
  userUiScale = std::clamp(user, 0.5f, 3.f);
  pendingUiScale = userUiScale;  // rebuilt before the next frame (the font atlas can't change mid-frame)
}

// ------------------------------------------------------------------ preferences

void App::drawPreferences() {
  if (!showPreferences) return;
  // (0.36: as tall as it needs, but no taller than the window: then it scrolls)
  ImGui::SetNextWindowSizeConstraints(ImVec2(520 * uiScale, 0), ImVec2(520 * uiScale, ImGui::GetMainViewport()->WorkSize.y * 0.92f));
  if (!ImGui::Begin(L_("Preferences"), &showPreferences,
                    ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  ImGui::SeparatorText(T_("Keyboard shortcuts"));
  for (int p = 0; p < (int)KeymapPreset::Count; ++p) {
    if (p) ImGui::SameLine();
    if (ImGui::RadioButton(p ? L_("Blender keymap") : L_("Patina keymap"), (int)keymapPreset == p) && (int)keymapPreset != p)
      applyKeymapPreset((KeymapPreset)p, true);
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", keymapPreset == KeymapPreset::Blender
                               ? T_("Blender's keys: S samples a color, X flips colors, F / Shift+F set brush size / opacity "
                                    "(G / Shift+G flow / hardness, Ctrl+F the angle), "
                                    "Shift+A adds a layer, Shift+D duplicates, Ctrl+G groups, Numpad / solos, Home frames, "
                                    "Ctrl+Space hides the panels. Choosing a keymap also sets the navigation below.")
                               : T_("Patina's own keys (see Help > Shortcuts & usage). Choosing a keymap also sets the "
                                    "navigation below."));
  ImGui::PopStyleColor();
  if (ImGui::Button(L_("Edit individual shortcuts..."))) showKeymap = true;
  ImGui::SameLine();
  if (ImGui::Button(L_("Show all shortcuts"))) showHelp = true;
  ImGui::SeparatorText(T_("Mouse"));
  ImGui::SetNextItemWidth(-1);
  if (ImGui::BeginCombo("##nav", T_(navPresetName(navPreset)))) {
    for (int i = 0; i < (int)NavPreset::Count; ++i)
      if (ImGui::Selectable(L_(navPresetName((NavPreset)i)), (int)navPreset == i)) navPreset = (NavPreset)i;
    ImGui::EndCombo();
  }
  ImGui::Checkbox(L_("Hold Ctrl while painting to paint with the secondary color"), &ctrlPaintsSecondary);
  if (viewSnapping()) {
    ImGui::Checkbox(L_("Side, top and bottom views are orthographic"), &autoOrtho);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("The numpad, the axis gizmo and Alt while orbiting show them without perspective, until you\n"
                                 "turn away (Blender's Auto Perspective). Numpad 0 goes back to the view you had before."));
  }
  ImGui::Checkbox(L_("Turn to the side views (instead of jumping)"), &animateViews);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("A click on the axis gizmo turns the view in a quarter second."));
  if (prefsScrollTo == "pies") ImGui::SetScrollHereY(0.f), prefsScrollTo.clear();
  drawPiePrefs();
  ImGui::SeparatorText(T_("Display"));
  float pct = userUiScale * 100.f;
  ImGui::SetNextItemWidth(200 * uiScale);
  if (ImGui::SliderFloat(L_("UI scale"), &pct, 50.f, 300.f, T_("%.0f%%"))) userUiScale = pct / 100.f;
  if (ImGui::IsItemDeactivatedAfterEdit()) applyUiScale(userUiScale);
  // (1.2.7) the colors, and where the panels are
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(T_("Theme"));
  static const char* kThemeNames[] = {N_("Patina"), N_("Vivid Dark")};  // (ui::ThemeId's order)
  for (int t = 0; t < (int)ui::ThemeId::Count; ++t) {
    ImGui::SameLine();
    if (ImGui::RadioButton((std::string(T_(kThemeNames[t])) + "##theme").c_str(), (int)ui::themeId() == t)) setThemeUI((ui::ThemeId)t);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", t ? T_("KDE Plasma's Vivid Dark: navy panels and a cyan accent. The same colors as the\n"
                                     "\"Vivid Dark\" Blender theme, so the two programs look alike.")
                                : T_("Patina's own: gray panels and a copper accent."));
  }
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(T_("Layout"));
  {
    const bool withGuided = engine.features.on("guided_layout");
    const bool guided = layoutMode == LayoutMode::Guided && withGuided;
    if (withGuided) {
      ImGui::SameLine();
      if (ImGui::RadioButton(L_("Guided##prefs"), guided)) setLayoutMode(LayoutMode::Guided);
    }
    ImGui::SameLine();
    if (ImGui::RadioButton(L_("Full##prefs"), !guided && layoutMode != LayoutMode::Blender)) setLayoutMode(LayoutMode::Full);
    ImGui::SameLine();
    if (ImGui::RadioButton(L_("Blender##prefs"), layoutMode == LayoutMode::Blender)) setLayoutMode(LayoutMode::Blender);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("Arranged as Blender is: the steps as workspace tabs, the Outliner and the Properties\n"
                                 "editor at the right, the asset shelf under the 3D view."));
  }
  if (toolbarIcons() && ImGui::Checkbox(L_("Names beside the toolbar icons"), &toolbarNames)) saveSession(true);
  if (prefsScrollTo == "language") ImGui::SetScrollHereY(0.f), prefsScrollTo.clear();
  drawLanguagePrefs();
  ImGui::SeparatorText(T_("Saving"));
  int mins = (int)std::lround(engine.autosaveMinutes);
  ImGui::SetNextItemWidth(200 * uiScale);
  if (ImGui::SliderInt(L_("Autosave every"), &mins, 0, 30, mins ? T_("%d min") : T_("off"))) engine.autosaveMinutes = mins;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("A copy of your work goes to the cache folder this often while there are unsaved changes,\n"
                               "written in the background. After a crash, Patina offers to bring it back."));
  drawUndoPrefs();
  drawPenPrefs();
  if (prefsScrollTo == "http") ImGui::SetScrollHereY(0.f), prefsScrollTo.clear();
  drawNetworkPrefs();
  if (prefsScrollTo == "features") ImGui::SetScrollHereY(0.f), prefsScrollTo.clear();
  ImGui::SeparatorText(T_("Features"));
  {
    ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
    engine.uiRects["prefs.features"] = {ra.x, ra.y, rb.x - ra.x, rb.y - ra.y};  // (the manual's screenshots)
  }
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", T_("Newer features. Switch off any that get in your way; they can come back at any time."));
  ImGui::PopStyleColor();
  for (auto& f : Features::all()) {
    bool on = engine.features.on(f.id);
    if (ImGui::Checkbox(L_(f.name), &on)) {
      engine.features.set(f.id, on);
      saveSession(true);
    }
    ImGui::SameLine();
    ImGui::TextDisabled("(%s)", f.since);
    if (ImGui::IsItemHovered() || ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) ImGui::SetTooltip("%s", T_(f.description));
    ImGui::Indent();
    ImGui::PushTextWrapPos(0.f);
    ImGui::TextDisabled("%s", T_(f.description));
    ImGui::PopTextWrapPos();
    if (std::string(f.id) == "patina_assistant" && on) assistantModelControls();
    ImGui::Unindent();
  }
  ImGui::Spacing();
  ImGui::TextDisabled(T_("Saved to %s"), paths::configDir().string().c_str());
  ImGui::End();
}

// ------------------------------------------------------------------ errors

void App::showError(const std::string& title, const std::string& text) {
  errorTitle = title;
  errorText = text;
  errorPending = true;
  PLOG_E("%s: %s", title.c_str(), text.c_str());
  setStatus(title + ": " + text);
}

void App::drawErrorPopup() {
  if (errorPending) {
    ImGui::OpenPopup("Patina - error");
    errorPending = false;
  }
  ImGui::SetNextWindowSize(ImVec2(460 * uiScale, 0), ImGuiCond_Appearing);
  if (ImGui::BeginPopupModal(L_("Patina - error"), nullptr, ImGuiWindowFlags_NoSavedSettings)) {
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.55f, 0.45f, 1));
    ImGui::TextWrapped("%s", errorTitle.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();
    ImGui::TextWrapped("%s", errorText.c_str());
    ImGui::Spacing();
    if (ImGui::Button(L_("Copy message"))) ImGui::SetClipboardText((errorTitle + ": " + errorText).c_str());
    ImGui::SameLine();
    bool ok = ImGui::Button(L_("OK"), ImVec2(100 * uiScale, 0));
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["error.ok"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
    if (ok || ImGui::IsKeyPressed(ImGuiKey_Escape) || ImGui::IsKeyPressed(ImGuiKey_Enter)) {
      ImGui::CloseCurrentPopup();
      engine.uiRects.erase("error.ok");
    }
    ImGui::EndPopup();
  }
}

}  // namespace patina

namespace patina {

// (1.2.7) How much undo keeps: steps per object, the copies on the graphics card, the compressed ones in RAM
void App::drawUndoPrefs() {
  if (prefsScrollTo == "undo") ImGui::SetScrollHereY(0.f), prefsScrollTo.clear();
  ImGui::SeparatorText(T_("Undo"));
  UndoLimits& L = UndoStack::limits();
  bool changed = false;
  int steps = (int)L.steps;
  ImGui::SetNextItemWidth(200 * uiScale);
  if (ImGui::SliderInt(L_("Undo steps"), &steps, 10, 1000, "%d", ImGuiSliderFlags_Logarithmic)) L.steps = (size_t)steps, changed = true;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("How many steps each object's history keeps (default 100)"));
  size_t mem = physicalMemoryBytes();
  float maxGb = mem ? std::max(1.f, float(mem / 2 / 1048576) / 1024.f) : 32.f;
  float gb = float(L.ramBytes >> 20) / 1024.f;
  ImGui::SetNextItemWidth(200 * uiScale);
  if (ImGui::SliderFloat(L_("Undo memory (RAM)"), &gb, 0.5f, maxGb, T_("%.1f GB")))
    L.ramBytes = size_t(std::clamp(gb, 0.5f, maxGb) * 1024.f) << 20, changed = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(T_("Older undo steps are compressed into RAM. Past this much, the oldest steps are dropped,\n"
                         "so undo never takes all your memory. The default is a quarter of this computer's memory\n"
                         "(1 to 8 GB): %.1f GB. Undo is never written to disk, to spare your SSD."),
                      UndoLimits::defaultRam() / 1073741824.0);
  int gpu = (int)(L.gpuBytes >> 20);
  ImGui::SetNextItemWidth(200 * uiScale);
  if (ImGui::SliderInt(L_("Instant undo (graphics card)"), &gpu, 256, 8192, T_("%d MB"), ImGuiSliderFlags_Logarithmic))
    L.gpuBytes = size_t(std::max(gpu, 256)) << 20, changed = true;
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("The latest steps keep their copies on the graphics card, so they undo at once (default 1024 MB).\n"
                               "Past this much, older steps are compressed into RAM: they undo a moment slower."));
  ImGui::TextDisabled(T_("Now: %.0f MB on the graphics card, %.0f MB compressed"), UndoStack::totalBytes() / 1048576.0,
                      UndoStack::totalRamBytes() / 1048576.0);
  // applied (steps dropped, copies packed) and saved when the slider is let go
  static bool pending = false;
  pending |= changed;
  if (pending && !ImGui::IsAnyItemActive()) {
    pending = false;
    UndoStack::enforceAll();
    saveSession(true);
  }
}

}  // namespace patina
