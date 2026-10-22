#include "engine/Assistant.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <regex>
#include <set>

#include "engine/Engine.h"
#include "engine/SmartMaterial.h"

namespace patina::assistant {

namespace {

const std::regex& refPattern() {
  static const std::regex r(R"(^\$(\d+)\.([a-z_][a-z0-9_]*)$)");
  return r;
}
bool isRef(const json& v) { return v.is_string() && std::regex_match(v.get<std::string>(), refPattern()); }

// every string in `v` that starts with '$': must be a reference to an earlier step
std::string checkRefs(const json& v, int step) {
  if (v.is_string()) {
    const std::string& s = v.get_ref<const std::string&>();
    if (s.empty() || s[0] != '$') return "";
    std::smatch m;
    if (!std::regex_match(s, m, refPattern())) return "'" + s + "' is not a reference (write $<step>.<field>)";
    int n = std::stoi(m[1].str());
    if (n >= step) return "'" + s + "' refers to step " + std::to_string(n) + ", which has not run yet";
    return "";
  }
  if (v.is_array() || v.is_object())
    for (auto& x : v) {
      std::string e = checkRefs(x, step);
      if (!e.empty()) return e;
    }
  return "";
}

json substitute(const json& v, const std::vector<json>& results) {
  if (isRef(v)) {
    std::smatch m;
    std::string s = v.get<std::string>();
    std::regex_match(s, m, refPattern());
    size_t n = std::stoul(m[1].str());
    std::string field = m[2].str();
    if (n >= results.size() || !results[n].is_object() || !results[n].contains(field))
      throw CommandError("step " + std::to_string(n) + " gave no '" + field + "'");
    return results[n][field];
  }
  if (v.is_object()) {
    json o = json::object();
    for (auto& [k, x] : v.items()) o[k] = substitute(x, results);
    return o;
  }
  if (v.is_array()) {
    json a = json::array();
    for (auto& x : v) a.push_back(substitute(x, results));
    return a;
  }
  return v;
}

std::string str(const json& a, const char* k, const std::string& d = "") {
  return a.contains(k) && a[k].is_string() ? a[k].get<std::string>() : d;
}

std::string partWords(const std::string& id) {
  if (id == "selection") return "the selected faces";
  if (id == "all") return "the whole model";
  auto colon = id.find(':');
  if (colon == std::string::npos) return id;
  std::string kind = id.substr(0, colon), name = id.substr(colon + 1);
  if (kind == "region") return "the " + name;
  if (kind == "piece") return "piece " + name;
  return name;
}

std::string regionWords(const json& a) {
  if (!a.contains("region")) return "";
  const json& r = a["region"];
  std::vector<std::string> in, out;
  if (r.is_string()) in.push_back(r.get<std::string>());
  else if (r.is_object()) {
    if (r.contains("part") && r["part"].is_string()) in.push_back(r["part"].get<std::string>());
    for (auto& p : r.value("parts", json::array()))
      if (p.is_string()) in.push_back(p.get<std::string>());
    for (auto& p : r.value("except", json::array()))
      if (p.is_string()) out.push_back(p.get<std::string>());
  }
  std::string s;
  for (size_t i = 0; i < in.size(); ++i) s += (i ? (i + 1 == in.size() ? " and " : ", ") : "") + partWords(in[i]);
  if (s.empty() && !out.empty()) s = "everything";
  for (size_t i = 0; i < out.size(); ++i) s += (i ? ", " : " except ") + partWords(out[i]);
  return s.empty() ? "" : " on " + s;
}

std::string maskWords(const json& a) {
  if (!a.contains("mask") || !a["mask"].is_array() || a["mask"].empty()) return "";
  static const std::map<std::string, std::string> words = {
      {"cavity", "in the creases"},       {"edges", "on the edges"},       {"occlusion", "in sheltered places"},
      {"facing_up", "where dust settles"}, {"facing_down", "on surfaces facing down"},
      {"facing", "on surfaces facing one way"}, {"gradient_up", "fading in toward the top"},
      {"gradient_down", "fading in toward the bottom"}, {"noise", "broken up by noise"}, {"thin", "on thin parts"},
      {"gradient", "fading in one way"}, {"drips", "in streaks running down"}, {"pattern", "in a pattern"}};
  std::string s;
  for (auto& m : a["mask"]) {
    std::string t = str(m, "type");
    auto it = words.find(t);
    std::string w = it != words.end() ? it->second : t;
    if (m.value("invert", false)) w = "away from " + w.substr(w.find(' ') + 1);
    s += (s.empty() ? ", " : " and ") + w;
  }
  return s;
}

std::string pct(const json& a, const char* k) {
  if (!a.contains(k) || !a[k].is_number()) return "";
  return std::to_string((int)std::lround(a[k].get<double>() * 100)) + "%";
}

}  // namespace

const std::vector<KnownModel>& knownModels() {
  static const std::vector<KnownModel> list = {
      {"Patina Assistant 1.7B (0.12.0 round 7, Q4_K_M)",
       "c0105bb18bceb516259312df7fc2a3e6ca45a8ccbadad5eefd1f6b214c89e9e0", 1107408640ull},
      {"Patina Assistant 1.7B (0.11.0 round 5, Q4_K_M)",
       "b0704b177aa143def9bba9acec57e12fa3fcc7f753f41dd20fab83b2db92b1cd", 1107408640ull},
      {"Patina Assistant 1.7B (0.11.0 round 2, Q4_K_M)",
       "a772c78dcc3fe33817b6c626bdb3b9a3ae0848d4a475e048610c072a7d90482b", 1107408640ull},
  };
  return list;
}

const std::vector<std::string>& allowedCommands() {
  static const std::vector<std::string> list = {
      // layers
      "add_tint_layer", "add_material_layer", "update_layer_mask", "add_layer", "update_layer", "remove_layer",
      "duplicate_layer",
      "move_layer", "select_layer", "group_into_folder", "move_layer_into", "add_mask_effect", "set_layer_mask",
      "clear_layer", "apply_smart_material", "copy_layer_settings", "add_instance", "solo_layer", "auto_texture",
      // parts and UVs
      "select_part", "select_faces", "uv_island_per_part", "uv_unwrap", "uv_relax", "uv_pack", "uv_density",
      "uv_mirror", "uv_orient", "uv_fix_overlaps", "uv_make_island", "uv_pin", "uv_seams", "uv_rectify",
      // mesh maps
      "bake_mesh_maps",
      // painting set-up and view
      "set_brush", "set_paint_material", "set_paint_channels", "set_tool", "set_symmetry", "fill", "set_view",
      "set_camera", "set_environment", "set_compare", "add_note",
      // 0.12: windows, and texture sets / resolution (they clear the undo history: last step only)
      "open_window", "texture_set_from_faces", "set_resolution", "texture_tiles", "set_stencil"};
  return list;
}

// arguments a plan may not use: they read or write files
static bool fileArg(const std::string& cmd, const std::string& arg) {
  return (cmd == "set_stencil" && (arg == "path" || arg == "save_image")) || (cmd == "set_environment" && arg == "hdr_path");
}

bool clearsHistory(const std::string& cmd) {
  return cmd == "texture_set_from_faces" || cmd == "set_resolution" || cmd == "texture_tiles";
}

bool isAllowed(const std::string& cmd) {
  const auto& l = allowedCommands();
  return std::find(l.begin(), l.end(), cmd) != l.end();
}

bool isDestructive(const std::string& cmd, const json& args) {
  if (cmd == "remove_layer" || cmd == "clear_layer") return true;
  if (cmd == "set_layer_mask") return args.value("remove", false) || args.value("from_selection", false);
  if (cmd == "update_layer") return args.contains("mask_effects");
  if (cmd == "uv_island_per_part" || cmd == "uv_unwrap" || cmd == "uv_pack" || cmd == "uv_mirror" ||
      cmd == "uv_make_island" || cmd == "uv_rectify")
    return true;
  if (cmd == "uv_seams") return str(args, "mode") == "clear" || str(args, "mode") == "from_islands";
  if (cmd == "uv_density") return args.contains("scale") && args.value("pack", true);  // repacks every island
  return false;
}

std::string checkSchema(const json& s, const json& v, const std::string& where) {
  if (!s.is_object()) return "";
  if (isRef(v)) return "";  // filled in when the step runs, checked again then
  if (s.contains("enum")) {
    bool found = false;
    for (auto& e : s["enum"]) found |= e == v;
    if (!found) return where + ": '" + (v.is_string() ? v.get<std::string>() : v.dump()) + "' is not one of " + s["enum"].dump();
  }
  std::string type = s.value("type", std::string());
  if (type == "string" && !v.is_string()) return where + " should be text";
  if (type == "boolean" && !v.is_boolean()) return where + " should be true or false";
  if (type == "integer" && !v.is_number_integer()) return where + " should be a whole number";
  if (type == "number" && !v.is_number()) return where + " should be a number";
  if (type == "array" && !v.is_array()) return where + " should be a list";
  if (type == "object" && !v.is_object()) return where + " should be an object";
  if (v.is_number()) {
    double x = v.get<double>();
    if (s.contains("minimum") && x < s["minimum"].get<double>()) return where + " is below " + s["minimum"].dump();
    if (s.contains("maximum") && x > s["maximum"].get<double>()) return where + " is above " + s["maximum"].dump();
  }
  if (v.is_array()) {
    if (s.contains("minItems") && v.size() < s["minItems"].get<size_t>()) return where + " has too few items";
    if (s.contains("maxItems") && v.size() > s["maxItems"].get<size_t>()) return where + " has too many items";
    if (s.contains("items"))
      for (size_t i = 0; i < v.size(); ++i) {
        std::string e = checkSchema(s["items"], v[i], where + "[" + std::to_string(i) + "]");
        if (!e.empty()) return e;
      }
  }
  if (v.is_object()) {
    const json props = s.value("properties", json::object());
    for (auto& r : s.value("required", json::array()))
      if (!v.contains(r.get<std::string>())) return where + " needs '" + r.get<std::string>() + "'";
    for (auto& [k, x] : v.items()) {
      if (props.contains(k)) {
        std::string e = checkSchema(props[k], x, where + "." + k);
        if (!e.empty()) return e;
      } else if (s.contains("additionalProperties") && s["additionalProperties"] == false) {
        return where + ": unknown argument '" + k + "'";
      }
    }
  }
  return "";
}

PlanCheck checkPlan(const CommandRegistry& commands, const json& plan) {
  PlanCheck r;
  auto fail = [&](const std::string& e) {
    r.ok = false;
    r.error = e;
    return r;
  };
  if (!plan.is_object()) return fail("a plan is an object");
  for (auto& [k, v] : plan.items()) {
    if (k != "say" && k != "ask" && k != "steps" && k != "suggest") return fail("unknown plan field '" + k + "'");
    if ((k == "say" || k == "ask") && !v.is_string()) return fail(k + " should be text");
    if (k == "suggest") {
      if (!v.is_array() || v.empty() || (int)v.size() > kMaxSuggest) return fail("suggest is a list of 1 to 4 requests");
      for (auto& x : v)
        if (!x.is_string() || x.get<std::string>().empty()) return fail("a suggestion should be text");
    }
  }
  json steps = plan.value("steps", json::array());
  if (!steps.is_array()) return fail("steps should be a list");
  if ((int)steps.size() > kMaxSteps) return fail("a plan has at most " + std::to_string(kMaxSteps) + " steps");
  if (steps.empty() && !plan.contains("say") && !plan.contains("ask")) return fail("the plan does nothing and says nothing");
  for (size_t i = 0; i < steps.size(); ++i) {
    const json& st = steps[i];
    std::string at = "step " + std::to_string(i);
    if (!st.is_object() || !st.contains("cmd") || !st["cmd"].is_string()) return fail(at + " has no command");
    for (auto& [k, v] : st.items())
      if (k != "cmd" && k != "args") return fail(at + ": unknown field '" + k + "'");
    std::string cmd = st["cmd"];
    if (!isAllowed(cmd)) return fail(at + ": '" + cmd + "' can't be used by the Assistant");
    const Command* c = commands.find(cmd);
    if (!c) return fail(at + ": unknown command '" + cmd + "'");
    json args = st.value("args", json::object());
    if (!args.is_object()) return fail(at + ": args should be an object");
    for (auto& [k, v] : args.items())
      if (fileArg(cmd, k)) return fail(at + ": the Assistant doesn't use files ('" + k + "')");
    std::string e = checkRefs(args, (int)i);
    if (!e.empty()) return fail(at + ": " + e);
    e = checkSchema(c->schema, args, at + " (" + cmd + ")");
    if (!e.empty()) return fail(e);
    if (clearsHistory(cmd) && i + 1 != steps.size())
      return fail(at + ": '" + cmd + "' clears the undo history, so it can only be the last step");
    if (cmd == "texture_tiles" && !args.contains("mode")) return fail(at + ": texture_tiles needs a mode");
    if (isDestructive(cmd, args)) r.warnings.push_back(describeStep(st));
  }
  r.ok = true;
  return r;
}

bool planClearsHistory(const json& plan) {
  json steps = plan.value("steps", json::array());
  return !steps.empty() && steps.back().is_object() && clearsHistory(steps.back().value("cmd", std::string()));
}

json applyPlan(Engine& e, const json& plan, const std::string& label) {
  PlanCheck pc = checkPlan(e.commands, plan);
  if (!pc.ok) throw CommandError("the plan was refused: " + pc.error);
  json steps = plan.value("steps", json::array());
  std::vector<json> results;
  std::set<uint32_t> before;
  for (auto& l : e.doc.layers) before.insert(l->uid);
  // A layer number written in the plan means the layer the model was shown at that number, even
  // after an earlier step deleted or added layers ("delete 3, then 4" deletes both).
  static const std::set<std::string> byIndex = {"update_layer", "remove_layer", "duplicate_layer", "update_layer_mask",
                                                "clear_layer", "select_layer", "group_into_folder", "solo_layer"};
  std::vector<uint32_t> shown;  // uid per index, as the stack was when the plan was written
  for (auto& l : e.doc.layers) shown.push_back(l->uid);
  // a last step that clears the undo history runs after the group has closed
  size_t grouped = planClearsHistory(plan) ? steps.size() - 1 : steps.size();
  e.doc.beginUndoGroup(label);
  for (size_t i = 0; i < grouped; ++i) {
    std::string cmd = steps[i]["cmd"];
    try {
      json args = substitute(steps[i].value("args", json::object()), results);
      if (byIndex.count(cmd) && args.is_object() && steps[i].value("args", json::object()).contains("index") &&
          steps[i]["args"]["index"].is_number_integer()) {
        int shownIndex = steps[i]["args"]["index"].get<int>();
        if (shownIndex >= 0 && shownIndex < (int)shown.size()) {
          int now = e.doc.indexOf(shown[shownIndex]);
          if (now < 0) throw CommandError("layer " + std::to_string(shownIndex) + " was already removed by an earlier step");
          args["index"] = now;
        }
      }
      std::string err = checkSchema(e.commands.find(cmd)->schema, args, "args");
      if (!err.empty()) throw CommandError(err);
      results.push_back(e.execute(cmd, args));
      e.painter.update();
    } catch (std::exception& ex) {
      e.doc.cancelUndoGroup();
      throw CommandError("step " + std::to_string(i) + " (" + cmd + ") failed: " + ex.what() +
                         (i ? "; the steps before it were undone" : ""));
    }
  }
  // what the request made is easy to find: a color tag, and several new layers share a folder
  // named after the request (all in the same undo step)
  Document& d = e.doc;
  std::vector<uint32_t> made;
  for (auto& l : d.layers)
    if (!before.count(l->uid) && d.effectiveParent(*l) == 0 && !l->isBaked()) made.push_back(l->uid);  // (baked map layers: theirs)
  for (uint32_t uid : made)
    if (Layer* l = d.findLayer(uid); l && l->colorTag == 0) l->colorTag = kLayerTag;
  if (made.size() > 1) {
    int f = d.groupIntoFolder(d.indexOf(made.back()));
    if (f >= 0) {
      uint32_t folder = d.layers[f]->uid;
      std::string name = plan.value("say", label);
      d.layers[f]->name = name.size() > 40 ? name.substr(0, 39) + "…" : name;
      d.layers[f]->colorTag = kLayerTag;
      for (size_t i = 0; i + 1 < made.size(); ++i) d.setParent(d.indexOf(made[i]), folder);
    }
  }
  d.checkpoint();
  e.doc.endUndoGroup();
  bool cleared = false;
  if (grouped < steps.size()) {
    std::string cmd = steps.back()["cmd"];
    try {
      json args = substitute(steps.back().value("args", json::object()), results);
      std::string err = checkSchema(e.commands.find(cmd)->schema, args, "args");
      if (!err.empty()) throw CommandError(err);
      results.push_back(e.execute(cmd, args));
      e.painter.update();
      cleared = true;
    } catch (std::exception& ex) {
      throw CommandError("step " + std::to_string(grouped) + " (" + cmd + ") failed: " + ex.what() +
                         (grouped ? " (the steps before it ran: Ctrl+Z takes them back)" : ""));
    }
  }
  json warnings = json::array();
  for (auto& w : pc.warnings) warnings.push_back(w);
  json notes = json::array();  // what the steps found worth telling (a UV layout that folds)
  for (auto& r : results)
    if (r.is_object() && r.contains("notes"))
      for (auto& n : r["notes"]) notes.push_back(n);
  return json{{"results", results}, {"warnings", warnings}, {"notes", notes}, {"say", plan.value("say", std::string())},
              {"steps", steps.size()}, {"cleared_history", cleared}};
}

namespace {

// Writes GBNF rules; each distinct right-hand side gets one rule name.
struct GrammarWriter {
  std::map<std::string, std::string> rules;  // name -> body
  std::map<std::string, std::string> byBody;
  int next = 0;
  std::string rule(const std::string& hint, const std::string& body) {
    auto it = byBody.find(body);
    if (it != byBody.end()) return it->second;
    std::string name = hint;
    for (auto& c : name)
      if (!std::isalnum((unsigned char)c)) c = '-';
    while (rules.count(name)) name = hint + "-" + std::to_string(++next);
    rules[name] = body;
    byBody[body] = name;
    return name;
  }
  static std::string lit(const std::string& s) {  // a GBNF string literal of the JSON text s
    std::string o = "\"";
    for (char c : s) {
      if (c == '"' || c == '\\') o += '\\';
      o += c;
    }
    return o + "\"";
  }
  static std::string jsonLit(const std::string& s) { return lit(json(s).dump()); }
  std::string text() const {
    std::string t;
    for (auto& [n, b] : rules) t += n + " ::= " + b + "\n";
    return t;
  }
};

const char* kBase =
    "ws ::= \"\"\n"
    "jstring ::= \"\\\"\" ( [^\"\\\\\\x00-\\x1f] | \"\\\\\" ([\"\\\\/bfnrt] | \"u\" [0-9a-fA-F]{4}) )* \"\\\"\"\n"
    "jint ::= \"-\"? ([0-9] | [1-9] [0-9]{0,8})\n"
    "jnumber ::= \"-\"? ([0-9] | [1-9] [0-9]{0,8}) (\".\" [0-9]{1,6})? ([eE] [-+]? [0-9]{1,3})?\n"
    "jbool ::= \"true\" | \"false\"\n"
    "jvalue ::= jobject | jarray | jstring | jnumber | jbool | \"null\"\n"
    "jobject ::= \"{\" ( jstring \":\" jvalue ( \",\" jstring \":\" jvalue )* )? \"}\"\n"
    "jarray ::= \"[\" ( jvalue ( \",\" jvalue )* )? \"]\"\n"
    "jref ::= \"\\\"$\" [0-9] [0-9]? \".\" [a-z_] [a-z0-9_]* \"\\\"\"\n";

struct Live {
  std::vector<std::string> parts, materials, smart, brushes;
  int layers = 0;
};

std::string enumRule(GrammarWriter& g, const std::string& hint, const std::vector<std::string>& values) {
  if (values.empty()) return "jstring";
  std::string body;
  for (auto& v : values) body += (body.empty() ? "" : " | ") + GrammarWriter::jsonLit(v);
  return g.rule(hint, body);
}

std::string valueRule(GrammarWriter& g, const json& s, const std::string& path, const Live& live);

std::string objectRule(GrammarWriter& g, const json& s, const std::string& path, const Live& live) {
  json props = s.value("properties", json::object());
  if (props.empty()) return "jobject";
  std::vector<std::string> names;
  for (auto& [k, v] : props.items()) names.push_back(k);  // alphabetical (nlohmann objects are sorted)
  std::set<std::string> req;
  for (auto& r : s.value("required", json::array())) req.insert(r.get<std::string>());
  size_t n = names.size();
  std::vector<std::string> kv(n);
  for (size_t i = 0; i < n; ++i)
    kv[i] = GrammarWriter::lit(json(names[i]).dump() + ":") + " " + valueRule(g, props[names[i]], path + "." + names[i], live);
  auto anyRequiredFrom = [&](size_t k) {
    for (size_t j = k; j < n; ++j)
      if (req.count(names[j])) return true;
    return false;
  };
  // from[k]: the properties from k on, at least one of them
  std::vector<std::string> from(n + 1);
  for (size_t k = n; k-- > 0;) {
    std::string body;
    for (size_t j = k; j < n; ++j) {
      std::string alt = kv[j];
      if (j + 1 < n) alt += anyRequiredFrom(j + 1) ? " \",\" " + from[j + 1] : " ( \",\" " + from[j + 1] + " )?";
      body += (body.empty() ? "" : " | ") + alt;
      if (req.count(names[j])) break;  // a required property can't be skipped
    }
    from[k] = g.rule(path + "-from-" + names[k], body);
  }
  std::string inner = anyRequiredFrom(0) ? from[0] : "( " + from[0] + " )?";
  return g.rule(path, "\"{\" " + inner + " \"}\"");
}

std::string valueRule(GrammarWriter& g, const json& s, const std::string& path, const Live& live) {
  // the live enums, by where the value sits
  auto ends = [&](const char* suffix) {
    std::string sf = suffix;
    return path.size() >= sf.size() && path.compare(path.size() - sf.size(), sf.size(), sf) == 0;
  };
  if (ends(".region.part") || ends(".region.parts[]") || ends(".region.except[]") || ends("select_part.part") ||
      ends("uv_island_per_part.parts[]"))
    return enumRule(g, "part-id", live.parts);
  if (ends("add_material_layer.material") || ends("set_paint_material.id")) {
    std::vector<std::string> v = live.materials;
    if (ends("set_paint_material.id")) v.push_back("");
    return enumRule(g, ends(".id") ? "paint-material-id" : "material-id", v);
  }
  if (ends("apply_smart_material.id")) return enumRule(g, "smart-id", live.smart);
  if (ends("set_brush.preset")) return enumRule(g, "brush-id", live.brushes);
  std::string type = s.value("type", std::string());
  bool layerIndex = ends(".index") || ends("move_layer.from") || ends("move_layer.to") || ends("copy_layer_settings.from") ||
                    ends(".parent") || ends(".folder");
  if (s.contains("enum")) {
    std::vector<std::string> v;
    for (auto& e : s["enum"])
      if (e.is_string()) v.push_back(e.get<std::string>());
    return enumRule(g, path, v);
  }
  if (type == "integer") {
    if (layerIndex && live.layers > 0) {
      std::string body;
      for (int i = 0; i <= live.layers; ++i) body += (body.empty() ? "" : " | ") + GrammarWriter::lit(std::to_string(i));
      return g.rule("layer-index", "( " + body + " ) | jref");
    }
    return g.rule("int-or-ref", "jint | jref");
  }
  if (type == "number") return "jnumber";
  if (type == "boolean") return "jbool";
  if (type == "string") return "jstring";
  if (type == "object") return objectRule(g, s, path, live);
  if (type == "array") {
    if (!s.contains("items")) return "jarray";
    std::string item = valueRule(g, s["items"], path + "[]", live);
    int lo = s.value("minItems", 0), hi = s.value("maxItems", -1);
    if (lo == hi && lo > 0 && lo <= 4) {  // fixed length (colors, directions)
      std::string body = "\"[\" " + item;
      for (int i = 1; i < lo; ++i) body += " \",\" " + item;
      return g.rule(path + "-array", body + " \"]\"");
    }
    std::string list = item + " ( \",\" " + item + " )*";
    return g.rule(path + "-array", "\"[\" " + (lo > 0 ? list : "( " + list + " )?") + " \"]\"");
  }
  return "jvalue";
}

}  // namespace

std::string buildGrammar(Engine& e) {
  Live live;
  if (e.doc.hasMesh()) {
    for (auto& p : e.meshParts().parts) live.parts.push_back(p.id);
    if (!e.doc.selectedFaces().empty()) live.parts.push_back("selection");
  }
  for (auto& m : e.materials.entries()) live.materials.push_back(m->def.id);
  for (auto& m : listSmartMaterials()) live.smart.push_back(m.id);
  for (auto& b : e.brushes.presets()) live.brushes.push_back(b.id);
  live.layers = (int)e.doc.layers.size();
  GrammarWriter g;
  std::string steps;
  for (auto& cmd : allowedCommands()) {
    const Command* c = e.commands.find(cmd);
    if (!c) continue;
    json schema = c->schema;
    if (schema.contains("properties"))
      for (auto it = schema["properties"].begin(); it != schema["properties"].end();)
        it = fileArg(cmd, it.key()) ? schema["properties"].erase(it) : std::next(it);
    std::string args = objectRule(g, schema, cmd, live);
    std::string body = GrammarWriter::lit("{\"cmd\":" + json(cmd).dump() + ",\"args\":") + " " + args + " \"}\"";
    steps += (steps.empty() ? "" : " | ") + g.rule("step-" + cmd, body);
  }
  g.rule("step", steps);
  std::string list = "step ( \",\" step ){0," + std::to_string(kMaxSteps - 1) + "}";
  // suggestions: short requests, no newlines, at most kMaxSuggest
  g.rule("suggest", GrammarWriter::lit(",\"suggest\":[") + " jstring ( \",\" jstring ){0," + std::to_string(kMaxSuggest - 1) + "} \"]\"");
  std::string root = GrammarWriter::lit("{\"say\":") + " jstring ( " + GrammarWriter::lit(",\"steps\":[") + " " + list +
                     " \"]\" )? suggest? \"}\" | " + GrammarWriter::lit("{\"ask\":") + " jstring suggest? \"}\"";
  return std::string("root ::= ") + root + "\n" + kBase + g.text();
}

// "layer 3", or "the new layer" for an earlier step's result ("$0.index")
std::string layerRef(const json& a) {
  if (!a.contains("index")) return "the selected layer";
  const json& i = a["index"];
  if (i.is_string()) return "the new layer";
  return "layer " + i.dump();
}

std::string describeStep(const json& step, const std::function<std::string(const std::string&)>& materialName) {
  auto matName = [&](const std::string& id) {
    std::string n = materialName ? materialName(id) : "";
    return n.empty() ? id : n;
  };
  std::string cmd = step.value("cmd", std::string());
  json a = step.value("args", json::object());
  std::string name = str(a, "name");
  std::string named = name.empty() ? "" : " '" + name + "'";
  if (cmd == "add_tint_layer") {
    std::string s = std::string(a.value("glow", false) ? "Add a glow layer" : "Add a tint layer") + named + regionWords(a) + maskWords(a);
    if (a.contains("strength")) s += ", strength " + pct(a, "strength");
    return s;
  }
  if (cmd == "add_material_layer")
    return "Add " + matName(str(a, "material")) + (a.contains("tint") ? " (colored)" : "") + named + regionWords(a) + maskWords(a);
  if (cmd == "add_layer") {
    std::string type = str(a, "type", "paint");
    if (type == "adjust") return "Add an adjustment layer" + named;
    return "Add a " + type + " layer" + named;
  }
  if (cmd == "update_layer") {
    std::vector<std::string> what;
    for (auto& [k, v] : a.items())
      if (k != "index") what.push_back(k == "opacity" ? "opacity " + pct(a, "opacity") : k == "visible" ? (v == true ? "show it" : "hide it") : k);
    std::string s = "Change " + layerRef(a) + ":";
    for (size_t i = 0; i < what.size(); ++i) s += (i ? ", " : " ") + what[i];
    return s;
  }
  if (cmd == "remove_layer") return "Delete " + layerRef(a);
  if (cmd == "clear_layer") return "Clear the paint on " + layerRef(a);
  if (cmd == "duplicate_layer") return "Duplicate " + layerRef(a);
  if (cmd == "move_layer") return "Move layer " + a.value("from", json()).dump() + " to position " + a.value("to", json()).dump();
  if (cmd == "select_layer") return "Select layer " + a.value("index", json()).dump();
  if (cmd == "update_layer_mask") {
    std::string s = "Change where " + layerRef(a) + " shows";
    if (a.value("everywhere", false)) return s + ": everywhere";
    return s + ":" + regionWords(a) + maskWords(a);
  }
  if (cmd == "select_part") return "Select " + partWords(str(a, "part"));
  if (cmd == "uv_island_per_part") {
    std::string s = "Rebuild the UVs: one island each for ";
    auto p = a.value("parts", json::array());
    for (size_t i = 0; i < p.size(); ++i)
      s += (i ? (i + 1 == p.size() ? " and " : ", ") : "") + partWords(p[i].is_string() ? p[i].get<std::string>() : "");
    return s;
  }
  if (cmd == "uv_unwrap") return "Unwrap the UVs again (" + str(a, "method", "lscm") + ")";
  if (cmd == "uv_pack") return "Pack the UV islands";
  if (cmd == "uv_relax") return "Relax the UVs";
  if (cmd == "uv_mirror") return "Mirror the UVs across the model's symmetry";
  if (cmd == "uv_make_island") return "Make the selected faces one UV island";
  if (cmd == "uv_density") {
    if (a.contains("scale")) {
      double k = a["scale"].is_number() ? a["scale"].get<double>() : 1.0;
      char buf[96];
      snprintf(buf, sizeof buf, "%s texture space for the selected faces (%.2gx the detail), then repack", k >= 1 ? "More" : "Less", k);
      return buf;
    }
    return "Even out the texel density";
  }
  if (cmd == "uv_fix_overlaps") return "Move overlapping UV islands apart";
  if (cmd == "bake_mesh_maps") return "Bake the mesh maps (ambient occlusion, curvature)";
  if (cmd == "apply_smart_material") return "Apply the smart material " + str(a, "id") + regionWords(a) + maskWords(a);
  if (cmd == "auto_texture") return "Pick a material for each material slot from its name";
  if (cmd == "set_paint_material") return "Paint with " + (str(a, "id").empty() ? std::string("a flat color") : matName(str(a, "id")));
  if (cmd == "set_brush") return "Set the brush" + (a.contains("preset") ? " to " + str(a, "preset") : std::string(""));
  if (cmd == "set_tool") return "Use the " + str(a, "tool") + " tool";
  if (cmd == "set_view") return "Show " + str(a, "mode", "the view");
  if (cmd == "set_camera") return "Move the camera" + (a.contains("view") ? " to the " + str(a, "view") + " view" : std::string(""));
  if (cmd == "fill") return "Fill " + str(a, "mode", "faces") + " with the paint material";
  if (cmd == "group_into_folder") return "Put the layer in a folder";
  if (cmd == "add_mask_effect") return "Add a mask effect";
  if (cmd == "set_layer_mask")
    return a.value("remove", false) ? "Remove the layer's mask" : a.value("from_selection", false) ? "Mask the layer to the selected faces" : "Add a mask to the layer";
  if (cmd == "set_symmetry") return "Set painting symmetry";
  if (cmd == "texture_set_from_faces")
    return "Give " + (a.contains("part") ? partWords(str(a, "part")) : std::string("the selected faces")) +
           (a.contains("part") ? " a texture set of its own" : " a texture set of their own") +
           (name.empty() ? std::string("") : " called " + name) + " (clears the undo history)";
  if (cmd == "set_resolution") return "Set the texture resolution to " + a.value("resolution", json()).dump() + " px (clears the undo history)";
  if (cmd == "texture_tiles") return "Switch the texture layout to " + str(a, "mode") + " (clears the undo history)";
  if (cmd == "open_window")
    return "Open the " + str(a, "window") + " window" + (a.contains("preset") ? " (" + str(a, "preset") + ")" : std::string(""));
  if (cmd == "select_faces") return "Selection: " + str(a, "mode");
  if (cmd == "set_stencil")
    return a.contains("text") ? "Put the text '" + str(a, "text") + "' over the view as a stencil to paint through"
                              : a.value("clear", false) ? "Remove the stencil" : "Change the stencil";
  if (cmd == "set_environment") return "Change the lighting";
  if (cmd == "set_compare") return "Compare before and after";
  if (cmd == "uv_pin") return a.value("pin", true) ? "Pin the UV islands" : "Unpin the UV islands";
  return cmd;
}

}  // namespace patina::assistant
