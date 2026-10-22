#include "ui/UiIdCheck.h"

#define IMGUI_ENABLE_TEST_ENGINE  // (the hooks' declarations)

#include <cstdlib>
#include <set>
#include <unordered_map>
#include <unordered_set>

#include "core/Log.h"
#include "imgui.h"
#include "imgui_internal.h"

namespace patina::ui {
namespace {
struct Seen {
  int count = 0;
  std::string window, label;
};
std::unordered_map<ImGuiID, Seen> frame;  // this frame's widgets
std::vector<ImGuiID> clashes;             // this frame's IDs drawn twice
std::set<std::string> known;
std::vector<std::string> conflicts;
bool checking = false;
// (0.42) the widgets that can take a typed value (sliders, drags): this frame's and the last one's
std::unordered_set<ImGuiID> inputable, inputableBefore;
}  // namespace

void idCheckInit() {
  // the hooks run always (cheap): they also note the widgets a right-click types into (valueEntryNewFrame)
  ImGui::GetCurrentContext()->TestEngineHookItems = true;
  if (!std::getenv("PATINA_UI_ID_CHECK")) return;
  checking = true;
  PLOG_I("UI: checking widget IDs for clashes");
}

// (0.42) Right-click a slider (or a drag) to type its value: what Ctrl+click does in Dear ImGui. Run right after
// ImGui::NewFrame: a right-click on a widget that can take typed input (last frame's) activates it the way
// keyboard navigation does with "prefer input", and the right button is held for it until let go, so no right-click
// menu opens under it.
void valueEntryNewFrame(bool enabled) {
  inputableBefore.swap(inputable);
  inputable.clear();
  ImGuiContext& g = *ImGui::GetCurrentContext();
  if (!enabled || !g.IO.MouseClicked[ImGuiMouseButton_Right]) return;
  ImGuiID id = g.HoveredIdPreviousFrame;
  if (!id || !inputableBefore.count(id) || g.ActiveId == id) return;
  g.NavActivateId = g.NavActivateDownId = g.NavActivatePressedId = id;
  g.NavActivateFlags = ImGuiActivateFlags_PreferInput;
  ImGui::SetKeyOwner(ImGuiKey_MouseRight, id, ImGuiInputFlags_LockUntilRelease);
}

void idCheckEndFrame() {
  if (!checking) return;
  for (ImGuiID id : clashes) {
    const Seen& s = frame[id];
    std::string what = s.window + ": " + (s.label.empty() ? "(no label)" : s.label);
    if (known.insert(what).second) {
      conflicts.push_back(what);
      PLOG_W("UI: two widgets share an ID (%s)", what.c_str());
    }
  }
  frame.clear(), clashes.clear();
}

const std::vector<std::string>& idCheckConflicts() { return conflicts; }

}  // namespace patina::ui

// Dear ImGui's test engine hooks (patina_imgui is built with IMGUI_ENABLE_TEST_ENGINE; they run only while
// TestEngineHookItems is set)
void ImGuiTestEngineHook_ItemAdd(ImGuiContext* ctx, ImGuiID id, const ImRect&, const ImGuiLastItemData* item) {
  using namespace patina::ui;
  if (item && (item->ItemFlags & ImGuiItemFlags_Inputable) && !(item->ItemFlags & ImGuiItemFlags_Disabled)) inputable.insert(id);
  if (!checking) return;
  if (!item || (item->ItemFlags & ImGuiItemFlags_AllowDuplicateId)) return;  // (windows themselves: no item data)
  Seen& s = frame[id];
  if (++s.count == 1) s.window = ctx->CurrentWindow ? ctx->CurrentWindow->Name : "";
  else if (s.count == 2) clashes.push_back(id);
}

void ImGuiTestEngineHook_ItemInfo(ImGuiContext*, ImGuiID id, const char* label, ImGuiItemStatusFlags) {
  using namespace patina::ui;
  if (!checking) return;
  auto it = frame.find(id);
  if (it != frame.end() && label && it->second.label.empty()) {
    it->second.label = label;
    if (size_t hash = it->second.label.find("##"); hash != std::string::npos && hash > 0) it->second.label.resize(hash);
  }
}

void ImGuiTestEngineHook_Log(ImGuiContext*, const char*, ...) {}

const char* ImGuiTestEngine_FindItemDebugLabel(ImGuiContext*, ImGuiID) { return nullptr; }
