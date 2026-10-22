// (0.39) The path tracer's picture (engine/PathTracer.cpp): the samples so far, the model's light tone-mapped as the
// raster view's, over the background the raster view draws, the ground's shadow darkening it.
#include "common.glsl"
in vec2 vUV;
out vec4 oColor;
uniform sampler2D uAccumA;  // rgb: the model's light, a: samples
uniform sampler2D uAccumB;  // ground: light reaching it, its weight, its samples; a: model samples
uniform mat4 uInvViewProjBg;
uniform samplerCube uEnvSpec;
uniform mat3 uEnvRot;
uniform float uBlurLod;
uniform float uExposure;
uniform float uEnvIntensity;
uniform int uMode;  // 0 the environment, 1 a gradient
uniform vec3 uTop;
uniform vec3 uBottom;
uniform int uTransparent;
vec3 acesFilm(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 A = texelFetch(uAccumA, p, 0), B = texelFetch(uAccumB, p, 0);
  float n = max(A.a, 1.0);
  // the background, as skybox.fs.glsl draws it
  vec3 bgLin = vec3(0.0), bg;
  if (uMode == 1) {
    bg = mix(uBottom, uTop, vUV.y);
  } else {
    vec4 f = uInvViewProjBg * vec4(vUV * 2.0 - 1.0, 1.0, 1.0), o = uInvViewProjBg * vec4(vUV * 2.0 - 1.0, -1.0, 1.0);
    vec3 d = normalize(f.xyz / f.w - o.xyz / o.w);
    bgLin = textureLod(uEnvSpec, uEnvRot * d, uBlurLod).rgb * uEnvIntensity * 0.9 + vec3(0.035, 0.037, 0.042);
    bg = linearToSrgb(acesFilm(bgLin * uExposure));
  }
  float ratio = B.y > 0.0 ? clamp(B.x / B.y, 0.0, 1.0) : 1.0;
  vec3 shadow = uMode == 1 ? bg * ratio : linearToSrgb(acesFilm(bgLin * ratio * uExposure));
  vec3 model = B.w > 0.0 ? linearToSrgb(acesFilm(A.rgb / B.w * uExposure)) : vec3(0.0);
  float fm = B.w / n, fg = B.z / n, fb = max(0.0, 1.0 - fm - fg);
  if (uTransparent == 1) {
    // the model over nothing, the ground's shadow as black that lets the rest through
    float alpha = clamp(fm + fg * (1.0 - ratio), 0.0, 1.0);
    oColor = vec4(alpha > 0.0 ? model * fm / alpha : vec3(0.0), alpha);
  } else {
    oColor = vec4(model * fm + shadow * fg + bg * fb, 1.0);
  }
}
