#pragma once
// Patina Assistant (0.11): the short text description of the open project that the model reads
// before it writes a plan. Kept to about 1,500 tokens: the mesh, its parts, the texture layout,
// the layer stack and a shortlist of library materials that fit the request.
#include <string>

#include <nlohmann/json.hpp>

namespace patina {

class Engine;

struct AssistantContextOptions {
  int maxMaterials = 20;
  int maxLayers = 24;
  int maxParts = 24;
};

// The fixed system prompt (the same text in training and in Patina).
const char* assistantSystemPrompt();

// {"text": the description, "parts": [{id, label, faces}], "materials": [ids], "tokens": estimate,
//  "system": the system prompt}
nlohmann::json buildAssistantContext(Engine& e, const std::string& request, const AssistantContextOptions& opt = {});

}  // namespace patina
