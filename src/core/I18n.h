#pragma once
// (0.36) Translations: Patina's interface in other languages, from gettext .po files (the format Poedit, Weblate,
// Crowdin and Lokalize work with). The English text in the code is the key (msgid); text without a translation
// stays English. Files are read from the languages folder shipped with Patina and from the user's own
// (<user data>/languages, which wins); languages/patina.pot is the template (scripts/extract_strings.py makes it
// from the code). The manual's "Translating Patina" page is the guide for translators.
//
// The UI wraps its text (src/ui/Tr.h): T() for text, L() for widget labels and window names, TZ() for combo
// lists, TN() for plurals, TC() with a context. A translated label keeps the English label's ImGui ID:
// "Peindre###Paint" (third_party/imgui hashes "label###id" as "id" alone, a Patina change), so the layout,
// docking and widget IDs are the same in every language. Everything here runs on the main thread.
#include <cstdint>
#include <string>
#include <vector>

// marks a string for the translation template without translating it (static tables: translate it where it shows)
#define N_(s) s

namespace patina::i18n {

const char* tr(const char* en);                                 // the text in the current language
const char* trc(const char* context, const char* en);           // an English word with two meanings ("Fill": tool / layer)
const char* trn(const char* en, const char* enPlural, long n);  // plural forms ("%d face" / "%d faces")
const char* label(const char* en);                              // an ImGui label (keeps its ID; "##" parts stay)
const char* list(const char* en);                               // "a\0b\0c\0" item lists (ImGui::Combo)
std::string format(const char* en, ...);                        // printf with the translated format

struct Language {
  std::string code;   // "fr", "pt_BR": the file's name (en: English, x-test: the test language)
  std::string name;   // in the language itself ("Français"), from the file's header
  std::string path;   // the .po file ("" for English and the test language)
  bool user = false;  // from the user's languages folder
};
std::vector<Language> available();  // English, the shipped and the user's files, the test language
bool setLanguage(const std::string& code, std::string* err = nullptr);  // "en" or "": English
const std::string& current();
bool reload(std::string* err = nullptr);  // read the current language's file again (after editing it)
uint64_t version();                       // changes with every language change or reload

struct Stats {
  int entries = 0, translated = 0, fuzzy = 0;  // in the file (fuzzy ones are not used, as gettext does)
  std::vector<std::string> rejected;            // translations not used: their % codes differ from the English
  std::string font;                             // a font the file asks for (X-Patina-Font), resolved
  std::string glyphs;                           // every character the translations use (UTF-8): the font atlas needs them
  std::string author;                           // Last-Translator
};
const Stats& stats();

std::string userDir();     // <user data>/languages
std::string shippedDir();  // <data>/languages (the template and the shipped translations)
std::string templatePath();
// a new translation: the template as <code>.po in the user's folder, its header filled in; `path`: where
bool makeTranslation(const std::string& code, const std::string& name, std::string* path, std::string* err);
std::vector<std::string> untranslatedSeen();  // shown this session without a translation (sorted)
bool writeUntranslated(const std::string& path, std::string* err);  // ... as a .po to fill in

// ---- the parts, for tests
struct PoEntry {
  std::string context, id, idPlural;
  std::vector<std::string> str;
  bool fuzzy = false;
};
struct PoFile {
  std::vector<PoEntry> entries;  // without the header
  std::string language, languageName, pluralForms, author, font, charset;
};
bool parsePo(const std::string& text, PoFile& out, std::string* err);
bool formatsMatch(const char* en, const char* translated);  // the same % conversions in the same order
// the index of the plural form for n ("nplurals=3; plural=(n==1 ? 0 : ...);"); -1 for a bad expression
int pluralIndex(const std::string& pluralForms, long n);
std::string pseudo(const char* en);  // the test language: accents, brackets and a third longer, % codes kept

}  // namespace patina::i18n
