#pragma once
// (0.36) The interface's text in the user's language (core/I18n.h). Every string a user reads goes through one
// of these, so scripts/extract_strings.py finds it for the template (languages/patina.pot):
//   T_("text")                  text: ImGui::Text, tooltips, status lines, dialogs; also as a printf format
//   L_("Label")                 a widget's label or a window's name (Button, MenuItem, Checkbox, Begin...): it
//                               keeps the English ID, so "##id" parts, docking and the layout work in every language
//   Z_("One\0Two\0")            a Combo's item list
//   P_("%d face", "%d faces", n)  plurals
//   C_("layer", "Fill")         with a context, for an English word with two meanings
//   F_("Added %s", name)        printf with the translated format, into a std::string (status lines)
//   N_("text")                  in a static table: only marks it; T_() it where it is shown
// Not translated: IDs ("##x"), command names and arguments, file names, log lines (PLOG_*), MCP text.
#include <string>

#include "core/I18n.h"

namespace patina {

#ifdef PATINA_FORMAT_CHECK
// (scripts/dev/format_check.py) the English text as it is, so -Wformat checks every format against its arguments
#define T_(s) (s)
#define L_(s) (s)
#define Z_(s) (s)
#define P_(one, many, n) ((n) == 1 ? (one) : (many))
#define C_(context, s) (s)
#else
inline const char* T_(const char* s) { return i18n::tr(s); }
inline const char* L_(const char* s) { return i18n::label(s); }
inline const char* Z_(const char* s) { return i18n::list(s); }
inline const char* P_(const char* one, const char* many, long n) { return i18n::trn(one, many, n); }
inline const char* C_(const char* context, const char* s) { return i18n::trc(context, s); }
#endif
std::string F_(const char* fmt, ...)
#if defined(__GNUC__) || defined(__clang__)
    __attribute__((format(printf, 1, 2)))
#endif
    ;

}  // namespace patina
