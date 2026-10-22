#pragma once
// For tests (0.22.3): two widgets in one window with the same ID (the same label with nothing to tell them
// apart) fight over clicks, and Dear ImGui says so only when the pointer is over one of them. With
// PATINA_UI_ID_CHECK set, every widget drawn is counted each frame (through Dear ImGui's test engine hooks)
// and each clash is logged once and kept for the ui_id_conflicts command.
#include <string>
#include <vector>

namespace patina::ui {

void idCheckInit();      // after the ImGui context is made: on when PATINA_UI_ID_CHECK is set
void idCheckEndFrame();  // after ImGui::Render
// the clashes seen so far ("window: label"), each once
const std::vector<std::string>& idCheckConflicts();
// (0.42) right after ImGui::NewFrame: a right-click on a slider (or drag) opens it for a typed value
void valueEntryNewFrame(bool enabled);

}  // namespace patina::ui
