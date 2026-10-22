#include "mcp/McpServer.h"

#include <cmath>
#include <cstdio>
#include <iostream>

#include "core/Log.h"
#include "mcp/McpGuides.h"

namespace patina {

static const char* kVersions[] = {"2025-06-18", "2025-03-26", "2024-11-05"};

const char* McpServer::instructions() {
  return "Patina post-processes 3D models the way you would a photo (non-destructive, layered, and scriptable): it opens a model (glb gltf obj fbx stl 3mf ply, or "
         "an object sent from Blender) with its own textures, checks and mends the mesh, develops and paints the "
         "textures, makes it lighter, adds collision and levels of detail, and exports it (glTF, OBJ, STL, PLY, "
         "textures for any engine, back to Blender), one model or whole folders by recipe.\n"
         "Start with the patina_guide tool (topic workflow): it gives the order of the tools; topic parameters lists "
         "every setting with its range, default and effect. The same guides are MCP resources (patina://guide/...), "
         "and prompts cover common jobs (finish_model, develop_textures, game_ready, batch_folder).\n"
         "Work by looking and measuring: render_view returns an image; mesh_check, texture_check, mesh_reduce "
         "(preview) and mesh_collision (fit) return numbers to check a step by. Positions are in Blender's axes, in "
         "meters. undo / redo cover paint and layers; mesh changes keep a copy first (mesh_fix_revert).";
}

static std::string fillTemplate(std::string t, const json& args) {
  size_t a;
  while ((a = t.find("{{")) != std::string::npos) {
    size_t b = t.find("}}", a);
    if (b == std::string::npos) break;
    std::string key = t.substr(a + 2, b - a - 2);
    std::string v = args.is_object() && args.contains(key) && args[key].is_string() ? args[key].get<std::string>() : "";
    t.replace(a, b - a + 2, v);
  }
  return t;
}

// 1 rather than 1.0, 0.05 rather than 0.050000000000000003
static std::string compact(const json& v) {
  if (!v.is_number_float()) return v.dump();
  double d = v.get<double>();
  if (d == std::floor(d) && std::abs(d) < 1e15) return std::to_string((long long)d);
  char buf[32];
  snprintf(buf, sizeof buf, "%.6g", d);
  return buf;
}

std::string McpServer::guideText(const std::string& id) const {
  for (auto& g : mcpGuides()) {
    if (id != g.id) continue;
    if (*g.text) return g.text;
    // "parameters": written from the tools' schemas, so it says what the tools really take
    std::string out = "# Every setting\n\nEach tool's settings: type, range, default, and what they do.\n";
    json tools = lister_ ? lister_() : json::array();
    for (auto& t : tools) {
      json props = t.value("inputSchema", json::object()).value("properties", json::object());
      if (props.empty()) continue;
      out += "\n## " + t.value("name", std::string()) + "\n" + t.value("description", std::string()) + "\n";
      for (auto& [k, v] : props.items()) {
        std::string line = "- `" + k + "` (" + v.value("type", std::string("any"));
        if (v.contains("enum")) {
          std::string e;
          for (auto& x : v["enum"]) e += (e.empty() ? "" : " | ") + x.get<std::string>();
          line += ": " + e;
        }
        if (v.contains("minimum") && v.contains("maximum"))
          line += ", " + compact(v["minimum"]) + " to " + compact(v["maximum"]);
        if (v.contains("default")) line += ", default " + compact(v["default"]);
        line += "): " + v.value("description", std::string());
        if (v.contains("properties"))
          for (auto& [k2, v2] : v["properties"].items()) {
            line += "\n  - `" + k2 + "`";
            if (v2.contains("minimum") && v2.contains("maximum"))
              line += " (" + compact(v2["minimum"]) + " to " + compact(v2["maximum"]) +
                      (v2.contains("default") ? ", default " + compact(v2["default"]) : std::string()) + ")";
            line += ": " + v2.value("description", std::string());
          }
        out += line + "\n";
      }
    }
    return out;
  }
  return "";
}

json McpServer::guideTool() {
  json topics = json::array();
  std::string list;
  for (auto& g : mcpGuides()) {
    topics.push_back(g.id);
    list += std::string(list.empty() ? "" : "; ") + g.id + ": " + g.description;
  }
  return {{"name", "patina_guide"},
          {"description", "How to use Patina's tools well: read a guide before a job. Topics: " + list},
          {"inputSchema", {{"type", "object"},
                           {"properties", {{"topic", {{"type", "string"}, {"enum", topics},
                                                      {"description", "Which guide (default workflow)"}}}}}}}};
}

json McpServer::toolResult(const json& resp) {
  json content = json::array();
  bool ok = resp.value("ok", false);
  if (!ok) {
    content.push_back({{"type", "text"}, {"text", "Error: " + resp.value("error", std::string("unknown error"))}});
    return {{"content", content}, {"isError", true}};
  }
  json result = resp.value("result", json());
  if (result.is_object() && result.contains("image_png_base64")) {
    content.push_back({{"type", "image"}, {"data", result["image_png_base64"]}, {"mimeType", "image/png"}});
    result.erase("image_png_base64");
  }
  std::string text = result.is_null() ? "ok" : result.dump(1);
  if (text.size() > 60000) text = text.substr(0, 60000) + "\n... (truncated)";
  content.push_back({{"type", "text"}, {"text", text}});
  return {{"content", content}, {"isError", false}};
}

json McpServer::handle(const json& msg) {
  if (!msg.is_object()) return {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32600}, {"message", "Invalid Request"}}}};
  std::string method = msg.value("method", std::string());
  bool isRequest = msg.contains("id") && !msg["id"].is_null();
  json id = msg.value("id", json());
  json params = msg.value("params", json::object());
  auto reply = [&](json result) { return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}; };
  auto error = [&](int code, const std::string& m) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", m}}}};
  };
  if (!isRequest) return nullptr;  // notifications (initialized, cancelled, ...)

  if (method == "initialize") {
    std::string requested = params.value("protocolVersion", std::string(kVersions[0]));
    std::string version = kVersions[0];
    for (auto v : kVersions)
      if (requested == v) version = v;
    return reply({{"protocolVersion", version},
                  {"capabilities", {{"tools", {{"listChanged", false}}},
                                    {"resources", {{"listChanged", false}}},
                                    {"prompts", {{"listChanged", false}}}}},
                  {"serverInfo", {{"name", "patina"}, {"title", "Patina Texture Painter"}, {"version", PATINA_VERSION}}},
                  {"instructions", instructions()}});
  }
  if (method == "ping") return reply(json::object());
  if (method == "tools/list") {
    json tools = lister_ ? lister_() : json::array();
    for (auto& t : extraTools) tools.push_back(t);
    tools.push_back(guideTool());
    return reply({{"tools", tools}});
  }
  if (method == "tools/call") {
    std::string name = params.value("name", std::string());
    json args = params.value("arguments", json::object());
    if (name == "patina_guide") {
      std::string topic = args.is_object() ? args.value("topic", std::string("workflow")) : std::string("workflow");
      std::string text = guideText(topic);
      if (text.empty()) return reply(toolResult({{"ok", false}, {"error", "no guide '" + topic + "'"}}));
      return reply({{"content", json::array({{{"type", "text"}, {"text", text}}})}, {"isError", false}});
    }
    if (localTool) {
      bool handled = false;
      json r = localTool(name, args, handled);
      if (handled) return reply(toolResult(r));
    }
    json resp;
    try {
      resp = backend_(name, args);
    } catch (std::exception& e) {
      resp = {{"ok", false}, {"error", e.what()}};
    }
    return reply(toolResult(resp));
  }
  if (method == "resources/list") {
    json list = json::array();
    for (auto& g : mcpGuides())
      list.push_back({{"uri", std::string("patina://guide/") + g.id}, {"name", g.id}, {"title", g.title},
                      {"description", g.description}, {"mimeType", "text/markdown"}});
    return reply({{"resources", list}});
  }
  if (method == "resources/templates/list") return reply({{"resourceTemplates", json::array()}});
  if (method == "resources/read") {
    std::string uri = params.value("uri", std::string());
    const std::string prefix = "patina://guide/";
    std::string text = uri.rfind(prefix, 0) == 0 ? guideText(uri.substr(prefix.size())) : "";
    if (text.empty()) return error(-32002, "Resource not found: " + uri);
    return reply({{"contents", json::array({{{"uri", uri}, {"mimeType", "text/markdown"}, {"text", text}}})}});
  }
  if (method == "prompts/list") {
    json list = json::array();
    for (auto& p : mcpPrompts()) {
      json args = json::array();
      for (auto& a : p.args) args.push_back({{"name", a.name}, {"description", a.description}, {"required", a.required}});
      list.push_back({{"name", p.name}, {"title", p.title}, {"description", p.description}, {"arguments", args}});
    }
    return reply({{"prompts", list}});
  }
  if (method == "prompts/get") {
    std::string name = params.value("name", std::string());
    for (auto& p : mcpPrompts()) {
      if (name != p.name) continue;
      json args = params.value("arguments", json::object());
      for (auto& a : p.args)
        if (a.required && !(args.is_object() && args.contains(a.name)))
          return error(-32602, std::string("Missing argument: ") + a.name);
      std::string text = fillTemplate(p.text, args);
      return reply({{"description", p.description},
                    {"messages", json::array({{{"role", "user"}, {"content", {{"type", "text"}, {"text", text}}}}})}});
    }
    return error(-32602, "Unknown prompt: " + name);
  }
  return error(-32601, "Method not found: " + method);
}

void McpServer::run(std::istream& in, std::ostream& out) {
  std::string line;
  while (std::getline(in, line)) {
    if (line.find_first_not_of(" \r\t") == std::string::npos) continue;
    json msg, resp;
    try {
      msg = json::parse(line);
    } catch (std::exception& e) {
      resp = {{"jsonrpc", "2.0"}, {"id", nullptr}, {"error", {{"code", -32700}, {"message", "Parse error"}}}};
    }
    if (resp.is_null()) {
      if (msg.is_array()) {  // batch (older protocol revisions)
        json arr = json::array();
        for (auto& m : msg) {
          json r = handle(m);
          if (!r.is_null()) arr.push_back(r);
        }
        if (!arr.empty()) resp = arr;
      } else {
        resp = handle(msg);
      }
    }
    if (!resp.is_null()) {
      std::lock_guard<std::mutex> lk(outMutex_);
      out << resp.dump(-1, ' ', false, json::error_handler_t::replace) << "\n";
      out.flush();
    }
  }
}

}  // namespace patina
