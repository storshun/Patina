#pragma once
#include <nlohmann/json.hpp>

#include "core/Math.h"

namespace patina {

// Orbit camera around a target point. Y is up.
struct Camera {
  vec3 target{0.f};
  float yaw = 0.6f;     // radians around Y
  float pitch = 0.35f;  // radians
  float distance = 3.f;
  float fovY = 0.7f;    // radians
  bool ortho = false;
  float nearPlane = 0.01f;
  float farPlane = 100.f;
  // (1.2) The space the camera's eye, direction, matrices and rays are given in: the object a scene object is drawn,
  // painted and picked in (its object -> world matrix), or the world (identity). target, distance and the turns stay
  // in the world. Set with inSpace() on a copy; the 3D view's own camera stays in the world.
  mat4 space{1}, spaceInv{1};
  float spaceScale = 1.f;  // how much bigger one object unit is than one world unit (its scale)
  Camera inSpace(const mat4& objectToWorld) const;
  float eyeDistance() const { return distance / spaceScale; }  // from the eye to the target, in the camera's space

  vec3 forward() const;  // from eye toward target
  vec3 eye() const;
  mat4 view() const;
  mat4 proj(float aspect) const;
  mat4 viewProj(float aspect) const { return proj(aspect) * view(); }
  // Ray through a viewport pixel (pixel coords, origin top-left)
  void ray(vec2 px, vec2 viewport, vec3& origin, vec3& dir) const;
  // World size covered by one pixel at unit distance (perspective) or absolute (ortho)
  float pixelWorld(float viewportHeight) const;

  void orbit(float dYaw, float dPitch);
  void pan(vec2 deltaPx, vec2 viewport);
  void zoom(float factor);
  void frame(const AABB& box, float radius = -1.f);  // radius: bounding sphere (default box diagonal / 2)
  void updateClip(const AABB& box);
  void setView(const std::string& name);  // front back left right top bottom persp
  // (0.38) the yaw and pitch of a side, top or bottom view (front back left right top bottom); false for others
  static bool viewAngles(const std::string& name, float& yaw, float& pitch);

  nlohmann::json toJson() const;  // (the world camera: space is not kept)
  void fromJson(const nlohmann::json& j);

 private:
  vec3 worldForward() const;
  vec3 worldEye() const { return target - worldForward() * distance; }
  mat4 worldView() const;
};

}  // namespace patina
