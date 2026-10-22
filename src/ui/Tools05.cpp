// Patina 0.5 interface pieces: material folder import with a report, network settings.
#include <imgui.h>

#include <chrono>
#include <filesystem>
#include <future>

#include "core/Log.h"
#include "ui/App.h"

namespace patina {
namespace fs = std::filesystem;

void App::importMaterialFolder(const std::string& dir) {
  std::string err;
  MaterialLibrary::ImageImportOptions opt;
  std::vector<MaterialLibrary::ImportReport> rep;
  auto mats = engine.materials.importImageFolderAll(dir, opt, &rep, &err);
  if (mats.empty()) {
    showError(F_("No material in %s", fs::path(dir).filename().string().c_str()), err);
    return;
  }
  engine.selectMaterial(mats.front()->def.id);
  for (auto& r : rep) {
    std::string line = "Imported '" + r.name + "':";
    for (auto& [k, f] : r.files) line += " " + k + "=" + f;
    if (r.normalDirectX) line += " (DirectX normal)";
    for (auto& n : r.notes) line += "; " + n;
    PLOG_I("%s", line.c_str());
  }
  if (mats.size() == 1)
    setStatus(F_("Imported material %s (%zu maps%s)", mats.front()->def.name.c_str(), rep.front().files.size(),
                 rep.front().normalDirectX ? T_(", DirectX normal") : ""));
  else
    setStatus(F_("Imported %zu materials (one per texture set) from %s", mats.size(),
                 fs::path(dir).filename().string().c_str()));
}

}  // namespace patina

// ------------------------------------------------------------------ HTTP API (Preferences)

namespace patina {

bool App::startHttpApi(bool fromSettings) {
  httpApi.stop();
  httpError.clear();
  net::HttpApiOptions o = launchArgs.httpOptions();
  if (fromSettings) {
    o.port = httpPortSetting;
    o.lan = httpLan;
  }
  auto exec = [this](const json& req, int timeoutS) {
    auto f = engine.post(req);
    if (f.wait_for(std::chrono::seconds(timeoutS)) != std::future_status::ready)
      return json{{"ok", false}, {"error", "timed out"}};
    return f.get();
  };
  auto describe = [this] { return engine.commands.describe(true); };
  std::string err;
  if (!httpApi.start(o, exec, describe, &err)) {
    httpError = err;
    setStatus(F_("HTTP API not started: %s", err.c_str()));
    return false;
  }
  setStatus(F_("HTTP API on %s", httpApi.url().c_str()));
  return true;
}

void App::drawNetworkPrefs() {
  const float sectionTop = ImGui::GetCursorScreenPos().y;
  ImGui::SeparatorText(T_("HTTP API (other applications)"));
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextWrapped("%s", T_("Lets pipeline tools, asset servers and render farms send commands and jobs (bakes, "
                              "exports, renders) over HTTP. 'Local network' answers other machines with private "
                              "addresses only, never the internet, and needs the token."));
  ImGui::PopStyleColor();
  bool changed = false;
  changed |= ImGui::Checkbox(L_("Enable"), &httpEnabled);
  {
    ImVec2 ra = ImGui::GetItemRectMin(), rb = ImGui::GetItemRectMax();
    engine.uiRects["prefs.http_enable"] = {ra.x, ra.y, rb.x - ra.x, rb.y - ra.y};  // (the manual's screenshots)
  }
  ImGui::SameLine();
  changed |= ImGui::Checkbox(L_("Local network"), &httpLan);
  ImGui::SameLine();
  ImGui::SetNextItemWidth(90 * uiScale);
  if (ImGui::InputInt(L_("Port##http"), &httpPortSetting, 0, 0)) httpPortSetting = std::clamp(httpPortSetting, 1, 65535);
  if (ImGui::IsItemDeactivatedAfterEdit()) changed = true;
  if (changed) {
    if (httpEnabled) startHttpApi(true);
    else httpApi.stop(), setStatus(T_("HTTP API stopped"));
    saveSession(true);
  }
  if (httpApi.running()) {
    ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.5f, 1), T_("Running: %s"), httpApi.url().c_str());
    if (!httpApi.token().empty()) {
      std::string t = httpApi.token();
      ImGui::Text(T_("Token: %s"), t.c_str());
      if (ImGui::SmallButton(L_("Copy the token"))) ImGui::SetClipboardText(t.c_str());
      if (!httpApi.tokenFile().empty()) {
        ImGui::SameLine();
        if (ImGui::SmallButton(L_("New token"))) {
          std::error_code ec;
          std::filesystem::remove(httpApi.tokenFile(), ec);
          startHttpApi(!launchArgs.httpGiven);
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Clients using the old token stop working"));
      }
    }
    ImGui::TextDisabled(T_("Workspace: %s"), httpApi.workspace().c_str());
    json st = httpApi.status();
    if (st.contains("job"))
      ImGui::Text(T_("Job %s: %s (%.0f%%)"), st["job"].value("id", std::string()).c_str(),
                  st["job"].value("label", std::string()).c_str(), st["job"].value("progress", 0.0) * 100.0);
    else if (st.value("queued", 0) > 0)
      ImGui::Text(T_("%d job(s) queued"), st.value("queued", 0));
  } else if (!httpError.empty()) {
    ImGui::TextColored(ImVec4(1, 0.55f, 0.45f, 1), "%s", httpError.c_str());
  }
  if (launchArgs.httpGiven) ImGui::TextDisabled("%s", T_("Started with --http options on the command line."));
  {
    ImVec2 wp = ImGui::GetWindowPos();
    float bottom = ImGui::GetCursorScreenPos().y;
    engine.uiRects["prefs.http_section"] = {wp.x, sectionTop, ImGui::GetWindowWidth(), bottom - sectionTop};  // (the manual's screenshots)
  }
}

}  // namespace patina

// ------------------------------------------------------------------ pen / mouse strokes

namespace patina {

int App::strokeStartSample() const {
  const auto& v = pen.frameSamples();
  for (size_t i = 0; i < v.size(); ++i)
    if (v[i].down) return (int)i;
  return -1;
}

StrokePoint App::strokePointOf(const PointerSample& s, const std::function<vec2(vec2)>& toLocal) const {
  StrokePoint p;
  p.pos = toLocal(s.pos);
  p.pressure = s.pen ? std::max(s.pressure, 0.f) : 1.f;
  p.tilt = s.pen ? s.tilt : vec2(0.f);
  return p;
}

int App::feedStroke(Painter& P, int from, const std::function<vec2(vec2)>& toLocal) {
  const auto& v = pen.frameSamples();
  int n = 0;
  for (size_t i = (size_t)std::max(0, from); i < v.size(); ++i) {
    if (!v[i].down) continue;
    P.addPoint(strokePointOf(v[i], toLocal));
    ++n;
  }
  return n;
}

}  // namespace patina
