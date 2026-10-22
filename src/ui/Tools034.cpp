// 0.34: Mesh > Collision: collision shapes for game engines, shown in green over the model.
#include <imgui.h>

#include <algorithm>
#include <cmath>

#include "ui/App.h"

namespace patina {

void App::drawCollisionWindow() {
  if (!showCollisionWindow) return;
  if (!engine.features.on("collision") || !engine.doc.hasMesh()) {
    showCollisionWindow = false;
    return;
  }
  ImGui::SetNextWindowSizeConstraints(ImVec2(420 * uiScale, 0), ImVec2(560 * uiScale, FLT_MAX));
  if (!ImGui::Begin(L_("Collision"), &showCollisionWindow, ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoCollapse |
                                                               ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::End();
    return;
  }
  Document& d = engine.doc;
  CollisionOptions& o = d.collisionOptions;
  ImGui::TextWrapped("%s", T_("Simple shapes a game engine uses for collisions instead of the model itself."));
  static const char* modes[] = {"pieces", "parts", "hull", "boxes", "box"};
  static const char* labels[] = {N_("Convex pieces (the model split where it bends in)"), N_("A hull per separate part"),
                                 N_("One convex hull"), N_("A box per separate part"), N_("One box")};
  const char* shown[5];
  for (int i = 0; i < 5; ++i) shown[i] = T_(labels[i]);
  int mi = 0;
  for (int i = 0; i < 5; ++i)
    if (o.mode == modes[i]) mi = i;
  ImGui::SetNextItemWidth(320 * uiScale);
  if (ImGui::Combo(L_("Shapes"), &mi, shown, 5)) o.mode = modes[mi];
  if (o.mode == "pieces" || o.mode == "parts" || o.mode == "boxes") {
    ImGui::SetNextItemWidth(320 * uiScale);
    ImGui::SliderInt(L_("Most shapes"), &o.maxShapes, 1, 64);
  }
  if (o.mode != "box" && o.mode != "boxes") {
    ImGui::SetNextItemWidth(320 * uiScale);
    ImGui::SliderInt(L_("Most corners per hull"), &o.maxVerts, 8, 255);
    ImGui::SetItemTooltip("%s", T_("Engines like 32 or fewer (PhysX allows up to 255)"));
  }
  if (o.mode == "pieces") {
    float pct = o.concavity * 100.f;
    ImGui::SetNextItemWidth(320 * uiScale);
    if (ImGui::SliderFloat(L_("Empty space allowed"), &pct, 1.f, 30.f, T_("%.0f%%"))) o.concavity = pct / 100.f;
    ImGui::SetItemTooltip("%s", T_("A piece is split again while its hull holds more empty space than this. Lower: more, "
                                "closer pieces."));
    ImGui::SetNextItemWidth(320 * uiScale);
    ImGui::SliderInt(L_("Detail"), &o.resolution, 16, 96);
    ImGui::SetItemTooltip("%s", T_("How finely the model is looked at (voxels along its longest side)"));
  }
  if (ImGui::Button(L_("Make"), ImVec2(160 * uiScale, 0))) {
    json r;
    if (runCommand("mesh_collision", {{"mode", o.mode}, {"max_shapes", o.maxShapes}, {"max_vertices", o.maxVerts},
                                      {"concavity", o.concavity}, {"resolution", o.resolution}},
                   &r)) {
      int n = r.value("count", 0);
      float fit = r.value("fit", 0.f);
      char buf[200];
      if (fit > 0)
        snprintf(buf, sizeof buf,
                 fit >= 1.f ? P_("%d shape, holding %.0f%% more than the model",
                                 "%d shapes, holding %.0f%% more than the model", n)
                            : P_("%d shape, holding %.0f%% less than the model",
                                 "%d shapes, holding %.0f%% less than the model", n),
                 n, std::abs(fit - 1.f) * 100.f);
      else
        snprintf(buf, sizeof buf, P_("%d shape", "%d shapes", n), n);
      collisionStatus = buf;
      if (r.contains("note")) collisionStatus += " (" + r["note"].get<std::string>() + ")";
    }
  }
  const CollisionResult* cr = d.collisionShapes();
  if (cr) {
    ImGui::SameLine();
    ImGui::Checkbox(L_("Show them"), &engine.showCollision);
    ImGui::SameLine();
    if (ImGui::SmallButton(L_("Clear"))) runCommand("mesh_collision", {{"clear", true}}), collisionStatus.clear();
  } else if (!collisionStatus.empty()) {
    collisionStatus.clear();  // the model changed: the shapes are gone
  }
  if (!collisionStatus.empty()) ImGui::TextWrapped("%s", collisionStatus.c_str());
  ImGui::TextDisabled("%s", T_("File > Export Model writes them after the model, named for Unreal or Godot."));
  ImGui::End();
}

}  // namespace patina
