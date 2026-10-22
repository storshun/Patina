// (0.39) The path tracer's settings: the top bar's popup in the Path traced view mode, and the Render image dialog.
#include <imgui.h>

#include <algorithm>

#include "ui/App.h"

namespace patina {

void App::pathTraceSettingsUI() {
  PathTraceSettings& s = engine.viewOptions.pathTrace;
  const PathTracer& t = engine.renderer.pathTracer();
  ImGui::TextDisabled("%s", T_("Path tracing"));
  ImGui::Separator();
  const float w = 220.f * uiScale;
  if (engine.viewOptions.mode == ViewMode::PathTraced) {
    std::string done = t.done(s) ? F_("%d samples: done", t.samples()) : F_("%d / %d samples", t.samples(), s.maxSamples);
    ImGui::ProgressBar(std::min(1.f, float(t.samples()) / float(std::max(1, s.maxSamples))), ImVec2(w, 0), done.c_str());
  }
  ImGui::SetNextItemWidth(w);
  ImGui::SliderInt(L_("Samples"), &s.maxSamples, 16, 16384, "%d", ImGuiSliderFlags_Logarithmic);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("The view stops adding samples here. More: less grain in soft shadows and dull metal."));
  ImGui::SetNextItemWidth(w);
  ImGui::SliderInt(L_("Light bounces"), &s.bounces, 0, 16);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("How many times light may bounce off the model after the first hit: more lights the insides\n"
                               "of folds, cavities and between parts (and reflections of reflections). 0: direct light only."));
  ImGui::Checkbox(L_("Shadow on the ground"), &s.ground);
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("An invisible ground under the model that shows only its shadow, as in a product photo."));
  ImGui::SetNextItemWidth(w);
  ImGui::SliderFloat(L_("Brightest bounce"), &s.clamp, 0.f, 50.f, s.clamp <= 0.f ? T_("no limit") : "%.1f");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("%s", T_("Limits how bright light that has bounced may be. Lower: the bright speckles (fireflies)\n"
                               "go away sooner, at the cost of a little of the brightest glints."));
  ImGui::TextDisabled("%s", T_("Shift + right drag turns the light (the environment)."));
}

}  // namespace patina
