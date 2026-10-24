// Layers panel (tree with folders, solo, color tags, drag & drop, smart materials),
// History panel and Colors panel.
#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <functional>

#include "core/Log.h"
#include "core/Paths.h"
#include "engine/SmartMaterial.h"
#include "ui/App.h"
#include "ui/Theme.h"

namespace patina {

static ImTextureID texId(GLuint t) { return (ImTextureID)(intptr_t)t; }
static bool colorEdit(const char* label, vec3& c) { return ImGui::ColorEdit3(label, &c.x, ImGuiColorEditFlags_NoInputs); }

static const ImU32 kTagColors[8] = {0,
                                    IM_COL32(220, 72, 60, 255),
                                    IM_COL32(236, 146, 42, 255),
                                    IM_COL32(226, 202, 62, 255),
                                    IM_COL32(92, 182, 82, 255),
                                    IM_COL32(62, 182, 192, 255),
                                    IM_COL32(72, 122, 222, 255),
                                    IM_COL32(162, 92, 202, 255)};
static const char* kTagNames[8] = {N_("None"), N_("Red"),  N_("Orange"), N_("Yellow"),
                                   N_("Green"), N_("Cyan"), N_("Blue"),   N_("Purple")};

// Structural edits requested while the list is drawn run after it (indices stay valid).
static std::vector<std::function<void()>> g_deferred;
static bool g_focusRename = false;

// Fill layer with the current paint material; mask < 0: none, else a mask of that value.
void App::addFillLayerUI(float mask) {
  Document& d = engine.doc;
  Layer& l = d.addFillLayer(engine.painter.material);
  uint32_t uid = l.uid;
  if (!engine.painter.material.id.empty())
    if (auto* e = engine.materials.find(engine.painter.material.id)) l.name = e->def.name;
  d.checkpoint("Add " + l.name);
  if (mask >= 0) {
    d.addMask(d.indexOf(uid), mask);
    d.editMask = true;
  }
}

// (1.2.7) The new-layer list: one list, the same in Edit > New Layer, the Layers panel's right-click menus, the
// Add menu (Shift+A in the Blender keymap) and the smart materials button. Five lists had grown apart (some had
// the adjustment layer, the white mask, baked maps or smart materials, others not, under other names).
// `run` does an action now, or after the layer list is drawn (from a layer's own menu).
void App::newLayerItems(const std::function<void(std::function<void()>)>& run) {
  Document& d = engine.doc;
  if (ImGui::MenuItem(L_("Paint Layer"), keyLabel("layer.new_paint"))) run([&d] { d.addPaintLayer(); });
  if (ImGui::MenuItem(L_("Fill Layer"))) run([this] { addFillLayerUI(-1); });
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Covers the model with the current paint material"));
  if (ImGui::MenuItem(L_("Fill Layer with a Black Mask"))) run([this] { addFillLayerUI(0.f); });
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Hidden until you paint its mask - the usual way to paint a material on"));
  if (ImGui::MenuItem(L_("Fill Layer with a White Mask"))) run([this] { addFillLayerUI(1.f); });
  if (ImGui::MenuItem(L_("Adjustment Layer"))) run([&d] { d.addAdjustLayer(); });
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Hue / saturation / brightness / contrast and roughness / metallic / height offsets\n"
                               "applied to everything below"));
  if (ImGui::MenuItem(L_("Folder"))) run([&d] { d.addFolder(); });
  ImGui::Separator();
  if (ImGui::BeginMenu(L_("Baked Map Layer"))) {
    bool any = false;
    for (int k = 0; k < kBakedMapCount; ++k) {
      if (!d.tex.hasBakedMap(k) || d.bakedLayer(k)) continue;
      any = true;
      if (ImGui::MenuItem(L_(bakedMapLabel(k)), nullptr, false, engine.features.on("baked_layers"))) run([&d, k] { d.addBakedLayer(k); });
    }
    if (!any) ImGui::TextDisabled("%s", T_("Every baked map has its layer"));
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu(L_("Smart Material"))) {
    smartMaterialItems(run);
    ImGui::EndMenu();
  }
}

// the smart materials, Patina's then yours, with their descriptions
void App::smartMaterialItems(const std::function<void(std::function<void()>)>& run) {
  auto list = listSmartMaterials();
  bool anyUser = false;
  ImGui::SeparatorText(T_("Patina smart materials"));
  for (int pass = 0; pass < 2; ++pass)
    for (auto& m : list) {
      if (m.builtin != (pass == 0)) continue;
      if (pass == 1 && !anyUser) ImGui::SeparatorText(T_("Yours")), anyUser = true;
      if (ImGui::MenuItem((m.name + "##" + m.id).c_str())) {
        std::string path = m.path, name = m.name;
        run([this, path, name] {
          std::string err;
          if (applySmartMaterial(engine.doc, path, &err) < 0) showError(T_("Could not apply the smart material"), err);
          else setStatus(F_("Added the smart material %s", name.c_str()));
        });
      }
      if (ImGui::IsItemHovered() && !m.description.empty())
        ImGui::SetTooltip("%s%s%s", m.description.c_str(), m.hasPaint ? "\n" : "", m.hasPaint ? T_("(includes painted textures)") : "");
    }
}

// Blender-style Add menu (Shift+A in the Blender keymap), opened at the mouse.
void App::drawAddLayerMenu() {
  if (openAddLayerMenu) {
    ImGui::OpenPopup("##AddLayerMenu");
    openAddLayerMenu = false;
  }
  if (!ImGui::BeginPopup("##AddLayerMenu")) return;
  ImGui::TextDisabled("%s", T_("New Layer"));
  ImGui::Separator();
  newLayerItems([](std::function<void()> f) { f(); });
  ImGui::EndPopup();
}

void App::groupSelectedIntoFolder() {
  if (engine.doc.selected >= 0) engine.doc.groupIntoFolder(engine.doc.selected);
}

static void tagPicker(Layer& l) {
  for (int t = 0; t < 8; ++t) {
    if (t) ImGui::SameLine(0, 3);
    ImGui::PushID(t);
    ImVec4 c = t ? ImGui::ColorConvertU32ToFloat4(kTagColors[t]) : ImVec4(0.25f, 0.26f, 0.28f, 1);
    if (ImGui::ColorButton(kTagNames[t], c, ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight())))
      l.colorTag = t;
    if (l.colorTag == t)
      ImGui::GetWindowDrawList()->AddRect(ImGui::GetItemRectMin() - ImVec2(1, 1), ImGui::GetItemRectMax() + ImVec2(1, 1),
                                          IM_COL32(255, 255, 255, 230), 2.f, 0, 2.f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_(kTagNames[t]));
    ImGui::PopID();
  }
}

void App::layerContextMenu(int i) {
  Document& d = engine.doc;
  Layer& l = *d.layers[i];
  uint32_t uid = l.uid;
  auto idx = [&d, uid] { return d.indexOf(uid); };
  ImGui::TextDisabled("%s", l.name.c_str());
  ImGui::Separator();
  if (contextMenus()) {
    if (ImGui::MenuItem(l.visible ? L_("Hide") : L_("Show"), T_("click the check"))) l.visible = !l.visible, d.markComposite();
    if (l.isFolder() && ImGui::MenuItem(l.expanded ? L_("Close the Folder") : L_("Open the Folder"))) l.expanded = !l.expanded;
    if (ImGui::BeginMenu(L_("New Layer Above"))) {
      newLayerItems([&d, i](std::function<void()> f) {
        d.selected = i;  // (new layers go above the selection)
        g_deferred.push_back(std::move(f));
      });
      ImGui::EndMenu();
    }
    ImGui::Separator();
  }
  if (ImGui::MenuItem(L_("Rename"), T_("double-click"))) renameLayerUid = (int)uid, g_focusRename = true;
  if (ImGui::MenuItem(L_("Duplicate"), keyLabel("layer.duplicate"))) g_deferred.push_back([&d, idx] { d.duplicateLayer(idx()); });
  if (ImGui::MenuItem(L_("Delete"), keyLabel("layer.delete"))) g_deferred.push_back([&d, idx] { d.removeLayer(idx()); });
  if (!l.isFolder() && ImGui::MenuItem(L_("Make an Instance"), keyLabel("layer.instance")))
    g_deferred.push_back([&d, idx] { d.addInstance(idx()); });
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Same content, own opacity, blend mode, channels and mask. Painting either one changes both."));
  if (l.instanceOf) {
    if (ImGui::MenuItem(L_("Select the Source"))) {
      if (const Layer* src = d.contentLayer(l); src != &l) d.selected = d.indexOf(src->uid), d.editMask = false;
    }
    if (ImGui::MenuItem(L_("Make Independent (Copy the Content)"))) g_deferred.push_back([&d, idx] { d.makeIndependent(idx()); });
  }
  if (contextMenus()) {
    if (ImGui::MenuItem(L_("Move Up"))) g_deferred.push_back([&d, idx] { d.moveSibling(idx(), +1); });
    if (ImGui::MenuItem(L_("Move Down"))) g_deferred.push_back([&d, idx] { d.moveSibling(idx(), -1); });
    const Layer* content = d.contentLayer(l);
    if (content && content->type == LayerType::Fill) {
      ImGui::Separator();
      const MaterialRef& pm = engine.painter.material;
      if (ImGui::MenuItem(L_("Use the Paint Material"))) {
        std::string oldName, newName;
        if (auto* oe = engine.materials.find(content->material.id)) oldName = oe->def.name;
        if (auto* ne = engine.materials.find(pm.id)) newName = ne->def.name;
        MaterialRef r = pm;
        g_deferred.push_back([&d, idx, r, oldName, newName] { d.replaceFillMaterial(idx(), r, oldName, newName); });
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("The material (or flat color) the brush paints with becomes this layer's"));
      if (ImGui::MenuItem(L_("Paint with Its Material"))) engine.painter.material = content->material, engine.selectedMaterialId = content->material.id;
    }
  }
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Group into a Folder"), keyLabel("layer.group")))
    g_deferred.push_back([&d, idx] { d.groupIntoFolder(idx()); });
  if (d.effectiveParent(l) && ImGui::MenuItem(L_("Move Out of the Folder")))
    g_deferred.push_back([&d, idx] {
      // just above the folder it was in
      int f = d.indexOf(d.effectiveParent(*d.layers[idx()]));
      d.moveOnto(idx(), f, false);
    });
  std::vector<int> folders;
  for (int k = 0; k < (int)d.layers.size(); ++k)
    if (d.layers[k]->isFolder() && k != i && !d.isDescendant(d.layers[k]->uid, uid) && d.effectiveParent(l) != d.layers[k]->uid)
      folders.push_back(k);
  if (ImGui::BeginMenu(L_("Move into Folder"), !folders.empty())) {
    for (int k : folders) {
      uint32_t fu = d.layers[k]->uid;
      if (ImGui::MenuItem((d.layers[k]->name + "##" + std::to_string(fu)).c_str()))
        g_deferred.push_back([&d, idx, fu] { d.setParent(idx(), fu); });
    }
    ImGui::EndMenu();
  }
  bool soloed = d.soloUid == uid;
  if (ImGui::MenuItem(soloed ? L_("Show All Layers (End Solo)") : L_("Solo"), T_("Alt+click eye"))) {
    d.soloUid = soloed ? 0 : uid;
    d.compositeDirty = true;
  }
  if (ImGui::BeginMenu(L_("Color Tag"))) {
    tagPicker(l);
    ImGui::EndMenu();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Mask from the Face Selection"), nullptr, false, !d.selectedFaces().empty()))
    g_deferred.push_back([&d, idx] { d.maskFromSelection(idx()); });
  if (!l.mask) {
    if (ImGui::MenuItem(L_("Add White Mask"))) g_deferred.push_back([&d, idx] { d.addMask(idx(), 1.f); });
    if (ImGui::MenuItem(L_("Add Black Mask"))) g_deferred.push_back([&d, idx] { d.addMask(idx(), 0.f); });
  } else {
    if (ImGui::MenuItem(L_("Edit Mask"))) d.selected = i, d.editMask = true;
    if (ImGui::MenuItem(L_("Remove Mask"))) g_deferred.push_back([&d, idx] { d.removeMask(idx()); });
  }
  bool painted = false;
  for (int c = 0; c < kChannelCount; ++c) painted |= l.hasChannel(c);
  if (l.type == LayerType::Paint && ImGui::MenuItem(L_("Clear Paint (Keep the Mask)"), nullptr, false, painted))
    g_deferred.push_back([&d, idx] { d.clearPaint(idx()); });
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Copy Layer Settings"))) {
    layerClipboard = copyLayerSettings(l);
    setStatus(F_("Copied the settings of %s", l.name.c_str()));
  }
  if (ImGui::MenuItem(L_("Paste Layer Settings"), nullptr, false, !layerClipboard.is_null())) {
    pasteLayerSettings(l, layerClipboard);
    d.pendingLabel = "Paste settings: " + l.name;
    d.markComposite();
  }
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Save as Smart Material..."))) {
    d.selected = i;
    smartSaveName = l.name;
    smartSaveIncludePaint = false;
    g_deferred.push_back([] { ImGui::OpenPopup("Save smart material"); });
  }
  if (ImGui::MenuItem(L_("Export This Layer..."))) d.selected = i, exportWhat = 1, showExport = true;
  if (ImGui::MenuItem(L_("Export Its Mask..."), nullptr, false, (bool)l.mask)) d.selected = i, exportWhat = 2, showExport = true;
}

// (0.22.5) the selected layer's smart mask in the properties (the item just drawn): a smart mask or
// another layer's mask dropped there
void App::smartMaskTarget(int index) {
  if (!ImGui::BeginDragDropTarget()) return;
  Document& d = engine.doc;
  if (index < 0 || index >= (int)d.layers.size()) return ImGui::EndDragDropTarget();
  uint32_t uid = d.layers[index]->uid;
  if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PATINA_SMARTMASK", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
    int t = *(const int*)p->Data;
    dropHint = F_("Smart mask for %s", d.layers[index]->name.c_str());
    if (p->IsDelivery()) g_deferred.push_back([&d, t, uid] { d.addSmartMask(d.indexOf(uid), t); });
  }
  if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PATINA_MASK", ImGuiDragDropFlags_AcceptBeforeDelivery)) {
    uint32_t src = *(const uint32_t*)p->Data;
    if (src != uid) {
      dropHint = F_("Copies the mask to %s", d.layers[index]->name.c_str());
      if (p->IsDelivery()) g_deferred.push_back([&d, src, uid] { d.copyMaskTo(d.indexOf(src), d.indexOf(uid)); });
    }
  }
  ImGui::EndDragDropTarget();
}

// (0.22.5) a layer's mask (painted, smart mask, mask effects), dragged onto another layer to copy it there
void App::maskDragSource(const Layer& l, unsigned tex) {
  uint32_t uid = l.uid;
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["layers.mask:" + std::to_string(uid)] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
  }
  if (!engine.features.on("drag_drop") || !ImGui::BeginDragDropSource()) return;
  ImGui::SetDragDropPayload("PATINA_MASK", &uid, sizeof uid);
  if (tex) ImGui::Image(texId(tex), ImVec2(40, 40) * uiScale, ImVec2(0, 1), ImVec2(1, 0)), ImGui::SameLine();
  ImGui::BeginGroup();
  ImGui::Text(T_("The mask of %s"), l.name.c_str());
  ImGui::TextDisabled("%s", dropHint.empty() ? T_("Drop on another layer to copy it there") : dropHint.c_str());
  ImGui::EndGroup();
  dropHint.clear();
  ImGui::EndDragDropSource();
}

void App::drawLayerRow(int i, int depth) {
  Document& d = engine.doc;
  Layer& l = *d.layers[i];
  uint32_t uid = l.uid;
  ImGui::PushID((int)uid);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float fh = ImGui::GetFrameHeight();
  // rows are taller than a widget so the thumbnails are readable; widgets sit centered
  float rowH = std::round(fh * 1.5f);
  float y0 = ImGui::GetCursorPosY();
  auto center = [&](float h) { ImGui::SetCursorPosY(y0 + std::floor((rowH - h) * 0.5f)); };
  ImVec2 rowMin = ImGui::GetCursorScreenPos();
  bool selected = d.selected == i;
  // color tag strip
  if (l.colorTag > 0 && l.colorTag < 8) dl->AddRectFilled(rowMin, rowMin + ImVec2(4 * uiScale, rowH), kTagColors[l.colorTag], 1.5f);
  ImGui::Dummy(ImVec2(6 * uiScale, rowH));
  ImGui::SameLine(0, 0);
  center(fh);
  // visibility; Alt+click = solo
  bool soloed = d.soloUid == uid;
  bool vis = l.visible;
  if (ImGui::Checkbox("##vis", &vis)) {
    if (ImGui::GetIO().KeyAlt) {
      d.soloUid = soloed ? 0 : uid;
      d.compositeDirty = true;
    } else {
      l.visible = vis;
      d.markComposite();
    }
  }
  if (soloed)
    dl->AddRect(ImGui::GetItemRectMin() - ImVec2(2, 2), ImGui::GetItemRectMax() + ImVec2(2, 2), IM_COL32(255, 200, 60, 255), 3.f, 0, 2.f);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    ImGui::SetTooltip("%s", soloed ? T_("Soloed - Alt+click to show all layers again") : T_("Visible\nAlt+click: solo (show only this)"));
  ImGui::SameLine(0, 4 * uiScale);
  center(fh);
  if (depth) {
    ImGui::Dummy(ImVec2(depth * 14.f * uiScale, fh));
    ImGui::SameLine(0, 0);
    center(fh);
  }
  if (l.isFolder()) {
    if (ImGui::ArrowButton("##exp", l.expanded ? ImGuiDir_Down : ImGuiDir_Right)) l.expanded = !l.expanded;
  } else {
    ImGui::Dummy(ImVec2(fh, fh));
  }
  ImGui::SameLine(0, 4 * uiScale);
  // thumbnail (paint / fill content) with the type letter, or the type badge
  const LayerThumb& th = layerThumb(l);
  float thumbS = rowH - 4;
  center(thumbS);
  if (th.hasContent && !l.isFolder()) {
    ImVec2 p = ImGui::GetCursorScreenPos();
    if (ImGui::InvisibleButton("##content", ImVec2(thumbS, thumbS))) d.selected = i, d.editMask = false;
    dl->AddImage(texId(th.content.id()), p, p + ImVec2(thumbS, thumbS), ImVec2(0, 1), ImVec2(1, 0));
    // (0.22.5) a fill layer's material, dragged onto another fill layer or between layers
    if (l.type == LayerType::Fill || d.contentLayer(l)->type == LayerType::Fill) {
      engine.uiRects["layers.content:" + std::to_string(uid)] = {p.x, p.y, thumbS, thumbS};  // (for tests)
      if (engine.features.on("drag_drop") && ImGui::BeginDragDropSource()) {
        ImGui::SetDragDropPayload("PATINA_FILL", &uid, sizeof uid);
        ImGui::Image(texId(th.content.id()), ImVec2(40, 40) * uiScale, ImVec2(0, 1), ImVec2(1, 0));
        ImGui::SameLine();
        ImGui::BeginGroup();
        ImGui::Text(T_("The material of %s"), l.name.c_str());
        ImGui::TextDisabled("%s", dropHint.empty() ? T_("Drop on a fill layer to use it there") : dropHint.c_str());
        ImGui::EndGroup();
        dropHint.clear();
        ImGui::EndDragDropSource();
      }
    }
    const char* t = l.instanceOf ? C_("layer type letter", "I") : l.type == LayerType::Fill ? C_("layer type letter", "F")
                                                                                            : C_("layer type letter", "P");
    ImVec2 ts = ImGui::CalcTextSize(t);
    float bw = ts.x + 4 * uiScale;
    ImVec2 b0 = p + ImVec2(thumbS - bw, thumbS - ts.y), b1 = p + ImVec2(thumbS, thumbS);
    dl->AddRectFilled(b0, b1, l.instanceOf ? IM_COL32(60, 140, 190, 230) : l.type == LayerType::Fill ? IM_COL32(200, 112, 36, 230)
                                                                                               : IM_COL32(86, 100, 118, 230), 2.f);
    dl->AddText(b0 + ImVec2(2 * uiScale, 0), IM_COL32(255, 255, 255, 240), t);
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
      ImGui::SetTooltip("%s%s%s",
                        l.instanceOf ? F_("Instance of %s", d.contentLayer(l)->name.c_str()).c_str()
                                     : (l.type == LayerType::Fill ? T_("Fill layer") : T_("Paint layer")),
                        d.contentLayer(l)->type == LayerType::Fill ? "\n" : "",
                        d.contentLayer(l)->type == LayerType::Fill ? T_("Drag it onto another fill layer to give it this material") : "");
  } else {
    ImVec2 p = ImGui::GetCursorScreenPos();
    float s = fh * 0.78f;
    ImVec2 a = p + ImVec2(0, (fh - s) * 0.5f), b = a + ImVec2(s, s);
    if (l.isFolder()) {
      ImU32 c = IM_COL32(214, 168, 82, 255);
      dl->AddRectFilled(a + ImVec2(0, s * 0.22f), b - ImVec2(0, s * 0.08f), c, 2.f);
      dl->AddRectFilled(a + ImVec2(0, s * 0.08f), a + ImVec2(s * 0.45f, s * 0.3f), c, 1.5f);
    } else {
      const char* t = l.type == LayerType::Fill     ? C_("layer type letter", "F")
                      : l.type == LayerType::Adjust ? C_("layer type letter", "A")
                                                    : C_("layer type letter", "P");
      ImU32 c = l.type == LayerType::Fill ? IM_COL32(200, 112, 36, 255) : l.type == LayerType::Adjust ? IM_COL32(138, 96, 196, 255)
                                                                           : IM_COL32(86, 100, 118, 255);
      dl->AddRectFilled(a, b, c, 3.f);
      ImVec2 ts = ImGui::CalcTextSize(t);
      dl->AddText(a + (ImVec2(s, s) - ts) * 0.5f, IM_COL32(255, 255, 255, 240), t);
    }
    ImGui::Dummy(ImVec2(thumbS, fh));
  }
  ImGui::SameLine(0, 6 * uiScale);
  center(rowH);
  float maskW = std::max(52 * uiScale, rowH + 14 * uiScale);
  float nameW = std::max(40.f, ImGui::GetContentRegionAvail().x - maskW);
  if (renameLayerUid == (int)uid) {
    ImGui::SetNextItemWidth(nameW);
    center(fh);
    if (g_focusRename) ImGui::SetKeyboardFocusHere();
    bool done = ImGui::InputText("##rename", &l.name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (done || (!g_focusRename && !ImGui::IsItemActive())) {
      if (l.name.empty()) l.name = "Layer";
      renameLayerUid = 0;
      d.pendingLabel = "Rename: " + l.name;
    }
    g_focusRename = false;
  } else {
    std::string label = l.name;
    if (l.isFolder()) label += "  (" + std::to_string(d.childrenOf(uid).size()) + ")";
    if (!l.visible) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
    bool clickedName = ImGui::Selectable(label.c_str(), selected && !d.editMask, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(nameW, rowH));
    ImGui::PopStyleVar();
    if (clickedName) {
      d.selected = i;
      d.editMask = false;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) renameLayerUid = (int)uid, g_focusRename = true;
    }
    if (!l.visible) ImGui::PopStyleColor();
    if (ImGui::BeginPopupContextItem("##ctx")) {
      layerContextMenu(i);
      ImGui::EndPopup();
    }
    if (ImGui::BeginDragDropSource()) {
      ImGui::SetDragDropPayload("PATINA_LAYER", &uid, sizeof uid);
      ImGui::TextUnformatted((ImGui::GetIO().KeyCtrl && engine.features.on("drag_drop") ? F_("Copy %s", l.name.c_str())
                                                                                        : F_("Move %s", l.name.c_str()))
                                 .c_str());
      ImGui::TextDisabled("%s", dropHint.empty() ? T_("Ctrl: a copy") : dropHint.c_str());
      dropHint.clear();
      ImGui::EndDragDropSource();
    }
  }
  ImGui::SameLine(0, 4 * uiScale);
  bool maskSel = selected && d.editMask;
  const float maskBtnH = std::round(ImGui::GetTextLineHeight() + 6 * uiScale);
  center(th.hasMask ? rowH - 2 : maskBtnH);
  if (th.hasMask) {
    // the mask (painted, or the result of the effect stack) as a clickable thumbnail
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1, 1));
    bool clickedMask = ImGui::ImageButton("##maskthumb", texId(th.mask.id()), ImVec2(rowH - 4, rowH - 4), ImVec2(0, 1), ImVec2(1, 0));
    ImGui::PopStyleVar();
    if (clickedMask) {
      d.selected = i;
      d.editMask = true;
    }
    maskDragSource(l, th.mask.id());
    if (maskSel) dl->AddRect(p - ImVec2(1, 1), ImGui::GetItemRectMax() + ImVec2(1, 1), ui::themeColor(ui::theme().accent), 2.f, 0, 2.f);
    if (!l.maskEffects.empty()) dl->AddText(p + ImVec2(rowH + 1, 0), IM_COL32(200, 200, 210, 230), C_("mask effects badge", "fx"));
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", l.maskEffects.empty()
                                  ? T_("The mask: click to paint it (white reveals, erase hides)\nDrag it onto another layer to copy it there")
                                  : T_("The mask (effect stack): click to paint its Paint effect\nDrag it onto another layer to copy it there"));
  } else if (l.mask || l.masksOnly()) {
    std::string tip = std::string(l.mask ? T_("Paint this layer's mask (white reveals, erase hides)") : T_("Paint to create this layer's mask (it starts black: paint reveals)")) +
                      (l.mask || l.generator.type != MaskGenerator::None || !l.maskEffects.empty() ? std::string("\n") + T_("Drag it onto another layer to copy the mask there") : "");
    if (iconButton("maskbtn", "mask", l.mask ? T_("Mask") : T_("Add mask"), maskSel ? ui::IconState::Chosen : ui::IconState::Normal, maskBtnH, tip.c_str())) {
      d.selected = i;
      d.editMask = true;
    }
    if (l.mask || l.generator.type != MaskGenerator::None || !l.maskEffects.empty()) maskDragSource(l, 0);
  } else {
    if (iconButton("addmask", "mask", T_("Add mask"), ui::IconState::Normal, maskBtnH, T_("Add a white mask and paint it (erase hides)"))) {
      g_deferred.push_back([&d, uid] {
        int k = d.indexOf(uid);
        d.addMask(k, 1.f);
        d.selected = k;
        d.editMask = true;
      });
    }
  }
  // (0.22.5) anything dragged onto the row (see stackDropTarget)
  ImRect row(rowMin, ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x, rowMin.y + rowH));
  engine.uiRects["layers.row:" + std::to_string(uid)] = {row.Min.x, row.Min.y, row.GetWidth(), row.GetHeight()};  // (for tests)
  stackDropTarget(row, i);
  ImGui::PopID();
}

// (0.41) the baked map layers, under the stack: their maps are what smart masks, the 3D view and exports read
void App::drawBakedSection() {
  Document& d = engine.doc;
  ImGui::SeparatorText(T_("Baked maps"));
  {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects["layers.baked"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("The ambient occlusion, curvature and thickness baked from the model's shape.\n"
                               "Select one and paint on the model to fix it; its properties have adjustments.\n"
                               "Smart masks, the 3D view and exports use the fixed map."));
  if (contextMenus() && ImGui::BeginPopupContextItem("##bakedhdr")) {
    if (!bakedMapMenuItems()) ImGui::TextDisabled("%s", T_("Every baked map has its layer"));
    ImGui::EndPopup();
  }
  std::vector<int> rows;
  for (int i = 0; i < (int)d.layers.size(); ++i)
    if (d.layers[i]->isBaked()) rows.push_back(i);
  std::stable_sort(rows.begin(), rows.end(), [&](int a, int b) { return d.layers[a]->bakedMap < d.layers[b]->bakedMap; });
  for (int i : rows) drawBakedRow(i);
  // (0.44.2) maps baked for other UVs: UV edits moved them along with the paint, which blurs and breaks them where
  // islands changed (dark patches in the 3D view)
  if (int stale = d.bakesOutOfDate()) {
    ImGui::TextColored(ImVec4(1.f, 0.72f, 0.42f, 1.f), "%s", T_("Baked for other UVs"));
    ImGui::SameLine();
    if (ImGui::SmallButton(L_("Re-bake##stalebakes"))) g_deferred.push_back([&d] { d.rebakeOutOfDate(); });
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects["baked.rebake"] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(T_("The UVs changed since %s%s%s were baked. They moved along with the paint, which\n"
                           "blurs them and breaks them where islands changed (dark patches in the 3D view).\n"
                           "Re-bake makes them exact again; your fixes stay."),
                        stale & 1 ? T_("ambient occlusion ") : "", stale & 2 ? T_("curvature ") : "",
                        stale & 4 ? T_("thickness ") : "");
  } else {
    engine.uiRects.erase("baked.rebake");  // (tests: gone when the bakes fit)
  }
  if (!engine.features.on("baked_layers"))
    ImGui::TextDisabled("%s", T_("Switched off in Preferences > Features: they change nothing."));
  // maps baked without a layer (it was deleted): a button gives them one
  for (int k = 0; k < kBakedMapCount && engine.features.on("baked_layers"); ++k) {
    if (!d.tex.hasBakedMap(k) || d.bakedLayer(k)) continue;
    std::string label = F_("Add a layer for the %s", T_(bakedMapLabel(k))) + "##addbaked" + std::to_string(k);
    if (ImGui::SmallButton(label.c_str())) g_deferred.push_back([&d, k] { d.addBakedLayer(k); });
    {
      ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
      engine.uiRects[std::string("baked.add:") + bakedMapKey(k)] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Without a layer the map is used as it was baked"));
  }
}

void App::drawBakedRow(int i) {
  Document& d = engine.doc;
  Layer& l = *d.layers[i];
  uint32_t uid = l.uid;
  ImGui::PushID((int)uid);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  float fh = ImGui::GetFrameHeight();
  float rowH = std::round(fh * 1.5f);
  float y0 = ImGui::GetCursorPosY();
  auto center = [&](float h) { ImGui::SetCursorPosY(y0 + std::floor((rowH - h) * 0.5f)); };
  ImVec2 rowMin = ImGui::GetCursorScreenPos();
  bool selected = d.selected == i;
  ImGui::Dummy(ImVec2(6 * uiScale, rowH));
  ImGui::SameLine(0, 0);
  center(fh);
  bool vis = l.visible;
  if (ImGui::Checkbox("##vis", &vis)) l.visible = vis, d.markComposite();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal))
    ImGui::SetTooltip("%s", T_("The map on or off in the 3D view and in exports (ambient occlusion shades the model\n"
                               "and goes out as a map of its own). Smart masks keep using it either way.\n"
                               "On: with your fixes and adjustments."));
  ImGui::SameLine(0, 4 * uiScale);
  center(fh);
  ImGui::Dummy(ImVec2(fh, fh));  // (where folders have their arrow)
  ImGui::SameLine(0, 4 * uiScale);
  // the map it makes, with the type letter
  const LayerThumb& th = layerThumb(l);
  float thumbS = rowH - 4;
  center(thumbS);
  ImVec2 p = ImGui::GetCursorScreenPos();
  if (ImGui::InvisibleButton("##content", ImVec2(thumbS, thumbS))) d.selected = i, d.editMask = false;
  if (th.hasContent) dl->AddImage(texId(th.content.id()), p, p + ImVec2(thumbS, thumbS), ImVec2(0, 1), ImVec2(1, 0));
  const char* t = C_("layer type letter", "B");
  ImVec2 ts = ImGui::CalcTextSize(t);
  float bw = ts.x + 4 * uiScale;
  ImVec2 b0 = p + ImVec2(thumbS - bw, thumbS - ts.y), b1 = p + ImVec2(thumbS, thumbS);
  dl->AddRectFilled(b0, b1, IM_COL32(46, 150, 138, 230), 2.f);
  dl->AddText(b0 + ImVec2(2 * uiScale, 0), IM_COL32(255, 255, 255, 240), t);
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal)) ImGui::SetTooltip(T_("Baked map: %s"), T_(bakedMapLabel(l.bakedMap)));
  ImGui::SameLine(0, 6 * uiScale);
  center(rowH);
  float stateW = 44 * uiScale;
  float nameW = std::max(40.f, ImGui::GetContentRegionAvail().x - stateW);
  if (renameLayerUid == (int)uid) {
    ImGui::SetNextItemWidth(nameW);
    center(fh);
    if (g_focusRename) ImGui::SetKeyboardFocusHere();
    bool done = ImGui::InputText("##rename", &l.name, ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll);
    if (done || (!g_focusRename && !ImGui::IsItemActive())) {
      if (l.name.empty()) l.name = bakedMapLabel(l.bakedMap);
      renameLayerUid = 0;
      d.pendingLabel = "Rename: " + l.name;
    }
    g_focusRename = false;
  } else {
    if (!l.visible) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushStyleVar(ImGuiStyleVar_SelectableTextAlign, ImVec2(0.f, 0.5f));
    bool clicked = ImGui::Selectable(l.name.c_str(), selected, ImGuiSelectableFlags_AllowDoubleClick, ImVec2(nameW, rowH));
    ImGui::PopStyleVar();
    if (clicked) {
      d.selected = i;
      d.editMask = false;
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) renameLayerUid = (int)uid, g_focusRename = true;
    }
    if (!l.visible) ImGui::PopStyleColor();
    if (ImGui::BeginPopupContextItem("##ctx")) {
      bakedContextMenu(i);
      ImGui::EndPopup();
    }
  }
  // what changes the map: painted fixes, adjustments, strength
  ImGui::SameLine(0, 4 * uiScale);
  center(ImGui::GetTextLineHeight());
  std::string state;
  if (l.mask) state += C_("baked layer badge: painted fixes", "fix");
  if (!l.maskEffects.empty()) state += std::string(state.empty() ? "" : " ") + C_("mask effects badge", "fx");
  if (l.bakedStrength != 1.f) state += (state.empty() ? "" : " ") + F_("x%.1f", l.bakedStrength);
  ImGui::TextDisabled("%s", state.c_str());
  if (!state.empty() && ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("fix: fixes painted over the bake\nfx: adjustments\nx: the strength"));
  ImGui::SetCursorPosY(y0 + rowH + ImGui::GetStyle().ItemSpacing.y);
  ImGui::Dummy(ImVec2(0, 0));
  ImRect row(rowMin, ImVec2(ImGui::GetWindowPos().x + ImGui::GetWindowContentRegionMax().x, rowMin.y + rowH));
  engine.uiRects["layers.row:" + std::to_string(uid)] = {row.Min.x, row.Min.y, row.GetWidth(), row.GetHeight()};  // (for tests)
  ImGui::PopID();
}

void App::bakedContextMenu(int i) {
  Document& d = engine.doc;
  Layer& l = *d.layers[i];
  uint32_t uid = l.uid;
  auto idx = [&d, uid] { return d.indexOf(uid); };
  ImGui::TextDisabled("%s", l.name.c_str());
  ImGui::Separator();
  if (ImGui::MenuItem(l.visible ? L_("Show the Bake as It Came Out") : L_("Turn the Fixes On Again"), T_("click the check")))
    l.visible = !l.visible, d.markComposite();
  if (ImGui::MenuItem(L_("Rename"), T_("double-click"))) renameLayerUid = (int)uid, g_focusRename = true;
  ImGui::Separator();
  auto rectOf = [this](const char* key) {
    ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    engine.uiRects[key] = {a.x, a.y, b.x - a.x, b.y - a.y};  // (for tests)
  };
  if (ImGui::MenuItem(L_("Reset the Fixes"), nullptr, false, (bool)l.mask)) g_deferred.push_back([&d, idx] { d.removeMask(idx()); });
  rectOf("menu.baked_reset");
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Takes the painted fixes away: the bake shows again (undoable)"));
  if (ImGui::MenuItem(L_("Remove the Adjustments"), nullptr, false, !l.maskEffects.empty() || l.bakedStrength != 1.f)) {
    l.maskEffects.clear();
    l.bakedStrength = 1.f;
    d.markComposite();
  }
  if (ImGui::MenuItem(L_("Bake Again"))) {
    int k = l.bakedMap;
    g_deferred.push_back([this, k] { bakeMapAgain(k); });
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("A new bake under the same fixes and adjustments"));
  if (ImGui::MenuItem(L_("Export This Map..."))) d.selected = i, exportWhat = 1, showExport = true;
  ImGui::Separator();
  if (ImGui::MenuItem(L_("Delete (Use the Bake as It Is)"), keyLabel("layer.delete")))
    g_deferred.push_back([&d, idx] { d.removeLayer(idx()); });
  rectOf("menu.baked_delete");
}

// (0.22.5) a material dropped on the layer stack: from the library (only its id: the layer's own placement
// and tweaks stay, or the paint material's for a new layer) or from a fill layer (all its settings); on a
// fill layer it replaces the material, elsewhere it makes a fill layer (target -1: the bottom)
void App::dropMaterial(const MaterialRef& m, bool fromLibrary, int target, bool replace, bool into, bool below) {
  Document& d = engine.doc;
  const auto* e = m.id.empty() ? nullptr : engine.materials.find(m.id);
  if (fromLibrary && !e) return;
  std::string name = e ? e->def.name : std::string("Fill");
  if (replace && target >= 0 && target < (int)d.layers.size()) {
    const Layer* c = d.contentLayer(*d.layers[target]);
    const auto* old = c->material.id.empty() ? nullptr : engine.materials.find(c->material.id);
    MaterialRef r = fromLibrary ? c->material : m;
    if (fromLibrary) r.id = e->def.id;
    if (d.replaceFillMaterial(target, r, old ? old->def.name : std::string("Fill"), name))
      setStatus(F_("Replaced the material of %s with %s", d.layers[target]->name.c_str(), name.c_str()));
    d.selected = target, d.editMask = false;
    return;
  }
  MaterialRef r = fromLibrary ? engine.painter.material : m;
  if (fromLibrary) r.id = e->def.id;
  Layer& l = d.addFillLayerAt(r, name, target, into, below);
  setStatus(F_("Added the fill layer %s", l.name.c_str()));
}

// (0.22.5) drag and drop onto the layer stack: a layer row (index i), or the space under the rows (i = -1,
// the bottom of the stack). A layer moves there (Ctrl: a copy); a material from the library or a fill
// layer's material replaces a fill layer's in the middle of its row, else makes a new fill layer there; a
// smart material goes in there; a smart mask or another layer's mask goes onto the layer. In the middle of a
// folder's row things go into it.
void App::stackDropTarget(const ImRect& r, int i) {
  const ImGuiPayload* pl = ImGui::GetDragDropPayload();
  if (!pl || !ImGui::BeginDragDropTargetCustom(r, ImGui::GetID(i >= 0 ? "##rowdrop" : "##bottomdrop"))) return;
  Document& d = engine.doc;
  const Layer* l = i >= 0 && i < (int)d.layers.size() ? d.layers[i].get() : nullptr;
  const uint32_t uid = l ? l->uid : 0;
  const float y = ImGui::GetIO().MousePos.y, h = r.GetHeight();
  const bool middle = l && y > r.Min.y + h * 0.25f && y < r.Max.y - h * 0.25f;
  const bool below = l && y > r.GetCenter().y;
  const bool folder = l && l->isFolder();
  const bool fill = l && !folder && d.contentLayer(*l)->type == LayerType::Fill;
  const std::string name = l ? l->name : std::string();
  enum Kind { Line, Into, Onto } kind = Line;
  // (whole sentences, for the translations: at the bottom, in the folder, below or above the layer)
  auto where = [&](const std::string& bottom, const std::string& in, const std::string& under, const std::string& over) {
    if (!l) return bottom;
    if (kind == Into) return in;
    return below ? under : over;
  };
  const char* n = name.c_str();
  auto at = [&d, uid] { return uid ? d.indexOf(uid) : -1; };  // (at the time it runs)
  const ImGuiDragDropFlags fl = ImGuiDragDropFlags_AcceptBeforeDelivery | ImGuiDragDropFlags_AcceptNoDrawDefaultRect;
  bool shown = false;
  if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PATINA_LAYER", fl)) {
    uint32_t src = *(const uint32_t*)p->Data;
    bool copy = ImGui::GetIO().KeyCtrl && engine.features.on("drag_drop");
    kind = folder && middle ? Into : Line;
    dropHint = src == uid && !copy ? ""
               : copy ? where(T_("A copy at the bottom"), F_("A copy in %s", n), F_("A copy below %s", n), F_("A copy above %s", n))
                      : where(T_("Moves it at the bottom"), F_("Moves it in %s", n), F_("Moves it below %s", n), F_("Moves it above %s", n));
    shown = src != uid || copy;
    if (p->IsDelivery()) {
      bool into = kind == Into;
      g_deferred.push_back([&d, src, at, into, below, copy] {
        if (copy) d.copyLayerTo(d.indexOf(src), at(), into, below);
        else d.moveOnto(d.indexOf(src), at(), into, below);
      });
    }
  }
  for (const char* type : {"PATINA_MATERIAL", "PATINA_FILL"})
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type, fl)) {
      bool lib = type[7] == 'M';
      MaterialRef m;
      if (lib) m.id = (const char*)p->Data;
      else if (const Layer* src = d.findLayer(*(const uint32_t*)p->Data)) m = d.contentLayer(*src)->material;
      kind = fill && middle ? Onto : folder && middle ? Into : Line;
      dropHint = kind == Onto ? F_("Replaces the material of %s", n)
                              : where(T_("A new fill layer at the bottom"), F_("A new fill layer in %s", n),
                                      F_("A new fill layer below %s", n), F_("A new fill layer above %s", n));
      shown = true;
      if (p->IsDelivery()) {
        bool onto = kind == Onto, into = kind == Into;
        g_deferred.push_back([this, m, lib, at, onto, into, below] { dropMaterial(m, lib, at(), onto, into, below); });
      }
    }
  if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PATINA_SMART", fl)) {
    std::string path((const char*)p->Data);
    kind = folder && middle ? Into : Line;
    dropHint = where(T_("The smart material at the bottom"), F_("The smart material in %s", n), F_("The smart material below %s", n),
                     F_("The smart material above %s", n));
    shown = true;
    if (p->IsDelivery()) {
      bool into = kind == Into;
      g_deferred.push_back([this, &d, path, at, into, below] {
        std::string err;
        if (applySmartMaterialAt(d, path, at(), into, below, &err) < 0) showError(T_("Could not apply the smart material"), err);
        else setStatus(F_("Added the smart material %s", d.selectedLayer()->name.c_str()));
      });
    }
  }
  if (l) {
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PATINA_SMARTMASK", fl)) {
      int t = *(const int*)p->Data;
      kind = Onto;
      dropHint = l->maskEffects.empty() ? F_("Smart mask for %s", n) : F_("Smart mask added to the mask effects of %s", n);
      shown = true;
      if (p->IsDelivery())
        g_deferred.push_back([this, &d, t, at] {
          if (d.addSmartMask(at(), t)) d.selected = at(), d.editMask = false, setStatus(F_("Smart mask on %s", d.layers[at()]->name.c_str()));
        });
    }
    if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("PATINA_MASK", fl)) {
      uint32_t src = *(const uint32_t*)p->Data;
      kind = Onto;
      shown = src != uid;
      dropHint = shown ? F_("Copies the mask to %s", n) : "";
      if (p->IsDelivery() && shown)
        g_deferred.push_back([this, &d, src, at] {
          if (d.copyMaskTo(d.indexOf(src), at())) d.selected = at(), setStatus(F_("Copied the mask to %s", d.layers[at()]->name.c_str()));
        });
    }
  }
  if (shown) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 c = ui::themeColor(ui::theme().accent);
    if (kind == Onto) dl->AddRectFilled(r.Min, r.Max, ui::themeColor(ui::theme().accent, 0.196f), 3.f), dl->AddRect(r.Min, r.Max, c, 3.f, 0, 2.f);
    else if (kind == Into) dl->AddRect(r.Min, r.Max, c, 3.f, 0, 2.f);
    else {
      float ly = !l ? r.Min.y + 1 : below ? r.Max.y : r.Min.y;
      dl->AddLine(ImVec2(r.Min.x, ly), ImVec2(r.Max.x, ly), c, 2.5f);
    }
  }
  ImGui::EndDragDropTarget();
}

void App::drawLayers() {
  thumbBudget = 3;  // layer thumbnails refreshed this frame
  beginPanel(L_("Layers"));
  Document& d = engine.doc;
  g_deferred.clear();
  drawObjectsSection();  // (1.2) the scene's objects: the layers below are the active one's
  if (engine.features.on("scene_objects") && engine.objectCount() > 1) {
    ImGui::Spacing();
    ImGui::SeparatorText(F_("Layers of %s", d.displayName().c_str()).c_str());
  }
  layersButtons();
  layersTree(0.f);
  layerProperties();
  endPanel();
}

// (1.2.7) the Layers panel's parts, which the Blender layout's Outliner and Properties editor take apart

void App::layersButtons() {
  Document& d = engine.doc;
  // ---- add buttons (0.36: icons, as in a paint program's layers panel)
  using ui::IconState;
  const float fh = ImGui::GetFrameHeight();
  if (iconButton("layer.new_paint", "add_paint", T_("Paint layer"), IconState::Normal, fh, T_("New paint layer above the selection"),
                 "layer.new_paint"))
    d.addPaintLayer();
  ImGui::SameLine(0, 2);
  auto addFill = [&](float mask) { addFillLayerUI(mask); };
  if (iconButton("layer.new_fill", "add_fill", T_("Fill layer"), IconState::Normal, fh,
                 T_("New fill layer with the current paint material\n(arrow: with a black or white mask)")))
    addFill(-1);
  ImGui::SameLine(0, 0);
  if (ImGui::ArrowButton("##fillmore", ImGuiDir_Down)) ImGui::OpenPopup("FillMore");
  if (ImGui::BeginPopup("FillMore")) {
    if (ImGui::MenuItem(L_("Fill Layer"))) addFill(-1);
    if (ImGui::MenuItem(L_("Fill Layer with a Black Mask"))) addFill(0.f);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Hidden until you paint its mask - the usual way to paint a material on"));
    if (ImGui::MenuItem(L_("Fill Layer with a White Mask"))) addFill(1.f);
    ImGui::EndPopup();
  }
  ImGui::SameLine(0, 2);
  if (iconButton("layer.new_adjust", "add_adjust", T_("Adjustment layer"), IconState::Normal, fh,
                 T_("New adjustment layer: hue / saturation / brightness / contrast and\n"
                    "roughness / metallic / height offsets applied to everything below")))
    d.addAdjustLayer();
  ImGui::SameLine(0, 2);
  {
    std::string tip = F_("New folder above the selection. Drag layers onto it, or select a layer and press %s\n"
                         "to put it into a new folder. Folders have their own opacity, blend mode and mask.",
                         keyLabel("layer.group") ? keyLabel("layer.group") : "-");
    if (iconButton("layer.new_folder", "add_folder", T_("Folder"), IconState::Normal, fh, tip.c_str())) d.addFolder();
  }
  ImGui::SameLine(0, 2);
  if (iconButton("layer.smart", "smart", T_("Smart materials"), IconState::Normal, fh,
                 T_("Ready-made layer stacks that adapt to the mesh (edges, cavities...)")))
    ImGui::OpenPopup("SmartMenu");
  if (ImGui::BeginPopup("SmartMenu")) {
    smartMaterialItems([](std::function<void()> f) { f(); });
    ImGui::Separator();
    if (ImGui::MenuItem(L_("Save the Selected Layer as a Smart Material..."), nullptr, false, d.selectedLayer() != nullptr)) {
      smartSaveName = d.selectedLayer()->name;
      smartSaveIncludePaint = false;
      g_deferred.push_back([] { ImGui::OpenPopup("Save smart material"); });
    }
    if (ImGui::MenuItem(L_("Open the Smart Materials Folder"))) {
#ifdef _WIN32
      std::string url = "file:///" + paths::ensureDir(paths::userDataDir() / "smart_materials").generic_string();
#else
      std::string url = "file://" + paths::ensureDir(paths::userDataDir() / "smart_materials").generic_string();
#endif
      SDL_OpenURL(url.c_str());
    }
    ImGui::EndPopup();
  }
  // ---- arrange buttons (on the same row when the panel is wide enough)
  {
    float need = 4 * iconButtonWidth(T_("Duplicate"), fh) + 6 + 2 * ImGui::GetStyle().ItemSpacing.x;
    if (ImGui::GetContentRegionAvail().x - (ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x) > need) ImGui::SameLine(0, ImGui::GetStyle().ItemSpacing.x * 2.5f);
  }
  const bool bakedSel = d.selectedLayer() && d.selectedLayer()->isBaked();  // (0.41) one layer per baked map, under the stack
  ImGui::BeginDisabled(d.selected < 0 || bakedSel);
  if (iconButton("layer.duplicate", "duplicate", T_("Duplicate"), IconState::Normal, fh, T_("Duplicate the selected layer"), "layer.duplicate"))
    g_deferred.push_back([&d] { d.duplicateLayer(d.selected); });
  ImGui::EndDisabled();
  ImGui::BeginDisabled(d.selected < 0);
  ImGui::SameLine(0, 2);
  if (iconButton("layer.delete", "delete", T_("Delete"), IconState::Normal, fh, T_("Delete the selected layer (with its contents if it is a folder)"),
                 "layer.delete"))
    g_deferred.push_back([&d] { d.removeLayer(d.selected); });
  ImGui::EndDisabled();
  ImGui::BeginDisabled(d.selected < 0 || bakedSel);
  ImGui::SameLine(0, 2);
  if (iconButton("layer.up", "move_up", T_("Move up"), IconState::Normal, fh, T_("Move the selected layer up the stack")))
    g_deferred.push_back([&d] { d.moveSibling(d.selected, +1); });
  ImGui::SameLine(0, 2);
  if (iconButton("layer.down", "move_down", T_("Move down"), IconState::Normal, fh, T_("Move the selected layer down the stack")))
    g_deferred.push_back([&d] { d.moveSibling(d.selected, -1); });
  ImGui::EndDisabled();
  if (d.soloUid) {
    ImGui::SameLine();
    int si = d.indexOf(d.soloUid);
    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.75f, 0.58f, 0.15f, 1));
    if (ImGui::SmallButton((F_("Solo: %s  x", (si >= 0 ? d.layers[si]->name : std::string("?")).c_str()) + "###solo").c_str())) {
      d.soloUid = 0;
      d.compositeDirty = true;
    }
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Only this layer is shown. Click to show all layers again."));
  }

}

void App::layersTree(float height) {
  Document& d = engine.doc;
  // ---- the tree (top of the stack first)
  // (rows are half as tall again as a widget: drawLayerRow; and room under them to drop things at the bottom)
  const float rowStep = std::round(ImGui::GetFrameHeight() * 1.5f) + ImGui::GetStyle().ItemSpacing.y;
  bool bakedSection = false;  // (0.41) baked map layers, or maps that could have one
  for (auto& l : d.layers) bakedSection |= l->isBaked();
  for (int k = 0; k < kBakedMapCount && !bakedSection; ++k) bakedSection = d.tex.hasBakedMap(k) && engine.features.on("baked_layers");
  size_t stackRows = 0;
  for (auto& l : d.layers) stackRows += !l->isBaked();
  float listH = std::min(420.f * uiScale, 16.f * uiScale + (stackRows + 1) * rowStep);
  // (0.22.5) and no taller than the window shows: a long stack scrolls in the list, which keeps the space under
  // its rows in sight (the window's own scroll was the only way down to them)
  float bakedH = bakedSection ? (float(d.layers.size() - stackRows) + 1.4f) * rowStep : 0.f;  // (room for the section under it)
  listH = std::min(listH, std::max(4 * rowStep, ImGui::GetContentRegionAvail().y - 4 * uiScale - bakedH));
  if (height > 0.f) listH = std::max(4 * rowStep, height - bakedH);  // (1.2.7: the Outliner gives it its height)
  ImGui::BeginChild("##layerlist", ImVec2(0, listH), ImGuiChildFlags_Borders);
  {
    ImVec2 a = ImGui::GetWindowPos(), b = ImGui::GetWindowSize();
    engine.uiRects["layers.list"] = {a.x, a.y, b.x, b.y};  // (for tests)
  }
  // (1.2.7) the Outliner's search: the layers whose name has it, and the folders they are in (opened)
  std::string want = outlinerDrawing ? blui.outlinerFilter : std::string();
  std::transform(want.begin(), want.end(), want.begin(), ::tolower);
  std::function<bool(int)> matches = [&](int i) {
    std::string name = d.layers[i]->name;
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    if (name.find(want) != std::string::npos) return true;
    if (d.layers[i]->isFolder())
      for (auto k : d.childrenOf(d.layers[i]->uid))
        if (matches(k)) return true;
    return false;
  };
  std::function<void(uint32_t, int)> level = [&](uint32_t parent, int depth) {
    auto kids = d.childrenOf(parent);
    for (auto it = kids.rbegin(); it != kids.rend(); ++it) {
      if (d.layers[*it]->isBaked()) continue;  // (their own section, under the stack)
      if (!want.empty() && !matches(*it)) continue;
      drawLayerRow(*it, depth);
      const Layer& l = *d.layers[*it];
      if (l.isFolder() && (l.expanded || !want.empty()) && depth < 32) level(l.uid, depth + 1);
    }
  };
  level(0, 0);
  // drop below everything = bottom of the stack
  ImGui::InvisibleButton("##bottomdrop", ImVec2(std::max(1.f, ImGui::GetContentRegionAvail().x), std::max(rowStep, ImGui::GetContentRegionAvail().y)));
  if (contextMenus() && ImGui::BeginPopupContextItem("##stackctx")) layersEmptyMenu(), ImGui::EndPopup();
  {
    ImRect r(ImGui::GetItemRectMin(), ImGui::GetItemRectMax());
    engine.uiRects["layers.bottom"] = {r.Min.x, r.Min.y, r.GetWidth(), r.GetHeight()};  // (for tests)
    stackDropTarget(r, -1);
  }
  // (0.22.5) something dragged near the list's top or bottom edge scrolls it
  if (ImGui::GetDragDropPayload() && ImGui::GetScrollMaxY() > 0) {
    ImVec2 m = ImGui::GetIO().MousePos, wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const float edge = 28 * uiScale, speed = 600 * uiScale * ImGui::GetIO().DeltaTime;
    if (m.x >= wp.x && m.x <= wp.x + ws.x) {
      if (m.y >= wp.y - edge * 0.5f && m.y < wp.y + edge) ImGui::SetScrollY(std::max(0.f, ImGui::GetScrollY() - speed));
      else if (m.y > wp.y + ws.y - edge && m.y <= wp.y + ws.y + edge * 0.5f)
        ImGui::SetScrollY(std::min(ImGui::GetScrollMaxY(), ImGui::GetScrollY() + speed));
    }
  }
  ImGui::EndChild();
  if (bakedSection) drawBakedSection();  // (0.41) under the list: the stack's scrolling and drop places stay as they were
  for (auto& f : g_deferred) f();
  g_deferred.clear();

  // ---- save-as-smart-material dialog
  if (ImGui::BeginPopupModal(L_("Save smart material"), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::SetNextItemWidth(260 * uiScale);
    ImGui::InputText(L_("Name"), &smartSaveName);
    ImGui::Checkbox(L_("Include painted textures and masks"), &smartSaveIncludePaint);
    ImGui::TextDisabled("%s", T_("Without paint the smart material works on any mesh (fill layers, smart masks, adjustments).\n"
                                 "Painted textures only fit meshes with the same UV layout."));
    if (ImGui::Button(L_("Save"), ImVec2(110 * uiScale, 0))) {
      std::string path, err;
      if (saveSmartMaterial(d, d.selected, smartSaveName, "", smartSaveIncludePaint, &path, &err))
        setStatus(F_("Saved smart material %s", path.c_str()));
      else
        showError(T_("Could not save the smart material"), err);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button(L_("Cancel")) || ImGui::IsKeyPressed(ImGuiKey_Escape)) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

}

void App::layerProperties() {
  Document& d = engine.doc;
  // ---- properties of the selected layer
  Layer* l = d.selectedLayer();
  if (!l) {
    ImGui::TextDisabled("%s", T_("No layer selected"));
    return;
  }
  if (l->isBaked()) {
    drawBakedProperties(*l);
    for (auto& f : g_deferred) f();
    g_deferred.clear();
    return;
  }
  bool changed = false;
  if (ImGui::InputText(L_("Name"), &l->name)) d.pendingLabel = "Rename: " + l->name;
  ImGui::AlignTextToFramePadding();
  ImGui::TextUnformatted(T_("Tag"));
  ImGui::SameLine(ImGui::GetStyle().ItemInnerSpacing.x + 60 * uiScale);
  tagPicker(*l);
  changed |= ImGui::SliderFloat(L_("Opacity"), &l->opacity, 0.f, 1.f);
  int bm = (int)l->blend;
  const char* blends = Z_("Normal\0Multiply\0Screen\0Overlay\0Add\0Subtract\0Darken\0Lighten\0Add Signed\0Soft Light\0");
  if (ImGui::Combo(L_("Blend"), &bm, blends)) l->blend = (BlendMode)bm, changed = true;
  ImGui::TextUnformatted(T_("Channels"));
  bool moreChannels = engine.features.on("more_channels");
  for (int c = 0; c < kChannelCount; ++c) {
    if (isExtraChannel(c) && !moreChannels) continue;
    bool on = l->channelMask & (1 << c);
    if (c % 3) ImGui::SameLine();
    if (ImGui::Checkbox(L_((std::string(channelInfo((Channel)c).label) + "##ch").c_str()), &on)) {
      l->channelMask = on ? l->channelMask | (1 << c) : l->channelMask & ~(1 << c);
      changed = true;
    }
    // (1.2.2) two things are called opacity here: the layer's, and the see-through channel's
    if (c == (int)Channel::Opacity && ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("The see-through channel (alpha): 0 is see-through, 1 is solid. Hair cards, lace and "
                                 "leaves use it; a model's color texture with alpha brings it in. The Opacity slider "
                                 "above is how strongly the whole layer shows."));
  }
  if (ImGui::TreeNode(L_("Channel opacity"))) {
    for (int c = 0; c < kChannelCount; ++c)
      if (!isExtraChannel(c) || moreChannels) {
        changed |= ImGui::SliderFloat(L_((std::string(channelInfo((Channel)c).label) + "##cho").c_str()), &l->channelOpacity[c], 0.f, 1.f);
        if (c == (int)Channel::Opacity && ImGui::IsItemHovered())
          ImGui::SetTooltip("%s", T_("How strongly this layer's see-through values apply: 1 as they are, 0 not at all "
                                     "(the layers below show through as before)"));
      }
    ImGui::TreePop();
  }
  if (l->isFolder()) {
    ImGui::SeparatorText(T_("Folder"));
    ImGui::TextWrapped("%s", T_("Opacity, blend mode, channels and the mask below apply to everything in this folder. "
                                "Painting on a folder paints its mask."));
  }
  Layer* content = d.contentLayer(*l);  // instances edit their source's content
  if (content != l) {
    ImGui::SeparatorText(T_("Instance"));
    ImGui::TextWrapped(T_("Shows the content of '%s'. Opacity, blend, channels and the mask are this layer's own; "
                          "the material / paint below is shared with the source."), content->name.c_str());
    if (ImGui::SmallButton(L_("Select the source"))) d.selected = d.indexOf(content->uid), d.editMask = false;
    ImGui::SameLine();
    if (ImGui::SmallButton(L_("Make independent"))) {
      int si = d.selected;
      g_deferred.push_back([&d, si] { d.makeIndependent(si); });
    }
  }
  if (content->type == LayerType::Adjust) {
    ImGui::SeparatorText(T_("Adjustment"));
    AdjustParams& a = content->adjust;
    // (0.33) develop the color, as a photo editor's basic controls
    bool develop = engine.features.on("texture_develop");
    if (develop) {
    ImGui::TextDisabled("%s", T_("White balance"));
    changed |= ImGui::SliderFloat(L_("Temperature"), &a.temperature, -1.f, 1.f);
    ImGui::SetItemTooltip("%s", T_("Cooler (blue) to warmer (yellow)"));
    changed |= ImGui::SliderFloat(L_("Tint"), &a.tint, -1.f, 1.f);
    ImGui::SetItemTooltip("%s", T_("Greener to more magenta"));
    ImGui::TextDisabled("%s", T_("Tone"));
    changed |= ImGui::SliderFloat(L_("Exposure"), &a.exposure, -3.f, 3.f, T_("%+.2f stops"));
    changed |= ImGui::SliderFloat(L_("Contrast"), &a.contrast, 0.f, 3.f);
    changed |= ImGui::SliderFloat(L_("Highlights"), &a.highlights, -1.f, 1.f);
    ImGui::SetItemTooltip("%s", T_("The bright parts darker or brighter"));
    changed |= ImGui::SliderFloat(L_("Shadows"), &a.shadows, -1.f, 1.f);
    ImGui::SetItemTooltip("%s", T_("The dark parts darker or brighter"));
    changed |= ImGui::SliderFloat(L_("Brightness"), &a.brightness, 0.f, 3.f);
    ImGui::TextDisabled("%s", T_("Presence"));
    changed |= ImGui::SliderFloat(L_("Vibrance"), &a.vibrance, -1.f, 1.f);
    ImGui::SetItemTooltip("%s", T_("More (or less) color, mostly in the dull colors: skin and bright colors keep theirs"));
    changed |= ImGui::SliderFloat(L_("Saturation"), &a.saturation, 0.f, 3.f);
    changed |= ImGui::SliderFloat(L_("Hue shift"), &a.hue, -0.5f, 0.5f, T_("%.3f turns"));
    ImGui::TextDisabled("%s", T_("Baked lighting"));
    float dl = a.delight;
    if (ImGui::SliderFloat(L_("De-light"), &a.delight, 0.f, 1.f)) {
      changed = true;
      if (dl <= 0.f && a.delight > 0.f) d.ensureBakedFor(*content);  // the ambient occlusion it reads
    }
    ImGui::SetItemTooltip("%s", T_("Takes the shadows baked into the color out (scans, generated models): brightens what "
                                   "the ambient occlusion darkens. Bakes the ambient occlusion if it is not there."));
    } else {
      changed |= ImGui::SliderFloat(L_("Hue shift"), &a.hue, -0.5f, 0.5f, T_("%.3f turns"));
      changed |= ImGui::SliderFloat(L_("Saturation"), &a.saturation, 0.f, 3.f);
      changed |= ImGui::SliderFloat(L_("Brightness"), &a.brightness, 0.f, 3.f);
      changed |= ImGui::SliderFloat(L_("Contrast"), &a.contrast, 0.f, 3.f);
    }
    ImGui::TextDisabled("%s", T_("Surface"));
    changed |= ImGui::SliderFloat(L_("Roughness offset"), &a.roughness, -1.f, 1.f);
    changed |= ImGui::SliderFloat(L_("Metallic offset"), &a.metallic, -1.f, 1.f);
    changed |= ImGui::SliderFloat(L_("Height offset"), &a.height, -0.5f, 0.5f);
    if (ImGui::Button(L_("Reset adjustment"))) a = AdjustParams{}, changed = true;
    ImGui::TextDisabled("%s", T_("Paint with the brush to edit this layer's mask."));
  }
  if (content->type == LayerType::Fill) {
    ImGui::SeparatorText(T_("Fill material"));
    MaterialRef& m = content->material;
    auto* e = m.id.empty() ? nullptr : engine.materials.find(m.id);
    ImGui::BeginGroup();
    if (e) {
      GLuint t = engine.materials.thumbnail(*e, engine.env);
      ImGui::Image(texId(t), ImVec2(48, 48) * uiScale);
      ImGui::SameLine();
      ImGui::BeginGroup();
      ImGui::Text("%s", e->def.name.c_str());
      ImGui::TextDisabled("%s", T_("Drag a material here, or onto the layer, to replace it"));
      ImGui::EndGroup();
    } else {
      ImGui::TextDisabled("%s", T_("Flat color. Drag a material here, or onto the layer, to use it."));
    }
    ImGui::EndGroup();
    // (0.22.5) a material dropped here replaces this one
    if (ImGui::BeginDragDropTarget()) {
      for (const char* type : {"PATINA_MATERIAL", "PATINA_FILL"})
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload(type, ImGuiDragDropFlags_AcceptBeforeDelivery)) {
          bool lib = type[7] == 'M';
          MaterialRef dm;
          if (lib) dm.id = (const char*)p->Data;
          else if (const Layer* src = d.findLayer(*(const uint32_t*)p->Data)) dm = d.contentLayer(*src)->material;
          dropHint = F_("Replaces the material of %s", l->name.c_str());
          if (p->IsDelivery()) {
            int si = d.selected;
            g_deferred.push_back([this, dm, lib, si] { dropMaterial(dm, lib, si, true, false, false); });
          }
        }
      ImGui::EndDragDropTarget();
    }
    if (!e) {
      changed |= colorEdit(L_("Color"), m.flat.color);
      changed |= ImGui::SliderFloat(L_("Roughness##f"), &m.flat.roughness, 0.f, 1.f);
      changed |= ImGui::SliderFloat(L_("Metallic##f"), &m.flat.metallic, 0.f, 1.f);
    }
    if (ImGui::Button(L_("Use current paint material"))) {
      m = engine.painter.material;
      changed = true;
    }
    int mp = m.mapping;
    if (ImGui::Combo(L_("Projection"), &mp, Z_("Tri-planar\0UV\0Screen\0"))) m.mapping = mp, changed = true;
    changed |= ImGui::SliderFloat(L_("Scale"), &m.scale, 0.05f, 20.f, "%.2f", ImGuiSliderFlags_Logarithmic);
    changed |= ImGui::SliderFloat(L_("Rotation"), &m.rotationDeg, -180.f, 180.f);
    changed |= colorEdit(L_("Tint"), m.tint);
    changed |= ImGui::SliderFloat(L_("Rough scale"), &m.roughnessScale, 0.f, 2.f);
    changed |= ImGui::SliderFloat(L_("Rough offset"), &m.roughnessOffset, -1.f, 1.f);
    changed |= ImGui::SliderFloat(L_("Height scale"), &m.heightScale, 0.f, 4.f);
    changed |= ImGui::SliderFloat(L_("Normal strength"), &m.normalStrength, 0.f, 4.f);
  }
  ImGui::SeparatorText(T_("Mask"));
  int si = d.selected;
  if (l->mask) {
    if (ImGui::Button(L_("Fill white"))) d.removeMask(si), d.addMask(si, 1.f);
    ImGui::SameLine();
    if (ImGui::Button(L_("Fill black"))) d.removeMask(si), d.addMask(si, 0.f);
    ImGui::SameLine();
    if (ImGui::Button(L_("Remove"))) d.removeMask(si);
    l = d.selectedLayer();
  } else {
    if (ImGui::Button(L_("Add white mask"))) d.addMask(si, 1.f);
    ImGui::SameLine();
    if (ImGui::Button(L_("Add black mask"))) d.addMask(si, 0.f);
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(d.selectedFaces().empty());
  if (ImGui::Button(L_("From selection"))) d.maskFromSelection(si);
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
    ImGui::SetTooltip("%s", T_("Mask = the selected faces (Select tool S, Island brush L, or UV editor)"));
  l = d.selectedLayer();
  if (l->type == LayerType::Paint) {
    ImGui::SameLine();
    if (ImGui::Button(L_("Clear paint"))) d.clearPaint(si);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Removes the painted channels and keeps the mask (undoable)"));
  }
  if (l->maskEffects.empty()) {
    ImGui::BeginGroup();
    changed |= generatorWidgets(l->generator, L_("Smart mask"));
    ImGui::EndGroup();
    smartMaskTarget(d.selected);
    if (ImGui::BeginCombo("##addfx", T_("Add a mask effect..."))) {
      int t = maskEffectMenu();
      ImGui::EndCombo();
      if (t >= 0) {
        l->convertMaskToStack();
        l->maskEffects.push_back(newMaskEffect(t));
        openFxNext = (int)l->maskEffects.size() - 1;
        changed = true;
      }
    }
    changed |= maskPatternTarget(*l);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("A stack of mask effects: several smart masks, painting, noise, levels, blur, invert,\n"
                                 "each with a blend mode and opacity. The current mask becomes the first effects."));
  } else {
    ImGui::BeginGroup();
    changed |= maskStackWidgets(*l);
    ImGui::EndGroup();
    smartMaskTarget(d.selected);
  }
  if (changed) d.markComposite();
  // (edits asked for in the properties: a material dropped on the fill material, Make independent)
  for (auto& f : g_deferred) f();
  g_deferred.clear();
}

// ------------------------------------------------------------------ history

static void dockLike(const char* windowName, const char* besideName) {
  if (ImGuiWindow* w = ImGui::FindWindowByName(besideName))
    if (w->DockId && !ImGui::FindWindowByName(windowName)) ImGui::SetNextWindowDockID(w->DockId, ImGuiCond_FirstUseEver);
}

void App::drawHistory() {
  if (!panelEmbedded) dockLike("History", "Properties");
  if (!beginPanel(L_("History"))) {  // hidden tab: submit nothing (scrolling a hidden list steals the next click)
    endPanel();
    return;
  }
  Document& d = engine.doc;
  UndoStack& u = d.undo;
  const UndoLimits& lim = UndoStack::limits();
  ImGui::TextDisabled(T_("%zu steps, %.0f MB on the graphics card, %.0f MB compressed"), u.count(), u.bytes() / 1048576.0,
                      u.ramBytes() / 1048576.0);
  ImGui::SameLine();
  ImGui::TextDisabled("(?)");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(T_("Click a step to go back to it; later steps stay until you change something.\n"
                         "Recent steps stay on the graphics card (up to %zu MB for every object), so they undo at once.\n"
                         "Older ones are compressed into RAM. The oldest steps are dropped after %zu steps, or when the\n"
                         "compressed ones pass %.1f GB (Preferences > Undo). Undo is never written to disk."),
                      lim.gpuBytes >> 20, lim.steps, lim.ramBytes / 1073741824.0);
  ImGui::BeginChild("##hist", ImVec2(0, 0), ImGuiChildFlags_Borders);
  static uint64_t lastVersion = 0;
  bool scroll = u.version() != lastVersion;
  lastVersion = u.version();
  size_t pos = u.position();
  int jump = -1;
  if (ImGui::Selectable(L_("(opened)"), pos == 0)) jump = 0;
  if (contextMenus()) historyContextMenu(0);
  for (size_t i = 0; i < u.count(); ++i) {
    bool future = i + 1 > pos;
    if (future) ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::PushID((int)i);
    if (ImGui::Selectable(u.label(i).c_str(), i + 1 == pos)) jump = (int)i + 1;
    if (future) ImGui::PopStyleColor();
    if (contextMenus()) historyContextMenu((int)i + 1);
    ImGui::PopID();
    if (scroll && i + 1 == pos) ImGui::SetScrollHereY(0.5f);
  }
  ImGui::EndChild();
  if (jump >= 0 && !engine.painter.active()) d.jumpTo((size_t)jump);
  endPanel();
}

// ------------------------------------------------------------------ colors

void App::drawColors() {
  if (!panelEmbedded) dockLike("Colors", "Properties");
  if (!beginPanel(L_("Colors"))) {
    endPanel();
    return;
  }
  Painter& P = engine.painter;
  MaterialRef& m = P.material;
  auto setPrimary = [&](vec3 c) {
    if (!m.id.empty()) {
      m.id.clear();
      engine.selectedMaterialId.clear();
    }
    m.flat.color = c;
  };
  if (!m.id.empty()) {
    auto* e = engine.materials.find(m.id);
    ImGui::TextWrapped(T_("Painting with the material \"%s\". Picking a color switches the brush to a flat color."),
                       e ? e->def.name.c_str() : m.id.c_str());
  }
  // primary / secondary with swap
  vec3 prim = m.flat.color;
  float big = 46 * uiScale;
  ImVec2 p0 = ImGui::GetCursorScreenPos();
  ImGui::ColorButton("##secondary", ImVec4(secondaryColor.x, secondaryColor.y, secondaryColor.z, 1), ImGuiColorEditFlags_NoTooltip,
                     ImVec2(big, big));
  if (contextMenus() && ImGui::BeginPopupContextItem("##secctx")) colorsContextMenu(1, secondaryColor, -1), ImGui::EndPopup();
  ImGui::SetCursorScreenPos(p0);
  ImGui::ColorButton("##primary", ImVec4(prim.x, prim.y, prim.z, 1), ImGuiColorEditFlags_NoTooltip, ImVec2(big * 0.7f, big * 0.7f));
  if (contextMenus() && ImGui::BeginPopupContextItem("##primctx")) colorsContextMenu(0, prim, -1), ImGui::EndPopup();
  ImGui::SetCursorScreenPos(p0 + ImVec2(big + 8 * uiScale, 0));
  ImGui::BeginGroup();
  if (ImGui::Button(L_("Swap"))) {
    std::swap(m.flat.color, secondaryColor);
    if (!m.id.empty()) m.id.clear(), engine.selectedMaterialId.clear();
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(T_("Swap primary and secondary color (%s)"), keyLabel("color.swap"));
  colorEdit(L_("Secondary"), secondaryColor);
  ImGui::EndGroup();
  ImGui::SetCursorScreenPos(p0 + ImVec2(0, big + 6 * uiScale));
  vec3 c = prim;
  ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x, 260 * uiScale));
  if (ImGui::ColorPicker3("##picker", &c.x,
                          ImGuiColorEditFlags_NoSidePreview | ImGuiColorEditFlags_NoSmallPreview | ImGuiColorEditFlags_DisplayHex))
    setPrimary(c);
  ImGui::SeparatorText(T_("Recent"));
  float sw = ImGui::GetFrameHeight() * 1.1f;
  int cols = std::max(1, int(ImGui::GetContentRegionAvail().x / (sw + 4)));
  if (recentColors.empty()) ImGui::TextDisabled("%s", T_("Colors you paint with appear here."));
  for (size_t i = 0; i < recentColors.size(); ++i) {
    if (i % cols) ImGui::SameLine(0, 4);
    ImGui::PushID((int)i + 1000);
    vec3 rc = recentColors[i];
    if (ImGui::ColorButton("##rc", ImVec4(rc.x, rc.y, rc.z, 1), 0, ImVec2(sw, sw))) setPrimary(rc);
    if (contextMenus() && ImGui::BeginPopupContextItem("##rcctx")) colorsContextMenu(3, rc, (int)i), ImGui::EndPopup();
    ImGui::PopID();
  }
  ImGui::SeparatorText(T_("Swatches"));
  int remove = -1;
  for (size_t i = 0; i < swatches.size(); ++i) {
    if (i % cols) ImGui::SameLine(0, 4);
    ImGui::PushID((int)i);
    vec3 s = swatches[i];
    if (ImGui::ColorButton("##sw", ImVec4(s.x, s.y, s.z, 1), 0, ImVec2(sw, sw))) setPrimary(s);
    if (ImGui::BeginPopupContextItem("##swctx")) {
      if (contextMenus()) {
        colorsContextMenu(2, s, (int)i);
      } else {
        if (ImGui::MenuItem(L_("Use as the Secondary Color"))) secondaryColor = s;
        if (ImGui::MenuItem(L_("Replace with the Primary Color"))) swatches[i] = m.flat.color;
        if (ImGui::MenuItem(L_("Remove"))) remove = (int)i;
      }
      ImGui::EndPopup();
    }
    ImGui::PopID();
  }
  if (!swatches.empty()) ImGui::SameLine(0, 4);
  if (ImGui::Button("+", ImVec2(sw, sw))) swatches.push_back(m.flat.color);
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Add the primary color (right-click a swatch for more)"));
  if (remove >= 0) swatches.erase(swatches.begin() + remove);
  ImGui::Spacing();
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped(T_("Hold %s over the model to pick a color, release to keep painting. Alt+right-click picks too."),
                     keyLabel("tool.picker") ? keyLabel("tool.picker") : T_("the picker key"));
  ImGui::PopStyleColor();
  endPanel();
}

}  // namespace patina
