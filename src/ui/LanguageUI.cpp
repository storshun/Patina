// (0.36) Preferences > Language: the interface in another language, from a translation file (core/I18n.h).
#include <imgui.h>
#include <imgui_stdlib.h>

#include <SDL3/SDL.h>

#include "core/Log.h"
#include "core/Paths.h"
#include "ui/App.h"

namespace patina {

static void openFolderUrl(const std::string& dir) {
#ifdef _WIN32
  std::string url = "file:///" + paths::ensureDir(dir).generic_string();
#else
  std::string url = "file://" + paths::ensureDir(dir).generic_string();
#endif
  SDL_OpenURL(url.c_str());
}

bool App::setLanguageUI(const std::string& code) {
  std::string err;
  if (!i18n::setLanguage(code, &err)) {
    showError(T_("Could not use that translation"), err);
    return false;
  }
  followLanguage();
  return true;
}

void App::followLanguage() {
  if (i18n::version() == languageVersion) return;
  languageVersion = i18n::version();
  pendingUiScale = userUiScale;  // the fonts again, with the glyphs the language needs
  if (languageSaved != i18n::current()) languageSaved = i18n::current(), saveSession(true);
}

void App::drawLanguagePrefs() {
  const float sectionTop = ImGui::GetCursorScreenPos().y;
  struct Section {  // (where the section is, for the manual's screenshots)
    App& app;
    float top;
    ~Section() {
      app.engine.uiRects["prefs.language"] = {ImGui::GetWindowPos().x, top, ImGui::GetWindowWidth(), ImGui::GetCursorScreenPos().y - top};
    }
  } section{*this, sectionTop};
  ImGui::SeparatorText(T_("Language"));
  if (!engine.features.on("translations")) {
    ImGui::TextDisabled("%s", T_("Switched off in Features below: Patina is in English."));
    return;
  }
  if (nowSeconds() - languageListAt > 3.0) languageList = i18n::available(), languageListAt = nowSeconds();
  std::string cur = i18n::current(), curName = cur;
  for (auto& l : languageList)
    if (l.code == cur) curName = l.code == "en" ? "English" : l.name;
  ImGui::SetNextItemWidth(260 * uiScale);
  if (ImGui::BeginCombo("##language", curName.c_str())) {
    for (auto& l : languageList) {
      std::string item = l.name + (l.code == "en" || l.code == "x-test" ? "" : "  (" + l.code + (l.user ? T_(", yours") : "") + ")");
      if (ImGui::Selectable((item + "##" + l.code).c_str(), l.code == cur)) setLanguageUI(l.code);
    }
    ImGui::EndCombo();
  }
  ImGui::SameLine();
  if (ImGui::Button(L_("Reload"))) {
    std::string err;
    if (!i18n::reload(&err)) showError(T_("Could not read the translation"), err);
    else followLanguage(), setStatus(T_("Translation read again"));
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", T_("Read the translation file again, after you save it in your editor"));
  const i18n::Stats& st = i18n::stats();
  if (cur != "en" && cur != "x-test") {
    ImGui::TextDisabled(T_("%d of %d texts translated"), st.translated, st.entries);
    if (!st.author.empty()) ImGui::TextDisabled(T_("By %s"), st.author.c_str());
    if (!st.rejected.empty()) {
      ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1, 0.7f, 0.4f, 1));
      ImGui::TextWrapped(P_("%d translation is not used: its %% codes are not the same as the English.",
                            "%d translations are not used: their %% codes are not the same as the English.", (long)st.rejected.size()),
                         (int)st.rejected.size());
      ImGui::PopStyleColor();
      if (ImGui::IsItemHovered()) {
        std::string list;
        for (size_t i = 0; i < st.rejected.size() && i < 12; ++i) list += st.rejected[i] + "\n";
        ImGui::SetTooltip("%s", list.c_str());
      }
    }
  } else if (cur == "x-test") {
    ImGui::TextDisabled("%s", T_("Every text is marked [like this]: what still shows plain English is not translatable yet."));
  }
  if (ImGui::Button(L_("Open the languages folder"))) openFolderUrl(i18n::userDir());
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip(T_("Your translation files (.po) go here:\n%s\nThe template, patina.pot, is in %s"), i18n::userDir().c_str(),
                      i18n::shippedDir().c_str());
  ImGui::SameLine();
  if (ImGui::Button(L_("Make a new translation..."))) ImGui::OpenPopup("##newlang");
  if (ImGui::BeginPopup("##newlang")) {
    ImGui::TextUnformatted(T_("A new translation from the template:"));
    ImGui::SetNextItemWidth(120 * uiScale);
    ImGui::InputTextWithHint(L_("Language code"), "fr, de, pt_BR...", &newLanguageCode);
    ImGui::SetNextItemWidth(200 * uiScale);
    ImGui::InputTextWithHint(L_("Its name in that language"), "Français, Deutsch...", &newLanguageName);
    ImGui::BeginDisabled(newLanguageCode.size() < 2);
    if (ImGui::Button(L_("Make it"))) {
      std::string path, err;
      if (i18n::makeTranslation(newLanguageCode, newLanguageName, &path, &err)) {
        setStatus(F_("%s is ready to translate (Poedit opens it)", path.c_str()));
        openFolderUrl(i18n::userDir());
        languageListAt = -1e9;
        ImGui::CloseCurrentPopup();
      } else {
        showError(T_("Could not make the translation"), err);
      }
    }
    ImGui::EndDisabled();
    ImGui::TextDisabled("%s", T_("It goes in your languages folder; an existing file is never replaced."));
    ImGui::EndPopup();
  }
  if (cur != "en") {
    ImGui::SameLine();
    if (ImGui::Button(L_("Save the untranslated text..."))) {
      std::string path = (paths::ensureDir(i18n::userDir()) / (cur + "-untranslated.po")).string(), err;
      if (i18n::writeUntranslated(path, &err)) setStatus(F_("Saved %zu untranslated texts to %s", i18n::untranslatedSeen().size(), path.c_str()));
      else showError(T_("Could not save"), err);
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", T_("The texts Patina has shown so far that have no translation, as a .po file to fill in"));
  }
  ImGui::TextDisabled("%s", T_("How to translate Patina: the manual's Translating Patina page."));
  ImGui::SameLine();
  if (ImGui::SmallButton(L_("Open it"))) openLocalPage("manual:translating");
}

}  // namespace patina
