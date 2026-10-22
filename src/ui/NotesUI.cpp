// Notes pinned to the model: pins in the 3D view, the Note tool, the edit popup and the
// Notes panel.
#include <algorithm>
#include <cstdlib>
#include <ctime>

#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include "ui/App.h"

namespace patina {

static std::string noteDate(int64_t t) {
  if (!t) return "";
  std::time_t tt = (std::time_t)t;
  char buf[32];
  std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", std::localtime(&tt));
  return buf;
}

static std::string firstLine(const std::string& s, size_t max = 60) {
  std::string l = s.substr(0, s.find('\n'));
  if (l.size() > max) l = l.substr(0, max - 3) + "...";
  return l.empty() ? T_("(empty)") : l;
}

void App::beginNoteEdit(uint32_t id, ImVec2 at) {
  Note* n = engine.doc.findNote(id);
  if (!n) return;
  noteEditId = id;
  noteEditBuf = n->text;
  noteEditPos = at;
  noteEditOpenRequest = true;
}

uint32_t App::addNoteAt(vec2 viewportPx, const std::string& text) {
  Document& d = engine.doc;
  RayHit h = engine.pick(viewportPx);
  if (!h.hit) return 0;
  Note n;
  n.id = d.nextNoteId++;
  n.position = h.position;
  n.normal = glm::length(h.normal) > 0 ? glm::normalize(h.normal) : vec3(0, 1, 0);
  n.text = text;
  const char* user = std::getenv("USER");
  if (!user) user = std::getenv("USERNAME");
  n.author = user ? user : "";
  n.created = (int64_t)std::time(nullptr);
  n.view = engine.camera.toJson();
  d.notes.push_back(n);
  d.dirty = true;
  return n.id;
}

bool App::notesOverlay(ImVec2 origin, ImVec2 size, bool hovered) {
  Document& d = engine.doc;
  ImGuiIO& io = ImGui::GetIO();
  bool noteTool = tool == ViewportTool::Note;
  bool overPin = false;
  if (d.hasMesh() && (showNotePins || noteTool) && !d.notes.empty()) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect(origin, origin + size, true);
    const Camera cam = engine.objectCamera();  // (1.2) the notes are on the active object
    float aspect = size.x / std::max(size.y, 1.f);
    mat4 vpm = cam.viewProj(aspect);
    vec3 eye = cam.ortho ? vec3(0) : cam.eye();
    float r = 10.f * uiScale;
    int hoverIdx = -1;
    struct Pin { ImVec2 p; bool hidden; int idx; };
    std::vector<Pin> pins;
    for (int i = 0; i < (int)d.notes.size(); ++i) {
      const Note& n = d.notes[i];
      if (n.resolved && !showResolvedNotes) continue;
      vec4 c = vpm * vec4(n.position, 1.f);
      if (c.w <= 0) continue;
      vec2 ndc = vec2(c) / c.w;
      if (std::abs(ndc.x) > 1.1f || std::abs(ndc.y) > 1.1f) continue;
      ImVec2 p(origin.x + (ndc.x * 0.5f + 0.5f) * size.x, origin.y + (0.5f - ndc.y * 0.5f) * size.y);
      // hidden behind the model: drawn faint
      vec3 o, dir;
      if (cam.ortho) {
        dir = cam.forward();
        o = n.position - dir * (cam.eyeDistance() * 4.f);
      } else {
        o = eye;
        dir = glm::normalize(n.position - eye);
      }
      float dist = glm::length(n.position - o);
      RayHit h = d.bvh.raycast(d.mesh, o, dir, dist * 1.01f, d.hiddenMask());
      bool hidden = h.hit && h.t < dist - std::max(1e-3f, dist * 0.004f);
      pins.push_back({p, hidden, i});
      float dx = io.MousePos.x - p.x, dy = io.MousePos.y - p.y;
      if (hovered && dx * dx + dy * dy <= r * r * 1.4f) hoverIdx = i;
    }
    for (auto& pin : pins) {
      const Note& n = d.notes[pin.idx];
      bool hot = pin.idx == hoverIdx || n.id == noteEditId;
      int alpha = pin.hidden ? 90 : 235;
      ImU32 fill = n.resolved ? IM_COL32(90, 150, 100, alpha) : IM_COL32(232, 133, 44, alpha);
      ImVec2 tip(pin.p.x, pin.p.y);
      ImVec2 head(pin.p.x + r * 0.9f, pin.p.y - r * 1.6f);
      dl->AddLine(tip, head, IM_COL32(20, 20, 20, alpha), 2.f);
      dl->AddCircleFilled(head, r + 1.5f, IM_COL32(20, 20, 20, alpha));
      dl->AddCircleFilled(head, r, fill);
      if (hot) dl->AddCircle(head, r + 3.f, IM_COL32(255, 255, 255, 230), 0, 2.f);
      char num[16];
      snprintf(num, sizeof num, "%d", pin.idx + 1);
      ImVec2 ts = ImGui::CalcTextSize(num);
      dl->AddText(ImVec2(head.x - ts.x * 0.5f, head.y - ts.y * 0.5f), IM_COL32(255, 255, 255, pin.hidden ? 140 : 255), num);
      float dx = io.MousePos.x - head.x, dy = io.MousePos.y - head.y;
      if (hovered && dx * dx + dy * dy <= r * r * 1.6f) hoverIdx = pin.idx;
    }
    dl->PopClipRect();
    if (hoverIdx >= 0) {
      overPin = true;
      const Note& n = d.notes[hoverIdx];
      ImGui::BeginTooltip();
      ImGui::TextDisabled("%s%s%s%s%s", F_("Note %d", hoverIdx + 1).c_str(), n.author.empty() ? "" : "  -  ", n.author.c_str(),
                          n.resolved ? "  " : "", n.resolved ? T_("(resolved)") : "");
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.f);
      ImGui::TextUnformatted(n.text.empty() ? T_("(empty)") : n.text.c_str());
      ImGui::PopTextWrapPos();
      ImGui::TextDisabled("%s%s%s", noteDate(n.created).c_str(), noteTool ? "  |  " : "", noteTool ? T_("click to edit") : "");
      ImGui::EndTooltip();
      if (noteTool && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) beginNoteEdit(n.id, io.MousePos);
    }
  }
  // the Note tool: click on the model for a new note
  if (noteTool && hovered && !overPin && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !io.KeyAlt) {
    vec2 local(io.MousePos.x - origin.x, io.MousePos.y - origin.y);
    if (uint32_t id = addNoteAt(local, "")) {
      beginNoteEdit(id, io.MousePos);
      noteEditIsNew = true;
    } else {
      setStatus(T_("Notes go on the model: click on the surface"));
    }
  }
  // edit popup
  if (noteEditOpenRequest) {
    ImGui::OpenPopup("##noteedit");
    noteEditOpenRequest = false;
    noteEditFocus = true;
  }
  ImGui::SetNextWindowPos(ImVec2(noteEditPos.x + 14, noteEditPos.y + 10), ImGuiCond_Appearing);
  if (ImGui::BeginPopup("##noteedit")) {
    Note* n = d.findNote(noteEditId);
    if (!n) {
      ImGui::CloseCurrentPopup();
    } else {
      int idx = int(n - d.notes.data());
      ImGui::TextDisabled("%s%s%s", F_("Note %d", idx + 1).c_str(), n->author.empty() ? "" : "  -  ", n->author.c_str());
      if (noteEditFocus) ImGui::SetKeyboardFocusHere(), noteEditFocus = false;
      ImGui::InputTextMultiline("##notetext", &noteEditBuf, ImVec2(300 * uiScale, ImGui::GetTextLineHeight() * 5));
      bool ctrlEnter = ImGui::IsItemFocused() && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Enter);
      ImGui::Checkbox(L_("Resolved"), &n->resolved);
      ImGui::SameLine();
      bool save = ImGui::Button(L_("Save")) || ctrlEnter;
      ImGui::SameLine();
      bool del = ImGui::Button(L_("Delete"));
      ImGui::SameLine();
      ImGui::TextDisabled("%s", T_("Ctrl+Enter saves"));
      if (save) {
        n->text = noteEditBuf;
        d.dirty = true;
        noteEditIsNew = false;
        ImGui::CloseCurrentPopup();
      }
      if (del) {
        d.notes.erase(d.notes.begin() + idx);
        d.dirty = true;
        noteEditIsNew = false;
        ImGui::CloseCurrentPopup();
      }
    }
    ImGui::EndPopup();
  } else if (noteEditId) {
    // closed without saving: a new note left empty goes away again
    if (noteEditIsNew) {
      Note* n = d.findNote(noteEditId);
      if (n && n->text.empty() && noteEditBuf.empty()) {
        d.notes.erase(d.notes.begin() + (n - d.notes.data()));
      } else if (n && n->text.empty()) {
        n->text = noteEditBuf;  // typed but clicked away: keep what was typed
        d.dirty = true;
      }
    }
    noteEditId = 0;
    noteEditIsNew = false;
  }
  return noteTool || overPin;
}

void App::drawNotes() {
  if (ImGuiWindow* h = ImGui::FindWindowByName("History"))
    if (h->DockId) ImGui::SetNextWindowDockID(h->DockId, ImGuiCond_FirstUseEver);
  ImGui::Begin(L_("Notes"));
  Document& d = engine.doc;
  ImGui::Checkbox(L_("Pins"), &showNotePins);
  ImGui::SameLine();
  ImGui::Checkbox(L_("Resolved"), &showResolvedNotes);
  ImGui::SameLine();
  if (ImGui::SmallButton(L_("Note tool"))) tool = ViewportTool::Note;
  if (ImGui::IsItemHovered()) ImGui::SetTooltip(T_("Click on the model to pin a note (%s)"), keyLabel("tool.note"));
  ImGui::SameLine();
  if (ImGui::SmallButton(L_("Copy all"))) {
    std::string md;
    for (size_t i = 0; i < d.notes.size(); ++i) {
      auto& n = d.notes[i];
      md += "- [" + std::string(n.resolved ? "x" : " ") + "] " + std::to_string(i + 1) + ". " + n.text;
      if (!n.author.empty()) md += " (" + n.author + ", " + noteDate(n.created) + ")";
      md += "\n";
    }
    ImGui::SetClipboardText(md.c_str());
    setStatus(T_("Copied the notes as a Markdown checklist"));
  }
  ImGui::Separator();
  if (d.notes.empty()) {
    ImGui::TextDisabled(T_("No notes. Pick the Note tool (%s) and click on the model."), keyLabel("tool.note"));
    ImGui::TextDisabled("%s", T_("Notes are saved in the project."));
  }
  int open = 0;
  for (auto& n : d.notes) open += !n.resolved;
  if (!d.notes.empty()) ImGui::TextDisabled(T_("%d open, %d resolved"), open, int(d.notes.size()) - open);
  int remove = -1;
  for (int i = 0; i < (int)d.notes.size(); ++i) {
    Note& n = d.notes[i];
    if (n.resolved && !showResolvedNotes) continue;
    ImGui::PushID((int)n.id);
    if (ImGui::Checkbox("##res", &n.resolved)) d.dirty = true;
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Resolved"));
    ImGui::SameLine();
    ImVec4 col = n.resolved ? ImVec4(0.55f, 0.75f, 0.6f, 1) : ImVec4(1, 0.65f, 0.3f, 1);
    ImGui::TextColored(col, "%d", i + 1);
    ImGui::SameLine();
    std::string label = firstLine(n.text) + "##sel";
    if (ImGui::Selectable(label.c_str(), noteEditId == n.id, ImGuiSelectableFlags_AllowDoubleClick,
                          ImVec2(ImGui::GetContentRegionAvail().x - 70 * uiScale, 0))) {
      if (n.view.is_object()) engine.camera.fromJson(n.view);
      else engine.camera.target = engine.toWorld(n.position);
      if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        vec2 vp = lastViewportSize;
        beginNoteEdit(n.id, ImVec2(lastViewportOrigin.x + vp.x * 0.5f, lastViewportOrigin.y + vp.y * 0.4f));
      }
    }
    if (contextMenus() && ImGui::BeginPopupContextItem("##notectx")) {
      if (notesContextMenu(n.id)) remove = i;
      ImGui::EndPopup();
    }
    if (ImGui::IsItemHovered()) {
      ImGui::BeginTooltip();
      ImGui::PushTextWrapPos(ImGui::GetFontSize() * 24.f);
      ImGui::TextUnformatted(n.text.c_str());
      ImGui::PopTextWrapPos();
      ImGui::TextDisabled(T_("%s %s | click: go there, double-click: edit"), n.author.c_str(), noteDate(n.created).c_str());
      ImGui::EndTooltip();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton(L_("Edit"))) {
      vec2 vp = lastViewportSize;
      if (n.view.is_object()) engine.camera.fromJson(n.view);
      beginNoteEdit(n.id, ImVec2(lastViewportOrigin.x + vp.x * 0.5f, lastViewportOrigin.y + vp.y * 0.4f));
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("x")) remove = i;
    ImGui::PopID();
  }
  if (remove >= 0) {
    d.notes.erase(d.notes.begin() + remove);
    d.dirty = true;
  }
  ImGui::End();
}

}  // namespace patina
