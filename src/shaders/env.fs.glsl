// Environment map passes. One cube face per draw (uFace 0..5).
#include "common.glsl"

in vec2 vUV;
out vec4 oColor;
uniform int uFace;

vec3 faceDir(int face, vec2 uv) {
  float sc = uv.x * 2.0 - 1.0, tc = uv.y * 2.0 - 1.0;
  if (face == 0) return normalize(vec3(1.0, -tc, -sc));
  if (face == 1) return normalize(vec3(-1.0, -tc, sc));
  if (face == 2) return normalize(vec3(sc, 1.0, tc));
  if (face == 3) return normalize(vec3(sc, -1.0, -tc));
  if (face == 4) return normalize(vec3(sc, -tc, 1.0));
  return normalize(vec3(-sc, -tc, -1.0));
}

#ifdef ENV_STUDIO
// Procedural photo-studio lighting: soft top box, key and rim strips, gray floor.
float softRect(vec3 d, vec3 center, vec3 axisU, vec3 axisV, vec2 halfSize, float soft) {
  float f = dot(d, center);
  if (f <= 0.0) return 0.0;
  vec3 p = d / f - center;
  vec2 q = vec2(dot(p, axisU), dot(p, axisV));
  vec2 e = abs(q) - halfSize;
  float dist = length(max(e, 0.0)) + min(max(e.x, e.y), 0.0);
  return 1.0 - smoothstep(-soft, soft, dist);
}
void main() {
  vec3 d = faceDir(uFace, vUV);
  float y = d.y;
  vec3 sky = mix(vec3(0.07, 0.075, 0.085), vec3(0.025, 0.027, 0.03), smoothstep(-0.05, 0.9, y));
  vec3 floorC = vec3(0.045, 0.043, 0.04) * (0.7 + 0.3 * smoothstep(-1.0, -0.2, y));
  vec3 c = y < -0.02 ? floorC : sky;
  c = mix(floorC, c, smoothstep(-0.04, 0.02, y));
  // big soft top light
  c += vec3(1.8) * softRect(d, vec3(0, 1, 0), vec3(1, 0, 0), vec3(0, 0, 1), vec2(0.6, 0.45), 0.25);
  // key light (warm) front-left
  vec3 kc = normalize(vec3(-0.7, 0.55, 0.45));
  vec3 ku = normalize(cross(vec3(0, 1, 0), kc));
  vec3 kv = cross(kc, ku);
  c += vec3(9.0, 8.2, 7.0) * softRect(d, kc, ku, kv, vec2(0.28, 0.38), 0.12);
  // rim light (cool) back-right strip
  vec3 rc = normalize(vec3(0.75, 0.3, -0.6));
  vec3 ru = normalize(cross(vec3(0, 1, 0), rc));
  vec3 rv = cross(rc, ru);
  c += vec3(3.0, 3.6, 4.4) * softRect(d, rc, ru, rv, vec2(0.08, 0.6), 0.06);
  // fill (right)
  vec3 fc = normalize(vec3(0.95, 0.05, 0.3));
  vec3 fu = normalize(cross(vec3(0, 1, 0), fc));
  vec3 fv = cross(fc, fu);
  c += vec3(0.45, 0.5, 0.6) * softRect(d, fc, fu, fv, vec2(0.5, 0.4), 0.3);
  oColor = vec4(c, 1.0);
}
#endif

#ifdef ENV_EQUIRECT
uniform sampler2D uEquirect;
void main() {
  vec3 d = faceDir(uFace, vUV);
  vec2 uv = vec2(atan(d.z, d.x) / (2.0 * PI) + 0.5, acos(clamp(d.y, -1.0, 1.0)) / PI);
  oColor = vec4(textureLod(uEquirect, vec2(uv.x, 1.0 - uv.y), 0.0).rgb, 1.0);
}
#endif

#ifdef ENV_PREFILTER
uniform samplerCube uSource;
uniform float uRoughness;
uniform float uSourceSize;
float radicalInverse(uint b) {
  b = (b << 16u) | (b >> 16u);
  b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
  b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
  b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
  b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
  return float(b) * 2.3283064365386963e-10;
}
void main() {
  vec3 N = faceDir(uFace, vUV);
  vec3 V = N;
  float a = uRoughness * uRoughness;
  vec3 up = abs(N.z) < 0.999 ? vec3(0, 0, 1) : vec3(1, 0, 0);
  vec3 tx = normalize(cross(up, N));
  vec3 ty = cross(N, tx);
  const uint COUNT = 96u;
  vec3 sum = vec3(0);
  float w = 0.0;
  for (uint i = 0u; i < COUNT; ++i) {
    vec2 xi = vec2(float(i) / float(COUNT), radicalInverse(i));
    float phi = 2.0 * PI * xi.x;
    float cosT = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    vec3 H = normalize(tx * cos(phi) * sinT + ty * sin(phi) * sinT + N * cosT);
    vec3 L = normalize(2.0 * dot(V, H) * H - V);
    float ndl = dot(N, L);
    if (ndl > 0.0) {
      float ndh = max(dot(N, H), 0.0);
      float d = (a * a) / (PI * pow(ndh * ndh * (a * a - 1.0) + 1.0, 2.0));
      float pdf = d * ndh / (4.0 * ndh) + 1e-4;
      float saTexel = 4.0 * PI / (6.0 * uSourceSize * uSourceSize);
      float saSample = 1.0 / (float(COUNT) * pdf + 1e-4);
      float lod = uRoughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel) + 1.0;
      sum += textureLod(uSource, L, lod).rgb * ndl;
      w += ndl;
    }
  }
  oColor = vec4(sum / max(w, 1e-4), 1.0);
}
#endif

#ifdef ENV_BRDF
float radicalInverse2(uint b) {
  b = (b << 16u) | (b >> 16u);
  b = ((b & 0x55555555u) << 1u) | ((b & 0xAAAAAAAAu) >> 1u);
  b = ((b & 0x33333333u) << 2u) | ((b & 0xCCCCCCCCu) >> 2u);
  b = ((b & 0x0F0F0F0Fu) << 4u) | ((b & 0xF0F0F0F0u) >> 4u);
  b = ((b & 0x00FF00FFu) << 8u) | ((b & 0xFF00FF00u) >> 8u);
  return float(b) * 2.3283064365386963e-10;
}
void main() {
  float ndv = max(vUV.x, 1e-3), rough = vUV.y;
  vec3 V = vec3(sqrt(1.0 - ndv * ndv), 0.0, ndv);
  float a = rough * rough;
  float A = 0.0, B = 0.0;
  const uint COUNT = 256u;
  for (uint i = 0u; i < COUNT; ++i) {
    vec2 xi = vec2(float(i) / float(COUNT), radicalInverse2(i));
    float phi = 2.0 * PI * xi.x;
    float cosT = sqrt((1.0 - xi.y) / (1.0 + (a * a - 1.0) * xi.y));
    float sinT = sqrt(1.0 - cosT * cosT);
    vec3 H = vec3(cos(phi) * sinT, sin(phi) * sinT, cosT);
    vec3 L = normalize(2.0 * dot(V, H) * H - V);
    float ndl = max(L.z, 0.0), ndh = max(H.z, 0.0), vdh = max(dot(V, H), 0.0);
    if (ndl > 0.0) {
      float k = a / 2.0;
      float g = (ndv / (ndv * (1.0 - k) + k)) * (ndl / (ndl * (1.0 - k) + k));
      float gv = g * vdh / (ndh * ndv);
      float fc = pow(1.0 - vdh, 5.0);
      A += (1.0 - fc) * gv;
      B += fc * gv;
    }
  }
  oColor = vec4(A / float(COUNT), B / float(COUNT), 0.0, 1.0);
}
#endif
