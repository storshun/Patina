// Viewport background: blurred environment or flat gradient.
#include "common.glsl"
in vec2 vUV;
out vec4 oColor;
uniform mat4 uInvViewProj;
uniform samplerCube uEnvSpec;
uniform mat3 uEnvRot;
uniform float uBlurLod;
uniform float uExposure;
uniform float uEnvIntensity;
uniform int uMode;  // 0 env, 1 gradient
uniform vec3 uTop;
uniform vec3 uBottom;
vec3 acesFilm(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
void main() {
  if (uMode == 1) {
    oColor = vec4(mix(uBottom, uTop, vUV.y), 1.0);
    return;
  }
  vec4 p = uInvViewProj * vec4(vUV * 2.0 - 1.0, 1.0, 1.0);
  vec4 o = uInvViewProj * vec4(vUV * 2.0 - 1.0, -1.0, 1.0);
  vec3 d = normalize(p.xyz / p.w - o.xyz / o.w);
  vec3 c = textureLod(uEnvSpec, uEnvRot * d, uBlurLod).rgb * uEnvIntensity * 0.9 + vec3(0.035, 0.037, 0.042);
  oColor = vec4(linearToSrgb(acesFilm(c * uExposure)), 1.0);
}
