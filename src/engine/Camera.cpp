#include "engine/Camera.h"

namespace patina {

vec3 Camera::worldForward() const {
  return -vec3(std::cos(pitch) * std::sin(yaw), std::sin(pitch), std::cos(pitch) * std::cos(yaw));
}

mat4 Camera::worldView() const {
  vec3 up = std::abs(std::cos(pitch)) < 1e-4f ? vec3(0, 0, pitch > 0 ? -1 : 1) : vec3(0, 1, 0);
  return glm::lookAt(worldEye(), target, up);
}

Camera Camera::inSpace(const mat4& m) const {
  Camera c = *this;
  c.space = m;
  c.spaceInv = glm::inverse(m);
  c.spaceScale = std::max(std::cbrt(std::abs(glm::determinant(mat3(m)))), 1e-12f);
  return c;
}

vec3 Camera::forward() const {
  vec3 f = worldForward();
  return spaceScale == 1.f && space == mat4(1) ? f : glm::normalize(mat3(spaceInv) * f);
}

vec3 Camera::eye() const { return vec3(spaceInv * vec4(worldEye(), 1.f)); }

mat4 Camera::view() const { return worldView() * space; }

mat4 Camera::proj(float aspect) const {
  if (ortho) {
    float h = distance * std::tan(fovY * 0.5f);
    return glm::ortho(-h * aspect, h * aspect, -h, h, -farPlane, farPlane);
  }
  return glm::perspective(fovY, aspect, nearPlane, farPlane);
}

void Camera::ray(vec2 px, vec2 vp, vec3& origin, vec3& dir) const {
  float aspect = vp.x / std::max(vp.y, 1.f);
  vec2 ndc((px.x / vp.x) * 2.f - 1.f, 1.f - (px.y / vp.y) * 2.f);
  mat4 inv = glm::inverse(viewProj(aspect));
  vec4 a = inv * vec4(ndc, -1.f, 1.f);
  vec4 b = inv * vec4(ndc, 1.f, 1.f);
  origin = vec3(a) / a.w;
  dir = glm::normalize(vec3(b) / b.w - origin);
}

float Camera::pixelWorld(float vh) const {
  float h = 2.f * std::tan(fovY * 0.5f) / std::max(vh, 1.f);
  return ortho ? h * distance / spaceScale : h;  // (perspective: per unit of distance, which scales the same)
}

void Camera::orbit(float dy, float dp) {
  yaw += dy;
  pitch = std::clamp(pitch + dp, -1.55f, 1.55f);
}

void Camera::pan(vec2 d, vec2 vp) {
  float scale = 2.f * distance * std::tan(fovY * 0.5f) / std::max(vp.y, 1.f);
  mat4 v = worldView();
  vec3 right(v[0][0], v[1][0], v[2][0]);
  vec3 up(v[0][1], v[1][1], v[2][1]);
  target += (-right * d.x + up * d.y) * scale;
}

void Camera::zoom(float f) { distance = std::clamp(distance * f, 1e-3f, 1e5f); }

void Camera::frame(const AABB& b, float radius) {
  if (!b.valid()) return;
  target = b.center();
  float r = std::max(radius > 0 ? radius : b.radius(), 1e-3f);
  distance = r / std::sin(fovY * 0.5f) * 1.12f;
  updateClip(b);
}

void Camera::updateClip(const AABB& b) {
  float r = b.valid() ? std::max(b.radius(), 1e-3f) : 1.f;
  nearPlane = std::max(distance - r * 4.f, std::max(r * 0.001f, distance * 0.002f));
  nearPlane = std::min(nearPlane, distance * 0.5f);
  farPlane = distance + r * 4.f;
}

bool Camera::viewAngles(const std::string& n, float& y, float& p) {
  if (n == "front") y = 0, p = 0;
  else if (n == "back") y = kPi, p = 0;
  else if (n == "right") y = kPi * 0.5f, p = 0;
  else if (n == "left") y = -kPi * 0.5f, p = 0;
  else if (n == "top") y = 0, p = 1.5699f;
  else if (n == "bottom") y = 0, p = -1.5699f;
  else return false;
  return true;
}

void Camera::setView(const std::string& n) {
  if (!viewAngles(n, yaw, pitch) && n == "persp") yaw = 0.6f, pitch = 0.35f;
}

nlohmann::json Camera::toJson() const {
  return {{"target", {target.x, target.y, target.z}}, {"yaw", yaw}, {"pitch", pitch},
          {"distance", distance}, {"fov", fovY}, {"ortho", ortho}};
}

void Camera::fromJson(const nlohmann::json& j) {
  if (j.contains("target")) target = {j["target"][0], j["target"][1], j["target"][2]};
  yaw = j.value("yaw", yaw);
  pitch = std::clamp(j.value("pitch", pitch), -1.5699f, 1.5699f);
  distance = j.value("distance", distance);
  fovY = j.value("fov", fovY);
  ortho = j.value("ortho", ortho);
}

}  // namespace patina
