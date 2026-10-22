#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "core/Math.h"

namespace patina {

// A comment pinned to a point on the model, saved in the project (review notes,
// to-dos, hand-over remarks). `view` keeps the camera it was written from.
struct Note {
  uint32_t id = 0;
  vec3 position{0.f};  // object space, on the surface
  vec3 normal{0.f, 1.f, 0.f};
  std::string text;
  std::string author;
  int64_t created = 0;  // unix seconds
  bool resolved = false;
  nlohmann::json view;  // Camera::toJson() when written (may be null)

  nlohmann::json toJson() const {
    return {{"id", id},
            {"position", {position.x, position.y, position.z}},
            {"normal", {normal.x, normal.y, normal.z}},
            {"text", text},
            {"author", author},
            {"created", created},
            {"resolved", resolved},
            {"view", view}};
  }
  static Note fromJson(const nlohmann::json& j) {
    Note n;
    n.id = j.value("id", 0u);
    auto v3 = [&](const char* k, vec3 d) {
      if (!j.contains(k) || !j[k].is_array() || j[k].size() < 3) return d;
      return vec3(j[k][0].get<float>(), j[k][1].get<float>(), j[k][2].get<float>());
    };
    n.position = v3("position", n.position);
    n.normal = v3("normal", n.normal);
    n.text = j.value("text", std::string());
    n.author = j.value("author", std::string());
    n.created = j.value("created", (int64_t)0);
    n.resolved = j.value("resolved", false);
    if (j.contains("view")) n.view = j["view"];
    return n;
  }
};

}  // namespace patina
