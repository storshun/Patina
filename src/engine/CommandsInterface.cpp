// (0.36) Commands for what the interface does: the layers that paint a spot (the 3D view's right-click menu),
// and the interface's language (core/I18n.h).
#include "core/I18n.h"
#include "engine/Engine.h"

namespace patina {

namespace {
json prop(const char* type, const char* desc) { return {{"type", type}, {"description", desc}}; }
json schema(json props = json::object(), std::vector<std::string> required = {}) {
  json s = {{"type", "object"}, {"properties", props}, {"additionalProperties", false}};
  if (!required.empty()) s["required"] = required;
  return s;
}
const char* typeName(LayerType t) {
  switch (t) {
    case LayerType::Fill: return "fill";
    case LayerType::Adjust: return "adjust";
    case LayerType::Folder: return "folder";
    case LayerType::Baked: return "baked";
    default: return "paint";
  }
}
json languageJson(const i18n::Language& l) {
  return {{"code", l.code}, {"name", l.name}, {"file", l.path}, {"user", l.user}};
}
}  // namespace

void Engine::registerInterfaceCommands() {
  CommandRegistry& R = commands;

  R.add({"layers_at",
         "The layers that paint the spot of the model under a viewport point (what the 3D view's right-click menu "
         "lists): each with its coverage there (paint or fill x mask x opacity, 0..1) and whether it is shown. Top "
         "of the stack first; folders are left out. Use it to find which layer to change for a spot you see in "
         "render_view.",
         schema({{"point", prop("array", "[x,y] normalized viewport point (0..1, 0,0 top left)")}}, {"point"}),
         [](CommandContext& c, const json& a) {
           Engine& e = c.engine;
           if (!a["point"].is_array() || a["point"].size() < 2) throw CommandError("point is [x, y]");
           vec2 p(a["point"][0].get<float>(), a["point"][1].get<float>());
           RayHit h = e.pick(p * e.viewportSize);
           if (!h.hit) throw CommandError("the point is not on the model");
           json list = json::array();
           for (auto& l : e.layersAt(h.uv)) {
             const Layer& L = *e.doc.layers[l.index];
             list.push_back({{"index", l.index}, {"uid", L.uid}, {"name", L.name}, {"type", typeName(L.type)},
                             {"amount", std::round(l.amount * 1000.f) / 1000.f}, {"shown", l.shown}});
           }
           return json{{"face", h.face}, {"uv", {h.meshUV.x, h.meshUV.y}}, {"layers", list}};
         }});

  R.add({"language_list",
         "The interface's languages: English, the translation files (.po) shipped with Patina and in the user's "
         "languages folder, and x-test (every text marked, for checking what is translatable). Also how much of the "
         "current one is translated and where the template and the folders are.",
         schema(),
         [](CommandContext&, const json&) {
           json langs = json::array();
           for (auto& l : i18n::available()) langs.push_back(languageJson(l));
           const auto& st = i18n::stats();
           return json{{"current", i18n::current()},
                       {"languages", langs},
                       {"translated", st.translated},
                       {"entries", st.entries},
                       {"rejected", st.rejected},
                       {"template", i18n::templatePath()},
                       {"user_folder", i18n::userDir()},
                       {"shipped_folder", i18n::shippedDir()}};
         }});

  R.add({"language_set",
         "Shows the interface in a language from language_list (code such as fr or pt_BR; en: English; x-test: the "
         "test language). Only the windows change: commands, results and logs stay in English.",
         schema({{"code", prop("string", "The language code (the .po file's name)")}}, {"code"}),
         [](CommandContext& c, const json& a) {
           if (!c.engine.features.on("translations") && a.value("code", std::string()) != "en")
             throw CommandError("translations are switched off (feature 'translations')");
           std::string err;
           if (!i18n::setLanguage(a.value("code", std::string("en")), &err)) throw CommandError(err);
           const auto& st = i18n::stats();
           return json{{"current", i18n::current()}, {"translated", st.translated}, {"entries", st.entries},
                       {"rejected", st.rejected.size()}};
         }});

  R.add({"language_new",
         "Starts a translation: copies the template (languages/patina.pot) to <code>.po in the user's languages "
         "folder with its header filled in (language, name, plural forms). An existing file is never replaced. Edit "
         "it with Poedit or a text editor, then language_set.",
         schema({{"code", prop("string", "Language code: fr, de, es, pt_BR, zh_CN, ja...")},
                 {"name", prop("string", "The language's name in that language: Français, Deutsch...")}},
                {"code"}),
         [](CommandContext&, const json& a) {
           std::string path, err;
           if (!i18n::makeTranslation(a.value("code", std::string()), a.value("name", std::string()), &path, &err))
             throw CommandError(err);
           return json{{"file", path}};
         }});

  R.add({"language_untranslated",
         "The texts the interface showed this session without a translation in the current language, and "
         "optionally a .po file of them to fill in.",
         schema({{"path", prop("string", "Write them to this .po file (optional)")}}),
         [](CommandContext&, const json& a) {
           json r = {{"language", i18n::current()}, {"texts", i18n::untranslatedSeen()}};
           if (a.contains("path")) {
             std::string err;
             if (!i18n::writeUntranslated(a["path"].get<std::string>(), &err)) throw CommandError(err);
             r["file"] = a["path"];
           }
           return r;
         }});
}

}  // namespace patina
