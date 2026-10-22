#pragma once
#include <string>
#include <vector>

#include "core/Container.h"
#include "core/Mesh.h"

namespace patina {

struct MeshImportOptions {
  bool weldVertices = true;  // glTF, FBX: merge vertices with identical positions
  float scale = 1.f;
};

// Loads .obj, .gltf, .glb, .fbx, .stl, .3mf, .ply or .ptmesh based on the file extension.
bool loadMesh(const std::string& path, Mesh& out, std::string* err = nullptr,
              const MeshImportOptions& opt = {});

bool loadObj(const std::string& path, Mesh& out, std::string* err);
bool loadGltf(const std::string& path, Mesh& out, std::string* err, const MeshImportOptions& opt);
// FBX (binary and ASCII, 6.x - 7.7): all meshes in the scene, in meters, Y up
bool loadFbx(const std::string& path, Mesh& out, std::string* err);
// STL (binary / ASCII) and 3MF: read as 3D-printing files (Z up, millimeters unless the 3MF says otherwise)
bool loadStl(const std::string& path, Mesh& out, std::string* err);
bool load3mf(const std::string& path, Mesh& out, std::string* err);
// PLY (ASCII and binary): faces, UVs (per vertex or MeshLab's per-face texcoord), normals and vertex colors (sRGB);
// Z up, as Blender reads it
bool loadPly(const std::string& path, Mesh& out, std::string* err);
// (1.2) The meshes of a file with several (Mesh::objectNames) apart: a mesh of its own for each, finalized, with the
// faces' UVs, normals, colors and material slots; `names` receives their names. Empty objects are left out.
void splitObjects(const Mesh& m, std::vector<Mesh>& parts, std::vector<std::string>& names);
// file extensions loadMesh reads
const std::vector<std::string>& meshExtensions();

// Patina bridge format (written by the Blender add-on). Blender is Z-up; the
// mesh is converted to Patina's Y-up on load. `blenderObject` receives the name.
bool loadPtMesh(const std::string& path, Mesh& out, std::string* err);
bool savePtMesh(const std::string& path, const Mesh& m, std::string* err);
bool decodePtMesh(const container::File& file, Mesh& out, std::string* err);
std::string encodePtMesh(const Mesh& m, bool blenderAxes);
// false when the Blender object had no UV layer (header "had_uvs": false)
bool hadUVs(const std::string& ptmeshPath);

// Per-loop UVs + edge seams, returned to Blender after UV editing.
// withSlots: the faces' material slots and the slots' names go too (texture sets made in Patina).
bool savePtUV(const std::string& path, const Mesh& m, std::string* err, bool withSlots = false);
bool loadPtUV(const std::string& path, Mesh& m, std::string* err);

bool saveObj(const std::string& path, const Mesh& m, std::string* err);

}  // namespace patina
