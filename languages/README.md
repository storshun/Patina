# Translations

Patina's interface can be shown in other languages. A translation is a gettext `.po` file: the format that
Poedit, Weblate, Crowdin, Lokalize and most translation tools read and write.

- `patina.pot` here is the **template**: every text of the interface, in English. Don't edit it; it is made from
  the code by `scripts/extract_strings.py`, and a test checks it is up to date.
- Translations shipped with Patina go in this folder as `<code>.po` (`fr.po`, `de.po`, `pt_BR.po`...).
- Your own translations go in your languages folder (Preferences > Language > Open the languages folder); a file
  there wins over a shipped one with the same name.

The manual's **Translating Patina** page (`docs/manual/src/23-translating.html`) is the guide for translators:
starting a translation, the rules for `%d` codes and line breaks, plural forms, fonts for other scripts, testing
and sharing.

## For developers: marking text in the code

Everything a user reads goes through one of the helpers in `src/ui/Tr.h` (they look the English text up in the
current language, `src/core/I18n.h`):

| Helper | For | Example |
|---|---|---|
| `T_("text")` | text: `Text`, tooltips, status lines, dialogs; also as a printf format | `ImGui::Text(T_("%d faces"), n)` |
| `L_("Label")` | a widget's label or a window's name | `ImGui::Button(L_("Make quads##bar"))` |
| `Z_("A\0B\0")` | a `Combo`'s item list | `ImGui::Combo("##m", &m, Z_("Face\0Island\0"))` |
| `P_(one, many, n)` | plurals | `ImGui::Text(P_("%d face", "%d faces", n), n)` |
| `C_(context, text)` | an English word with two meanings | `C_("layer", "Fill")` |
| `F_(fmt, ...)` | printf with the translated format, into a `std::string` | `setStatus(F_("Added %s", name.c_str()))` |
| `N_("text")` | a static table: only marks the text; `T_()` it where it is shown | `static const char* kNames[] = {N_("Edges"), ...}` |

Rules:

- A translated text is never a printf format without arguments: `ImGui::SetTooltip("%s", T_("..."))`, or
  `TextUnformatted(T_("..."))`, not `SetTooltip(T_("..."))`.
- Whole sentences with `%s` for the parts that change, never English pieces glued together:
  `F_("Added the smart material %s", m.name.c_str())`, not `T_("Added the smart material ") + m.name`.
- `L_()` keeps the English label's ImGui ID: the translated label is `"Peindre###Paint"`, and Patina's copy of
  Dear ImGui hashes `label###id` as `id` alone. So `##id` suffixes, `OpenPopup` / `BeginPopup` pairs,
  `FindWindowByName("Layers")`, `DockBuilderDockWindow("Layers", ...)` and the saved layout keep using the English
  names. Never compare a window's `Name` (it is the translated one): compare `window->ID` with `ImHashStr("Layers")`.
  A modal popup shows its title: `BeginPopupModal(L_("Title"))`; `OpenPopup("Title")` still finds it (same ID).
- Not translated: IDs (`##x`), command names and their arguments, JSON keys, file names and extensions, key names
  (`Ctrl+Z`), log lines (`PLOG_*`), MCP text, error messages from the engine, and data: material, brush, smart
  material, recipe and layer names.
- After changing text, run `scripts/extract_strings.py` and commit `languages/patina.pot` with the code.
- To see what is not translatable yet, choose the test language (Preferences > Language > Test language): every
  text that went through the helpers shows like `[Påïñt··]`; plain English is text that was missed.
