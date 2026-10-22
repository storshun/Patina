// Material preview: analytic sphere, tri-planar material, environment lighting.
#include "common.glsl"
in vec2 vUV;
out vec4 oColor;
uniform sampler2D uColorTex;
uniform sampler2D uRMHTex;
uniform sampler2D uNormalTex;
uniform sampler2D uEmissiveTex;
uniform sampler2D uExtraTex;  // opacity, sheen, clearcoat, subsurface
uniform samplerCube uEnvSpec;
uniform sampler2D uBrdfLut;
uniform vec4 uSH[9];
uniform float uEnvMips;
uniform float uTiles;
uniform int uShape;  // 0 sphere, 1 flat swatch
vec3 irradianceSH(vec3 n) {
  float x = n.x, y = n.y, z = n.z;
  vec3 r = uSH[0].rgb * 0.886227 + uSH[1].rgb * 1.023328 * y + uSH[2].rgb * 1.023328 * z +
           uSH[3].rgb * 1.023328 * x + uSH[4].rgb * 0.858086 * x * y + uSH[5].rgb * 0.858086 * y * z +
           uSH[6].rgb * 0.247708 * (3.0 * z * z - 1.0) + uSH[7].rgb * 0.858086 * x * z +
           uSH[8].rgb * 0.429043 * (x * x - y * y);
  return max(r, vec3(0.0)) / PI;
}
vec3 acesFilm(vec3 x) { return clamp((x * (2.51 * x + 0.03)) / (x * (2.43 * x + 0.59) + 0.14), 0.0, 1.0); }
void main() {
  vec2 p = vUV * 2.0 - 1.0;
  vec3 N, T, B;
  vec2 uv;
  if (uShape == 1) {
    N = vec3(0, 0, 1);
    T = vec3(1, 0, 0);
    B = vec3(0, 1, 0);
    uv = vUV * uTiles;
  } else {
    float r2 = dot(p, p);
    if (r2 > 1.0) {
      float a = 1.0 - smoothstep(1.0, 1.03, sqrt(r2));
      oColor = vec4(0.0, 0.0, 0.0, 0.0);
      if (a <= 0.0) return;
    }
    N = normalize(vec3(p, sqrt(max(1.0 - r2, 0.0))));
    // spherical uv
    uv = vec2(atan(N.x, N.z) / (2.0 * PI) + 0.5, acos(-N.y) / PI) * vec2(2.0, 1.0) * uTiles * 0.5;
    T = normalize(vec3(N.z, 0.0, -N.x) + 1e-5);
    B = cross(N, T);
  }
  vec3 base = srgbToLinear(texture(uColorTex, uv).rgb);
  vec4 rmh = texture(uRMHTex, uv);
  vec3 tn = decodeNormal(texture(uNormalTex, uv).rgb);
  vec3 n = normalize(T * tn.x + B * tn.y + N * tn.z);
  vec3 V = vec3(0, 0, 1);
  float rough = clamp(rmh.r, 0.03, 1.0), metal = rmh.g;
  float ndv = max(dot(n, V), 1e-4);
  vec3 F0 = mix(vec3(0.04), base, metal);
  vec2 brdf = texture(uBrdfLut, vec2(ndv, rough)).rg;
  vec3 F = F0 + (max(vec3(1.0 - rough), F0) - F0) * pow(1.0 - ndv, 5.0);
  vec3 R = reflect(-V, n);
  // rotate env so the key light reads well on the thumbnail
  vec3 Rr = vec3(R.x, R.y, R.z);
  vec3 spec = textureLod(uEnvSpec, Rr, rough * uEnvMips).rgb * (F0 * brdf.x + brdf.y);
  vec3 diff = irradianceSH(n) * base * (1.0 - F) * (1.0 - metal);
  vec4 xt = texture(uExtraTex, uv);
  if (xt.a > 0.001) diff = mix(diff, irradianceSH(normalize(mix(n, N, 0.75))) * base * (1.0 - metal) * vec3(1.05, 0.97, 0.93), xt.a);
  if (xt.g > 0.001) spec += xt.g * irradianceSH(n) * (0.02 + 0.4 * pow(1.0 - ndv, 4.0));
  if (xt.b > 0.001) {
    float ndvc = max(dot(N, V), 1e-4);
    vec2 bc = texture(uBrdfLut, vec2(ndvc, 0.04)).rg;
    float Fc = 0.04 + 0.96 * pow(1.0 - ndvc, 5.0);
    vec3 coat = textureLod(uEnvSpec, reflect(-V, N), 0.04 * uEnvMips).rgb * (0.04 * bc.x + bc.y);
    diff *= 1.0 - Fc * xt.b;
    spec = spec * (1.0 - Fc * xt.b) + coat * xt.b;
  }
  vec3 c = diff + spec + srgbToLinear(texture(uEmissiveTex, uv).rgb) * 4.0;
  float alpha = (uShape == 1 ? 1.0 : 1.0 - smoothstep(0.985, 1.0, length(p))) * xt.r;
  oColor = vec4(linearToSrgb(acesFilm(c * 1.1)), alpha);
}
