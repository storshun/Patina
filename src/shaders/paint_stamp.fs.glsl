// Accumulates brush dabs into the per-stroke coverage mask (texture space).
// Blend state: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_COLOR) -> a' = s + a(1-s)
#include "common.glsl"

in vec3 vPos;
in vec3 vNormal;
in vec4 vTangent;
in vec2 vUV;
in vec4 vAux;
flat in uint vFlags;
flat in uint vFace;

out vec4 oMask;

#define MAX_STAMPS 64
uniform int uMode;            // 0 screen projection, 1 3D surface sphere, 2 UV space, 3 face-flag fill
uniform int uStampCount;
uniform vec4 uStampA[MAX_STAMPS];  // screen/uv: xy center, z radius, w angle | surface: xyz center, w radius
uniform vec4 uStampB[MAX_STAMPS];  // x flow, y roundness, z angle (surface), w unused
uniform vec4 uStampN[MAX_STAMPS];  // surface: brush normal xyz
uniform sampler2D uAlpha;
uniform float uHardness;      // 0 soft .. 1 hard
uniform int uAlphaIsRound;    // 1 = procedural round falloff only

// screen mode
uniform mat4 uViewProj;       // object -> clip of the painting camera
uniform vec3 uCamPos;         // object space
uniform vec2 uViewport;       // pixels
uniform sampler2D uDepth;     // viewport depth buffer of the painting camera
uniform float uNear;
uniform float uFar;
uniform int uOrtho;
uniform float uPixelWorld;    // world size of one pixel at distance 1 (persp) or absolute (ortho)
uniform int uBackfaceCull;
uniform int uOcclusion;       // 1 = only paint visible surfaces
uniform float uAngleFalloff;  // 0 = none; otherwise cos of the grazing limit
uniform uint uFillFlag;
uniform uint uLockFlag;       // != 0: only faces with this loop flag (topology / island lock)

float linearDepth(float d) {
  if (uOrtho == 1) return uNear + d * (uFar - uNear);
  float z = d * 2.0 - 1.0;
  return 2.0 * uNear * uFar / (uFar + uNear - z * (uFar - uNear));
}

float dab(vec2 d, float angle, float roundness) {
  // d: offset from the dab center in units of radius
  d = rot2(-angle) * d;
  d.y /= max(roundness, 0.01);
  float r = length(d);
  if (r >= 1.0) return 0.0;
  float edge = clamp(uHardness, 0.0, 0.999);
  float fall = 1.0 - smoothstep(edge, 1.0, r);
  if (uAlphaIsRound == 1) return fall;
  float a = texture(uAlpha, d * 0.5 + 0.5).r;
  return a * fall;
}

float accumulateScreen(vec2 sp) {
  float keep = 1.0;
  for (int i = 0; i < uStampCount; ++i) {
    vec4 A = uStampA[i];
    vec4 B = uStampB[i];
    vec2 d = (sp - A.xy) / A.z;
    if (dot(d, d) >= 1.0 / max(B.y * B.y, 1e-4) + 1.0) continue;
    keep *= 1.0 - clamp(dab(d, A.w, B.y) * B.x, 0.0, 1.0);
  }
  return 1.0 - keep;
}

void main() {
  if ((vFlags & 32u) != 0u) discard;  // (1.2) a hidden face set's faces take no paint
  if (uLockFlag != 0u && (vFlags & uLockFlag) == 0u) discard;
  float s = 0.0;
  if (uMode == 3) {
    s = (vFlags & uFillFlag) != 0u ? 1.0 : 0.0;
  } else if (uMode == 2) {
    float keep = 1.0;
    for (int i = 0; i < uStampCount; ++i) {
      vec4 A = uStampA[i];
      vec4 B = uStampB[i];
      vec2 d = (vUV - A.xy) / A.z;
      keep *= 1.0 - clamp(dab(d, A.w, B.y) * B.x, 0.0, 1.0);
    }
    s = 1.0 - keep;
  } else if (uMode == 1) {
    vec3 N = normalize(vNormal);
    float keep = 1.0;
    for (int i = 0; i < uStampCount; ++i) {
      vec4 A = uStampA[i];
      vec4 B = uStampB[i];
      vec3 bn = normalize(uStampN[i].xyz);
      vec3 off = vPos - A.xyz;
      float r = A.w;
      if (dot(off, off) > r * r) continue;
      float facing = dot(N, bn);
      if (facing <= 0.05) continue;
      // brush plane frame
      vec3 up = abs(bn.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0);
      vec3 tx = normalize(cross(up, bn));
      vec3 ty = cross(bn, tx);
      vec2 d = vec2(dot(off, tx), dot(off, ty)) / r;
      float a = dab(d, B.z, B.y) * smoothstep(0.05, 0.35, facing);
      keep *= 1.0 - clamp(a * B.x, 0.0, 1.0);
    }
    s = 1.0 - keep;
  } else {
    vec4 clip = uViewProj * vec4(vPos, 1.0);
    if (clip.w <= 0.0) discard;
    vec3 ndc = clip.xyz / clip.w;
    if (any(greaterThan(abs(ndc.xy), vec2(1.02)))) discard;
    vec2 sp = (ndc.xy * 0.5 + 0.5) * uViewport;
    // facing
    vec3 N = normalize(vNormal);
    vec3 V = uOrtho == 1 ? normalize(uCamPos) : normalize(uCamPos - vPos);
    float ndv = dot(N, V);
    if (uBackfaceCull == 1 && ndv <= 0.0) discard;
    float facingW = 1.0;
    if (uAngleFalloff > 0.0) facingW = smoothstep(uAngleFalloff, min(uAngleFalloff + 0.25, 1.0), abs(ndv));
    // visibility against the viewport depth buffer
    float sceneD = linearDepth(texture(uDepth, ndc.xy * 0.5 + 0.5).r);
    float selfD = linearDepth(ndc.z * 0.5 + 0.5);
    float footprint = (uOrtho == 1 ? uPixelWorld : uPixelWorld * selfD) * 3.0 / max(abs(ndv), 0.15);
    if (uOcclusion == 1 && selfD > sceneD + max(footprint, selfD * 0.002)) discard;
    // 2x2 supersampling inside the texel footprint for clean edges
    vec2 dx = dFdx(sp) * 0.25, dy = dFdy(sp) * 0.25;
    if (length(dx) + length(dy) > 4.0) {
      s = accumulateScreen(sp);
    } else {
      s = 0.25 * (accumulateScreen(sp + dx + dy) + accumulateScreen(sp - dx + dy) +
                  accumulateScreen(sp + dx - dy) + accumulateScreen(sp - dx - dy));
    }
    s *= facingW;
  }
  if (s <= 0.0) discard;
  oMask = vec4(s);
}
