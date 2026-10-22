#pragma once
#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace patina {

// Switches for newer features (Preferences > Features): each can be turned off if it doesn't
// suit the way you work. Saved with the session (GUI); headless and batch runs use the defaults.
struct FeatureInfo {
  const char* id;
  const char* name;
  const char* description;
  bool defaultOn;
  const char* since;  // version it arrived in
};

class Features {
 public:
  static const std::vector<FeatureInfo>& all();
  static const FeatureInfo* find(const std::string& id);
  bool on(const std::string& id) const;
  bool set(const std::string& id, bool value);  // false: unknown feature
  void reset() { state_.clear(); }
  nlohmann::json toJson() const;   // only the switches that differ from their defaults
  void fromJson(const nlohmann::json& j);

 private:
  std::map<std::string, bool> state_;
};

}  // namespace patina
