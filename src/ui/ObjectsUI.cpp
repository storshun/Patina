// (1.2) The scene's objects in the window: the Objects list over the layers (each object has its own layer stack,
// texture set and history), its right-click menu, the active object's placement, and File > Add a Model to the Scene.
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <filesystem>

#include "ui/App.h"

namespace fs = std::filesystem;

namespace patina {

void App::queueSceneAction(std::function<void()> fn) { sceneActions.push_back(std::move(fn)); }

void App::runSceneActions() {
  auto a = std::move(sceneActions);
  sceneActions.clear();
  for (auto& f : a) f();
}

void App::objectSwitched() {
  // what the window kept about the object that was active
  layerThumbs.clear();
  visibleCacheKey = 0;
  visibleCache.clear();
  lastViewKey = 0;
  overlapVersion = ~0ull;
  uvIslandInfoKey = ~0ull;
  uvPress = UVPress();
  cloneMarkerValid = false;
  retopoVertAdjStart.clear(), retopoVertAdj.clear(), retopoStamp.clear();
}

void App::selectObjectUI(int i) {
  queueSceneAction([this, i] {
    if (i == engine.activeObject() || !engine.activateObject(i)) return;
    setStatus(F_("Painting %s: the Layers panel shows its layers", engine.doc.displayName().c_str()));
  });
}

void App::addModelToScene(const std::string& path) {
  std::string err;
  json r;
  int first = engine.addObjectsFromFile(path, 0, false, &err, &r);
  if (first < 0) {
    showError(F_("Could not add %s", fs::path(path).filename().string().c_str()), err);
    return;
  }
  size_t n = r.value("objects", json::array()).size();
  if (n == 1) setStatus(F_("Added %s to the scene: select it in the Objects list to paint it", engine.object(first).displayName().c_str()));
  else setStatus(F_("Added %d objects from %s", (int)n, fs::path(path).filename().string().c_str()));
  addRecentFile(path);
}

void App::objectMenu(int i) {
  Document& o = engine.object(i);
  const bool active = i == engine.activeObject();
  ImGui::TextDisabled("%s", o.displayName().c_str());
  ImGui::Separator();
  if (!active && ImGui::MenuItem(L_("Paint This Object"))) selectObjectUI(i);
  if (ImGui::MenuItem(L_("Rename..."))) objectRename = i, objectRenameText = o.displayName(), objectRenameOpen = true;
  if (ImGui::MenuItem(L_("Frame It"))) runCommand("frame_object", {{"object", i}});
  if (ImGui::MenuItem(L_("Only This Object (Local View)"), keyLabel("view.local"), active && engine.isolateActive))
    queueSceneAction([this, i] {
      bool on = !(engine.isolateActive && i == engine.activeObject());
      engine.activateObject(i);
      engine.isolateActive = on;
    });
  if (ImGui::MenuItem(o.objectHidden ? L_("Show It") : L_("Hide It")))
    runCommand("set_object", {{"object", i}, {"visible", o.objectHidden}});
  if (ImGui::MenuItem(L_("Hide the Others")))
    for (int k = 0; k < engine.objectCount(); ++k)
      if (k != i && !engine.object(k).objectHidden) runCommand("set_object", {{"object", k}, {"visible", false}});
  if (ImGui::MenuItem(L_("Show Every Object")))
    for (int k = 0; k < engine.objectCount(); ++k)
      if (engine.object(k).objectHidden) runCommand("set_object", {{"object", k}, {"visible", true}});
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Put It on the Ground"))) runCommand("set_object", {{"object", i}, {"on_ground", true}});
  if (ImGui::MenuItem(L_("Back to Where It Came In"), nullptr, false, o.placement.moved()))
    runCommand("set_object", {{"object", i}, {"reset", true}});
  if (ImGui::MenuItem(L_("Duplicate"))) {
    queueSceneAction([this, i] {
      std::string err;
      int k = engine.duplicateObject(i, &err);
      if (k < 0) showError(T_("Could not copy the object"), err);
      else setStatus(F_("Copied as %s: move it with the Object tool", engine.object(k).displayName().c_str()));
    });
  }
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Remove from the Scene..."), nullptr, false, engine.objectCount() > 1)) objectRemove = i;
}

void App::drawObjectsSection() {
  if (!engine.features.on("scene_objects")) return;
  const int n = engine.objectCount();
  const float fh = ImGui::GetFrameHeight();
  ImGui::PushID("##objects");
  // ---- the header: what it is, and a model to add (one object: this line is all, the layers keep the room)
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(n == 1 ? T_("Object") : T_("Objects"));
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    ImGui::SetTooltip("%s", T_("The models in this project: a body, its clothes, its eyes, its hair. Each has its own\n"
                               "layers and texture set. Click one to paint it: the layers below are its own."));
  ImGui::SameLine();
  if (n == 1) {
    std::string name = engine.doc.displayName();
    if (ImGui::Selectable((name + "##only").c_str(), false, ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(ImGui::GetContentRegionAvail().x - iconButtonWidth(T_("Add a model"), fh) - 8 * uiScale, 0)) &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
      objectRename = 0, objectRenameText = name, objectRenameOpen = true;
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["objects.row." + name] = {a.x, a.y, b.x - a.x, b.y - a.y};
      engine.uiRects["objects.list"] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    if (ImGui::BeginPopupContextItem("##objctx")) objectMenu(0), ImGui::EndPopup();
    ImGui::SameLine();
  } else {
    ImGui::TextDisabled("%d", n);
  }
  ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - iconButtonWidth(T_("Add a model"), fh));
  if (iconButton("objects.add", "add_object", T_("Add a model"), ui::IconState::Normal, fh,
                 T_("Add a model file to the scene as an object of its own (a character's clothes, eyes or hair),\n"
                    "where the file puts it, with its own layers"),
                 "file.add_model"))
    openFileDialog(DialogPurpose::AddObject);
  // ---- the list
  if (n == 1) {
    if (engine.doc.placement.moved()) objectPlacementUI();
    ImGui::PopID();
    objectPopups();
    return;
  }
  const float rowH = std::round(fh * 1.15f);
  float listH = std::min(n, 6) * (rowH + ImGui::GetStyle().ItemSpacing.y) + 2.f * ImGui::GetStyle().WindowPadding.y;
  ImGui::BeginChild("##objectlist", ImVec2(0, listH), ImGuiChildFlags_Borders);
  {
    ImVec2 a = ImGui::GetWindowPos(), b = ImGui::GetWindowSize();
    engine.uiRects["objects.list"] = {a.x, a.y, b.x, b.y};
  }
  for (int i = 0; i < n; ++i) {
    Document& o = engine.object(i);
    const bool active = i == engine.activeObject();
    ImGui::PushID(i);
    bool vis = !o.objectHidden;
    if (ImGui::Checkbox("##vis", &vis)) runCommand("set_object", {{"object", i}, {"visible", vis}});
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      ImGui::SetTooltip("%s", T_("Shown in the 3D view, renders and exports, or hidden"));
    ImGui::SameLine();
    std::string label = o.displayName() + (engine.isolateActive && active ? std::string("  ") + T_("(only this one)") : "");
    if (ImGui::Selectable((label + "##row").c_str(), active, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(0, 0))) {
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) objectRename = i, objectRenameText = o.displayName(), objectRenameOpen = true;
      else if (!active) selectObjectUI(i);
    }
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["objects.row." + o.displayName()] = {a.x, a.y, b.x - a.x, b.y - a.y};
    }
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      ImGui::SetTooltip("%s", F_("%u faces, %zu layers, %d x %d texture set%s", o.mesh.faceCount(), o.layers.size(), o.tileRes(),
                                 o.tileRes(), o.bridge.active ? ("\n" + F_("Linked to Blender's %s", o.bridge.object.c_str())).c_str() : "")
                                  .c_str());
    if (ImGui::BeginPopupContextItem("##objctx")) objectMenu(i), ImGui::EndPopup();
    if (o.bridge.active) {
      ImGui::SameLine(ImGui::GetContentRegionAvail().x + ImGui::GetCursorPosX() - ImGui::CalcTextSize("B").x - 4.f * uiScale);
      ImGui::TextDisabled("B");
    }
    ImGui::PopID();
  }
  ImGui::EndChild();
  // ---- the active object's placement (moved, turned, scaled from where it came in)
  if (n > 1 || engine.doc.placement.moved()) objectPlacementUI();
  ImGui::PopID();
  objectPopups();
}

void App::objectPopups() {
  // ---- rename and remove
  if (objectRenameOpen) ImGui::OpenPopup("##objrename"), objectRenameOpen = false;
  if (ImGui::BeginPopup("##objrename")) {
    ImGui::TextUnformatted(T_("Name"));
    if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
    bool done = ImGui::InputText("##name", &objectRenameText, ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    done |= ImGui::Button(L_("Rename"));
    if (done) {
      if (objectRename >= 0 && objectRename < engine.objectCount() && !objectRenameText.empty()) {
        json out;
        if (!runCommand("set_object", {{"object", objectRename}, {"name", objectRenameText}}, &out))
          setStatus(T_("Another object has that name"));
      }
      ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
  if (objectRemove >= 0) ImGui::OpenPopup("##objremove");
  if (ImGui::BeginPopupModal("##objremove", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar)) {
    int i = objectRemove;
    if (i < 0 || i >= engine.objectCount()) {
      objectRemove = -1;
      ImGui::CloseCurrentPopup();
    } else {
      ImGui::TextUnformatted(F_("Remove %s from the scene?", engine.object(i).displayName().c_str()).c_str());
      ImGui::TextDisabled("%s", T_("Its layers and its history go with it. This can't be undone."));
      if (ImGui::Button(L_("Remove"))) {
        queueSceneAction([this, i] {
          std::string name = engine.object(i).displayName();
          if (engine.removeObject(i)) setStatus(F_("Removed %s", name.c_str()));
        });
        objectRemove = -1;
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button(L_("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) objectRemove = -1, ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
  }
}

bool App::modelDropped(const std::string& path) {
  if (!engine.features.on("scene_objects") || !engine.doc.hasMesh() || engine.doc.untouchedStartup()) return false;
  // dropped on the Objects list: added; elsewhere: asked
  auto it = engine.uiRects.find("objects.list");
  ImVec2 m = ImGui::GetIO().MousePos;
  if (it != engine.uiRects.end() && m.x >= it->second[0] && m.y >= it->second[1] && m.x < it->second[0] + it->second[2] &&
      m.y < it->second[1] + it->second[3]) {
    addModelToScene(path);
    return true;
  }
  droppedModel = path;
  dropModelAsk = true;
  return true;
}

void App::drawDropModelPrompt() {
  if (dropModelAsk) ImGui::OpenPopup("###dropmodel"), dropModelAsk = false;
  ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  std::string title = std::string(T_("A model")) + "###dropmodel";
  if (!ImGui::BeginPopupModal(title.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;
  ImGui::TextUnformatted(fs::path(droppedModel).filename().string().c_str());
  ImGui::TextDisabled("%s", T_("Add it to this scene as an object of its own, or open it instead of this project?"));
  ImGui::Spacing();
  auto rectOf = [this](const char* key) {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects[key] = {a.x, a.y, b.x - a.x, b.y - a.y};
  };
  if (ImGui::Button(L_("Add to the scene"), ImVec2(150 * uiScale, 0))) {
    std::string p = droppedModel;
    queueSceneAction([this, p] { addModelToScene(p); });
    ImGui::CloseCurrentPopup();
  }
  rectOf("dropmodel.add");
  ImGui::SameLine();
  if (ImGui::Button(L_("Open instead"), ImVec2(150 * uiScale, 0))) {
    std::string p = droppedModel;
    ImGui::CloseCurrentPopup();
    replaceProject(T_("open another file"), [this, p] {
      std::string err;
      if (engine.openFile(p, &err)) setStatus(F_("Opened %s", p.c_str()));
      else showError(F_("Could not open %s", fs::path(p).filename().string().c_str()), err);
    });
  }
  rectOf("dropmodel.open");
  ImGui::SameLine();
  if (ImGui::Button(L_("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
  ImGui::EndPopup();
}

bool App::objectPlacementUI() {
  Document& d = engine.doc;
  if (!ImGui::CollapsingHeader((F_("Placement: %s", d.displayName().c_str()) + "###placement").c_str())) return false;
  Document::Placement p = d.placement;
  float size = d.mesh.bounds.valid() ? glm::length(d.mesh.bounds.size()) : 1.f;
  bool edited = false, released = false;
  auto track = [&] {
    if (ImGui::IsItemActivated()) placementBefore = d.placement;
    edited |= ImGui::IsItemEdited();
    released |= ImGui::IsItemDeactivatedAfterEdit();
  };
  ImGui::PushItemWidth(-ImGui::CalcTextSize(T_("Rotation")).x - 2 * ImGui::GetStyle().ItemSpacing.x);
  ImGui::DragFloat3(L_("Location"), &p.location.x, std::max(size, 1e-3f) * 0.003f, 0, 0, "%.3f m");
  track();
  ImGui::DragFloat3(L_("Rotation"), &p.rotation.x, 0.5f, -360.f, 360.f, "%.1f°");
  track();
  ImGui::DragFloat3(L_("Scale"), &p.scale.x, 0.005f, 0.001f, 1000.f, "%.3f");
  track();
  ImGui::PopItemWidth();
  for (int k = 0; k < 3; ++k)
    if (std::abs(p.scale[k]) < 1e-3f) p.scale[k] = 1e-3f;
  if (edited) d.placement = p, d.dirty = true;  // (live while dragging; one undo step once it is let go)
  if (released) {
    Document::Placement now = d.placement;
    d.placement = placementBefore;
    d.setPlacement(now, T_("Move the object"));
  }
  if (ImGui::SmallButton(L_("On the ground"))) runCommand("set_object", {{"on_ground", true}});
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Moved up or down so its lowest point is at height 0"));
  ImGui::SameLine();
  ImGui::BeginDisabled(!d.placement.moved());
  if (ImGui::SmallButton(L_("Where it came in"))) runCommand("set_object", {{"reset", true}});
  ImGui::EndDisabled();
  return true;
}

}  // namespace patina
