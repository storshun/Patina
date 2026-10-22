#pragma once
// Patina Assistant (0.11): plans. A plan is what the model writes for a request:
//   {"say": "what it does, in a sentence", "steps": [{"cmd": "...", "args": {...}}, ...]}
// or {"ask": "a question back"} / {"say": "why it can't"} with no steps, either of them with
// "suggest": ["a request it can do", ...] (0.12). Steps may use the
// result of an earlier step: "$0.index" is step 0's result field "index". Plans run through
// the same command layer as the console, MCP and macros, as one undo step.
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace patina {

class Engine;
class CommandRegistry;

namespace assistant {

using json = nlohmann::json;

constexpr int kMaxSteps = 16;
constexpr int kLayerTag = 5;  // the color tag of layers the Assistant makes

// The model files Patina was made with (Phase 3 of the Assistant: size and SHA-256 of each
// published file). A file not listed still works; the panel says it is not one it knows.
struct KnownModel {
  const char* name;
  const char* sha256;
  unsigned long long bytes;
};
const std::vector<KnownModel>& knownModels();

// The commands a plan may use (no files, saving, export, the Blender bridge, features, macros,
// or anything that clears the undo history).
const std::vector<std::string>& allowedCommands();
bool isAllowed(const std::string& cmd);

// Steps that remove or clear something, or rebuild UVs: the panel warns before they run.
bool isDestructive(const std::string& cmd, const json& args);

// Steps after which Patina clears the undo history (a new texture set, the resolution, the texture
// set layout). A plan may have one, as its last step; it can't be previewed or undone, so the panel
// asks before it runs.
bool clearsHistory(const std::string& cmd);
bool planClearsHistory(const json& plan);  // its last step does

// A plan may also suggest a few requests it can do ("suggest": [...], at most kMaxSuggest), when
// what was asked is not something Patina does: the panel shows them as buttons.
constexpr int kMaxSuggest = 4;

// A JSON-schema check of the subset the command schemas use: type, enum, minimum / maximum,
// items, minItems / maxItems, properties, required, additionalProperties. A "$n.field"
// reference stands for any value. Returns "" when the value fits.
std::string checkSchema(const json& schema, const json& value, const std::string& where = "args");

struct PlanCheck {
  bool ok = false;
  std::string error;
  std::vector<std::string> warnings;  // plain words, for the panel
};
// Shape, allowlist, argument schemas, references (to an earlier step, in the right form) and
// the step limit.
PlanCheck checkPlan(const CommandRegistry& commands, const json& plan);

// Runs a checked plan as one undo step named `label`. A step that fails stops the plan and
// undoes what already ran (then throws CommandError naming the step). Returns
// {"results": [...], "warnings": [...], "say": ...}.
json applyPlan(Engine& e, const json& plan, const std::string& label);

// The GBNF grammar the model's output is held to: exactly the plan's shape, written the one
// way training writes it (say | ask, then steps; cmd then args; argument keys in alphabetical
// order, no spaces), only allowlisted commands with their own arguments, and, from the open
// project, the part ids, layer indices, library materials, smart materials and brushes there are.
std::string buildGrammar(Engine& e);

// A step in plain words, for the plan list in the panel ("Add a tint layer 'Skin crease
// redness' on Skin, in the creases").
// `materialName` turns a library id into the name the user sees ("Scratched Steel").
std::string describeStep(const json& step, const std::function<std::string(const std::string&)>& materialName = {});

}  // namespace assistant
}  // namespace patina
