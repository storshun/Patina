#pragma once
#include <string>
#include <vector>

namespace patina {

// (0.34) Guides an agent can read (MCP resources patina://guide/<id>, and the patina_guide tool) and prompts for
// common jobs (MCP prompts). The "parameters" guide is written from the tools' own schemas, so it stays true.
struct McpGuide {
  const char* id;
  const char* title;
  const char* description;
  const char* text;  // Markdown; empty for the generated one
};
const std::vector<McpGuide>& mcpGuides();

struct McpPromptArg {
  const char* name;
  const char* description;
  bool required;
};
struct McpPrompt {
  const char* name;
  const char* title;
  const char* description;
  std::vector<McpPromptArg> args;
  const char* text;  // {{arg}} is replaced by the argument (or "" when not given)
};
const std::vector<McpPrompt>& mcpPrompts();

}  // namespace patina
