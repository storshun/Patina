#pragma once
// (0.44) Which saved project a Blender object was painted in. A project linked to an object (Send to Patina, then
// saved) is remembered when it is saved or opened, so when Blender sends that object again to a Patina that
// doesn't have it open (after a restart), Patina opens that project and keeps its layers instead of starting
// over. Kept in the config folder (blender_projects.json), newest first.
#include <string>
#include <vector>

#include "core/Mesh.h"

namespace patina::bridgeprojects {

// Remembers `project` as the latest project of the Blender object `object`.
void remember(const std::string& object, const std::string& project);
// The projects remembered for `object`, newest first (they may have been moved or deleted since).
std::vector<std::string> remembered(const std::string& object);
// The Blender object a project file is linked to (its header), or empty.
std::string objectOf(const std::string& project);
// (1.2) Whether any object of a project file (a scene) is linked to `object`
bool holds(const std::string& project, const std::string& object);
// The projects worth trying for `object`: the remembered ones and `others` (the window's recent files) that
// exist and are linked to it, the most recently saved first.
std::vector<std::string> candidates(const std::string& object, const std::vector<std::string>& others);
// The sent mesh is the model a project holds (not another object that happens to have the same name): the same
// faces and corners, or about the same place and size (it was remodeled).
bool sameModel(const Mesh& a, const Mesh& b);

}  // namespace patina::bridgeprojects
